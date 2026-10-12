// Builds nfsmw_shaders.nfsp from an extracted game folder (default.xex + NFS/), exactly as the installer does.
// Usage: node biblioteca_shaders.mjs <nfsmw-nx-installer clone> <game folder> <output .nfsp>
//   git clone https://github.com/StevensND/nfsmw-nx-installer out/host-tools/installer
import fs from 'node:fs';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { pathToFileURL } from 'node:url';

const [installer, game, output] = process.argv.slice(2);
if (!output) {
  console.error('uso: node biblioteca_shaders.mjs <clon de nfsmw-nx-installer> <carpeta del juego> <salida .nfsp>');
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
const { buildShaderLibrary } = await load('lib/shaders.js');
const quiet = () => ({ print: () => {}, printErr: () => {} });
const modules = { hlsl: await createHlslModule(quiet()), dxc: await createDxcModule(quiet()), pack: await createPackModule(quiet()), lzx: await createLzxModule(quiet()) };
const sha = (b) => createHash('sha256').update(b).digest('hex');
const manifest = JSON.parse(fs.readFileSync(new URL('release/manifest.json', base)));

const xex = new Uint8Array(fs.readFileSync(path.join(game, 'default.xex')));
const xexHash = sha(xex);
const build = manifest.builds.find((b) => b.xex_sha256 === xexHash);
console.log(`default.xex ${xexHash}: ${build ? build.edition : 'edicion desconocida'}`);
const { image } = await readXexImage(xex, async (c, bits, size) => {
  modules.lzx.FS.writeFile('/i.lzx', c);
  if (modules.lzx.callMain(['/i.lzx', '/i.bin', String(bits), String(size)])) throw new Error('LZX failed');
  return modules.lzx.FS.readFile('/i.bin');
});
const exe = new ContainerScanner('xex_');
exe.scanWhole('default.xex', image);

// Same files and order as the installer: NFS/*.bin sorted by lower-case path.
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
if (!blur) throw new Error('composition shader not found');
const library = await buildShaderLibrary(containers, modules, new Uint8Array(fs.readFileSync(new URL('shader_common.h', base))), () => {}, blur.name.slice(0, -4));
const libraryHash = sha(library);
console.log(`library: ${library.length} bytes, SHA-256 ${libraryHash}`);
console.log(build ? (libraryHash === build.library_sha256 ? 'OK: coincide con la biblioteca oficial' : `DIFIERE de la oficial (${build.library_sha256})`) : 'sin referencia');
fs.writeFileSync(output, library);
console.log(`written to ${output}`);
