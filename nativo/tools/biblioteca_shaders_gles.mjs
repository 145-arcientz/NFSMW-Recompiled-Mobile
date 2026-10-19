// Builds nfsmw_shaders_gles.nfsp, the shader library of the OpenGL ES backend, from an extracted game folder
// (default.xex + NFS/). Same containers, translator, DXC and packer as biblioteca_shaders.mjs (the installer's
// WASM modules), with two differences:
//
//   1. GLES has no descriptor indexing. The Vulkan library reads every texture from a heap
//      (g_Texture2DDescriptorHeap[i]) whose index comes from the shared constants. Here each sampler register
//      is its own resource: g_Tex2D_<register>, g_Tex3D_<register>, g_TexCube_<register> and
//      g_Sampler_<register>, in sets 0-3 with binding = register. The header (shader_common.h) is derived
//      from the installer's with its helpers taking the resources instead of the indices, and the translator's
//      `#define X_Texture2DDescriptorIndex (...)` lines are rewritten to name them.
//   2. No shadow minimum (nfsmw_nativo_sombra_minimo): its pair arrives through the 3D index word of the same
//      register, which here is a 3D texture. Without the marker in the SPIR-V the app does not ask for it.
//
// The cheap PCF and the composition blur switch are applied the same way as in the Vulkan library. The SPIR-V
// is converted to GLSL ES on the device (nfsmw_gles_shaders.cpp), so the library keeps the NFSSPV format and
// ShadersNativos loads it unchanged.
//
// Usage: node biblioteca_shaders_gles.mjs <nfsmw-nx-installer clone> <game folder> <output .nfsp> [hlsl folder]
// With a fourth argument the final HLSL of every shader is also written there (to debug a DXC rejection).
import fs from 'node:fs';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { pathToFileURL } from 'node:url';

const [installer, game, output, hlslOut] = process.argv.slice(2);
if (!output) {
  console.error('uso: node biblioteca_shaders_gles.mjs <clon de nfsmw-nx-installer> <carpeta del juego> <salida .nfsp> [carpeta hlsl]');
  process.exit(1);
}
const base = pathToFileURL(path.resolve(installer) + '/');
const load = async (rel) => import(new URL(rel, base));
const { default: createHlslModule } = await load('wasm/hlsl.mjs');
const { default: createDxcModule } = await load('wasm/dxc_web.mjs');
const { default: createPackModule } = await load('wasm/pack.mjs');
const { default: createLzxModule } = await load('wasm/lzx.mjs');
const { readXexImage } = await load('lib/xex.js');
const { ContainerScanner } = await load('lib/containers.js');
let dxcErrors = [];
const quiet = () => ({ print: () => {}, printErr: () => {} });
const modules = {
  hlsl: await createHlslModule(quiet()),
  dxc: await createDxcModule({ print: () => {}, printErr: (t) => dxcErrors.push(t) }),
  pack: await createPackModule(quiet()),
  lzx: await createLzxModule(quiet()),
};
const sha = (b) => createHash('sha256').update(b).digest('hex');
const manifest = JSON.parse(fs.readFileSync(new URL('release/manifest.json', base)));
const utf8 = new TextDecoder('utf-8');
const encoder = new TextEncoder();

function count(text, needle) {
  let n = 0;
  for (let i = text.indexOf(needle); i >= 0; i = text.indexOf(needle, i + needle.length)) n++;
  return n;
}

function fail(message) {
  console.error(message);
  process.exit(1);
}

// ---- 1. The GLES header, derived from the installer's ----

const REGISTERS = 16;
const DIMENSIONS = ['2D', '3D', 'Cube'];
const TYPES = { '2D': 'Texture2D<float4>', '3D': 'Texture3D<float4>', Cube: 'TextureCube<float4>' };

function glesHeader(original) {
  let h = original;
  const heaps =
    'Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);\n' +
    'Texture3D<float4> g_Texture3DDescriptorHeap[] : register(t0, space1);\n' +
    'TextureCube<float4> g_TextureCubeDescriptorHeap[] : register(t0, space2);\n' +
    'SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);\n';
  const normalized = h.replace(/\r\n/g, '\n');
  if (count(normalized, heaps) !== 1) fail('shader_common.h: the descriptor heap block is not there exactly once');
  const declarations = [];
  DIMENSIONS.forEach((d, set) => {
    for (let r = 0; r < REGISTERS; r++) declarations.push(`${TYPES[d]} g_Tex${d}_${r} : register(t${r}, space${set});`);
  });
  for (let r = 0; r < REGISTERS; r++) declarations.push(`SamplerState g_Sampler_${r} : register(s${r}, space3);`);
  h = normalized.replace(heaps, '// NFSMW (GLES): one resource per sampler register instead of descriptor heaps.\n' +
    declarations.join('\n') + '\n');

  // The shared block is 488 bytes (1/size of the 16 registers ends at 360 + 16 * 8), but the header declares
  // 23 float4: the 1/size of registers 2 and up is read past the end. Vulkan does not mind as long as the bound
  // range covers it; GLSL rejects a constant index out of range. 31 float4 = 496 bytes.
  const shared = 'struct NfsmwBloqueCompartidas { float4 v[23]; };';
  if (count(h, shared) !== 1) fail('shader_common.h: the shared block declaration is not there exactly once');
  h = h.replace(shared, 'struct NfsmwBloqueCompartidas { float4 v[31]; };');

  // The helpers take the resources: the heap lookup disappears and the parameter types change.
  const lookups = h.match(/g_(Texture2D|Texture3D|TextureCube|Sampler)DescriptorHeap\[(\w+)\]/g) || [];
  if (lookups.length < 8) fail(`shader_common.h: only ${lookups.length} heap lookups found`);
  h = h.replace(/g_(Texture2D|Texture3D|TextureCube|Sampler)DescriptorHeap\[(\w+)\]/g, '$2');
  let signatures = 0;
  h = h.replace(/^(\w+) (\w+)\(([^)\n]*DescriptorIndex[^)\n]*)\)$/gm, (line, ret, name, params) => {
    const texture = name === 'tfetch3D' ? TYPES['3D'] : name === 'tfetchCube' ? TYPES.Cube : TYPES['2D'];
    signatures++;
    const fixed = params
      .replace(/\buint (resourceDescriptorIndex|parejaDescriptorIndex)\b/g, `${texture} $1`)
      .replace(/\buint samplerDescriptorIndex\b/g, 'SamplerState samplerDescriptorIndex');
    return `${ret} ${name}(${fixed})`;
  });
  if (signatures < 8) fail(`shader_common.h: only ${signatures} helper signatures rewritten`);
  if (/DescriptorHeap|uint \w*DescriptorIndex/.test(h)) fail('shader_common.h: a heap or an index survived the rewrite');
  return '#define NFSMW_GLES 1\n' + h;
}

// ---- 2. Containers (same files and order as the installer) ----

const xex = new Uint8Array(fs.readFileSync(path.join(game, 'default.xex')));
const build = manifest.builds.find((b) => b.xex_sha256 === sha(xex));
console.log(`default.xex: ${build ? build.edition : 'edicion desconocida'}`);
const { image } = await readXexImage(xex, async (c, bits, size) => {
  modules.lzx.FS.writeFile('/i.lzx', c);
  if (modules.lzx.callMain(['/i.lzx', '/i.bin', String(bits), String(size)])) throw new Error('LZX failed');
  return modules.lzx.FS.readFile('/i.bin');
});
const exe = new ContainerScanner('xex_');
exe.scanWhole('default.xex', image);
const nfs = path.join(game, 'NFS');
const files = fs.readdirSync(nfs).filter((n) => /\.bin$/i.test(n)).map((n) => ({ rel: `NFS/${n}`, file: path.join(nfs, n) }))
  .map((f) => ({ ...f, size: fs.statSync(f.file).size })).filter((f) => f.size >= 24 && f.size <= 1024 * 1024 * 1024)
  .sort((a, b) => a.rel.toLowerCase().localeCompare(b.rel.toLowerCase()));
const disc = new ContainerScanner('');
const chunk = Buffer.alloc(16 << 20);
for (const f of files) {
  disc.beginFile(f.rel, f.size);
  const fd = fs.openSync(f.file, 'r');
  let done = 0;
  while (done < f.size) {
    const n = fs.readSync(fd, chunk, 0, chunk.length, done);
    done += n;
    disc.push(new Uint8Array(chunk.buffer, 0, n).slice(), done >= f.size);
  }
  fs.closeSync(fd);
}
const containers = [...disc.found, ...exe.found];
console.log(`containers: ${disc.found.length} on disc + ${exe.found.length} in the XEX`);
const blurSha = build ? build.blur_container_sha256 : 'e80cca037bd4becd8a192daa4583e3a3a9281e06436015746583f30701a296f9';
const blur = containers.find((c) => sha(c.bytes) === blurSha);
if (!blur) fail('composition shader not found');
const blurShader = blur.name.slice(0, -4);

// ---- 3. Translation and source rewrites ----

const { hlsl, dxc, pack } = modules;
hlsl.FS.mkdirTree('/in');
for (const c of containers) hlsl.FS.writeFile(`/in/${c.name}`, c.bytes);
const header = glesHeader(utf8.decode(fs.readFileSync(new URL('shader_common.h', base))));
hlsl.FS.writeFile('/shader_common.h', encoder.encode(header));
const translated = hlsl.callMain(['/in', '/out', '/shader_common.h']);
if (translated !== 0 && translated !== undefined) fail(`shader translation failed (${translated})`);

// `#define NAME_Texture2DDescriptorIndex (NFSMW_UBO ? NFSMW_COMPARTIDA_UINT(<byte>) : ...)`: the byte is
// dimension * 64 + register * 4 (textures) or 192 + register * 4 (sampler), as in shader_recompiler.cpp.
const INDEX_DEFINE = /^#define (\w+)_(Texture(2D|3D|Cube)|Sampler)DescriptorIndex \(NFSMW_UBO \? NFSMW_COMPARTIDA_UINT\((\d+)\) : [^\n]*$/gm;
const SHADOW_OLD = 'tfetch2D(SHADOWMAP_SAMPLER_';
const SHADOW_NEW = 'tfetch2DSombra(SHADOWMAP_SAMPLER_';
const BLUR_OLD = 'r5.xyz = r5.xyz * r0.xxx + r3.xyz;';
const BLUR_NEW = 'r5.xyz = (g_SpecConstants() & SPEC_CONSTANT_SIN_DESENFOQUE) ? r3.xyz : (r5.xyz * r0.xxx + r3.xyz);';

const sources = new Map();
let defines = 0;
let shadowCalls = 0;
for (const c of containers) {
  const stem = c.name.slice(0, -4);
  let text = utf8.decode(hlsl.FS.readFile(`/out/${stem}.hlsl`)).replace(/\r\n/g, '\n');
  text = text.replace(INDEX_DEFINE, (line, name, kind, dimension, byte) => {
    const offset = Number(byte);
    const first = dimension ? DIMENSIONS.indexOf(dimension) * 64 : 192;
    const register = (offset - first) / 4;
    if (!Number.isInteger(register) || register < 0 || register >= REGISTERS) fail(`${stem}: unexpected index define: ${line}`);
    defines++;
    return dimension ? `#define ${name}_Texture${dimension}DescriptorIndex g_Tex${dimension}_${register}`
                     : `#define ${name}_SamplerDescriptorIndex g_Sampler_${register}`;
  });
  if (/DescriptorIndex \(NFSMW_UBO/.test(text)) fail(`${stem}: an index define was not rewritten`);
  const n = count(text, SHADOW_OLD);
  if (n) {
    shadowCalls += n;
    text = text.split(SHADOW_OLD).join(SHADOW_NEW);
  }
  if (stem === blurShader) {
    if (count(text, BLUR_OLD) !== 1) fail('the composition blur mix was not found exactly once');
    text = text.replace(BLUR_OLD, BLUR_NEW);
  }
  sources.set(stem, text);
}
if (!shadowCalls) fail('no shadow map fetch was rewritten');
console.log(`translated ${sources.size}; ${defines} index defines rewritten; ${shadowCalls} shadow map fetches`);
if (hlslOut) {
  fs.mkdirSync(hlslOut, { recursive: true });
  for (const [stem, text] of sources) fs.writeFileSync(path.join(hlslOut, `${stem}.hlsl`), text);
}

// ---- 4. DXC and packing ----

// A SPIR-V module must not use the capabilities GLES cannot express (descriptor indexing, 64-bit integers,
// buffer device address). Returns the first one found, or null.
const FORBIDDEN = new Map([[5302, 'RuntimeDescriptorArray'], [11, 'Int64'], [5347, 'PhysicalStorageBufferAddresses']]);
function forbiddenCapability(spirv) {
  const words = new Uint32Array(spirv.buffer, spirv.byteOffset, Math.floor(spirv.length / 4));
  if (words[0] !== 0x07230203) return 'not SPIR-V';
  for (let i = 5; i < words.length;) {
    const n = words[i] >>> 16;
    if (!n || i + n > words.length) return 'malformed';
    if ((words[i] & 0xffff) === 17 && FORBIDDEN.has(words[i + 1])) return FORBIDDEN.get(words[i + 1]);
    i += n;
  }
  return null;
}

dxc.FS.mkdirTree('/work');
pack.FS.mkdirTree('/in');
pack.FS.mkdirTree('/spirv');
let done = 0;
for (const c of containers) {
  const stem = c.name.slice(0, -4);
  const input = `/work/${stem}.hlsl`;
  const out = `/work/${stem}.spv`;
  dxc.FS.writeFile(input, encoder.encode(sources.get(stem)));
  dxcErrors = [];
  const rc = dxc.ccall('compile', 'number', ['string', 'string', 'number'], [input, out, stem.startsWith('v_') || stem.startsWith('xex_v') ? 1 : 0]);
  if (rc !== 0) fail(`DXC rejected ${stem} (${rc})\n${dxcErrors.join('\n')}`);
  const spirv = dxc.FS.readFile(out);
  const capability = forbiddenCapability(spirv);
  if (capability) fail(`${stem}: SPIR-V uses ${capability}`);
  pack.FS.writeFile(`/spirv/${stem}.spv`, spirv);
  pack.FS.writeFile(`/in/${c.name}`, c.bytes);
  dxc.FS.unlink(input);
  dxc.FS.unlink(out);
  if (++done % 50 === 0) console.log(`compiled ${done}/${containers.length}`);
}
const packed = pack.callMain(['/in', '/spirv', '/nfsmw_shaders_gles.nfsp']);
if (packed !== 0 && packed !== undefined) fail(`packing failed (${packed})`);
const library = pack.FS.readFile('/nfsmw_shaders_gles.nfsp');
fs.mkdirSync(path.dirname(path.resolve(output)), { recursive: true });
fs.writeFileSync(output, library);
console.log(`library: ${library.length} bytes, SHA-256 ${sha(library)}, written to ${output}`);
