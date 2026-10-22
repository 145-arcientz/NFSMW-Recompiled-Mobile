// nfsmw - native renderer, parts C3-C6 on OpenGL ES (see the header).
//
// One draw, in the order of DibujosVulkanImpl::Dibujar (nfsmw_nativo_dibujos.cpp):
//   1. primitive type, render targets and the reasons to reject or skip it, read from the same registers;
//   2. indices (rebased to the first vertex on the CPU, so no BaseVertex is needed) and vertex ranges;
//   3. the vertex input of the patched VS (CalcularEntrada, with GL formats);
//   4. textures and samplers of the PS registers, and the shared constants (g_InputRemap, half pixel, alpha
//      test, NDC, 1/size of each texture);
//   5. vertices, indices and the three uniform blocks into a stream buffer (persistent with
//      GL_EXT_buffer_storage, glBufferSubData otherwise), fenced per segment;
//   6. the program (VS and PS of the library converted to GLSL ES, the VS keeping only the outputs the PS reads)
//      and the fixed-function state, applied only where it changes;
//   7. the draw.
// The fixed-function tables (blend factors and operations, compare functions, stencil operations, culling and
// front face) are those of RellenarEstadoFijo; GL numbers framebuffer rows like Vulkan for the same NDC, so the
// viewport, scissor and winding need no change (see nfsmw_gles_shaders.h).

#include "nfsmw_nativo_dibujos_gles.h"

#include "nfsmw_gles_shaders.h"
#include "nfsmw_nativo_shaders.h"
#include "nfsmw_nativo_texturas_comun.h"
#include "nfsmw_reflejo_demanda.h"  // nfsmw_reflejo_bajo_demanda, nfsmw_reflejo_visibilidad
#include "nfsmw_shader_library.h"

#include <EGL/egl.h>
#include <GLES2/gl2ext.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/ui/gles/provider.h>

#include <time.h>
#include <unistd.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// See nfsmw_shader_library.cpp: xxHash with memcpy reads.
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

REXCVAR_DECLARE(bool, nfsmw_nativo_sin_desenfoque);
REXCVAR_DECLARE(bool, nfsmw_nativo_mipmaps);
REXCVAR_DECLARE(bool, nfsmw_nativo_pcf_barato);
REXCVAR_DECLARE(int32_t, nfsmw_nativo_anisotropico);
REXCVAR_DECLARE(int32_t, nfsmw_nativo_sombras_sesgo_pendiente);
REXCVAR_DECLARE(int32_t, nfsmw_nativo_sombras_sesgo_constante);
REXCVAR_DECLARE(int32_t, nfsmw_nativo_texturas_mb_max);
// The same savings as the Vulkan draws, with the same cvars (see nfsmw_nativo_dibujos.cpp).
REXCVAR_DECLARE(bool, nfsmw_nativo_dedupe_vertices);
REXCVAR_DECLARE(bool, nfsmw_sombras_sin_vegetacion);
REXCVAR_DECLARE(bool, nfsmw_nativo_saltar_invisibles);
REXCVAR_DECLARE(bool, nfsmw_nativo_z_temprana);
REXCVAR_DECLARE(bool, nfsmw_nativo_indices_neon);
REXCVAR_DECLARE(bool, nfsmw_nativo_cielo_aplazado);
REXCVAR_DECLARE(bool, nfsmw_nativo_bc1_16_bits);

REXCVAR_DEFINE_BOOL(nfsmw_nativo_gles_etapas, false, "NFSMW",
                    "OpenGL ES: cronometra cada dibujo por fases (entrada, destinos, indices, vertices, texturas, "
                    "encuadre, programa, subidas, estado, dibujo) y las copias y Swaps de C2, y lo escribe en el log "
                    "cada 600 fotogramas como \"C6 GLES etapas\". Cuesta unas 20 lecturas de reloj por dibujo: solo "
                    "para medir");

REXCVAR_DEFINE_BOOL(nfsmw_nativo_gles_base_vertice, true, "NFSMW",
                    "OpenGL ES 3.2: los dibujos se desplazan con el vertice base (glDrawElementsBaseVertex). Los indices "
                    "de 16 bits van del juego al bufer de subida sin reescribirlos en la CPU, y con un solo stream de "
                    "vertices los atributos no se vuelven a apuntar en cada dibujo. false = como antes")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(nfsmw_nativo_gles_cache_programas, true, "NFSMW",
                    "OpenGL ES: guarda los programas enlazados (glGetProgramBinary) en "
                    "cache/nfsmw_nativo_programas_gles.bin y los carga de ahi en las partidas siguientes: sin los tirones "
                    "de 20-110 ms de convertir, compilar y enlazar cada uno la primera vez. Se descarta sola si cambia el "
                    "driver. false = se compilan siempre")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(nfsmw_nativo_gles_memo_texturas, true, "NFSMW",
                    "OpenGL ES: si un registro de textura tiene la misma fetch constant que en el dibujo anterior del mismo "
                    "fotograma (y C2 no ha copiado nada entre medias), se reutiliza su textura y su sampler sin buscarlos. "
                    "false = se buscan en cada dibujo");

REXCVAR_DEFINE_BOOL(nfsmw_nativo_gles_depurar, false, "NFSMW",
                    "OpenGL ES: escribe en el log los mensajes de depuracion del driver (KHR_debug), los 200 primeros. "
                    "Cuesta CPU: solo para buscar fallos")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace nfsmw::nativo {

// Incremented by the ring at every WAIT_REG_MEM (nfsmw_nativo_dibujos.cpp): there the vertex deduplication forgets
// what it recorded, as in Vulkan.
extern std::atomic<uint32_t> g_sincronizaciones_anillo;

namespace {

namespace gr = rex::graphics;
namespace xenos = rex::graphics::xenos;
using rex::ui::gles::GlesProvider;

constexpr uint32_t kRegConstantesVs = 0x4000;
constexpr uint32_t kRegConstantesPs = 0x4400;
constexpr uint32_t kRegFetch = 0x4800;
constexpr uint32_t kRegBooleanos = 0x4900;
constexpr uint32_t kPalabrasCompartidas = 122;
constexpr uint32_t kPalabraInvTamano = 90;
constexpr uint32_t kUboBytesVs = 256 * 16;
constexpr uint32_t kUboBytesPs = 224 * 16;
constexpr uint32_t kUboBytesCompartidas = 31 * 16;  // the GLES library's block (biblioteca_shaders_gles.mjs)
constexpr uint32_t kMaxVerticesPorDibujo = uint32_t(1) << 20;
constexpr uint64_t kMemoriaFisica = 0x20000000;
// Specialization bits of shader_common.h (the same as nfsmw_nativo_dibujos.cpp).
constexpr uint32_t kSpecInvTamanoTex = uint32_t(1) << 9;
constexpr uint32_t kSpecPcfBarato = uint32_t(1) << 15;
constexpr uint32_t kSpecFuncionAlfaDesplazamiento = 16;
constexpr uint32_t kSpecSinDesenfoque = uint32_t(1) << 19;
constexpr uint64_t kHuellaComposicion = 0x19C0C358044A29BFull;  // p_000139, the race VisualTreatment
constexpr uint64_t kHuellaBrightPass = 0xE849A9F6D3323B87ull;   // p_000094 (no early Z, as in Vulkan)
// Program variants (bits of the program key): early depth test, and the occlusion query's sample counter.
constexpr uint32_t kVarianteZTemprana = 0x1;
constexpr uint32_t kVarianteContar = 0x2;
// nfsmw_reflejo_visibilidad: our own query around a draw. Conservative: it may say "some" when there were none
// (the reflection is then drawn, the safe side) but never "none" when there were some.
constexpr GLenum kConsultaVisibilidad = GL_ANY_SAMPLES_PASSED_CONSERVATIVE;
constexpr uint64_t kTestigoCada = 32;  // frames between witnesses once verified (2 until then)
// nfsmw_nativo_cielo_aplazado (see nfsmw_nativo_dibujos.cpp): the sky's pixel shader, by its container's
// fingerprint (p_000098, CloudIntensity and SkyAlphaTag), and the dome's index count (160 triangles).
constexpr uint64_t kHuellaCielo = 0x28AA3CDAC6C19705ull;
constexpr GLsizei kIndicesDomoCielo = 480;
constexpr uint32_t kCieloFotogramasPrueba = 90;
constexpr uint32_t kCieloFotogramasConUno = 90;
// The phases of a draw for nfsmw_nativo_gles_etapas.
constexpr const char* kNombresEtapas[] = {"entrada", "destinos", "indices", "vertices", "texturas", "encuadre",
                                          "programa", "subidas", "estado", "dibujo"};
constexpr uint32_t kEtapas = sizeof(kNombresEtapas) / sizeof(kNombresEtapas[0]);

uint64_t RelojNs() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return uint64_t(t.tv_sec) * 1000000000ull + uint64_t(t.tv_nsec);
}
// The stream buffer: four segments, each fenced when the write head leaves it.
constexpr size_t kTamanoFlujo = size_t(64) << 20;
constexpr uint32_t kSegmentosFlujo = 4;

// GL_EXT_buffer_storage, loaded with eglGetProcAddress.
using FnBufferStorage = void(GL_APIENTRYP)(GLenum, GLsizeiptr, const void*, GLbitfield);
constexpr GLbitfield kMapaPersistente = 0x0040;  // GL_MAP_PERSISTENT_BIT_EXT
constexpr GLbitfield kMapaCoherente = 0x0080;    // GL_MAP_COHERENT_BIT_EXT
constexpr GLenum kMirrorClampToEdge = 0x8743;    // GL_MIRROR_CLAMP_TO_EDGE_EXT
constexpr GLenum kClampToBorder = 0x812D;        // GL_CLAMP_TO_BORDER (3.2)
constexpr GLenum kR16 = 0x822A, kRG16 = 0x822C, kRGBA16 = 0x805B;  // GL_EXT_texture_norm16
constexpr GLenum kAnisotropia = 0x84FE;          // GL_TEXTURE_MAX_ANISOTROPY_EXT

// One guest vertex binding into the stream buffer, as host words with the fetch constant's byte order
// (CopiarVertices in nfsmw_nativo_dibujos.cpp).
void CopiarPalabras(const uint8_t* origen, uint8_t* destino, uint32_t palabras, xenos::Endian orden) {
  if (orden == xenos::Endian::kNone) {
    std::memcpy(destino, origen, size_t(palabras) * 4);
    return;
  }
  uint32_t i = 0;
#if defined(__aarch64__)
  // 16 bytes per step, straight from the guest to the stream buffer (only stores: it may be uncached memory). The
  // same as GpuSwap: k8in32 reverses the 4 bytes, k8in16 the 2 of each half and k16in32 swaps the halves.
  const size_t bloques = palabras / 4;
  if (orden == xenos::Endian::k8in32) {
    for (size_t b = 0; b < bloques; ++b) vst1q_u8(destino + b * 16, vrev32q_u8(vld1q_u8(origen + b * 16)));
  } else if (orden == xenos::Endian::k8in16) {
    for (size_t b = 0; b < bloques; ++b) vst1q_u8(destino + b * 16, vrev16q_u8(vld1q_u8(origen + b * 16)));
  } else {
    for (size_t b = 0; b < bloques; ++b) {
      vst1q_u8(destino + b * 16, vreinterpretq_u8_u16(vrev32q_u16(vreinterpretq_u16_u8(vld1q_u8(origen + b * 16)))));
    }
  }
  i = uint32_t(bloques * 4);
#endif
  if (orden == xenos::Endian::k8in32) {
    for (; i < palabras; ++i) {
      uint32_t v;
      std::memcpy(&v, origen + size_t(i) * 4, 4);
      v = std::byteswap(v);
      std::memcpy(destino + size_t(i) * 4, &v, 4);
    }
  } else {
    for (; i < palabras; ++i) {
      uint32_t v;
      std::memcpy(&v, origen + size_t(i) * 4, 4);
      v = xenos::GpuSwap(v, orden);
      std::memcpy(destino + size_t(i) * 4, &v, 4);
    }
  }
}

// 16-bit indices, byte-swapped or not, with their minimum and maximum (IndicesDe16Escalar and IndicesDe16Neon of
// nfsmw_nativo_dibujos.cpp, the same bodies).
void IndicesDe16Escalar(const uint8_t* datos, uint32_t cuenta, uint16_t* salida, bool girar, uint32_t& minimo,
                        uint32_t& maximo) {
  minimo = 0xFFFF;
  maximo = 0;
  for (uint32_t i = 0; i < cuenta; ++i) {
    uint16_t v;
    std::memcpy(&v, datos + size_t(i) * 2, 2);
    if (girar) {
      v = std::byteswap(v);
    }
    salida[i] = v;
    minimo = std::min<uint32_t>(minimo, v);
    maximo = std::max<uint32_t>(maximo, v);
  }
}

void IndicesDe16Neon(const uint8_t* datos, uint32_t cuenta, uint16_t* salida, bool girar, uint32_t& minimo,
                     uint32_t& maximo) {
#if defined(__aarch64__)
  uint32_t i = 0;
  uint32_t mn = 0xFFFF;
  uint32_t mx = 0;
  if (cuenta >= 16) {
    uint16x8_t min_a = vdupq_n_u16(0xFFFF);
    uint16x8_t min_b = min_a;
    uint16x8_t max_a = vdupq_n_u16(0);
    uint16x8_t max_b = max_a;
    for (; i + 16 <= cuenta; i += 16) {
      uint8x16_t ba = vld1q_u8(datos + size_t(i) * 2);
      uint8x16_t bb = vld1q_u8(datos + size_t(i) * 2 + 16);
      if (girar) {
        ba = vrev16q_u8(ba);
        bb = vrev16q_u8(bb);
      }
      const uint16x8_t a = vreinterpretq_u16_u8(ba);
      const uint16x8_t b = vreinterpretq_u16_u8(bb);
      vst1q_u16(salida + i, a);
      vst1q_u16(salida + i + 8, b);
      min_a = vminq_u16(min_a, a);
      min_b = vminq_u16(min_b, b);
      max_a = vmaxq_u16(max_a, a);
      max_b = vmaxq_u16(max_b, b);
    }
    mn = vminvq_u16(vminq_u16(min_a, min_b));
    mx = vmaxvq_u16(vmaxq_u16(max_a, max_b));
  }
  for (; i < cuenta; ++i) {
    uint16_t v;
    std::memcpy(&v, datos + size_t(i) * 2, 2);
    if (girar) {
      v = std::byteswap(v);
    }
    salida[i] = v;
    mn = std::min<uint32_t>(mn, v);
    mx = std::max<uint32_t>(mx, v);
  }
  minimo = mn;
  maximo = mx;
#else
  IndicesDe16Escalar(datos, cuenta, salida, girar, minimo, maximo);
#endif
}

/*
 * The fragment stage of a program variant (kVariante*), from the GLSL of SPIRV-Cross:
 *  - early depth test: layout(early_fragment_tests), as nfsmw_nativo_z_temprana does in Vulkan;
 *  - sample counter: the shader's main becomes nfsmw_main, and the new main calls it and then adds 1 to the atomic
 *    counter at binding 0. A fragment that discards never gets there, and with the early test one that fails the
 *    depth or stencil test is not even shaded: the counter ends up with the samples that pass, what the occlusion
 *    query of the Xbox 360 counts (GL_ANY_SAMPLES_PASSED only says yes or no).
 * Empty if the source has no main.
 */
std::string VarianteFragmentos(std::string fuente, uint32_t variantes) {
  const size_t m = fuente.find("void main()");
  if (m == std::string::npos) {
    return {};
  }
  std::string declaraciones;
  if (fuente.find("early_fragment_tests") == std::string::npos) {
    declaraciones += "layout(early_fragment_tests) in;\n";
  }
  if (variantes & kVarianteContar) {
    declaraciones += "layout(binding = 0, offset = 0) uniform atomic_uint nfsmw_muestras;\n";
    fuente.replace(m, 11, "void nfsmw_main()");
    fuente += "\nvoid main()\n{\n    nfsmw_main();\n    atomicCounterIncrement(nfsmw_muestras);\n}\n";
  }
  fuente.insert(m, declaraciones);
  return fuente;
}

// KHR_debug messages: errors and warnings, the first 200.
void GL_APIENTRY MensajeDepuracion(GLenum fuente, GLenum tipo, GLuint id, GLenum gravedad, GLsizei longitud,
                                   const GLchar* mensaje, const void* usuario) {
  (void)longitud;
  (void)usuario;
  static uint32_t mensajes = 0;
  if (gravedad == GL_DEBUG_SEVERITY_NOTIFICATION || ++mensajes > 200) {
    return;
  }
  REXLOG_WARN("[nativo] GLES depuracion {}: fuente {:#x} tipo {:#x} id {} gravedad {:#x}: {}", mensajes, fuente, tipo,
              id, gravedad, mensaje ? mensaje : "");
}

// --- Vertex input ----------------------------------------------------------------------------------------------

struct FormatoAtributoGles {
  GLint componentes = 4;
  GLenum tipo = GL_FLOAT;
  bool normalizado = false;
  bool entero = false;  // glVertexAttribIPointer (uint4 inputs)
};

// The vertex formats of FormatoAtributo (nfsmw_nativo_dibujos.cpp) in GL terms. `entrada_entera`: the shader input
// is uint4 (BLENDINDICES); `entero`: the fetch asks for integers without normalizing (USCALED/SSCALED, which GL
// converts to float with normalized = false).
bool FormatoAtributoGl(uint32_t formato, bool entrada_entera, bool con_signo, bool entero, FormatoAtributoGles& f,
                       bool& r11g11b10) {
  r11g11b10 = false;
  f = FormatoAtributoGles{};
  f.entero = entrada_entera;
  f.normalizado = !entrada_entera && !entero;
  switch (formato) {
    case 6:  // k_8_8_8_8
      f.componentes = 4;
      f.tipo = con_signo ? GL_BYTE : GL_UNSIGNED_BYTE;
      return true;
    case 7:  // k_2_10_10_10
      if (entrada_entera) return false;
      f.componentes = 4;
      f.tipo = con_signo ? GL_INT_2_10_10_10_REV : GL_UNSIGNED_INT_2_10_10_10_REV;
      return true;
    case 16:  // k_10_11_11: packed normal decoded by the shader itself
      if (!entrada_entera) return false;
      r11g11b10 = true;
      f.componentes = 1;
      f.tipo = GL_UNSIGNED_INT;
      return true;
    case 25:  // k_16_16
      f.componentes = 2;
      f.tipo = con_signo ? GL_SHORT : GL_UNSIGNED_SHORT;
      return true;
    case 26:  // k_16_16_16_16
      f.componentes = 4;
      f.tipo = con_signo ? GL_SHORT : GL_UNSIGNED_SHORT;
      return true;
    case 31:  // k_16_16_FLOAT
    case 32:  // k_16_16_16_16_FLOAT
      if (entrada_entera) return false;
      f.componentes = formato == 31 ? 2 : 4;
      f.tipo = GL_HALF_FLOAT;
      f.normalizado = false;
      return true;
    case 33:  // k_32
    case 34:  // k_32_32
    case 35:  // k_32_32_32_32
      if (!entrada_entera) return false;
      f.componentes = formato == 33 ? 1 : formato == 34 ? 2 : 4;
      f.tipo = con_signo ? GL_INT : GL_UNSIGNED_INT;
      return true;
    // Floats: a uint4 input reinterprets the bits (asfloat in the shader).
    case 36:  // k_32_FLOAT
    case 37:  // k_32_32_FLOAT
    case 57:  // k_32_32_32_FLOAT
    case 38:  // k_32_32_32_32_FLOAT
      f.componentes = formato == 36 ? 1 : formato == 37 ? 2 : formato == 57 ? 3 : 4;
      f.tipo = entrada_entera ? GL_UNSIGNED_INT : GL_FLOAT;
      f.normalizado = false;
      return true;
    default:
      return false;
  }
}

struct AtributoGles {
  uint32_t ubicacion = 0;
  uint32_t enlace = 0;
  uint32_t offset = 0;
  FormatoAtributoGles formato;
};

struct EnlaceGles {
  uint32_t ranura = 0;   // vertex fetch constant (0-95)
  uint32_t zancada = 0;  // bytes
};

struct EntradaGles {
  std::vector<AtributoGles> atributos;
  std::vector<EnlaceGles> enlaces;
  std::array<uint32_t, 16> remapeos{};  // g_InputRemap by location
  uint32_t especializacion = 0;
};

// --- Stream buffer ---------------------------------------------------------------------------------------------

class BuferFlujo {
 public:
  ~BuferFlujo() {
    for (GLsync& v : vallas_) {
      if (v) glDeleteSync(v);
    }
    if (bufer_) {
      if (mapa_) {
        glBindBuffer(GL_ARRAY_BUFFER, bufer_);
        glUnmapBuffer(GL_ARRAY_BUFFER);
      }
      glDeleteBuffers(1, &bufer_);
    }
  }

  bool Iniciar(bool persistente) {
    glGenBuffers(1, &bufer_);
    glBindBuffer(GL_ARRAY_BUFFER, bufer_);
    const auto almacenamiento =
        persistente ? reinterpret_cast<FnBufferStorage>(eglGetProcAddress("glBufferStorageEXT")) : nullptr;
    if (almacenamiento) {
      almacenamiento(GL_ARRAY_BUFFER, GLsizeiptr(kTamanoFlujo), nullptr,
                     GL_MAP_WRITE_BIT | kMapaPersistente | kMapaCoherente);
      mapa_ = static_cast<uint8_t*>(glMapBufferRange(GL_ARRAY_BUFFER, 0, GLsizeiptr(kTamanoFlujo),
                                                     GL_MAP_WRITE_BIT | kMapaPersistente | kMapaCoherente));
      if (!mapa_) {
        REXLOG_WARN("[nativo] C6 GLES: no se pudo mapear el bufer persistente; se sube con glBufferSubData");
        glDeleteBuffers(1, &bufer_);
        glGenBuffers(1, &bufer_);
        glBindBuffer(GL_ARRAY_BUFFER, bufer_);
      }
    }
    if (!mapa_) {
      glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(kTamanoFlujo), nullptr, GL_STREAM_DRAW);
      sombra_.resize(kTamanoFlujo);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return glGetError() == GL_NO_ERROR;
  }

  bool persistente() const { return mapa_ != nullptr; }
  GLuint bufer() const { return bufer_; }

  // Room for `bytes` aligned to `alineacion`: where to write and its offset in the buffer. nullptr if it does
  // not fit in a segment.
  // The alignment need not be a power of two: a single vertex stream goes at a multiple of its stride (the draw's
  // base vertex is its offset divided by the stride).
  uint8_t* Reservar(size_t bytes, size_t alineacion, GLintptr& offset) {
    constexpr size_t kSegmento = kTamanoFlujo / kSegmentosFlujo;
    if (!alineacion || bytes + alineacion > kSegmento) {
      return nullptr;
    }
    const auto alinear = [alineacion](size_t x) { return (x + alineacion - 1) / alineacion * alineacion; };
    size_t inicio = alinear(cabeza_);
    // A reservation never straddles two segments: if it does not fit in this one, the next one starts (aligned
    // again: a segment's start is not a multiple of every stride).
    if (inicio / kSegmento != (inicio + bytes - 1) / kSegmento || inicio + bytes > kTamanoFlujo) {
      size_t siguiente = (inicio / kSegmento + 1) * kSegmento;
      if (siguiente >= kTamanoFlujo) {
        siguiente = 0;
      }
      inicio = alinear(siguiente);
    }
    const uint32_t segmento = uint32_t(inicio / kSegmento);
    if (segmento != segmento_actual_) {
      CambiarDeSegmento(segmento);
    }
    cabeza_ = inicio + bytes;
    offset = GLintptr(inicio);
    if (!mapa_) {
      pendiente_inicio_ = std::min(pendiente_inicio_, inicio);
      pendiente_fin_ = std::max(pendiente_fin_, cabeza_);
      return sombra_.data() + inicio;
    }
    return mapa_ + inicio;
  }

  // Before the draw that reads what was reserved: without a persistent map, it is uploaded now.
  void Confirmar() {
    if (mapa_ || pendiente_fin_ <= pendiente_inicio_) {
      return;
    }
    glBindBuffer(GL_ARRAY_BUFFER, bufer_);
    glBufferSubData(GL_ARRAY_BUFFER, GLintptr(pendiente_inicio_), GLsizeiptr(pendiente_fin_ - pendiente_inicio_),
                    sombra_.data() + pendiente_inicio_);
    pendiente_inicio_ = SIZE_MAX;
    pendiente_fin_ = 0;
  }

  // Absolute number of the segment being written (only grows): what was written in segment `n` stays valid
  // while the head has not come back to it.
  uint64_t SegmentoAbsoluto() const { return segmentos_recorridos_; }
  bool SigueVivo(uint64_t segmento) const { return segmentos_recorridos_ < segmento + kSegmentosFlujo - 1; }

  uint64_t esperas() const { return esperas_; }

 private:
  void CambiarDeSegmento(uint32_t nuevo) {
    // What is pending goes up before leaving: the segment's fence must cover it.
    Confirmar();
    // The commands issued so far read the segment being left: its fence goes after them.
    if (vallas_[segmento_actual_]) {
      glDeleteSync(vallas_[segmento_actual_]);
    }
    vallas_[segmento_actual_] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    // The one about to be overwritten: the GPU must have finished with it.
    if (vallas_[nuevo]) {
      const GLenum estado = glClientWaitSync(vallas_[nuevo], GL_SYNC_FLUSH_COMMANDS_BIT, 0);
      if (estado == GL_TIMEOUT_EXPIRED) {
        ++esperas_;
        glClientWaitSync(vallas_[nuevo], GL_SYNC_FLUSH_COMMANDS_BIT, GLuint64(5000000000));
      }
      glDeleteSync(vallas_[nuevo]);
      vallas_[nuevo] = nullptr;
    }
    segmento_actual_ = nuevo;
    ++segmentos_recorridos_;
  }

  GLuint bufer_ = 0;
  uint8_t* mapa_ = nullptr;
  std::vector<uint8_t> sombra_;  // without a persistent map: what glBufferSubData uploads
  size_t pendiente_inicio_ = SIZE_MAX;
  size_t pendiente_fin_ = 0;
  size_t cabeza_ = 0;
  uint32_t segmento_actual_ = 0;
  uint64_t segmentos_recorridos_ = 0;
  std::array<GLsync, kSegmentosFlujo> vallas_{};
  uint64_t esperas_ = 0;
};

// --- Program binaries on disk ----------------------------------------------------------------------------------
//
// cache/nfsmw_nativo_programas_gles.bin, next to Vulkan's pipelines file. A header ("NFGL", version, and the driver's
// identity: GL_VENDOR, GL_RENDERER and GL_VERSION) and then one record per program: key, binary format, sampler
// count, binary size, the samplers (4 bytes each) and the binary. Only the index stays in memory; a binary is read
// when its program is first needed. New programs are appended. Another driver or a damaged file starts from zero (a
// truncated last record, from a crash while writing, is cut off).
//
// The key (ClaveDisco) is the SPIR-V of both shaders, the specialization and the variants: a new shader library
// gives new keys by itself. kVersionProgramas must go up if the GLSL that nfsmw_gles_shaders.cpp emits changes.

constexpr uint32_t kMagiaProgramas = 0x4C47464Eu;  // "NFGL" in little-endian
constexpr uint32_t kVersionProgramas = 1;
constexpr const char* kFicheroProgramas = "nfsmw_nativo_programas_gles.bin";
constexpr uint64_t kMaxBytesProgramas = uint64_t(128) << 20;

struct SamplerEnDisco {
  uint8_t dimension, registro, unidad, solo_tamano;
};

class CacheProgramas {
 public:
  struct Registro {
    GLenum formato = 0;
    uint64_t offset = 0;  // of the binary in the file
    uint32_t bytes = 0;
    std::vector<gles::SamplerGlsl> samplers;
  };

  ~CacheProgramas() {
    if (escritura_) std::fclose(escritura_);
    if (lectura_) std::fclose(lectura_);
  }

  bool activa() const { return escritura_ != nullptr; }
  size_t registros() const { return indice_.size(); }

  void Iniciar(const std::string& identidad) {
    GLint formatos = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formatos);
    if (formatos <= 0) {
      REXLOG_INFO("[nativo] C6 GLES: el driver no guarda programas (sin formatos binarios): se compilan siempre");
      return;
    }
    ruta_ = rex::filesystem::GetExecutableFolder() / "cache" / kFicheroProgramas;
    std::error_code error;
    std::filesystem::create_directories(ruta_.parent_path(), error);
    const uint64_t validos = Leer(identidad);
    if (validos == 0) {
      // New file: just the header.
      indice_.clear();
      escritura_ = std::fopen(ruta_.string().c_str(), "wb");
      if (!escritura_) {
        return;
      }
      const uint32_t cabecera[3] = {kMagiaProgramas, kVersionProgramas, uint32_t(identidad.size())};
      std::fwrite(cabecera, sizeof(cabecera), 1, escritura_);
      std::fwrite(identidad.data(), identidad.size(), 1, escritura_);
      std::fflush(escritura_);
      tamano_ = sizeof(cabecera) + identidad.size();
    } else {
      if (validos < tamano_) {
        // A truncated record at the end: the file is cut where the last whole one ends.
        std::filesystem::resize_file(ruta_, validos, error);
        tamano_ = validos;
      }
      escritura_ = std::fopen(ruta_.string().c_str(), "ab");
    }
    lectura_ = std::fopen(ruta_.string().c_str(), "rb");
    REXLOG_INFO("[nativo] C6 GLES: cache de programas {} con {} programas ({} KB)", ruta_.string(), indice_.size(),
                tamano_ >> 10);
  }

  const Registro* Buscar(uint64_t clave) const {
    const auto it = indice_.find(clave);
    return it != indice_.end() ? &it->second : nullptr;
  }

  bool LeerBinario(const Registro& r, std::vector<uint8_t>& binario) {
    if (!lectura_) {
      return false;
    }
    binario.resize(r.bytes);
    return std::fseek(lectura_, long(r.offset), SEEK_SET) == 0 && std::fread(binario.data(), 1, r.bytes, lectura_) == r.bytes;
  }

  void Olvidar(uint64_t clave) { indice_.erase(clave); }

  void Guardar(uint64_t clave, GLuint programa, const std::vector<gles::SamplerGlsl>& samplers) {
    if (!escritura_ || tamano_ >= kMaxBytesProgramas) {
      return;
    }
    GLint longitud = 0;
    glGetProgramiv(programa, GL_PROGRAM_BINARY_LENGTH, &longitud);
    if (longitud <= 0) {
      return;
    }
    std::vector<uint8_t> binario(static_cast<size_t>(longitud));
    GLenum formato = 0;
    GLsizei escritos = 0;
    glGetProgramBinary(programa, longitud, &escritos, &formato, binario.data());
    if (escritos <= 0) {
      return;
    }
    const uint32_t cabecera[3] = {formato, uint32_t(samplers.size()), uint32_t(escritos)};
    std::vector<SamplerEnDisco> en_disco;
    for (const gles::SamplerGlsl& sm : samplers) {
      en_disco.push_back({uint8_t(sm.dimension), uint8_t(sm.registro), uint8_t(sm.unidad), uint8_t(sm.solo_tamano)});
    }
    std::fwrite(&clave, sizeof(clave), 1, escritura_);
    std::fwrite(cabecera, sizeof(cabecera), 1, escritura_);
    if (!en_disco.empty()) {
      std::fwrite(en_disco.data(), sizeof(SamplerEnDisco), en_disco.size(), escritura_);
    }
    std::fwrite(binario.data(), 1, size_t(escritos), escritura_);
    std::fflush(escritura_);
    Registro r;
    r.formato = formato;
    r.offset = tamano_ + sizeof(clave) + sizeof(cabecera) + en_disco.size() * sizeof(SamplerEnDisco);
    r.bytes = uint32_t(escritos);
    r.samplers = samplers;
    tamano_ = r.offset + r.bytes;
    indice_[clave] = std::move(r);
  }

 private:
  // The index of the existing file. Returns how many bytes of it are valid (0: none, start a new one).
  uint64_t Leer(const std::string& identidad) {
    std::FILE* f = std::fopen(ruta_.string().c_str(), "rb");
    if (!f) {
      return 0;
    }
    std::fseek(f, 0, SEEK_END);
    tamano_ = uint64_t(std::ftell(f));
    std::fseek(f, 0, SEEK_SET);
    uint32_t cabecera[3] = {};
    std::string suya;
    bool ok = std::fread(cabecera, sizeof(cabecera), 1, f) == 1 && cabecera[0] == kMagiaProgramas &&
              cabecera[1] == kVersionProgramas && cabecera[2] < 4096;
    if (ok) {
      suya.resize(cabecera[2]);
      ok = std::fread(suya.data(), 1, suya.size(), f) == suya.size() && suya == identidad;
    }
    if (!ok) {
      std::fclose(f);
      REXLOG_INFO("[nativo] C6 GLES: la cache de programas es de otro driver o de otra version: se empieza de cero");
      return 0;
    }
    uint64_t pos = sizeof(cabecera) + suya.size();
    for (;;) {
      uint64_t clave = 0;
      uint32_t r[3] = {};
      if (std::fread(&clave, sizeof(clave), 1, f) != 1 || std::fread(r, sizeof(r), 1, f) != 1 || r[1] > 64) {
        break;
      }
      std::vector<SamplerEnDisco> en_disco(r[1]);
      if (r[1] && std::fread(en_disco.data(), sizeof(SamplerEnDisco), r[1], f) != r[1]) {
        break;
      }
      const uint64_t inicio = pos + sizeof(clave) + sizeof(r) + uint64_t(r[1]) * sizeof(SamplerEnDisco);
      if (inicio + r[2] > tamano_) {
        break;
      }
      std::fseek(f, long(inicio + r[2]), SEEK_SET);
      Registro reg;
      reg.formato = r[0];
      reg.offset = inicio;
      reg.bytes = r[2];
      for (const SamplerEnDisco& d : en_disco) {
        gles::SamplerGlsl sm;
        sm.dimension = d.dimension;
        sm.registro = d.registro;
        sm.unidad = d.unidad;
        sm.solo_tamano = d.solo_tamano != 0;
        reg.samplers.push_back(sm);
      }
      indice_[clave] = std::move(reg);  // a later copy of the same key wins
      pos = inicio + r[2];
    }
    std::fclose(f);
    return pos;
  }

  std::filesystem::path ruta_;
  std::FILE* escritura_ = nullptr;
  std::FILE* lectura_ = nullptr;
  uint64_t tamano_ = 0;
  std::unordered_map<uint64_t, Registro> indice_;
};

// --- Programs and textures -------------------------------------------------------------------------------------

struct ProgramaGles {
  GLuint programa = 0;
  bool fallido = false;
  std::vector<gles::SamplerGlsl> samplers;
};

struct TexturaGles {
  GLuint textura = 0;
  GLenum objetivo = GL_TEXTURE_2D;
  uint32_t ancho = 0;  // host size (what 1/size reports)
  uint32_t alto = 0;
  uint32_t niveles = 1;
  bool flotante32 = false;   // not filterable without GL_OES_texture_float_linear
  bool preparada = false;
  uint64_t huella = 0;       // XXH3 of the guest bytes of base and mips
  uint64_t siguiente = 0;    // frame of the next check
  uint32_t intervalo = 1;    // frames between checks: 1 to 32
  uint64_t fotograma = UINT64_MAX;
  uint32_t swizzle = UINT32_MAX;  // GL swizzle last set on it (packed)
  uint64_t bytes = 0;             // what it counts in bytes_texturas_ (eviction)
  uint8_t formato16 = 0;          // nfsmw_nativo_bc1_16_bits: 0 no, 1 RGB565, 2 RGB5_A1
  uint64_t ultimo_uso = 0;        // last frame a draw sampled it
};

// The texture a register samples in this draw.
struct TexturaDeRegistro {
  GLuint textura = 0;
  GLenum objetivo = GL_TEXTURE_2D;
  GLuint sampler = 0;
  uint32_t ancho = 0;
  uint32_t alto = 0;
};

GLenum ComponenteGl(uint32_t c) {
  switch (c) {
    case 0: return GL_RED;
    case 1: return GL_GREEN;
    case 2: return GL_BLUE;
    case 3: return GL_ALPHA;
    case 4: return GL_ZERO;
    default: return GL_ONE;
  }
}

// MapeoComponentes (nfsmw_nativo_dibujos.cpp) packed as 4 x 3 bits: 0-3 a host channel, 4 zero, 5 one.
uint32_t SwizzleGl(uint32_t swizzle_guest, uint16_t swizzle_host) {
  uint32_t r = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t valor = (swizzle_guest >> (i * 3)) & 0x7;
    uint32_t c;
    if (valor <= 3) {
      c = (swizzle_host >> (valor * 3)) & 0x7;
    } else if (valor == 4) {
      c = 4;
    } else {
      c = 5;
    }
    r |= c << (i * 3);
  }
  return r;
}

void PonerSwizzle(GLenum objetivo, uint32_t swizzle) {
  glTexParameteri(objetivo, GL_TEXTURE_SWIZZLE_R, GLint(ComponenteGl(swizzle & 7)));
  glTexParameteri(objetivo, GL_TEXTURE_SWIZZLE_G, GLint(ComponenteGl((swizzle >> 3) & 7)));
  glTexParameteri(objetivo, GL_TEXTURE_SWIZZLE_B, GLint(ComponenteGl((swizzle >> 6) & 7)));
  glTexParameteri(objetivo, GL_TEXTURE_SWIZZLE_A, GLint(ComponenteGl((swizzle >> 9) & 7)));
}

// The compare functions of Xenos are numbered as Vulkan's (and as these).
constexpr GLenum kComparaciones[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL,
                                      GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
// xenos::StencilOp, in VkStencilOp order.
constexpr GLenum kOperacionesEstencil[8] = {GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP,
                                            GL_DECR_WRAP};
// The blend factors of RellenarEstadoFijo (kFactores).
constexpr GLenum kFactores[17] = {GL_ZERO, GL_ONE, GL_ZERO, GL_ZERO, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR,
                                  GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR,
                                  GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_CONSTANT_COLOR,
                                  GL_ONE_MINUS_CONSTANT_COLOR, GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA,
                                  GL_SRC_ALPHA_SATURATE};

GLenum FactorGl(uint32_t f) {
  return f < 17 ? kFactores[f] : GL_ZERO;
}

GLenum OperacionGl(uint32_t op) {
  switch (op) {
    case 1: return GL_FUNC_SUBTRACT;
    case 2: return GL_MIN;
    case 3: return GL_MAX;
    case 4: return GL_FUNC_REVERSE_SUBTRACT;
    default: return GL_FUNC_ADD;
  }
}

// State that is only set when it changes. Valid only while `valido`.
struct EstadoAplicado {
  bool valido = false;
  GLuint fbo = 0;
  GLuint programa = 0;
  std::array<uint32_t, 4> mezcla{};
  uint32_t mascaras = 0;
  uint32_t presentes = 0;  // color targets of the framebuffer (bits 0-3)
  uint32_t profundidad = 0;
  bool con_profundidad = false;
  uint32_t rasterizado = 0;
  float sesgo[2] = {};
  uint32_t estencil[2] = {};
  float mezcla_constante[4] = {};
  GLint viewport[4] = {};
  float rango[2] = {};
  GLint tijera[4] = {};
  bool reinicio = false;
  std::array<GLuint, 48> texturas{};
  std::array<GLenum, 48> objetivos{};
  std::array<GLuint, 48> samplers{};
  std::array<GLintptr, 3> ubos{};  // offsets bound at each uniform block binding
  GLuint contador = 0;             // GL_ATOMIC_COUNTER_BUFFER at binding 0
};

// Where a texture's current GL swizzle is noted and the one this draw needs (the deferred sky checks it again
// when it is emitted: another register may have changed it in between).
struct SwizzleDeRegistro {
  uint32_t* donde = nullptr;
  uint32_t valor = 0;
  GLenum objetivo = GL_TEXTURE_2D;
};

// What a texture register gave in the last draw of this frame (nfsmw_nativo_gles_memo_texturas).
struct MemoTextura {
  uint32_t fetch[6] = {};
  uint64_t generacion = UINT64_MAX;
  TexturaDeRegistro salida;
  uint64_t* uso = nullptr;        // the cached texture's last use, or null
  uint32_t* swizzle = nullptr;    // where the texture's current GL swizzle is noted, or null
  uint32_t swizzle_esperado = 0;
  GLenum objetivo = GL_TEXTURE_2D;
};

class DibujosGlesImpl final : public DibujosGles {
 public:
  DibujosGlesImpl(const GlesProvider* provider, rex::memory::Memory* memoria, ContextoDestinosGles* contexto)
      : provider_(provider), memoria_(memoria), contexto_(contexto) {}

  ~DibujosGlesImpl() override {
    // On the ring thread with its context current.
    for (auto& [clave, p] : programas_) {
      if (p.programa) glDeleteProgram(p.programa);
    }
    for (auto& [clave, s] : shaders_gl_) {
      if (s) glDeleteShader(s);
    }
    for (auto& [clave, t] : texturas_) {
      if (t.textura) glDeleteTextures(1, &t.textura);
    }
    for (auto& [clave, s] : samplers_) {
      glDeleteSamplers(1, &s);
    }
    for (auto& [clave, f] : fbos_) {
      glDeleteFramebuffers(1, &f.fbo);
    }
    for (GLuint t : vacias_) {
      if (t) glDeleteTextures(1, &t);
    }
    if (vao_) glDeleteVertexArrays(1, &vao_);
    for (const ConsultaVisibilidad& c : visibilidad_pendientes_) visibilidad_libres_.push_back(c.consulta);
    if (!visibilidad_libres_.empty()) {
      glDeleteQueries(GLsizei(visibilidad_libres_.size()), visibilidad_libres_.data());
    }
  }

  bool Inicializar() {
    const auto& c = provider_->capacidades();
    opciones_glsl_.version = c.version >= 32 ? 320 : 310;
    opciones_glsl_.clip_control = false;
    indexado_ = c.version >= 32;
    base_vertice_ = c.version >= 32 && REXCVAR_GET(nfsmw_nativo_gles_base_vertice);
    if (REXCVAR_GET(nfsmw_nativo_gles_cache_programas)) {
      cache_.Iniciar(c.vendor + "|" + c.renderer + "|" + c.version_texto);
    }
    if (!flujo_.Iniciar(c.buffer_storage)) {
      REXLOG_ERROR("[nativo] C6 GLES: no se pudo crear el bufer de subida");
      return false;
    }
    alineacion_ubo_ = std::max<uint32_t>(16, uint32_t(c.uniform_buffer_offset_alignment));
    glGenVertexArrays(1, &vao_);
    unidad_trabajo_ = uint32_t(std::max(c.max_combined_texture_units, 49) - 1);
    // One empty texture per dimension, for the registers without a texture (a texture unit with nothing bound
    // samples (0, 0, 0, 1) in GL too, but an incomplete or mismatched one is undefined).
    const GLenum objetivos[3] = {GL_TEXTURE_2D, GL_TEXTURE_3D, GL_TEXTURE_CUBE_MAP};
    const uint8_t negro[4 * 6] = {};
    glGenTextures(3, vacias_.data());
    for (uint32_t d = 0; d < 3; ++d) {
      glBindTexture(objetivos[d], vacias_[d]);
      if (d == 1) {
        glTexStorage3D(GL_TEXTURE_3D, 1, GL_RGBA8, 1, 1, 1);
        glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, negro);
      } else {
        glTexStorage2D(objetivos[d], 1, GL_RGBA8, 1, 1);
        if (d == 0) {
          glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, negro);
        } else {
          for (uint32_t cara = 0; cara < 6; ++cara) {
            glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + cara, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, negro);
          }
        }
      }
      glBindTexture(objetivos[d], 0);
    }
    LeerAjustes();
    if (const GLenum error = glGetError(); error != GL_NO_ERROR) {
      REXLOG_ERROR("[nativo] C6 GLES: error de GL al inicializar los dibujos ({:#x})", error);
      return false;
    }
    // KHR_debug (core in GLES 3.2): what the driver reports beyond glGetError, on this same thread.
    if (REXCVAR_GET(nfsmw_nativo_gles_depurar) && c.version >= 32) {
      glEnable(GL_DEBUG_OUTPUT);
      glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
      glDebugMessageCallback(MensajeDepuracion, nullptr);
      REXLOG_INFO("[nativo] C6 GLES: salida de depuracion del driver encendida (nfsmw_nativo_gles_depurar)");
    }
    REXLOG_INFO("[nativo] C6 GLES: dibujos sobre GLSL ES {}; bufer de subida {} MB {}; UBO alineados a {}; "
                "mips {}, PCF barato {}, anisotropico {}; vertice base {}, sin duplicar vertices {}, sombras sin "
                "vegetacion {}, saltar invisibles {}, Z temprana {}",
                opciones_glsl_.version, kTamanoFlujo >> 20,
                flujo_.persistente() ? "persistente (GL_EXT_buffer_storage)" : "con glBufferSubData",
                alineacion_ubo_, mipmaps_ ? "SI" : "no", pcf_barato_ ? "SI" : "no", anisotropico_,
                base_vertice_ ? "SI" : "no", dedupe_ ? "SI" : "no", sin_vegetacion_ ? "SI" : "no",
                saltar_invisibles_ ? "SI" : "no", z_temprana_ ? "SI" : "no");
    return true;
  }

  void EstadoPerdido() override {
    estado_.valido = false;
    ++memo_generacion_;  // C2 may resolve to a texture a register already noted
  }

  void FinDeFotograma() override {
    ++fotograma_;
    ++memo_generacion_;  // the per-frame texture checks run again
    copias_.clear();
    LeerAjustes();
    EmitirCielo(kCieloPorFinDePase);  // the Swap already emitted it; just in case
    CerrarFotogramaDelCielo();
    InformeEtapas();
    // nfsmw_reflejo_visibilidad, once per frame (its guard may switch it off mid-session).
    RecogerVisibilidad(false);
    medir_visibilidad_ = nfsmw::reflejo_demanda::MedirVisibilidad();
    if (medir_visibilidad_ && fotograma_ >= testigo_siguiente_) {
      testigo_pendiente_ = true;
    }
    ExpulsarTexturasSiHaceFalta();
    ComprobarGl("durante el fotograma");
    if (fotograma_ % 1800 == 0) {
      REXLOG_INFO("[nativo] C6 GLES ahorros: {} dibujos; saltados {} de vegetacion en sombras y {} invisibles; {} con Z "
                  "temprana; vertices reutilizados {} veces ({} MB); texturas de registro reutilizadas {}; programas "
                  "{} ({} de disco, {} compilados en {:.0f} ms, {} en la cache); oclusiones contadas {}; reflejo: {} "
                  "consultas de visibilidad, {} sin muestras",
                  dibujados_, saltados_vegetacion_, saltados_invisibles_, dibujos_z_temprana_, aciertos_dedupe_,
                  bytes_dedupe_ >> 20, aciertos_memo_, programas_.size(), programas_disco_, programas_compilados_,
                  ms_programas_, cache_.registros(), dibujos_contados_, consultas_visibilidad_, reflejos_ocultos_);
    }
  }

  EstadisticasDibujos Estadisticas() const override {
    EstadisticasDibujos e;
    e.dibujados = dibujados_;
    e.rechazados = rechazados_;
    e.pipelines = programas_.size();
    e.texturas = texturas_.size();
    e.megas_texturas = bytes_texturas_ >> 20;
    e.subidas_textura = subidas_textura_;
    e.megas_subidos = bytes_subidos_ >> 20;
    e.bytes_vertices = bytes_vertices_;
    e.bytes_indices = bytes_indices_;
    for (const auto& [causa, n] : causas_) {
      e.causas.emplace_back(causa, n);
    }
    std::sort(e.causas.begin(), e.causas.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    return e;
  }

  bool Dibujar(const PeticionDibujo& p) override {
    const uint32_t* r = p.registros;
    if (!r || !p.vs || !p.vs->shader) {
      return false;
    }
    marca_ = cronometrar_ ? RelojNs() : 0;
    // The deferred sky's data in the stream buffer must still be there when it is emitted: before this draw can
    // move the write head into the segment that holds the oldest of them, it goes out.
    if (cielo_pendiente_ && flujo_.SegmentoAbsoluto() >= cielo_segmento_ + 1) {
      EmitirCielo(kCieloPorBufer);
    }
    const uint64_t segmento_inicio = flujo_.SegmentoAbsoluto();
    // --- Primitive type ------------------------------------------------------------------------------------
    const uint32_t modo_edram = r[gr::XE_GPU_REG_RB_MODECONTROL] & 0x7;
    if (modo_edram != uint32_t(xenos::EdramMode::kColorDepth) && modo_edram != 5) {
      return Rechazar(1, "modo EDRAM sin color ni profundidad");
    }
    // Mode 5 (depth only): no pixel shader (IsPixelShaderNeededWithRasterization).
    const EntradaShader* ps = modo_edram == uint32_t(xenos::EdramMode::kColorDepth) ? p.ps : nullptr;
    if (!ps && modo_edram == uint32_t(xenos::EdramMode::kColorDepth)) {
      return false;
    }
    if (ps && !ps->shader) {
      return false;
    }
    const uint32_t iniciador = r[gr::XE_GPU_REG_VGT_DRAW_INITIATOR];
    const uint32_t tipo = iniciador & 0x3F;
    const uint32_t fuente = (iniciador >> 6) & 0x3;
    const bool indices32 = (iniciador >> 11) & 0x1;
    const uint32_t cuenta = iniciador >> 16;
    if (!cuenta) {
      return true;
    }
    GLenum modo;
    bool cuadrilateros = false;
    bool admite_reinicio = false;
    switch (tipo) {
      case 2: modo = GL_LINES; break;
      case 3: modo = GL_LINE_STRIP; admite_reinicio = true; break;
      case 4: modo = GL_TRIANGLES; break;
      case 5: modo = GL_TRIANGLE_FAN; admite_reinicio = true; break;
      case 6: modo = GL_TRIANGLE_STRIP; admite_reinicio = true; break;
      case 13: modo = GL_TRIANGLES; cuadrilateros = true; break;
      default: return Rechazar(100 + tipo, "tipo de primitiva todavia no soportado");
    }
    if (fuente == uint32_t(xenos::SourceSelect::kImmediate)) {
      return Rechazar(2, "indices inmediatos");
    }
    const bool sin_recorte = (r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL] >> 16) & 0x1;
    const uint32_t modo_sc = r[gr::XE_GPU_REG_PA_SU_SC_MODE_CNTL];
    if (((modo_sc >> 3) & 0x3) == 2 && ((modo_sc >> 5) & 0x7) != 2) {
      return Rechazar(4, "poligonos dibujados como puntos o lineas");
    }
    const EntradaGles* entrada = EntradaDe(p);
    if (!entrada) {
      return false;
    }
    Etapa(0);

    // --- Render targets ------------------------------------------------------------------------------------
    const uint32_t pitch = r[gr::XE_GPU_REG_RB_SURFACE_INFO] & 0x3FFF;
    const uint32_t mascara_registro =
        modo_edram == uint32_t(xenos::EdramMode::kColorDepth) ? r[gr::XE_GPU_REG_RB_COLOR_MASK] : 0;
    static constexpr uint32_t kInfoColor[4] = {gr::XE_GPU_REG_RB_COLOR_INFO, gr::XE_GPU_REG_RB_COLOR1_INFO,
                                               gr::XE_GPU_REG_RB_COLOR2_INFO, gr::XE_GPU_REG_RB_COLOR3_INFO};
    uint64_t claves[5] = {};
    uint32_t mascaras = 0;
    bool hay_destino = false;
    for (uint32_t i = 0; i < 4; ++i) {
      const uint32_t mascara = (mascara_registro >> (i * 4)) & 0xF;
      if (!mascara || !ps || !((ps->salidas >> i) & 0x1)) {
        continue;
      }
      const uint32_t info = r[kInfoColor[i]];
      const uint32_t formato = (info >> 16) & 0xF;
      if (formato != uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8) &&
          formato != uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)) {
        return Rechazar(200 + formato, "formato de destino de color todavia no soportado");
      }
      claves[i] = (uint64_t(1) << 63) | (uint64_t(info & 0xFFF) << 24) | (uint64_t(formato) << 16) | pitch;
      mascaras |= mascara << (i * 4);
      hay_destino = true;
    }
    const uint32_t control_profundidad = r[gr::XE_GPU_REG_RB_DEPTHCONTROL];
    if (control_profundidad & 0x3) {  // stencil o z
      const uint32_t info = r[gr::XE_GPU_REG_RB_DEPTH_INFO];
      claves[4] = (uint64_t(1) << 62) | (uint64_t(info & 0xFFF) << 24) | (uint64_t((info >> 16) & 0x1) << 16) | pitch;
      hay_destino = true;
    }
    if (!hay_destino || !pitch) {
      return true;  // writes nothing visible
    }
    const bool oclusion = contexto_->OclusionAbierta();
    /*
     * Draws that cannot change a single pixel (nfsmw_nativo_saltar_invisibles, as in Vulkan): blending that leaves
     * the destination as it is on every written channel, or the alpha test with the NEVER function. If they write
     * neither depth nor stencil either, they leave no trace. Not while an occlusion query is open: the game reads
     * those samples.
     */
    if (saltar_invisibles_ && mascaras && !oclusion) {
      static constexpr uint32_t kMezclaDe[4] = {gr::XE_GPU_REG_RB_BLENDCONTROL0, gr::XE_GPU_REG_RB_BLENDCONTROL1,
                                                gr::XE_GPU_REG_RB_BLENDCONTROL2, gr::XE_GPU_REG_RB_BLENDCONTROL3};
      const uint32_t cc = r[gr::XE_GPU_REG_RB_COLORCONTROL];
      const bool alfa_nunca = ((cc >> 3) & 0x1) && (cc & 0x7) == 0;
      uint32_t utiles = 0;
      for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t mascara = (mascaras >> (i * 4)) & 0xF;
        if (!mascara) {
          continue;
        }
        const uint32_t m = r[kMezclaDe[i]] & 0x1FFF1FFF;
        const bool copia_color = (m & 0x1F) == 0 && ((m >> 8) & 0x1F) == 1 && ((m >> 5) & 0x7) == 0;
        const bool copia_alfa = ((m >> 16) & 0x1F) == 0 && ((m >> 24) & 0x1F) == 1 && ((m >> 21) & 0x7) == 0;
        if (((mascara & 0x7) && !copia_color) || ((mascara & 0x8) && !copia_alfa)) {
          utiles |= mascara << (i * 4);
        }
      }
      if (!utiles || alfa_nunca) {
        const uint32_t dc = r[gr::XE_GPU_REG_RB_DEPTHCONTROL];
        const bool deja_rastro = !alfa_nunca && ((((dc >> 1) & 0x1) && ((dc >> 2) & 0x1)) || (dc & 0x1));
        if (!deja_rastro) {
          ++saltados_invisibles_;
          return true;
        }
      }
    }
    // A pixel shader with no color to write that neither discards nor writes depth does nothing: without it
    // the program has an empty fragment stage (the image is the same).
    if (!mascaras && ps) {
      const uint32_t cc = r[gr::XE_GPU_REG_RB_COLORCONTROL];
      const bool prueba_alfa = ((cc >> 3) & 0x1) && (cc & 0x7) != 7;
      if (!prueba_alfa && !ps->descarta && !(ps->salidas & 0x10)) {
        ps = nullptr;
      } else if (sin_vegetacion_ && !oclusion) {
        // nfsmw_sombras_sin_vegetacion (the "Xbox 360" shadow quality turns it off), as in Vulkan: with no color,
        // the pixel shader is only there for its alpha test or kill. In the shadow map those are the trees, bushes
        // and wire fences: half of the pass's draws for a few percent of its triangles. They stop casting shadow.
        ++saltados_vegetacion_;
        return true;
      }
    }

    Etapa(1);
    // --- Depth bias (GetPreferredFacePolygonOffset) ------------------------------------------------------------
    float escala_sesgo = 0.0f, desplazamiento_sesgo = 0.0f;
    if (tipo >= 4) {
      if (((modo_sc >> 11) & 0x1) && !(modo_sc & 0x1)) {
        escala_sesgo = Flotante(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE]);
        desplazamiento_sesgo = Flotante(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET]);
      }
      if (((modo_sc >> 12) & 0x1) && !((modo_sc >> 1) & 0x1) && escala_sesgo == 0.0f && desplazamiento_sesgo == 0.0f) {
        escala_sesgo = Flotante(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE]);
        desplazamiento_sesgo = Flotante(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET]);
      }
    } else if ((modo_sc >> 13) & 0x1) {
      escala_sesgo = Flotante(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE]);
      desplazamiento_sesgo = Flotante(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET]);
    }
    float sesgo[2] = {
        desplazamiento_sesgo *
            (((r[gr::XE_GPU_REG_RB_DEPTH_INFO] >> 16) & 0x1) ? float(uint32_t(1) << 24) : float((uint32_t(1) << 24) - 1)),
        escala_sesgo * (1.0f / 16.0f)};
    // nfsmw_nativo_sombras_sesgo_*, as in Vulkan: only in the shadow map (a depth-only target of 1600 or more) and
    // only if the game does not set its own. Without it the surfaces shadow themselves (acne): a grid of dots that
    // trembles as the car moves.
    const bool destino_sombras = pitch >= 1600 && claves[4] && !claves[0] && !claves[1] && !claves[2] && !claves[3];
    if (destino_sombras && tipo >= 4 && sesgo[0] == 0.0f && sesgo[1] == 0.0f &&
        (sombras_sesgo_constante_ != 0 || sombras_sesgo_pendiente_ != 0)) {
      sesgo[0] = float(sombras_sesgo_constante_) * 1000.0f;
      sesgo[1] = float(sombras_sesgo_pendiente_) * 0.1f;
    }
    const bool con_sesgo = sesgo[0] != 0.0f || sesgo[1] != 0.0f;

    // --- Indices ---------------------------------------------------------------------------------------------
    // With the base vertex (GLES 3.2, nfsmw_nativo_gles_base_vertice) the usual case, 16 bits without restart,
    // goes from the guest straight into the stream buffer, unrebased (NEON, as nfsmw_nativo_indices_neon in Vulkan):
    // the draw's base vertex takes the minimum off. The rest is rebased to the first vertex on the CPU.
    const uint32_t desplazamiento = r[gr::XE_GPU_REG_VGT_INDX_OFFSET] & 0xFFFFFF;
    const bool reinicio = admite_reinicio && ((modo_sc >> 21) & 0x1);
    uint32_t vmin = UINT32_MAX, vmax = 0;
    bool con_indices = false;
    bool indices_directos = false;
    GLintptr offset_indices = 0;
    GLenum tipo_indices = GL_UNSIGNED_SHORT;
    GLsizei cuenta_indices = 0;
    int32_t ajuste_base = 0;  // added to the base vertex: minus the minimum of unrebased indices
    indices_.clear();
    if (fuente == uint32_t(xenos::SourceSelect::kDMA)) {
      const uint32_t tamano = r[gr::XE_GPU_REG_VGT_DMA_SIZE];
      const auto orden = static_cast<xenos::Endian>(tamano >> 30);
      const uint32_t bytes = indices32 ? 4 : 2;
      const uint32_t base = r[gr::XE_GPU_REG_VGT_DMA_BASE] & ~(bytes - 1);
      if (cuenta > (tamano & 0xFFFFFF) || uint64_t(base & 0x1FFFFFFF) + uint64_t(cuenta) * bytes > kMemoriaFisica) {
        return Rechazar(7, "indices fuera de su bufer");
      }
      const uint8_t* datos = memoria_->TranslatePhysical(base & 0x1FFFFFFF);
      const uint32_t indice_reinicio = r[gr::XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX] & 0xFFFFFF;
      if (base_vertice_ && !indices32 && !reinicio && !cuadrilateros && desplazamiento <= 0xFFFFFF - 0xFFFF &&
          (orden == xenos::Endian::k8in16 || orden == xenos::Endian::kNone)) {
        uint8_t* destino = flujo_.Reservar(size_t(cuenta) * 2, 4, offset_indices);
        if (!destino) {
          return Rechazar(14, "indices mayores que un segmento del bufer de subida");
        }
        indices16_.resize(cuenta);
        uint32_t minimo = 0, maximo = 0;
        IndicesDe16(datos, cuenta, indices16_.data(), orden == xenos::Endian::k8in16, minimo, maximo);
        std::memcpy(destino, indices16_.data(), size_t(cuenta) * 2);
        bytes_indices_ += size_t(cuenta) * 2;
        vmin = minimo + desplazamiento;
        vmax = maximo + desplazamiento;
        ajuste_base = -int32_t(minimo);
        cuenta_indices = GLsizei(cuenta);
        indices_directos = true;
        con_indices = true;
      }
      if (!indices_directos) {
        indices_.resize(cuenta);
        for (uint32_t i = 0; i < cuenta; ++i) {
          uint32_t v;
          if (indices32) {
            uint32_t crudo;
            std::memcpy(&crudo, datos + size_t(i) * 4, 4);
            v = xenos::GpuSwap(crudo, orden) & 0xFFFFFF;
          } else {
            uint16_t crudo;
            std::memcpy(&crudo, datos + size_t(i) * 2, 2);
            v = xenos::GpuSwap(crudo, orden);
          }
          if (reinicio && v == indice_reinicio) {
            indices_[i] = UINT32_MAX;
            continue;
          }
          v = (v + desplazamiento) & 0xFFFFFF;
          vmin = std::min(vmin, v);
          vmax = std::max(vmax, v);
          indices_[i] = v;
        }
        if (vmin > vmax) {
          return true;  // resets only
        }
        con_indices = true;
      }
    } else {
      vmin = desplazamiento;
      vmax = desplazamiento + cuenta - 1;
      if (cuadrilateros) {
        indices_.resize(cuenta);
        for (uint32_t i = 0; i < cuenta; ++i) {
          indices_[i] = desplazamiento + i;
        }
        con_indices = true;
      }
    }
    if (vmax - vmin >= kMaxVerticesPorDibujo) {
      return Rechazar(8, "rango de vertices demasiado grande");
    }
    if (cuadrilateros) {
      const size_t n = indices_.size() / 4;
      convertidos_.resize(n * 6);
      for (size_t q = 0; q < n; ++q) {
        const uint32_t* i = &indices_[q * 4];
        uint32_t* o = &convertidos_[q * 6];
        o[0] = i[0]; o[1] = i[1]; o[2] = i[2];
        o[3] = i[0]; o[4] = i[2]; o[5] = i[3];
      }
      indices_.swap(convertidos_);
    }
    const uint32_t vertices = vmax - vmin + 1;
    Etapa(2);

    // --- Vertex sources ----------------------------------------------------------------------------------------
    struct Origen {
      const uint8_t* datos;
      xenos::Endian orden;
      uint32_t bytes;
      uint32_t fisica;  // guest physical address of the first byte (vertex deduplication)
    };
    std::array<Origen, 16> origenes{};
    if (entrada->enlaces.size() > origenes.size()) {
      return Rechazar(9, "demasiados streams de vertices");
    }
    for (size_t b = 0; b < entrada->enlaces.size(); ++b) {
      const EnlaceGles& enlace = entrada->enlaces[b];
      const uint32_t d0 = r[kRegFetch + enlace.ranura * 2];
      const uint32_t d1 = r[kRegFetch + enlace.ranura * 2 + 1];
      if ((d0 & 0x3) != uint32_t(xenos::FetchConstantType::kVertex)) {
        return Rechazar(10, "fetch constant de vertices invalida");
      }
      const uint64_t direccion = uint64_t(d0 & 0x1FFFFFFC);
      const uint64_t disponibles = uint64_t((d1 >> 2) & 0xFFFFFF) * 4;
      const uint64_t inicio = uint64_t(vmin) * enlace.zancada;
      uint64_t necesarios = uint64_t(vertices) * enlace.zancada;
      if (inicio + necesarios > disponibles) {
        necesarios = disponibles > inicio ? (disponibles - inicio) / enlace.zancada * enlace.zancada : 0;
      }
      if (!necesarios || direccion + inicio + necesarios > kMemoriaFisica) {
        return Rechazar(12, "vertices fuera de la memoria");
      }
      origenes[b] = {memoria_->TranslatePhysical(uint32_t(direccion + inicio)), static_cast<xenos::Endian>(d1 & 0x3),
                     uint32_t(necesarios), uint32_t(direccion + inicio)};
    }

    Etapa(3);
    // --- Textures, samplers and shared constants -------------------------------------------------------------
    uint32_t compartidas[kPalabrasCompartidas] = {};
    std::array<SwizzleDeRegistro, 16> swizzles{};
    std::array<TexturaDeRegistro, 16> por_registro{};
    bool lee_reflejo = false;  // nfsmw_reflejo_visibilidad
    if (ps) {
      for (const SamplerShader& sampler : ps->samplers) {
        if (sampler.registro >= 16) {
          continue;
        }
        const uint32_t* fetch = r + kRegFetch + uint32_t(sampler.registro) * 6;
        // nfsmw_reflejo_visibilidad: this draw samples the road reflection (from the fetch constant, on every
        // draw: the per-register cache below would only see the first one).
        if (medir_visibilidad_ && (fetch[0] & 0x3) == uint32_t(xenos::FetchConstantType::kTexture) &&
            ((fetch[1] & 0xFFFFF000u) & 0x1FFFFFFFu) == nfsmw::reflejo_demanda::kDireccion) {
          lee_reflejo = true;
        }
        TexturaDeRegistro& t = por_registro[sampler.registro];
        MemoTextura& m = memo_texturas_[sampler.registro];
        if (memo_activo_ && m.generacion == memo_generacion_ && std::memcmp(m.fetch, fetch, sizeof(m.fetch)) == 0) {
          // Same fetch constant as this register's last draw in this frame, and no C2 copy since: the same texture.
          t = m.salida;
          if (m.uso) {
            *m.uso = fotograma_;
          }
          if (m.swizzle && *m.swizzle != m.swizzle_esperado) {
            // Another register gave the same texture another swizzle in between.
            EnlazarParaTrabajar(m.objetivo, t.textura);
            PonerSwizzle(m.objetivo, m.swizzle_esperado);
            *m.swizzle = m.swizzle_esperado;
          }
          ++aciertos_memo_;
        } else {
          m = MemoTextura{};
          PrepararTextura(fetch, t, m);
          std::memcpy(m.fetch, fetch, sizeof(m.fetch));
          m.generacion = memo_generacion_;
          m.salida = t;
        }
        swizzles[sampler.registro] = {m.swizzle, m.swizzle_esperado, m.objetivo};
        const float inv[2] = {t.ancho ? 1.0f / float(t.ancho) : 0.0f, t.alto ? 1.0f / float(t.alto) : 0.0f};
        std::memcpy(compartidas + kPalabraInvTamano + sampler.registro * 2, inv, sizeof(inv));
      }
    }

    Etapa(4);
    // --- Framebuffer -------------------------------------------------------------------------------------------
    const FramebufferGles* fb = FramebufferDe(claves, pitch);
    if (!fb) {
      return false;
    }
    // A new pass (GL has none: another framebuffer) closes the previous one, deferred sky included.
    if (fb->fbo != fbo_pase_) {
      EmitirCielo(kCieloPorFinDePase);
      fbo_pase_ = fb->fbo;
      dibujos_en_pase_ = 0;
    }
    // --- Framing (CalcularEncuadre) -----------------------------------------------------------------------------
    const uint32_t vte = r[gr::XE_GPU_REG_PA_CL_VTE_CNTL];
    float viewport[6];  // x, y, ancho, alto, z minima, z maxima
    float ndc[4];
    int32_t tijera[4];
    if (!CalcularEncuadre(r, vte, sin_recorte, modo_sc, fb->ancho, fb->alto, viewport, ndc, tijera)) {
      return true;  // empty viewport or scissor
    }
    // GL has no negative viewport height: the flip goes into the NDC (the VS does oPos.xy * g_NdcScale +
    // g_NdcOffset * w before -fvk-invert-y), and the half pixel with it.
    bool volteado = false;
    if (viewport[3] < 0.0f) {
      viewport[1] += viewport[3];
      viewport[3] = -viewport[3];
      ndc[1] = -ndc[1];
      ndc[3] = -ndc[3];
      volteado = true;
    }
    uint32_t especializacion = entrada->especializacion;
    if (ps && ps->shader->huella == kHuellaComposicion && sin_desenfoque_) {
      especializacion |= kSpecSinDesenfoque;
    }
    const uint32_t control_color = r[gr::XE_GPU_REG_RB_COLORCONTROL];
    float umbral_alfa = 0.0f;
    uint32_t funcion_alfa = 7;  // always
    if (ps && ((control_color >> 3) & 0x1) && (control_color & 0x7) != 7) {
      umbral_alfa = Flotante(r[gr::XE_GPU_REG_RB_ALPHA_REF]);
      funcion_alfa = control_color & 0x7;
      especializacion |= 0x2;
      especializacion |= (funcion_alfa & 0x7u) << kSpecFuncionAlfaDesplazamiento;
    }
    especializacion |= kSpecInvTamanoTex;  // 1/size always goes in the shared constants
    if (pcf_barato_) {
      especializacion |= kSpecPcfBarato;
    }
    compartidas[64] = (r[kRegBooleanos] & 0xFFFF) | ((r[kRegBooleanos + 4] & 0xFFFF) << 16);
    compartidas[65] = 0;  // g_SwappedTexcoords
    if (!(r[gr::XE_GPU_REG_PA_SU_VTX_CNTL] & 0x1)) {  // PixelCenter::kD3DZero
      const float medio[2] = {1.0f / viewport[2], (volteado ? 1.0f : -1.0f) / viewport[3]};
      std::memcpy(&compartidas[66], medio, sizeof(medio));
    }
    std::memcpy(&compartidas[68], &umbral_alfa, sizeof(umbral_alfa));
    compartidas[69] = funcion_alfa;
    std::memcpy(&compartidas[70], ndc, sizeof(ndc));
    std::copy(entrada->remapeos.begin(), entrada->remapeos.end(), compartidas + 74);

    Etapa(5);
    // --- Program variants ------------------------------------------------------------------------------------
    uint32_t variantes = 0;
    /*
     * nfsmw_nativo_z_temprana, with Vulkan's conditions: a shader that can discard (alpha test, kill) makes the GPU
     * shade before testing depth. If the draw writes neither depth nor stencil, testing first cannot change anything
     * and the GPU stops shading what is hidden (smoke, particles, glass). Not inside an occlusion query, nor for the
     * bright pass and the composite.
     */
    if (z_temprana_ && ps && claves[4] && mascaras && !oclusion) {
      const bool prueba_z = (control_profundidad >> 1) & 0x1;
      const bool escribe_z = prueba_z && ((control_profundidad >> 2) & 0x1);
      const bool estencil = control_profundidad & 0x1;
      const bool ps_escribe_z = ps->salidas & 0x10;
      const bool tardio = (especializacion & 0x2) || ps->descarta || ps_escribe_z;
      if (prueba_z && tardio && !escribe_z && !ps_escribe_z && !estencil && ps->shader->huella != kHuellaBrightPass &&
          ps->shader->huella != kHuellaComposicion) {
        variantes |= kVarianteZTemprana;
        ++dibujos_z_temprana_;
      }
    }
    // Inside an occlusion query with a sample counter (DestinosGles), the draw counts its samples if the early test
    // gives the same result as the late one: no discard where the draw writes depth or stencil, and no shader depth.
    GLuint contador = 0;
    if (oclusion) {
      contador = contexto_->ContadorOclusion();
      if (contador) {
        const bool escribe = claves[4] && ((((control_profundidad >> 1) & 0x1) && ((control_profundidad >> 2) & 0x1)) ||
                                           (control_profundidad & 0x1));
        const bool descarta = ps && ((especializacion & 0x2) || ps->descarta);
        if ((ps && (ps->salidas & 0x10)) || (escribe && descarta)) {
          contexto_->OclusionSinContar();
          contador = 0;
        } else {
          variantes = kVarianteContar;
        }
      }
    }

    // --- Program -----------------------------------------------------------------------------------------------
    const ProgramaGles* programa = ProgramaDe(*p.vs, ps, especializacion, variantes);
    if (!programa && (variantes & kVarianteContar)) {
      // The counting variant did not compile: the query gives yes or no.
      contexto_->OclusionSinContar();
      contador = 0;
      programa = ProgramaDe(*p.vs, ps, especializacion, 0);
    }
    if (!programa) {
      return false;
    }
    Etapa(6);

    // --- Uploads: vertices, indices and constants ----------------------------------------------------------------
    uint64_t segmento_minimo = segmento_inicio;  // the oldest stream buffer segment this draw reads
    /*
     * Vertices. nfsmw_nativo_dedupe_vertices, as in Vulkan: the same guest range twice in a frame is copied once (the
     * game cannot rewrite vertices a draw already references without waiting for the GPU, and at that wait,
     * WAIT_REG_MEM, the list is forgotten). With a single stream and the base vertex, its copy goes at a multiple of
     * its stride: the attributes keep pointing at 0 and the draw is shifted by the copy's slot.
     */
    std::array<GLintptr, 16> offsets_vertices{};
    const bool un_enlace = base_vertice_ && entrada->enlaces.size() == 1;
    if (const uint32_t sincronizacion = g_sincronizaciones_anillo.load(std::memory_order_relaxed);
        sincronizacion != sincronizacion_copias_) {
      copias_.clear();
      sincronizacion_copias_ = sincronizacion;
    }
    for (size_t b = 0; b < entrada->enlaces.size(); ++b) {
      const Origen& origen = origenes[b];
      const size_t alineacion = un_enlace ? entrada->enlaces[b].zancada : 4;
      const uint64_t clave_copia =
          uint64_t(origen.fisica) | (uint64_t(origen.bytes) << 32) | (uint64_t(origen.orden) << 62);
      GLintptr offset = -1;
      if (dedupe_) {
        if (const auto it = copias_.find(clave_copia);
            it != copias_.end() && flujo_.SigueVivo(it->second.segmento) && it->second.offset % GLintptr(alineacion) == 0) {
          offset = it->second.offset;
          segmento_minimo = std::min(segmento_minimo, it->second.segmento);
          ++aciertos_dedupe_;
          bytes_dedupe_ += origen.bytes;
        }
      }
      if (offset < 0) {
        uint8_t* destino = flujo_.Reservar(origen.bytes, alineacion, offset);
        if (!destino) {
          return Rechazar(13, "vertices mayores que un segmento del bufer de subida");
        }
        CopiarPalabras(origen.datos, destino, origen.bytes / 4, origen.orden);
        bytes_vertices_ += origen.bytes;
        if (dedupe_) {
          copias_[clave_copia] = {offset, flujo_.SegmentoAbsoluto()};
        }
      }
      offsets_vertices[b] = offset;
    }
    GLint base_copia = 0;  // the single stream's slot: the attributes point at 0
    std::array<GLintptr, 16> bases{};
    if (un_enlace) {
      base_copia = GLint(offsets_vertices[0] / GLintptr(entrada->enlaces[0].zancada));
    } else {
      bases = offsets_vertices;
    }
    if (con_indices && !indices_directos) {
      // Rebased to vmin (the copy of the vertices starts there); the reset index stays as it is.
      const bool de_16 = vmax - vmin < 0xFFFF && !(reinicio && indices32);
      const size_t n = indices_.size();
      tipo_indices = de_16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
      uint8_t* destino = flujo_.Reservar(n * (de_16 ? 2 : 4), 4, offset_indices);
      if (!destino) {
        return Rechazar(14, "indices mayores que un segmento del bufer de subida");
      }
      if (de_16) {
        uint16_t* salida = reinterpret_cast<uint16_t*>(destino);
        for (size_t i = 0; i < n; ++i) {
          salida[i] = indices_[i] == UINT32_MAX ? uint16_t(0xFFFF) : uint16_t(indices_[i] - vmin);
        }
      } else {
        uint32_t* salida = reinterpret_cast<uint32_t*>(destino);
        for (size_t i = 0; i < n; ++i) {
          salida[i] = indices_[i] == UINT32_MAX ? UINT32_MAX : indices_[i] - vmin;
        }
      }
      bytes_indices_ += n * (de_16 ? 2 : 4);
      cuenta_indices = GLsizei(n);
    }
    // The float constants: the whole block (GL may read the full declared range), reused while their generation
    // and their place in the stream buffer last.
    if (constantes_vs_generacion_ != p.generacion_constantes_vs || !flujo_.SigueVivo(constantes_vs_segmento_)) {
      uint8_t* destino = flujo_.Reservar(kUboBytesVs, alineacion_ubo_, constantes_vs_offset_);
      if (!destino) return false;
      std::memcpy(destino, r + kRegConstantesVs, kUboBytesVs);
      constantes_vs_generacion_ = p.generacion_constantes_vs;
      constantes_vs_segmento_ = flujo_.SegmentoAbsoluto();
    }
    if (ps && (constantes_ps_generacion_ != p.generacion_constantes_ps || !flujo_.SigueVivo(constantes_ps_segmento_))) {
      uint8_t* destino = flujo_.Reservar(kUboBytesPs, alineacion_ubo_, constantes_ps_offset_);
      if (!destino) return false;
      std::memcpy(destino, r + kRegConstantesPs, kUboBytesPs);
      constantes_ps_generacion_ = p.generacion_constantes_ps;
      constantes_ps_segmento_ = flujo_.SegmentoAbsoluto();
    }
    if (!compartidas_validas_ || !flujo_.SigueVivo(compartidas_segmento_) ||
        std::memcmp(compartidas, compartidas_previas_, sizeof(compartidas)) != 0) {
      uint8_t* destino = flujo_.Reservar(kUboBytesCompartidas, alineacion_ubo_, compartidas_offset_);
      if (!destino) return false;
      std::memset(destino, 0, kUboBytesCompartidas);
      std::memcpy(destino, compartidas, sizeof(compartidas));
      std::memcpy(compartidas_previas_, compartidas, sizeof(compartidas));
      compartidas_segmento_ = flujo_.SegmentoAbsoluto();
      compartidas_validas_ = true;
    }
    flujo_.Confirmar();
    segmento_minimo = std::min({segmento_minimo, constantes_vs_segmento_, compartidas_segmento_});
    if (ps) {
      segmento_minimo = std::min(segmento_minimo, constantes_ps_segmento_);
    }
    Etapa(7);

    // --- The draw, ready to emit ------------------------------------------------------------------------------
    static constexpr uint32_t kMezcla[4] = {gr::XE_GPU_REG_RB_BLENDCONTROL0, gr::XE_GPU_REG_RB_BLENDCONTROL1,
                                            gr::XE_GPU_REG_RB_BLENDCONTROL2, gr::XE_GPU_REG_RB_BLENDCONTROL3};
    DibujoGles d;
    d.programa = programa;
    d.fb = fb;
    for (uint32_t i = 0; i < 4; ++i) {
      if (claves[i]) {
        d.mezcla[i] = r[kMezcla[i]] & 0x1FFF1FFF;
      }
    }
    d.mascaras = mascaras;
    d.mezcla_constante[0] = Flotante(r[gr::XE_GPU_REG_RB_BLEND_RED]);
    d.mezcla_constante[1] = Flotante(r[gr::XE_GPU_REG_RB_BLEND_GREEN]);
    d.mezcla_constante[2] = Flotante(r[gr::XE_GPU_REG_RB_BLEND_BLUE]);
    d.mezcla_constante[3] = Flotante(r[gr::XE_GPU_REG_RB_BLEND_ALPHA]);
    d.con_profundidad = claves[4] != 0;
    d.profundidad = claves[4] ? control_profundidad : 0;
    d.estencil[0] = r[gr::XE_GPU_REG_RB_STENCILREFMASK];
    d.estencil[1] = ((control_profundidad >> 7) & 0x1) ? r[gr::XE_GPU_REG_RB_STENCILREFMASK_BF]
                                                        : r[gr::XE_GPU_REG_RB_STENCILREFMASK];
    d.rasterizado = (modo_sc & 0x7) | (con_sesgo ? 0x10 : 0);
    d.sesgo[0] = sesgo[0];
    d.sesgo[1] = sesgo[1];
    d.reinicio = reinicio;
    for (uint32_t i = 0; i < 4; ++i) {
      d.vp[i] = GLint(std::lround(viewport[i]));
      d.tijera[i] = tijera[i];
    }
    d.rango[0] = viewport[4];
    d.rango[1] = viewport[5];
    d.por_registro = por_registro;
    d.swizzles = swizzles;
    d.ubos = {constantes_vs_offset_, ps ? constantes_ps_offset_ : GLintptr(-1), compartidas_offset_};
    d.contador = contador;
    d.entrada = entrada;
    d.bases = bases;
    d.modo = modo;
    d.con_indices = con_indices;
    d.cuenta_indices = cuenta_indices;
    d.tipo_indices = tipo_indices;
    d.offset_indices = offset_indices;
    d.base = con_indices ? base_copia + ajuste_base : base_copia;
    d.cuenta = GLsizei(cuenta);

    /*
     * The sky, deferred until after the opaque draws of its pass (nfsmw_nativo_cielo_aplazado), with the
     * criterion and the self-checking guard of Vulkan (see nfsmw_nativo_dibujos.cpp): the sky's pixel shader by
     * fingerprint, the dome's 480 indices, the first draw of the scene pass, opaque, testing depth without
     * writing it, no stencil and no game occlusion query open. It shades 90 % of the screen on an empty Z-buffer
     * and the world then paints over it: deferred, the depth test discards it where the world already is. Only
     * after 90 race frames with exactly one dome per frame, never two.
     */
    bool opaco_en_todos = true;
    for (uint32_t i = 0; i < 4 && opaco_en_todos; ++i) {
      const uint32_t mascara = (mascaras >> (i * 4)) & 0xF;
      if (!mascara) {
        continue;
      }
      const uint32_t m = d.mezcla[i];
      const bool color_directo = (m & 0x1F) == 1 && ((m >> 8) & 0x1F) == 0 && ((m >> 5) & 0x7) == 0;
      const bool alfa_directo = ((m >> 16) & 0x1F) == 1 && ((m >> 24) & 0x1F) == 0 && ((m >> 21) & 0x7) == 0;
      if (((mascara & 0x7) && !color_directo) || ((mascara & 0x8) && !alfa_directo)) {
        opaco_en_todos = false;
      }
    }
    const bool prueba_z_dibujo = claves[4] && ((control_profundidad >> 1) & 0x1);
    const bool escribe_z_dibujo = prueba_z_dibujo && ((control_profundidad >> 2) & 0x1);
    bool es_cielo = false;
    if (ps && ps->shader->huella == kHuellaCielo && mascaras) {
      ++cielo_vistos_;
      const bool estencil = claves[4] && (control_profundidad & 0x1);
      const bool en_la_escena = !(claves[0] == 0 && claves[1] == 0 && claves[2] == 0 && claves[3] == 0) &&
                                pitch >= 1280 && claves[4] != 0;
      const bool es_el_domo = en_la_escena && opaco_en_todos && prueba_z_dibujo && !escribe_z_dibujo && !estencil &&
                              !(ps->salidas & 0x10) && !oclusion && con_indices &&
                              cuenta_indices == kIndicesDomoCielo && dibujos_en_pase_ == 0;
      if (es_el_domo) {
        if (++cielo_guardia_en_fotograma_ > 1 && cielo_guardia_ != kCieloDescartado) {
          cielo_guardia_ = kCieloDescartado;
          REXLOG_WARN("[nativo] C6 GLES cielo: la guardia ve {} domos en el MISMO fotograma; no se aplaza nada en "
                      "toda la sesion. La imagen queda intacta",
                      cielo_guardia_en_fotograma_);
        }
      }
      es_cielo = cielo_aplazado_ && es_el_domo && cielo_guardia_ == kCieloAplazando;
    }
    /*
     * The sky may only be moved past a draw that is opaque, writes color and writes depth: wherever it paints, the
     * final color is its own, and its z makes the sky's LEQUAL test fail there. Anything else (blending, no Z
     * write, no color at all) and the sky goes first. Stricter than Vulkan in one case: a draw with no color is
     * not passed (it would leave its z where the sky's color should have stayed).
     */
    if (cielo_pendiente_) {
      if (es_cielo) {
        EmitirCielo(kCieloPorOtroCielo);
      } else if (!(opaco_en_todos && escribe_z_dibujo && mascaras)) {
        EmitirCielo(opaco_en_todos ? kCieloPorSinZ : kCieloPorMezcla);
      }
    }
    if (es_cielo) {
      if (lee_reflejo) {
        nfsmw::reflejo_demanda::AnotarVisible(false);  // the deferred sky is not measured
      }
      cielo_ = d;
      cielo_segmento_ = segmento_minimo;
      cielo_al_aplazar_ = dibujos_en_pase_;
      cielo_pendiente_ = true;
      ++cielo_aplazados_;
      ++dibujados_;
      return true;
    }

    // --- Draw --------------------------------------------------------------------------------------------------
    // Diagnostic: the first draws, then 1 in 100,000, check GL right after the draw and log its state (CrearTextura
    // empties the error queue, so the per-frame check can miss what a draw did).
    const bool diagnosticar = dibujados_ < 12 || dibujados_ % 100000 == 0;
    if (diagnosticar) {
      ComprobarGl("antes del dibujo diagnosticado");
    }
    /*
     * nfsmw_reflejo_visibilidad, as in Vulkan: a query around this draw alone if it samples the reflection; no
     * samples = it wrote nothing anywhere, so that reflection never reaches the image. While a game query is open
     * ours cannot be (one occlusion query at a time): not measured, it counts as visible. Every kTestigoCada
     * frames the final composite, which covers the whole screen, is measured the same way and must leave samples
     * (the guard in nfsmw_recortes_carrera.cpp).
     */
    GLuint consulta_visibilidad = 0;
    bool es_testigo = false;
    if (medir_visibilidad_) {
      if (lee_reflejo) {
        if (!oclusion) {
          consulta_visibilidad = EmpezarVisibilidad();
        }
        if (!consulta_visibilidad) {
          nfsmw::reflejo_demanda::AnotarVisible(false);
        }
      } else if (testigo_pendiente_ && ps && ps->shader->huella == kHuellaComposicion && !oclusion) {
        consulta_visibilidad = EmpezarVisibilidad();
        if (consulta_visibilidad) {
          es_testigo = true;
          testigo_pendiente_ = false;
          testigo_siguiente_ = fotograma_ + (nfsmw::reflejo_demanda::VisibilidadComprobada() ? kTestigoCada : 2);
        }
      }
    }
    Emitir(d);
    if (contador) {
      ++dibujos_contados_;
    }
    if (consulta_visibilidad) {
      glEndQuery(kConsultaVisibilidad);
      visibilidad_pendientes_.push_back({consulta_visibilidad, es_testigo});
      ++consultas_visibilidad_;
    }
    if (diagnosticar) {
      const GLenum error = glGetError();
      const GLenum estado_fb = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
      GLint fbo_actual = 0, programa_actual = 0;
      glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo_actual);
      glGetIntegerv(GL_CURRENT_PROGRAM, &programa_actual);
      REXLOG_INFO("[nativo] C6 GLES diag dibujo {}: error {:#x}, framebuffer {} ({} {}x{}, estado {:#x}), programa {} "
                  "({}), VS n{} PS n{}, modo {:#x} {} {}, viewport {},{} {}x{} z {:.2f}-{:.2f}, tijera {},{} {}x{}, "
                  "mascaras {:04X}, profundidad {:08X}, ndc {:.3f},{:.3f},{:.3f},{:.3f}, atributos {}, enlaces {}",
                  dibujados_, error, fbo_actual, fb->fbo, fb->ancho, fb->alto, estado_fb, programa_actual,
                  programa->programa, p.vs->numero, ps ? int(ps->numero) : -1, modo,
                  con_indices ? "indices" : "secuencial", con_indices ? uint32_t(cuenta_indices) : cuenta, d.vp[0], d.vp[1], d.vp[2],
                  d.vp[3], viewport[4], viewport[5], tijera[0], tijera[1], tijera[2], tijera[3], mascaras,
                  claves[4] ? control_profundidad : 0, ndc[0], ndc[1], ndc[2], ndc[3], entrada->atributos.size(),
                  entrada->enlaces.size());
    }
    ++dibujados_;
    return true;
  }

  // The pass is closing (a C2 copy or clear, an occlusion query of the game, the Swap): the deferred sky goes
  // out before any of them reads or measures the target.
  void TerminarPase() override {
    EmitirCielo(kCieloPorFinDePase);
    fbo_pase_ = 0;
    dibujos_en_pase_ = 0;
  }

 private:
  struct FramebufferGles {
    GLuint fbo = 0;
    uint32_t ancho = 0;
    uint32_t alto = 0;
    uint32_t presentes = 0;
  };

  // A draw with everything it needs to be emitted (Emitir), now or later (the deferred sky).
  struct DibujoGles {
    const ProgramaGles* programa = nullptr;
    const FramebufferGles* fb = nullptr;
    std::array<uint32_t, 4> mezcla{};
    uint32_t mascaras = 0;
    float mezcla_constante[4] = {};
    bool con_profundidad = false;
    uint32_t profundidad = 0;
    uint32_t estencil[2] = {};
    uint32_t rasterizado = 0;
    float sesgo[2] = {};
    bool reinicio = false;
    GLint vp[4] = {};
    float rango[2] = {};
    int32_t tijera[4] = {};
    std::array<TexturaDeRegistro, 16> por_registro{};
    std::array<SwizzleDeRegistro, 16> swizzles{};
    std::array<GLintptr, 3> ubos{};  // -1: not bound (no pixel shader)
    GLuint contador = 0;
    const EntradaGles* entrada = nullptr;
    std::array<GLintptr, 16> bases{};
    GLenum modo = GL_TRIANGLES;
    bool con_indices = false;
    GLsizei cuenta_indices = 0;
    GLenum tipo_indices = GL_UNSIGNED_SHORT;
    GLintptr offset_indices = 0;
    GLint base = 0;
    GLsizei cuenta = 0;
  };

  bool Rechazar(uint32_t causa, const char* texto) {
    ++rechazados_;
    ++causas_[causa];
    if (avisados_.insert(causa).second) {
      REXLOG_WARN("[nativo] C6 GLES: {} (causa {})", texto, causa);
    }
    return false;
  }

  // Logs the pending GL errors with where they were seen (the first 32; all of them are counted).
  void ComprobarGl(const char* donde) {
    for (GLenum error = glGetError(); error != GL_NO_ERROR; error = glGetError()) {
      if (++errores_gl_ <= 32) {
        REXLOG_WARN("[nativo] C6 GLES: error de GL {:#x} {} (error {})", error, donde, errores_gl_);
      }
    }
  }

  void Avisar(uint32_t causa, const char* texto) {
    if (avisados_.insert(causa).second) {
      REXLOG_WARN("[nativo] C6 GLES: {} (causa {})", texto, causa);
    }
  }

  void LeerAjustes() {
    sin_desenfoque_ = REXCVAR_GET(nfsmw_nativo_sin_desenfoque);
    pcf_barato_ = REXCVAR_GET(nfsmw_nativo_pcf_barato);
    mipmaps_ = REXCVAR_GET(nfsmw_nativo_mipmaps);
    anisotropico_ = std::max(0, REXCVAR_GET(nfsmw_nativo_anisotropico));
    sombras_sesgo_pendiente_ = REXCVAR_GET(nfsmw_nativo_sombras_sesgo_pendiente);
    sombras_sesgo_constante_ = REXCVAR_GET(nfsmw_nativo_sombras_sesgo_constante);
    dedupe_ = REXCVAR_GET(nfsmw_nativo_dedupe_vertices);
    sin_vegetacion_ = REXCVAR_GET(nfsmw_sombras_sin_vegetacion);
    saltar_invisibles_ = REXCVAR_GET(nfsmw_nativo_saltar_invisibles);
    z_temprana_ = REXCVAR_GET(nfsmw_nativo_z_temprana);
    memo_activo_ = REXCVAR_GET(nfsmw_nativo_gles_memo_texturas);
    cielo_aplazado_ = REXCVAR_GET(nfsmw_nativo_cielo_aplazado);
    bc1_16_ = REXCVAR_GET(nfsmw_nativo_bc1_16_bits);
    const bool cronometrar = REXCVAR_GET(nfsmw_nativo_gles_etapas);
    if (cronometrar != cronometrar_) {
      cronometrar_ = cronometrar;
      etapas_ns_.fill(0);
      fotogramas_cronometrados_ = 0;
      dibujados_al_cronometrar_ = dibujados_;
    }
  }

  // The 16-bit indices of a draw (nfsmw_nativo_indices_neon), with Vulkan's guard: the first 20,000 draws, then 1 in
  // 4,096, also go through the plain loop and are compared; on a difference, the plain loop for the rest of the
  // session.
  void IndicesDe16(const uint8_t* datos, uint32_t cuenta, uint16_t* salida, bool girar, uint32_t& minimo,
                   uint32_t& maximo) {
    if (indices_neon_ < 0) {
      indices_neon_ = REXCVAR_GET(nfsmw_nativo_indices_neon) ? 1 : 0;
    }
    if (indices_neon_ == 0) {
      IndicesDe16Escalar(datos, cuenta, salida, girar, minimo, maximo);
      return;
    }
    IndicesDe16Neon(datos, cuenta, salida, girar, minimo, maximo);
    const uint64_t n = ++indices_neon_dibujos_;
    if (n > 20000 && (n & 4095) != 0) {
      return;
    }
    indices_prueba_.resize(cuenta);
    uint32_t mn = 0, mx = 0;
    IndicesDe16Escalar(datos, cuenta, indices_prueba_.data(), girar, mn, mx);
    if (mn != minimo || mx != maximo ||
        (cuenta && std::memcmp(indices_prueba_.data(), salida, size_t(cuenta) * 2) != 0)) {
      if (cuenta) {
        std::memcpy(salida, indices_prueba_.data(), size_t(cuenta) * 2);
      }
      minimo = mn;
      maximo = mx;
      indices_neon_ = 0;
      REXLOG_ERROR("[nativo] C6 GLES indices de 16 bits: DIFERENCIA entre NEON y el bucle de siempre ({} indices): el "
                   "bucle de siempre para el resto de la sesion",
                   cuenta);
    }
  }

  // --- Emitting a draw ------------------------------------------------------------------------------------------

  // Sets the state of `d` where it changed and draws it.
  void Emitir(const DibujoGles& d) {
    if (!estado_.valido) {
      AplicarTodo();
    }
    if (estado_.fbo != d.fb->fbo) {
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, d.fb->fbo);
      estado_.fbo = d.fb->fbo;
    }
    if (estado_.programa != d.programa->programa) {
      glUseProgram(d.programa->programa);
      estado_.programa = d.programa->programa;
    }
    AplicarMezcla(d.mezcla, d.mascaras, d.fb->presentes);
    if (std::memcmp(d.mezcla_constante, estado_.mezcla_constante, sizeof(d.mezcla_constante)) != 0) {
      glBlendColor(d.mezcla_constante[0], d.mezcla_constante[1], d.mezcla_constante[2], d.mezcla_constante[3]);
      std::memcpy(estado_.mezcla_constante, d.mezcla_constante, sizeof(d.mezcla_constante));
    }
    AplicarProfundidad(d.con_profundidad, d.profundidad, d.estencil);
    AplicarRasterizado(d.rasterizado, d.sesgo);
    if (d.reinicio != estado_.reinicio) {
      d.reinicio ? glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX) : glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
      estado_.reinicio = d.reinicio;
    }
    if (std::memcmp(d.vp, estado_.viewport, sizeof(d.vp)) != 0) {
      glViewport(d.vp[0], d.vp[1], d.vp[2], d.vp[3]);
      std::memcpy(estado_.viewport, d.vp, sizeof(d.vp));
    }
    if (d.rango[0] != estado_.rango[0] || d.rango[1] != estado_.rango[1]) {
      glDepthRangef(d.rango[0], d.rango[1]);
      estado_.rango[0] = d.rango[0];
      estado_.rango[1] = d.rango[1];
    }
    if (std::memcmp(d.tijera, estado_.tijera, sizeof(d.tijera)) != 0) {
      glScissor(d.tijera[0], d.tijera[1], d.tijera[2], d.tijera[3]);
      std::memcpy(estado_.tijera, d.tijera, sizeof(d.tijera));
    }
    // Textures on the units the program's samplers use (dimension * 16 + register).
    for (const gles::SamplerGlsl& sm : d.programa->samplers) {
      const TexturaDeRegistro& t = d.por_registro[sm.registro];
      static constexpr GLenum kObjetivos[3] = {GL_TEXTURE_2D, GL_TEXTURE_3D, GL_TEXTURE_CUBE_MAP};
      const GLenum objetivo = kObjetivos[sm.dimension];
      const GLuint textura = t.textura && t.objetivo == objetivo ? t.textura : vacias_[sm.dimension];
      const GLuint sampler = t.textura && t.objetivo == objetivo ? t.sampler : sampler_vacio_;
      if (estado_.texturas[sm.unidad] != textura || estado_.objetivos[sm.unidad] != objetivo) {
        glActiveTexture(GL_TEXTURE0 + sm.unidad);
        glBindTexture(objetivo, textura);
        estado_.texturas[sm.unidad] = textura;
        estado_.objetivos[sm.unidad] = objetivo;
      }
      if (estado_.samplers[sm.unidad] != sampler) {
        glBindSampler(sm.unidad, sampler);
        estado_.samplers[sm.unidad] = sampler;
      }
    }
    // Uniform blocks: 0 VS, 1 PS, 2 shared (nfsmw_gles_shaders.h), only where their place changed.
    static constexpr GLuint kBloques[3] = {gles::kBloqueVertices, gles::kBloquePixeles, gles::kBloqueCompartidas};
    static constexpr GLsizeiptr kBytesBloques[3] = {kUboBytesVs, kUboBytesPs, kUboBytesCompartidas};
    for (uint32_t i = 0; i < 3; ++i) {
      if (d.ubos[i] >= 0 && estado_.ubos[i] != d.ubos[i]) {
        glBindBufferRange(GL_UNIFORM_BUFFER, kBloques[i], flujo_.bufer(), d.ubos[i], kBytesBloques[i]);
        estado_.ubos[i] = d.ubos[i];
      }
    }
    if (d.contador && estado_.contador != d.contador) {
      glBindBufferBase(GL_ATOMIC_COUNTER_BUFFER, 0, d.contador);
      estado_.contador = d.contador;
    }
    // Vertex attributes, only if the input or the streams' places changed (with a single stream and the base
    // vertex they stay at 0).
    if (d.entrada != entrada_atributos_ || d.bases != bases_atributos_) {
      glBindBuffer(GL_ARRAY_BUFFER, flujo_.bufer());
      uint32_t ubicaciones = 0;
      for (const AtributoGles& a : d.entrada->atributos) {
        const GLsizei zancada = GLsizei(d.entrada->enlaces[a.enlace].zancada);
        const void* puntero = reinterpret_cast<const void*>(d.bases[a.enlace] + GLintptr(a.offset));
        if (a.formato.entero) {
          glVertexAttribIPointer(a.ubicacion, a.formato.componentes, a.formato.tipo, zancada, puntero);
        } else {
          glVertexAttribPointer(a.ubicacion, a.formato.componentes, a.formato.tipo,
                                a.formato.normalizado ? GL_TRUE : GL_FALSE, zancada, puntero);
        }
        ubicaciones |= 1u << a.ubicacion;
      }
      const uint32_t cambian = ubicaciones ^ ubicaciones_activas_;
      for (uint32_t u = 0; u < 16; ++u) {
        if ((cambian >> u) & 1) {
          ((ubicaciones >> u) & 1) ? glEnableVertexAttribArray(u) : glDisableVertexAttribArray(u);
        }
      }
      ubicaciones_activas_ = ubicaciones;
      entrada_atributos_ = d.entrada;
      bases_atributos_ = d.bases;
    }
    Etapa(8);
    if (d.con_indices) {
      // GL_ELEMENT_ARRAY_BUFFER is the stream buffer, bound with the VAO in AplicarTodo.
      const void* desde = reinterpret_cast<const void*>(d.offset_indices);
      if (base_vertice_) {
        glDrawElementsBaseVertex(d.modo, d.cuenta_indices, d.tipo_indices, desde, d.base);
      } else {
        glDrawElements(d.modo, d.cuenta_indices, d.tipo_indices, desde);
      }
    } else {
      glDrawArrays(d.modo, d.base, d.cuenta);
    }
    Etapa(9);
    ++dibujos_en_pase_;
  }

  // --- Deferred sky (nfsmw_nativo_cielo_aplazado) ------------------------------------------------------------------

  enum : uint32_t {
    kCieloPorMezcla = 0,
    kCieloPorSinZ,
    kCieloPorFinDePase,
    kCieloPorOtroCielo,
    kCieloPorBufer,
    kCieloMotivos,
  };
  enum : uint32_t { kCieloMirando = 0, kCieloAplazando = 1, kCieloDescartado = 2 };

  void EmitirCielo(uint32_t motivo) {
    if (!cielo_pendiente_) {
      return;
    }
    cielo_pendiente_ = false;
    if (!flujo_.SigueVivo(cielo_segmento_)) {
      // Cannot happen (it goes out before the head reaches its data), but a stale upload must never be drawn.
      if (++cielo_perdidos_ <= 4) {
        REXLOG_WARN("[nativo] C6 GLES cielo: el cielo aplazado se ha perdido (sus datos ya no estan en el bufer de "
                    "subida)");
      }
      return;
    }
    // Another register may have given one of its textures another swizzle meanwhile.
    for (const SwizzleDeRegistro& sw : cielo_.swizzles) {
      if (sw.donde && *sw.donde != sw.valor) {
        EnlazarParaTrabajar(sw.objetivo, cielo_textura_de(sw));
        PonerSwizzle(sw.objetivo, sw.valor);
        *sw.donde = sw.valor;
      }
    }
    const uint64_t marca = marca_;
    Emitir(cielo_);
    marca_ = marca;  // its time is charged to the draw that emitted it, not split into its phases
    ++cielo_emitidos_[motivo];
    cielo_posicion_suma_ += dibujos_en_pase_ - 1 - cielo_al_aplazar_;
  }

  // The texture whose swizzle `sw` notes, among the sky's registers.
  GLuint cielo_textura_de(const SwizzleDeRegistro& sw) const {
    for (size_t i = 0; i < cielo_.swizzles.size(); ++i) {
      if (cielo_.swizzles[i].donde == sw.donde) {
        return cielo_.por_registro[i].textura;
      }
    }
    return 0;
  }

  // Once per frame: the guard's test (as CerrarFotogramaDeLaGuardiaDelCielo in Vulkan) and the report.
  void CerrarFotogramaDelCielo() {
    const uint32_t en_este = cielo_guardia_en_fotograma_;
    cielo_guardia_en_fotograma_ = 0;
    if (cielo_guardia_ == kCieloMirando && en_este != 0) {
      // Frames without a dome (menus, loading screens) say nothing about the criterion.
      cielo_guardia_max_ = std::max(cielo_guardia_max_, en_este);
      if (en_este == 1) {
        ++cielo_guardia_con_uno_;
      }
      if (++cielo_guardia_fotogramas_ >= kCieloFotogramasPrueba) {
        if (cielo_guardia_max_ == 1 && cielo_guardia_con_uno_ >= kCieloFotogramasConUno) {
          cielo_guardia_ = kCieloAplazando;
          REXLOG_INFO("[nativo] C6 GLES cielo: guardia SUPERADA ({} de {} fotogramas con exactamente un domo, nunca "
                      "dos): se aplaza el cielo",
                      cielo_guardia_con_uno_, cielo_guardia_fotogramas_);
        } else {
          cielo_guardia_ = kCieloDescartado;
          REXLOG_WARN("[nativo] C6 GLES cielo: guardia NO superada (maximo {} domos en un fotograma, {} de {} con "
                      "exactamente uno): no se aplaza nada",
                      cielo_guardia_max_, cielo_guardia_con_uno_, cielo_guardia_fotogramas_);
        }
      }
    }
    if (fotograma_ % 1800 == 0 && cielo_vistos_) {
      const uint64_t emitidos = cielo_emitidos_[0] + cielo_emitidos_[1] + cielo_emitidos_[2] + cielo_emitidos_[3] +
                                cielo_emitidos_[4];
      REXLOG_INFO("[nativo] C6 GLES cielo aplazado ({}, guardia {}): {} dibujos del cielo, {} aplazados; emitidos {} "
                  "por un dibujo con mezcla, {} por uno sin Z, {} al cerrarse el pase, {} por otro cielo, {} por el "
                  "bufer; perdidos {}; {:.0f} dibujos por delante de media",
                  cielo_aplazado_ ? "encendido" : "APAGADO",
                  cielo_guardia_ == kCieloAplazando ? "superada" : cielo_guardia_ == kCieloDescartado ? "descartada"
                                                                                                      : "mirando",
                  cielo_vistos_, cielo_aplazados_, cielo_emitidos_[kCieloPorMezcla], cielo_emitidos_[kCieloPorSinZ],
                  cielo_emitidos_[kCieloPorFinDePase], cielo_emitidos_[kCieloPorOtroCielo],
                  cielo_emitidos_[kCieloPorBufer], cielo_perdidos_,
                  emitidos ? double(cielo_posicion_suma_) / double(emitidos) : 0.0);
    }
  }

  // --- Phases (nfsmw_nativo_gles_etapas) ---------------------------------------------------------------------------

  void Etapa(uint32_t i) {
    if (!marca_) {
      return;
    }
    const uint64_t ahora = RelojNs();
    etapas_ns_[i] += ahora - marca_;
    marca_ = ahora;
  }

  void InformeEtapas() {
    if (!cronometrar_) {
      return;
    }
    ++fotogramas_cronometrados_;
    if (fotogramas_cronometrados_ < 600) {
      return;
    }
    const double dibujos = double(std::max<uint64_t>(dibujados_ - dibujados_al_cronometrar_, 1));
    const double fotogramas = double(fotogramas_cronometrados_);
    uint64_t total = 0;
    std::string partes;
    for (uint32_t i = 0; i < kEtapas; ++i) {
      total += etapas_ns_[i];
      partes += fmt::format("{}{} {:.2f} us ({:.2f} ms)", i ? ", " : "", kNombresEtapas[i],
                            double(etapas_ns_[i]) / 1000.0 / dibujos, double(etapas_ns_[i]) / 1e6 / fotogramas);
    }
    REXLOG_INFO("[nativo] C6 GLES etapas: {:.0f} dibujos por fotograma, {:.2f} us por dibujo ({:.2f} ms por fotograma): "
                "{}",
                dibujos / fotogramas, double(total) / 1000.0 / dibujos, double(total) / 1e6 / fotogramas, partes);
    etapas_ns_.fill(0);
    fotogramas_cronometrados_ = 0;
    dibujados_al_cronometrar_ = dibujados_;
  }

  // --- Reflection visibility (nfsmw_reflejo_visibilidad) ---------------------------------------------------------

  GLuint EmpezarVisibilidad() {
    if (visibilidad_pendientes_.size() >= 64) {
      RecogerVisibilidad(true);  // the GPU is far behind: the oldest count as visible, without waiting
    }
    GLuint consulta = 0;
    if (!visibilidad_libres_.empty()) {
      consulta = visibilidad_libres_.back();
      visibilidad_libres_.pop_back();
    } else {
      glGenQueries(1, &consulta);
    }
    glBeginQuery(kConsultaVisibilidad, consulta);
    return consulta;
  }

  // The results already available, in order and without waiting. With `vaciar`, the oldest half is given up
  // too: the water counts as visible and the witness neither for nor against.
  void RecogerVisibilidad(bool vaciar) {
    size_t hechas = 0;
    for (; hechas < visibilidad_pendientes_.size(); ++hechas) {
      const ConsultaVisibilidad& c = visibilidad_pendientes_[hechas];
      GLuint disponible = GL_FALSE;
      glGetQueryObjectuiv(c.consulta, GL_QUERY_RESULT_AVAILABLE, &disponible);
      if (!disponible) {
        if (!vaciar || hechas >= visibilidad_pendientes_.size() / 2) {
          break;
        }
        if (!c.testigo) {
          nfsmw::reflejo_demanda::AnotarVisible(false);
        }
      } else {
        GLuint muestras = 0;
        glGetQueryObjectuiv(c.consulta, GL_QUERY_RESULT, &muestras);
        if (c.testigo) {
          nfsmw::reflejo_demanda::AnotarTestigo(muestras != 0);
        } else if (muestras) {
          nfsmw::reflejo_demanda::AnotarVisible(true);
        } else {
          nfsmw::reflejo_demanda::AnotarOculto();
          ++reflejos_ocultos_;
        }
      }
      visibilidad_libres_.push_back(c.consulta);
    }
    visibilidad_pendientes_.erase(visibilidad_pendientes_.begin(), visibilidad_pendientes_.begin() + ptrdiff_t(hechas));
  }

  // --- Vertex input (CalcularEntrada) --------------------------------------------------------------------------

  const EntradaGles* EntradaDe(const PeticionDibujo& p) {
    if (entrada_generacion_ == p.generacion_vs && entrada_vs_ == p.vs && entrada_actual_) {
      return entrada_actual_;
    }
    const EntradaShader& vs = *p.vs;
    const auto& parcheado = p.vs_microcodigo;
    if (parcheado.size() != vs.microcodigo.size() || vs.elementos.size() > 32) {
      Rechazar(20, "el VS del anillo no tiene la longitud del contenedor");
      return nullptr;
    }
    // Key: everything read from the elements and both microcodes, seeded with the VS (ClaveEntrada).
    std::array<uint32_t, 32 * 6> palabras;
    size_t n = 0;
    for (const ElementoVertice& elemento : vs.elementos) {
      const size_t i = size_t(elemento.instruccion) * 3;
      if (i + 2 >= parcheado.size()) {
        Rechazar(20, "el VS del anillo no tiene la longitud del contenedor");
        return nullptr;
      }
      palabras[n++] = (uint32_t(elemento.instruccion) << 16) | (uint32_t(elemento.uso) << 8) | elemento.indice_uso;
      palabras[n++] = vs.microcodigo[i];
      palabras[n++] = vs.microcodigo[i + 1];
      palabras[n++] = parcheado[i];
      palabras[n++] = parcheado[i + 1];
      palabras[n++] = parcheado[i + 2];
    }
    const uint64_t clave = XXH3_64bits_withSeed(palabras.data(), n * sizeof(uint32_t), uint64_t(uintptr_t(p.vs)));
    auto it = entradas_.find(clave);
    if (it == entradas_.end()) {
      EntradaGles entrada;
      if (!CalcularEntrada(p, entrada)) {
        return nullptr;
      }
      it = entradas_.emplace(clave, std::move(entrada)).first;
    }
    entrada_generacion_ = p.generacion_vs;
    entrada_vs_ = p.vs;
    entrada_actual_ = &it->second;
    return entrada_actual_;
  }

  bool CalcularEntrada(const PeticionDibujo& p, EntradaGles& entrada) {
    const EntradaShader& vs = *p.vs;
    const auto& parcheado = p.vs_microcodigo;
    entrada.remapeos.fill(kRemapeoIdentidad);
    uint32_t ubicaciones_usadas = 0;
    for (const ElementoVertice& elemento : vs.elementos) {
      const uint32_t registro = (vs.microcodigo[size_t(elemento.instruccion) * 3] >> 12) & 0x3F;
      const uint32_t original = vs.microcodigo[size_t(elemento.instruccion) * 3 + 1] & 0xFFF;
      size_t q = SIZE_MAX;
      for (const ElementoVertice& otro : vs.elementos) {
        const size_t i = size_t(otro.instruccion) * 3;
        if (((parcheado[i] >> 12) & 0x3F) != registro || (parcheado[i] & 0x1F) != 0) {
          continue;
        }
        if (q == SIZE_MAX) {
          q = i;
        }
        if (MascaraEscrita(parcheado[i + 1] & 0xFFF) == MascaraEscrita(original)) {
          q = i;  // the one that writes the same components
          break;
        }
      }
      if (q == SIZE_MAX) {
        return Rechazar(21, "fetch de vertices no encontrado en el VS parcheado");
      }
      const uint32_t d0 = parcheado[q], d1 = parcheado[q + 1], d2 = parcheado[q + 2];
      const uint32_t ranura = ((d0 >> 20) & 0x1F) * 3 + ((d0 >> 25) & 0x3);
      const uint32_t formato = (d1 >> 16) & 0x3F;
      const uint32_t zancada = (d2 & 0xFF) * 4;
      const int32_t offset = (int32_t(d2 << 1) >> 9) * 4;
      const int32_t ubicacion = UbicacionDeUso(elemento.uso, elemento.indice_uso);
      if (ubicacion < 0 || ((ubicaciones_usadas >> ubicacion) & 0x1)) {
        Avisar(22, "elemento de vertices sin ubicacion libre en el shader: se omite");
        continue;
      }
      bool r11g11b10 = false;
      AtributoGles a;
      if (!FormatoAtributoGl(formato, EntradaEntera(elemento.uso), (d1 >> 12) & 0x1, (d1 >> 13) & 0x1, a.formato,
                             r11g11b10)) {
        return Rechazar(300 + formato, "formato de vertice todavia no soportado");
      }
      if (r11g11b10) {
        entrada.especializacion |= 0x1;
      }
      uint32_t enlace = 0;
      while (enlace < entrada.enlaces.size() && entrada.enlaces[enlace].ranura != ranura) {
        ++enlace;
      }
      if (enlace == entrada.enlaces.size()) {
        entrada.enlaces.push_back({ranura, zancada});
      } else if (!entrada.enlaces[enlace].zancada) {
        entrada.enlaces[enlace].zancada = zancada;
      }
      if (offset < 0) {
        return Rechazar(24, "offset de vertice negativo");
      }
      a.ubicacion = uint32_t(ubicacion);
      a.enlace = enlace;
      a.offset = uint32_t(offset);
      entrada.atributos.push_back(a);
      entrada.remapeos[ubicacion] = CodigoRemapeo(original, d1 & 0xFFF);
      ubicaciones_usadas |= uint32_t(1) << ubicacion;
    }
    for (const EnlaceGles& enlace : entrada.enlaces) {
      if (!enlace.zancada) {
        return Rechazar(25, "stream de vertices sin zancada");
      }
    }
    return true;
  }

  // --- Framing (CalcularEncuadre in nfsmw_nativo_dibujos.cpp) -----------------------------------------------------

  bool CalcularEncuadre(const uint32_t* r, uint32_t vte, bool sin_recorte, uint32_t modo_sc, uint32_t pase_ancho,
                        uint32_t pase_alto, float viewport[6], float ndc[4], int32_t tijera[4]) {
    const float escala_x = (vte & 0x1) ? Flotante(r[gr::XE_GPU_REG_PA_CL_VPORT_XSCALE]) : 1.0f;
    float centro_x = (vte & 0x2) ? Flotante(r[gr::XE_GPU_REG_PA_CL_VPORT_XOFFSET]) : 0.0f;
    const float escala_y = (vte & 0x4) ? Flotante(r[gr::XE_GPU_REG_PA_CL_VPORT_YSCALE]) : 1.0f;
    float centro_y = (vte & 0x8) ? Flotante(r[gr::XE_GPU_REG_PA_CL_VPORT_YOFFSET]) : 0.0f;
    const float escala_z = (vte & 0x10) ? Flotante(r[gr::XE_GPU_REG_PA_CL_VPORT_ZSCALE]) : 1.0f;
    const float centro_z = (vte & 0x20) ? Flotante(r[gr::XE_GPU_REG_PA_CL_VPORT_ZOFFSET]) : 0.0f;
    const uint32_t ventana = r[gr::XE_GPU_REG_PA_SC_WINDOW_OFFSET];
    const int32_t ventana_x = int32_t((ventana & 0x7FFF) << 17) >> 17;
    const int32_t ventana_y = int32_t(((ventana >> 16) & 0x7FFF) << 17) >> 17;
    if ((modo_sc >> 16) & 0x1) {
      centro_x += float(ventana_x);
      centro_y += float(ventana_y);
    }
    viewport[0] = centro_x - std::abs(escala_x);
    viewport[2] = 2.0f * std::abs(escala_x);
    viewport[1] = centro_y + escala_y;
    viewport[3] = -2.0f * escala_y;
    viewport[4] = std::clamp(centro_z, 0.0f, 1.0f);
    viewport[5] = std::clamp(centro_z + escala_z, 0.0f, 1.0f);
    ndc[0] = 1.0f;
    ndc[1] = 1.0f;
    ndc[2] = 0.0f;
    ndc[3] = 0.0f;
    if (sin_recorte) {
      const float ancho = float(pase_ancho);
      const float alto = float(pase_alto);
      ndc[0] = escala_x * 2.0f / ancho;
      ndc[2] = (centro_x - ancho * 0.5f) * 2.0f / ancho;
      ndc[1] = -escala_y * 2.0f / alto;
      ndc[3] = -(centro_y - alto * 0.5f) * 2.0f / alto;
      viewport[0] = 0.0f;
      viewport[1] = 0.0f;
      viewport[2] = ancho;
      viewport[3] = alto;
    }
    if (viewport[2] < 1.0f || std::abs(viewport[3]) < 1.0f) {
      return false;
    }
    const uint32_t tl = r[gr::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL];
    const uint32_t br = r[gr::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
    int32_t x0 = int32_t(tl & 0x3FFF), y0 = int32_t((tl >> 16) & 0x3FFF);
    int32_t x1 = int32_t(br & 0x3FFF), y1 = int32_t((br >> 16) & 0x3FFF);
    if (!((tl >> 31) & 0x1)) {
      x0 += ventana_x;
      y0 += ventana_y;
      x1 += ventana_x;
      y1 += ventana_y;
    }
    x0 = std::clamp(x0, 0, int32_t(pase_ancho));
    x1 = std::clamp(x1, 0, int32_t(pase_ancho));
    y0 = std::clamp(y0, 0, int32_t(pase_alto));
    y1 = std::clamp(y1, 0, int32_t(pase_alto));
    if (x1 <= x0 || y1 <= y0) {
      return false;
    }
    tijera[0] = x0;
    tijera[1] = y0;
    tijera[2] = x1 - x0;
    tijera[3] = y1 - y0;
    return true;
  }

  // --- Framebuffers -------------------------------------------------------------------------------------------

  const FramebufferGles* FramebufferDe(const uint64_t claves[5], uint32_t pitch) {
    const uint64_t clave = XXH3_64bits(claves, sizeof(uint64_t) * 5);
    if (const auto it = fbos_.find(clave); it != fbos_.end()) {
      return &it->second;
    }
    std::array<ImagenGles*, 5> imagenes{};
    for (uint32_t i = 0; i < 4; ++i) {
      if (claves[i]) {
        imagenes[i] = contexto_->DestinoColor(uint32_t(claves[i] >> 24) & 0xFFF, uint32_t(claves[i] >> 16) & 0xF, pitch);
        if (!imagenes[i]) {
          Rechazar(40, "no hay destino de color");
          return nullptr;
        }
      }
    }
    if (claves[4]) {
      imagenes[4] =
          contexto_->DestinoProfundidad(uint32_t(claves[4] >> 24) & 0xFFF, uint32_t(claves[4] >> 16) & 0x1, pitch);
      if (!imagenes[4]) {
        Rechazar(41, "no hay destino de profundidad");
        return nullptr;
      }
    }
    // Getting the images may have cleared them (C2's Preparar), which opens the masks and drops the scissor
    // behind the state cache's back: everything is set again on this draw.
    estado_.valido = false;
    FramebufferGles fb;
    fb.ancho = UINT32_MAX;
    fb.alto = UINT32_MAX;
    glGenFramebuffers(1, &fb.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb.fbo);
    GLenum dibujo[4] = {GL_NONE, GL_NONE, GL_NONE, GL_NONE};
    for (uint32_t i = 0; i < 4; ++i) {
      if (imagenes[i]) {
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, imagenes[i]->textura, 0);
        dibujo[i] = GL_COLOR_ATTACHMENT0 + i;
        fb.presentes |= 1u << i;
      }
    }
    if (imagenes[4]) {
      glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, imagenes[4]->textura, 0);
    }
    glDrawBuffers(4, dibujo);
    for (const ImagenGles* imagen : imagenes) {
      if (imagen) {
        fb.ancho = std::min(fb.ancho, imagen->ancho);
        fb.alto = std::min(fb.alto, imagen->alto);
      }
    }
    const GLenum estado = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    estado_.fbo = fb.fbo;
    if (estado != GL_FRAMEBUFFER_COMPLETE) {
      REXLOG_ERROR("[nativo] C6 GLES: framebuffer incompleto ({:#x}) para los destinos {:016X} {:016X} {:016X} "
                   "{:016X} {:016X}",
                   estado, claves[0], claves[1], claves[2], claves[3], claves[4]);
      glDeleteFramebuffers(1, &fb.fbo);
      estado_.valido = false;
      Rechazar(43, "framebuffer incompleto");
      return nullptr;
    }
    return &fbos_.emplace(clave, fb).first->second;
  }

  // --- Programs ---------------------------------------------------------------------------------------------------

  const gles::ShaderGlsl* GlslDe(const EntradaShader& shader, bool vertices, uint64_t salidas) {
    const uint64_t clave = (uint64_t(shader.numero) << 1 | (vertices ? 1 : 0)) ^ (salidas * 0x9E3779B97F4A7C15ull);
    auto it = glsl_.find(clave);
    if (it == glsl_.end()) {
      it = glsl_.emplace(clave, gles::ConvertirAGlsl(shader.shader->spirv, vertices, opciones_glsl_, salidas)).first;
      if (!it->second.ok) {
        REXLOG_ERROR("[nativo] C6 GLES: el {} n{} no se convierte a GLSL ES: {}", vertices ? "VS" : "PS",
                     shader.numero, it->second.error);
      }
    }
    return it->second.ok ? &it->second : nullptr;
  }

  GLuint ShaderGl(GLenum etapa, const std::string& fuente, uint64_t clave, uint32_t numero) {
    if (const auto it = shaders_gl_.find(clave); it != shaders_gl_.end()) {
      return it->second;
    }
    GLuint shader = glCreateShader(etapa);
    const char* texto = fuente.c_str();
    glShaderSource(shader, 1, &texto, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
      char registro[2048] = {};
      glGetShaderInfoLog(shader, sizeof(registro) - 1, nullptr, registro);
      REXLOG_ERROR("[nativo] C6 GLES: el {} n{} no compila en el driver: {}",
                   etapa == GL_VERTEX_SHADER ? "VS" : "PS", numero, registro);
      glDeleteShader(shader);
      shader = 0;
    }
    shaders_gl_.emplace(clave, shader);
    return shader;
  }

  uint64_t HuellaSpirv(const EntradaShader& shader) {
    if (const auto it = huellas_spirv_.find(&shader); it != huellas_spirv_.end()) {
      return it->second;
    }
    const auto& spirv = shader.shader->spirv;
    const uint64_t huella = XXH3_64bits(spirv.data(), spirv.size() * sizeof(spirv[0]));
    huellas_spirv_.emplace(&shader, huella);
    return huella;
  }

  // The key of a program in the disk cache: everything its GLSL comes from.
  uint64_t ClaveDisco(const EntradaShader& vs, const EntradaShader* ps, uint32_t especializacion, uint32_t variantes) {
    const uint64_t partes[6] = {kVersionProgramas, uint64_t(opciones_glsl_.version), HuellaSpirv(vs),
                                ps ? HuellaSpirv(*ps) : 0, especializacion, variantes};
    return XXH3_64bits(partes, sizeof(partes));
  }

  const ProgramaGles* ProgramaDe(const EntradaShader& vs, const EntradaShader* ps, uint32_t especializacion,
                                 uint32_t variantes) {
    const uint64_t clave = (uint64_t(vs.numero) << 44) ^ (uint64_t(ps ? ps->numero + 1 : 0) << 32) ^ especializacion ^
                           (uint64_t(variantes) << 62);
    if (const auto it = programas_.find(clave); it != programas_.end()) {
      return it->second.fallido ? nullptr : &it->second;
    }
    ProgramaGles& programa = programas_[clave];
    programa.fallido = true;
    const auto antes = std::chrono::steady_clock::now();
    // From the disk cache: no conversion, compilation or link.
    const uint64_t clave_disco = cache_.activa() ? ClaveDisco(vs, ps, especializacion, variantes) : 0;
    if (const CacheProgramas::Registro* registro = cache_.activa() ? cache_.Buscar(clave_disco) : nullptr) {
      if (cache_.LeerBinario(*registro, binario_)) {
        GLuint p = glCreateProgram();
        glProgramBinary(p, registro->formato, binario_.data(), GLsizei(binario_.size()));
        GLint ok = GL_FALSE;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (ok == GL_TRUE) {
          programa.programa = p;
          programa.samplers = registro->samplers;
          programa.fallido = false;
          ++programas_disco_;
          estado_.programa = 0;
          return &programa;
        }
        glDeleteProgram(p);
      }
      while (glGetError() != GL_NO_ERROR) {
      }
      cache_.Olvidar(clave_disco);
      if (++programas_disco_fallidos_ <= 4) {
        REXLOG_WARN("[nativo] C6 GLES: un programa de la cache no se carga (VS n{} PS n{}): se compila", vs.numero,
                    ps ? int(ps->numero) : -1);
      }
    }
    // The PS first: its inputs decide which VS outputs stay.
    const gles::ShaderGlsl* glsl_ps = ps ? GlslDe(*ps, false, ~uint64_t(0)) : nullptr;
    if (ps && !glsl_ps) {
      Rechazar(50, "un pixel shader no se convierte a GLSL ES");
      return nullptr;
    }
    const uint64_t salidas = glsl_ps ? glsl_ps->ubicaciones : 0;
    const gles::ShaderGlsl* glsl_vs = GlslDe(vs, true, salidas);
    if (!glsl_vs) {
      Rechazar(51, "un vertex shader no se convierte a GLSL ES");
      return nullptr;
    }
    const uint64_t semilla = uint64_t(especializacion) << 32;
    const GLuint sv = ShaderGl(GL_VERTEX_SHADER, gles::ConEspecializacion(glsl_vs->fuente, especializacion),
                               XXH3_64bits_withSeed(&salidas, sizeof(salidas), semilla ^ (uint64_t(vs.numero) << 1 | 1)),
                               vs.numero);
    // The fragment stage: the PS, or an empty one for depth only, with the variants on top.
    std::string fuente_ps = glsl_ps ? gles::ConEspecializacion(glsl_ps->fuente, especializacion)
                                    : std::string(opciones_glsl_.version >= 320 ? "#version 320 es\nvoid main() {}\n"
                                                                                : "#version 310 es\nvoid main() {}\n");
    if (variantes) {
      fuente_ps = VarianteFragmentos(std::move(fuente_ps), variantes);
      if (fuente_ps.empty()) {
        Rechazar(54, "un pixel shader no tiene main()");
        return nullptr;
      }
    }
    const GLuint sp = ShaderGl(GL_FRAGMENT_SHADER, fuente_ps,
                               (glsl_ps ? semilla ^ (uint64_t(ps->numero) << 1) : 0xFFFFFFFFFFFFFFFFull) ^
                                   (uint64_t(variantes) << 60),
                               ps ? ps->numero : 0);
    if (!sv || !sp) {
      Rechazar(52, "un shader no compila en el driver");
      return nullptr;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, sv);
    glAttachShader(p, sp);
    if (cache_.activa()) {
      glProgramParameteri(p, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    }
    glLinkProgram(p);
    GLint ok = GL_FALSE;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
      char registro[2048] = {};
      glGetProgramInfoLog(p, sizeof(registro) - 1, nullptr, registro);
      REXLOG_ERROR("[nativo] C6 GLES: el programa VS n{} PS n{} no enlaza: {}", vs.numero, ps ? int(ps->numero) : -1,
                   registro);
      glDeleteProgram(p);
      Rechazar(53, "un programa no enlaza");
      return nullptr;
    }
    glDetachShader(p, sv);
    glDetachShader(p, sp);
    programa.programa = p;
    programa.fallido = false;
    if (glsl_ps) {
      programa.samplers = glsl_ps->samplers;
    }
    if (cache_.activa()) {
      cache_.Guardar(clave_disco, p, programa.samplers);
    }
    ++programas_compilados_;
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - antes).count();
    ms_programas_ += ms;
    if (programas_.size() <= 64 || ms > 50.0) {
      REXLOG_INFO("[nativo] C6 GLES: programa {} (VS n{} PS n{} especializacion {:08X}{}{}) en {:.1f} ms",
                  programas_.size(), vs.numero, ps ? int(ps->numero) : -1, especializacion,
                  (variantes & kVarianteZTemprana) ? ", Z temprana" : "",
                  (variantes & kVarianteContar) ? ", cuenta muestras" : "", ms);
    }
    estado_.programa = 0;  // glUseProgram again (the cache does not know this one)
    return &programa;
  }

  // --- State ------------------------------------------------------------------------------------------------------

  // Everything to a known value, after C2 or at the start.
  void AplicarTodo() {
    estado_ = EstadoAplicado{};
    estado_.valido = true;
    glBindVertexArray(vao_);
    // The stream buffer stays bound for vertices (global) and indices (the VAO's): every draw reads from it.
    glBindBuffer(GL_ARRAY_BUFFER, flujo_.bufer());
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, flujo_.bufer());
    estado_.ubos.fill(-1);
    entrada_atributos_ = nullptr;
    glEnable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_FALSE);
    glStencilMask(0);
    glBlendColor(0, 0, 0, 0);
    glDepthRangef(0.0f, 0.0f);
    glViewport(0, 0, 0, 0);
    glScissor(0, 0, 0, 0);
    glUseProgram(0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    estado_.mezcla.fill(0xFFFFFFFFu);  // forces the first AplicarMezcla
    estado_.mascaras = 0xFFFFFFFFu;
    estado_.profundidad = 0xFFFFFFFFu;
    estado_.rasterizado = 0xFFFFFFFFu;
    estado_.estencil[0] = estado_.estencil[1] = 0xFFFFFFFFu;
    estado_.objetivos.fill(0);
    estado_.texturas.fill(0xFFFFFFFFu);
    estado_.samplers.fill(0xFFFFFFFFu);
    // Unit bindings were changed by C2 or the presenter (shared object names, per-context bindings).
    ubicaciones_activas_ = 0xFFFF;
    for (uint32_t u = 0; u < 16; ++u) {
      glDisableVertexAttribArray(u);
    }
    ubicaciones_activas_ = 0;
  }

  void AplicarMezcla(const std::array<uint32_t, 4>& mezcla, uint32_t mascaras, uint32_t presentes) {
    if (mezcla == estado_.mezcla && mascaras == estado_.mascaras) {
      return;
    }
    const bool indexado = indexado_;
    for (uint32_t i = 0; i < 4; ++i) {
      if (!((presentes >> i) & 1)) {
        continue;
      }
      const uint32_t m = mezcla[i];
      const uint32_t mascara = (mascaras >> (i * 4)) & 0xF;
      const uint32_t fuente = m & 0x1F, op = (m >> 5) & 0x7, destino = (m >> 8) & 0x1F;
      const uint32_t fuente_a = (m >> 16) & 0x1F, op_a = (m >> 21) & 0x7, destino_a = (m >> 24) & 0x1F;
      const bool escribe_rgb = (mascara & 0x7) != 0;
      const bool escribe_alfa = (mascara & 0x8) != 0;
      const bool color_directo = fuente == 1 && destino == 0 && op == 0;
      const bool alfa_directo = fuente_a == 1 && destino_a == 0 && op_a == 0;
      const bool mezclar = (escribe_rgb && !color_directo) || (escribe_alfa && !alfa_directo);
      if (indexado) {
        glColorMaski(i, mascara & 1, (mascara >> 1) & 1, (mascara >> 2) & 1, (mascara >> 3) & 1);
        if (mezclar) {
          glEnablei(GL_BLEND, i);
          glBlendFuncSeparatei(i, FactorGl(fuente), FactorGl(destino), FactorGl(fuente_a), FactorGl(destino_a));
          glBlendEquationSeparatei(i, OperacionGl(op), OperacionGl(op_a));
        } else {
          glDisablei(GL_BLEND, i);
        }
      } else if (i == 0) {
        // GLES 3.1 without indexed state: target 0's blending for all of them.
        glColorMask(mascara & 1, (mascara >> 1) & 1, (mascara >> 2) & 1, (mascara >> 3) & 1);
        if (mezclar) {
          glEnable(GL_BLEND);
          glBlendFuncSeparate(FactorGl(fuente), FactorGl(destino), FactorGl(fuente_a), FactorGl(destino_a));
          glBlendEquationSeparate(OperacionGl(op), OperacionGl(op_a));
        } else {
          glDisable(GL_BLEND);
        }
      }
    }
    estado_.mezcla = mezcla;
    estado_.mascaras = mascaras;
  }

  void AplicarProfundidad(bool con_profundidad, uint32_t d, const uint32_t estencil[2]) {
    if (con_profundidad == estado_.con_profundidad && d == estado_.profundidad &&
        (!(d & 0x1) || (estencil[0] == estado_.estencil[0] && estencil[1] == estado_.estencil[1]))) {
      return;
    }
    const bool prueba_z = con_profundidad && ((d >> 1) & 0x1);
    const bool escribe_z = prueba_z && ((d >> 2) & 0x1);
    prueba_z ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
    glDepthMask(escribe_z ? GL_TRUE : GL_FALSE);
    if (prueba_z) {
      glDepthFunc(kComparaciones[(d >> 4) & 0x7]);
    }
    const bool con_estencil = con_profundidad && (d & 0x1);
    if (con_estencil) {
      glEnable(GL_STENCIL_TEST);
      const uint32_t frente = estencil[0], dorso = estencil[1];
      // Front: compare (8-10), fail (11-13), pass (14-16), depth fail (17-19); back from bit 20 if bit 7.
      glStencilFuncSeparate(GL_FRONT, kComparaciones[(d >> 8) & 0x7], GLint(frente & 0xFF), (frente >> 8) & 0xFF);
      glStencilOpSeparate(GL_FRONT, kOperacionesEstencil[(d >> 11) & 0x7], kOperacionesEstencil[(d >> 17) & 0x7],
                          kOperacionesEstencil[(d >> 14) & 0x7]);
      glStencilMaskSeparate(GL_FRONT, (frente >> 16) & 0xFF);
      if ((d >> 7) & 0x1) {
        glStencilFuncSeparate(GL_BACK, kComparaciones[(d >> 20) & 0x7], GLint(dorso & 0xFF), (dorso >> 8) & 0xFF);
        glStencilOpSeparate(GL_BACK, kOperacionesEstencil[(d >> 23) & 0x7], kOperacionesEstencil[(d >> 29) & 0x7],
                            kOperacionesEstencil[(d >> 26) & 0x7]);
      } else {
        glStencilFuncSeparate(GL_BACK, kComparaciones[(d >> 8) & 0x7], GLint(dorso & 0xFF), (dorso >> 8) & 0xFF);
        glStencilOpSeparate(GL_BACK, kOperacionesEstencil[(d >> 11) & 0x7], kOperacionesEstencil[(d >> 17) & 0x7],
                            kOperacionesEstencil[(d >> 14) & 0x7]);
      }
      glStencilMaskSeparate(GL_BACK, (dorso >> 16) & 0xFF);
    } else {
      glDisable(GL_STENCIL_TEST);
      glStencilMask(0);
    }
    estado_.con_profundidad = con_profundidad;
    estado_.profundidad = d;
    estado_.estencil[0] = estencil[0];
    estado_.estencil[1] = estencil[1];
  }

  void AplicarRasterizado(uint32_t rasterizado, const float sesgo[2]) {
    if (rasterizado != estado_.rasterizado) {
      const bool frente = rasterizado & 0x1, dorso = rasterizado & 0x2;
      if (frente || dorso) {
        glEnable(GL_CULL_FACE);
        glCullFace(frente && dorso ? GL_FRONT_AND_BACK : frente ? GL_FRONT : GL_BACK);
      } else {
        glDisable(GL_CULL_FACE);
      }
      // PA_SU_SC_MODE_CNTL.face: 1 = the front face is clockwise (VK_FRONT_FACE_CLOCKWISE in Vulkan). Inverted
      // here: GL and Vulkan number framebuffer rows the same way for the same NDC, but Vulkan's polygon area
      // carries a minus sign (to compensate for its y going down) and GL's does not, so the same triangle has the
      // opposite winding. Without the inversion every culled draw lost its visible faces.
      glFrontFace((rasterizado & 0x4) ? GL_CCW : GL_CW);
      (rasterizado & 0x10) ? glEnable(GL_POLYGON_OFFSET_FILL) : glDisable(GL_POLYGON_OFFSET_FILL);
      estado_.rasterizado = rasterizado;
    }
    if ((rasterizado & 0x10) && (sesgo[0] != estado_.sesgo[0] || sesgo[1] != estado_.sesgo[1])) {
      glPolygonOffset(sesgo[1], sesgo[0]);  // slope, constant (vkCmdSetDepthBias)
      estado_.sesgo[0] = sesgo[0];
      estado_.sesgo[1] = sesgo[1];
    }
  }

  // --- Texture eviction (ExpulsarTexturasSiHaceFalta in nfsmw_nativo_dibujos.cpp) -------------------------------
  //
  // Without it the cache only grows: the game reloads each zone's packs at new addresses, the address is part of
  // the key, and on a phone the GPU's memory is the system's (the A22 died at 2,147 MB of PSS in a race). The same
  // policy as Vulkan: from 75 % of nfsmw_nativo_texturas_mb_max, a few of the oldest ones unused for 120 frames per
  // frame; over the limit, a batch down to 75 % every 60 frames. A texture the game asks for again is created and
  // uploaded again.

  static constexpr uint64_t kFotogramasSinUsoParaSoltar = 120;
  static constexpr uint32_t kSoltarPorFotograma = 8;

  /*
   * The texture budget, capped by the phone's RAM. On the A22 (4 GB), with 512 MB of textures the Mali driver's
   * own accounting (GL mtrack) settled at ~1,250 MB and the whole process at ~1,970 MB, 150 MB short of the
   * 2,147 MB it was killed at: what is freed goes back to the driver's pool, not to the system, so the driver holds
   * about three times what the cache counts. 256 MB with 4 GB or less, 384 MB up to 6 GB; above that, the cvar.
   */
  uint64_t LimiteTexturas() {
    const int32_t cvar = std::max(REXCVAR_GET(nfsmw_nativo_texturas_mb_max), 0);
    if (ram_mb_ == 0) {
      const long paginas = sysconf(_SC_PHYS_PAGES);
      const long pagina = sysconf(_SC_PAGE_SIZE);
      ram_mb_ = paginas > 0 && pagina > 0 ? uint64_t(paginas) * uint64_t(pagina) >> 20 : UINT64_MAX;
      const int32_t tope = ram_mb_ <= 4608 ? 256 : ram_mb_ <= 6656 ? 384 : cvar;
      limite_mb_ = cvar == 0 ? uint32_t(tope) : uint32_t(std::min(cvar, tope));
      REXLOG_INFO("[nativo] C3 GLES: presupuesto de texturas {} MB (nfsmw_nativo_texturas_mb_max = {}, RAM {} MB)",
                  limite_mb_, cvar, ram_mb_);
    }
    return uint64_t(limite_mb_) << 20;
  }

  void ExpulsarTexturasSiHaceFalta() {
    const uint64_t limite = LimiteTexturas();
    if (!limite || bytes_texturas_ <= limite / 4 * 3) {
      return;
    }
    const bool por_encima = bytes_texturas_ > limite && fotograma_ >= intento_expulsion_ + 60;
    if (por_encima) {
      intento_expulsion_ = fotograma_;
    }
    // The coldest ones first, without sorting the whole cache: candidates gathered in one pass.
    std::vector<std::pair<uint64_t, uint64_t>> frias;  // (last use, key)
    for (const auto& [clave, t] : texturas_) {
      if (t.ultimo_uso + kFotogramasSinUsoParaSoltar < fotograma_) {
        frias.emplace_back(t.ultimo_uso, clave);
      }
    }
    if (frias.empty()) {
      return;
    }
    std::sort(frias.begin(), frias.end());
    const uint64_t objetivo = limite / 4 * 3;
    size_t soltadas = 0;
    uint64_t bytes_soltados = 0;
    for (const auto& [uso, clave] : frias) {
      if (por_encima ? bytes_texturas_ <= objetivo : soltadas >= kSoltarPorFotograma) {
        break;
      }
      auto it = texturas_.find(clave);
      if (it == texturas_.end()) {
        continue;
      }
      glDeleteTextures(1, &it->second.textura);
      bytes_texturas_ -= std::min(bytes_texturas_, it->second.bytes);
      bytes_soltados += it->second.bytes;
      texturas_.erase(it);
      ++soltadas;
    }
    if (soltadas) {
      // A deleted texture that was bound leaves its unit with nothing: the binding cache must not believe otherwise
      // (and the name may come back for another texture).
      estado_.valido = false;
      texturas_soltadas_ += soltadas;
      if (por_encima || (texturas_soltadas_ & 1023) < soltadas) {
        REXLOG_INFO("[nativo] C3 GLES: {} texturas soltadas ({} MB){}; quedan {} ({} MB de {} MB)", soltadas,
                    bytes_soltados >> 20, por_encima ? " por encima del limite" : "", texturas_.size(),
                    bytes_texturas_ >> 20, limite >> 20);
      }
    }
  }

  // --- Textures (PrepararTextura) -------------------------------------------------------------------------------

  // Texture uploads and parameter changes bind on a unit of their own, the last one the driver offers, which no
  // program uses (units 0-47): the bindings the draws keep track of stay as they are.
  void EnlazarParaTrabajar(GLenum objetivo, GLuint textura) {
    glActiveTexture(GL_TEXTURE0 + unidad_trabajo_);
    glBindTexture(objetivo, textura);
  }

  void PrepararTextura(const uint32_t* f, TexturaDeRegistro& salida, MemoTextura& memo) {
    salida = TexturaDeRegistro{};
    if ((f[0] & 0x3) != uint32_t(xenos::FetchConstantType::kTexture)) {
      return;
    }
    const uint32_t dimension = (f[5] >> 9) & 0x3;
    const uint32_t swizzle = (f[3] >> 1) & 0xFFF;
    const uint32_t base = ((f[1] >> 12) << 12) & 0x1FFFFFFF;
    if (dimension == uint32_t(xenos::DataDimension::k2DOrStacked)) {
      if (const ImagenGles* resuelta = contexto_->TexturaResuelta(base)) {
        // Depth (k_24_8): depth in R, distributed by the swizzle, unfiltered. Color: with copy_dest_swap the
        // swap goes into the host channels (the copy is image to image and does not swap them).
        const uint32_t sw = SwizzleGl(swizzle, resuelta->profundidad ? kSwizzleTexRRRR
                                               : resuelta->intercambio_rb ? kSwizzleTexBGRA
                                                                          : kSwizzleTexRGBA);
        if (resuelta->swizzle != sw) {
          EnlazarParaTrabajar(GL_TEXTURE_2D, resuelta->textura);
          PonerSwizzle(GL_TEXTURE_2D, sw);
          resuelta->swizzle = sw;
        }
        memo.swizzle = &resuelta->swizzle;
        memo.swizzle_esperado = sw;
        memo.objetivo = GL_TEXTURE_2D;
        salida.textura = resuelta->textura;
        salida.objetivo = GL_TEXTURE_2D;
        salida.ancho = resuelta->ancho;
        salida.alto = resuelta->alto;
        salida.sampler = SamplerDe(f, resuelta->profundidad, false);
        return;
      }
    }
    DescripcionTextura d;
    if (!DescribirTextura(f, mipmaps_, d)) {
      Avisar(30, "textura no soportada (1D, formato o fuera de memoria): se usa una vacia");
      return;
    }
    // The game's dynamic cubemap: C2 resolves its faces to those same addresses; they are copied to the cube's
    // faces once per frame.
    if (d.dimension == DescripcionTextura::kCubo) {
      std::array<const ImagenGles*, 6> caras{};
      bool resueltas = true;
      for (uint32_t c = 0; c < 6 && resueltas; ++c) {
        caras[c] = contexto_->TexturaResuelta(uint32_t((uint64_t(base) + c * d.zancada_cara) & 0x1FFFFFFF));
        resueltas = caras[c] && !caras[c]->profundidad && caras[c]->ancho >= d.ancho && caras[c]->alto >= d.alto;
      }
      if (resueltas) {
        const uint32_t claves_cubo[5] = {f[0] & 0xFFC003FC, f[1], f[2], (f[4] >> 2) & 0xFF, f[5] >> 9};
        const uint64_t clave = XXH3_64bits(claves_cubo, sizeof(claves_cubo)) ^ 0x9E3779B97F4A7C15ull;
        TexturaGles& t = texturas_[clave];
        if (!t.textura) {
          glGenTextures(1, &t.textura);
          EnlazarParaTrabajar(GL_TEXTURE_CUBE_MAP, t.textura);
          glTexStorage2D(GL_TEXTURE_CUBE_MAP, 1, GL_RGBA8, GLsizei(d.ancho), GLsizei(d.alto));
          t.objetivo = GL_TEXTURE_CUBE_MAP;
          t.ancho = d.ancho;
          t.alto = d.alto;
          t.preparada = true;
          t.bytes = uint64_t(d.ancho) * d.alto * 4 * 6;
          bytes_texturas_ += t.bytes;
        }
        t.ultimo_uso = fotograma_;
        if (t.fotograma != fotograma_) {
          t.fotograma = fotograma_;
          for (uint32_t c = 0; c < 6; ++c) {
            glCopyImageSubData(caras[c]->textura, GL_TEXTURE_2D, 0, 0, 0, 0, t.textura, GL_TEXTURE_CUBE_MAP, 0, 0, 0,
                               GLint(c), GLsizei(d.ancho), GLsizei(d.alto), 1);
          }
          ComprobarGl("al copiar las caras del cubo");
        }
        const uint32_t sw = SwizzleGl(swizzle, caras[0]->intercambio_rb ? kSwizzleTexBGRA : kSwizzleTexRGBA);
        if (t.swizzle != sw) {
          EnlazarParaTrabajar(GL_TEXTURE_CUBE_MAP, t.textura);
          PonerSwizzle(GL_TEXTURE_CUBE_MAP, sw);
          t.swizzle = sw;
        }
        memo.uso = &t.ultimo_uso;
        memo.swizzle = &t.swizzle;
        memo.swizzle_esperado = sw;
        memo.objetivo = GL_TEXTURE_CUBE_MAP;
        salida.textura = t.textura;
        salida.objetivo = GL_TEXTURE_CUBE_MAP;
        salida.ancho = t.ancho;
        salida.alto = t.alto;
        salida.sampler = SamplerDe(f, false, false);
        return;
      }
    }
    const uint32_t claves_textura[5] = {f[0] & 0xFFC003FC, f[1], f[2], (f[4] >> 2) & 0xFF, f[5] >> 9};
    const uint64_t clave = XXH3_64bits(claves_textura, sizeof(claves_textura));
    TexturaGles& t = texturas_[clave];
    GLenum interno = 0, formato = 0, tipo = 0;
    uint32_t bytes_texel = 0;
    bool a_8_bits = false;  // 16-bit unorm without GL_EXT_texture_norm16: only the high byte is kept
    FormatoGl(d.tf.clase, interno, formato, tipo, bytes_texel, a_8_bits);
    // nfsmw_nativo_bc1_16_bits: BC1 at 16 bits per texel, RGB565 until a texel with alpha shows up.
    const bool bc1_16 = bc1_16_ && d.tf.clase == ClaseTextura::kBC1;
    if (bc1_16) {
      interno = t.formato16 == 2 ? GL_RGB5_A1 : GL_RGB565;
      formato = t.formato16 == 2 ? GL_RGBA : GL_RGB;
      tipo = t.formato16 == 2 ? GL_UNSIGNED_SHORT_5_5_5_1 : GL_UNSIGNED_SHORT_5_6_5;
      bytes_texel = 2;
    }
    if (!t.textura) {
      if (!CrearTextura(d, interno, t)) {
        texturas_.erase(clave);
        Avisar(32, "no se pudo crear una textura");
        return;
      }
      if (bc1_16 && !t.formato16) {
        t.formato16 = 1;
      }
      t.bytes = BytesTextura(d, t.niveles, bytes_texel);
      bytes_texturas_ += t.bytes;
    }
    t.ultimo_uso = fotograma_;
    const uint32_t sw = SwizzleGl(swizzle, d.tf.swizzle_host);
    if (t.swizzle != sw) {
      EnlazarParaTrabajar(t.objetivo, t.textura);
      PonerSwizzle(t.objetivo, sw);
      t.swizzle = sw;
    }
    memo.uso = &t.ultimo_uso;
    memo.swizzle = &t.swizzle;
    memo.swizzle_esperado = sw;
    memo.objetivo = t.objetivo;
    salida.textura = t.textura;
    salida.objetivo = t.objetivo;
    salida.ancho = t.ancho;
    salida.alto = t.alto;
    salida.sampler = SamplerDe(f, false, t.flotante32 && !provider_->capacidades().float_linear);
    // Textures that do not change are checked less and less often (down to every 32 frames); those that change
    // (videos) go back to every frame.
    if (t.fotograma == fotograma_ || (t.preparada && fotograma_ < t.siguiente)) {
      return;
    }
    t.fotograma = fotograma_;
    const uint8_t* fisica = memoria_->TranslatePhysical(0);
    uint64_t huella = XXH3_64bits(fisica + d.dir_base, size_t(d.extension));
    if (d.extension_mips) {
      huella = XXH3_64bits_withSeed(fisica + d.dir_mips, size_t(d.extension_mips), huella);
    }
    if (t.preparada && huella == t.huella) {
      t.intervalo = std::min<uint32_t>(t.intervalo * 2, 32);
      t.siguiente = fotograma_ + t.intervalo + (t.intervalo == 32 ? (clave >> 7) & 7 : 0);
      return;
    }
    t.huella = huella;
    t.intervalo = 1;
    t.siguiente = fotograma_ + 1;
    std::array<uint32_t, 16> desplazamientos{};
    if (!LeerDatosTextura(fisica, d, true, datos_, desplazamientos, temporal_)) {
      Avisar(33, "textura fuera de la memoria o BC invalida");
      return;
    }
    if (t.formato16) {
      if (t.formato16 == 1 && HayAlfa(datos_)) {
        // The BC1 uses its 1-bit alpha: its storage is immutable, so it is created again as RGB5_A1.
        glDeleteTextures(1, &t.textura);
        t.textura = 0;
        bytes_texturas_ -= std::min(bytes_texturas_, t.bytes);
        if (!CrearTextura(d, GL_RGB5_A1, t)) {
          texturas_.erase(clave);
          memo = MemoTextura{};
          salida = TexturaDeRegistro{};
          Avisar(32, "no se pudo crear una textura");
          return;
        }
        t.formato16 = 2;
        t.bytes = BytesTextura(d, t.niveles, 2);
        bytes_texturas_ += t.bytes;
        EnlazarParaTrabajar(t.objetivo, t.textura);
        PonerSwizzle(t.objetivo, sw);
        t.swizzle = sw;
        salida.textura = t.textura;
        formato = GL_RGBA;
        tipo = GL_UNSIGNED_SHORT_5_5_5_1;
        // The old name was bound to units the binding cache remembers, and GL may give it out again.
        estado_.valido = false;
        ++bc1_con_alfa_;
      }
      EmpaquetarA16(datos_, t.formato16 == 2 ? Empaquetado16::kRgba5551 : Empaquetado16::kRgb565);
    }
    SubirTextura(d, t, formato, tipo, bytes_texel, a_8_bits, t.formato16 != 0, desplazamientos);
    t.preparada = true;
    ++subidas_textura_;
    bytes_subidos_ += datos_.size();
  }

  // GL format of each class. BC always arrives decoded on the CPU (LeerDatosTextura with bc_a_cpu): RGBA8, R8 for
  // BC4 and RG8 for BC5.
  void FormatoGl(ClaseTextura clase, GLenum& interno, GLenum& formato, GLenum& tipo, uint32_t& bytes_texel,
                 bool& a_8_bits) const {
    const bool norm16 = provider_->capacidades().texture_norm16;
    a_8_bits = false;
    switch (clase) {
      case ClaseTextura::kR8:
      case ClaseTextura::kBC4:
        interno = GL_R8; formato = GL_RED; tipo = GL_UNSIGNED_BYTE; bytes_texel = 1; return;
      case ClaseTextura::kRG8:
      case ClaseTextura::kBC5:
        interno = GL_RG8; formato = GL_RG; tipo = GL_UNSIGNED_BYTE; bytes_texel = 2; return;
      case ClaseTextura::kRGB10A2:
        interno = GL_RGB10_A2; formato = GL_RGBA; tipo = GL_UNSIGNED_INT_2_10_10_10_REV; bytes_texel = 4; return;
      case ClaseTextura::kR16:
        if (norm16) { interno = kR16; formato = GL_RED; tipo = GL_UNSIGNED_SHORT; bytes_texel = 2; return; }
        interno = GL_R8; formato = GL_RED; tipo = GL_UNSIGNED_BYTE; bytes_texel = 1; a_8_bits = true; return;
      case ClaseTextura::kRG16:
        if (norm16) { interno = kRG16; formato = GL_RG; tipo = GL_UNSIGNED_SHORT; bytes_texel = 4; return; }
        interno = GL_RG8; formato = GL_RG; tipo = GL_UNSIGNED_BYTE; bytes_texel = 2; a_8_bits = true; return;
      case ClaseTextura::kRGBA16:
        if (norm16) { interno = kRGBA16; formato = GL_RGBA; tipo = GL_UNSIGNED_SHORT; bytes_texel = 8; return; }
        interno = GL_RGBA8; formato = GL_RGBA; tipo = GL_UNSIGNED_BYTE; bytes_texel = 4; a_8_bits = true; return;
      case ClaseTextura::kR16F:
        interno = GL_R16F; formato = GL_RED; tipo = GL_HALF_FLOAT; bytes_texel = 2; return;
      case ClaseTextura::kRG16F:
        interno = GL_RG16F; formato = GL_RG; tipo = GL_HALF_FLOAT; bytes_texel = 4; return;
      case ClaseTextura::kRGBA16F:
        interno = GL_RGBA16F; formato = GL_RGBA; tipo = GL_HALF_FLOAT; bytes_texel = 8; return;
      case ClaseTextura::kR32F:
        interno = GL_R32F; formato = GL_RED; tipo = GL_FLOAT; bytes_texel = 4; return;
      case ClaseTextura::kRG32F:
        interno = GL_RG32F; formato = GL_RG; tipo = GL_FLOAT; bytes_texel = 8; return;
      case ClaseTextura::kRGBA32F:
        interno = GL_RGBA32F; formato = GL_RGBA; tipo = GL_FLOAT; bytes_texel = 16; return;
      default:  // kRGBA8, kBC1-3
        interno = GL_RGBA8; formato = GL_RGBA; tipo = GL_UNSIGNED_BYTE; bytes_texel = 4; return;
    }
  }

  bool CrearTextura(const DescripcionTextura& d, GLenum interno, TexturaGles& t) {
    ComprobarGl("antes de crear una textura");
    glGenTextures(1, &t.textura);
    t.objetivo = d.dimension == DescripcionTextura::kCubo ? GL_TEXTURE_CUBE_MAP
                 : d.dimension == DescripcionTextura::k3D ? GL_TEXTURE_3D
                                                          : GL_TEXTURE_2D;
    EnlazarParaTrabajar(t.objetivo, t.textura);
    if (t.objetivo == GL_TEXTURE_3D) {
      glTexStorage3D(GL_TEXTURE_3D, 1, interno, GLsizei(d.ancho_host), GLsizei(d.alto_host), GLsizei(d.fondo));
    } else {
      glTexStorage2D(t.objetivo, GLsizei(d.niveles), interno, GLsizei(d.ancho_host), GLsizei(d.alto_host));
    }
    if (glGetError() != GL_NO_ERROR) {
      glDeleteTextures(1, &t.textura);
      t.textura = 0;
      return false;
    }
    t.ancho = d.ancho_host;
    t.alto = d.alto_host;
    t.niveles = t.objetivo == GL_TEXTURE_3D ? 1 : d.niveles;
    t.flotante32 = interno == GL_R32F || interno == GL_RG32F || interno == GL_RGBA32F;
    if (texturas_.size() <= 48) {
      REXLOG_INFO("[nativo] C3 GLES: textura {:08X} {}x{}{} formato {} {}; niveles {}", uint32_t(d.dir_base), d.ancho,
                  d.alto, d.fondo ? fmt::format("x{}", d.fondo) : std::string(), d.formato,
                  d.dimension == DescripcionTextura::kCubo ? "(cubo)" : d.dimension == DescripcionTextura::k3D ? "(3D)" : "",
                  t.niveles);
    }
    return true;
  }

  // What a texture of `niveles` levels takes on the GPU at `bytes_texel` (for the eviction budget).
  static uint64_t BytesTextura(const DescripcionTextura& d, uint32_t niveles, uint32_t bytes_texel) {
    uint64_t bytes = 0;
    for (uint32_t n = 0; n < niveles; ++n) {
      bytes += uint64_t(std::max(d.ancho_host >> n, 1u)) * std::max(d.alto_host >> n, 1u) * bytes_texel * d.capas *
               (d.fondo ? d.fondo : 1);
    }
    return bytes;
  }

  // `empaquetado16`: the data was RGBA8 packed to 16 bits (EmpaquetarA16), so the level offsets halve.
  void SubirTextura(const DescripcionTextura& d, TexturaGles& t, GLenum formato, GLenum tipo, uint32_t bytes_texel,
                    bool a_8_bits, bool empaquetado16, const std::array<uint32_t, 16>& desplazamientos) {
    if (a_8_bits) {
      // 16-bit unorm to 8 bits: the high byte of each little-endian unit (the data is already in host order).
      for (size_t i = 0, j = 0; i + 1 < datos_.size(); i += 2, ++j) {
        datos_[j] = datos_[i + 1];
      }
      datos_.resize(datos_.size() / 2);
    }
    EnlazarParaTrabajar(t.objetivo, t.textura);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const uint32_t divisor = a_8_bits || empaquetado16 ? 2 : 1;
    if (t.objetivo == GL_TEXTURE_3D) {
      glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, GLsizei(d.ancho_host), GLsizei(d.alto_host), GLsizei(d.fondo), formato,
                      tipo, datos_.data());
      return;
    }
    for (uint32_t n = 0; n < t.niveles; ++n) {
      const uint32_t w = std::max(d.ancho_host >> n, 1u);
      const uint32_t h = std::max(d.alto_host >> n, 1u);
      const size_t bytes_capa = size_t(w) * h * bytes_texel;
      const uint8_t* nivel = datos_.data() + desplazamientos[n] / divisor;
      if (t.objetivo == GL_TEXTURE_CUBE_MAP) {
        for (uint32_t c = 0; c < 6; ++c) {
          glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + c, GLint(n), 0, 0, GLsizei(w), GLsizei(h), formato, tipo,
                          nivel + bytes_capa * c);
        }
      } else {
        glTexSubImage2D(GL_TEXTURE_2D, GLint(n), 0, 0, GLsizei(w), GLsizei(h), formato, tipo, nivel);
      }
    }
  }

  // RanuraSampler: filters, addressing, mip levels and border from the fetch constant. `puntual`: no filtering
  // (depth textures); `sin_lineal`: 32-bit floats without GL_OES_texture_float_linear.
  GLuint SamplerDe(const uint32_t* f, bool puntual, bool sin_lineal) {
    const uint32_t campo_filtros = (f[3] >> 19) & 0xFFF;  // mag in 0-1, min in 2-3, mip in 4-5
    const uint32_t campo_mips = (f[4] >> 2) & 0xFF;       // minimum in 0-3, maximum in 4-7
    const bool sin_filtro = puntual || sin_lineal;
    const uint32_t clave = ((f[0] >> 10) & 0x1FF) | (campo_filtros << 9) | (campo_mips << 21) | ((f[5] & 0x3) << 29) |
                           (sin_filtro ? 0x80000000u : 0u);
    if (const auto it = samplers_.find(clave); it != samplers_.end()) {
      return it->second;
    }
    const auto& c = provider_->capacidades();
    const auto modo = [&](uint32_t valor) -> GLint {
      switch (valor & 0x7) {
        case 0: return GL_REPEAT;
        case 1: return GL_MIRRORED_REPEAT;
        case 2:
        case 4: return GL_CLAMP_TO_EDGE;
        case 6: return c.version >= 32 ? GLint(kClampToBorder) : GL_CLAMP_TO_EDGE;
        default: return c.mirror_clamp ? GLint(kMirrorClampToEdge) : GL_CLAMP_TO_EDGE;
      }
    };
    GLuint sampler = 0;
    glGenSamplers(1, &sampler);
    const bool mag_lineal = ((f[3] >> 19) & 0x3) == 1 && !sin_filtro;
    const bool min_lineal = ((f[3] >> 21) & 0x3) == 1 && !sin_filtro;
    const uint32_t mip = (campo_filtros >> 4) & 0x3;
    const bool mip_lineal = mip == 1 && !sin_filtro;
    glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, mag_lineal ? GL_LINEAR : GL_NEAREST);
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER,
                        min_lineal ? (mip_lineal ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST)
                                   : (mip_lineal ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST));
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, modo(f[0] >> 10));
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, modo(f[0] >> 13));
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_R, modo(f[0] >> 16));
    const float lod_min = float(campo_mips & 0xF);
    const float lod_max = mip == 2 ? lod_min + 0.25f : std::max(lod_min, float((f[4] >> 6) & 0xF));
    glSamplerParameterf(sampler, GL_TEXTURE_MIN_LOD, lod_min);
    glSamplerParameterf(sampler, GL_TEXTURE_MAX_LOD, lod_max);
    if (c.version >= 32) {
      const float blanco[4] = {1, 1, 1, 1};
      const float negro[4] = {0, 0, 0, 0};
      glSamplerParameterfv(sampler, 0x1004 /* GL_TEXTURE_BORDER_COLOR */, (f[5] & 0x3) == 1 ? blanco : negro);
    }
    if (anisotropico_ > 1 && c.anisotropia_max > 1.0f && min_lineal && mip_lineal && lod_max > lod_min) {
      glSamplerParameterf(sampler, kAnisotropia, std::min(float(anisotropico_), c.anisotropia_max));
    }
    samplers_.emplace(clave, sampler);
    return sampler;
  }

  const GlesProvider* provider_;
  rex::memory::Memory* memoria_;
  ContextoDestinosGles* contexto_;
  gles::OpcionesGlsl opciones_glsl_;
  bool indexado_ = true;  // GLES 3.2: per-target blend and masks
  bool base_vertice_ = false;  // GLES 3.2 and nfsmw_nativo_gles_base_vertice
  CacheProgramas cache_;
  BuferFlujo flujo_;
  uint32_t alineacion_ubo_ = 256;
  GLuint vao_ = 0;
  std::array<GLuint, 3> vacias_{};
  GLuint sampler_vacio_ = 0;  // 0: the texture's own (nearest) parameters
  uint32_t unidad_trabajo_ = 47;
  EstadoAplicado estado_;
  uint32_t ubicaciones_activas_ = 0;

  // Settings read once per frame.
  bool sin_desenfoque_ = true;
  bool pcf_barato_ = false;
  bool mipmaps_ = true;
  int32_t anisotropico_ = 0;
  int32_t sombras_sesgo_pendiente_ = 0;
  int32_t sombras_sesgo_constante_ = 0;
  bool dedupe_ = true;
  bool sin_vegetacion_ = true;
  bool saltar_invisibles_ = true;
  bool z_temprana_ = true;
  bool memo_activo_ = true;
  uint64_t fotograma_ = 0;

  // Vertex input.
  std::unordered_map<uint64_t, EntradaGles> entradas_;
  const EntradaGles* entrada_actual_ = nullptr;
  uint64_t entrada_generacion_ = UINT64_MAX;
  const EntradaShader* entrada_vs_ = nullptr;
  std::vector<uint32_t> indices_;
  std::vector<uint32_t> convertidos_;
  std::vector<uint16_t> indices16_;
  std::vector<uint16_t> indices_prueba_;
  int32_t indices_neon_ = -1;
  uint64_t indices_neon_dibujos_ = 0;
  const EntradaGles* entrada_atributos_ = nullptr;  // the attribute pointers in effect
  std::array<GLintptr, 16> bases_atributos_{};
  // Vertex copies of this frame (nfsmw_nativo_dedupe_vertices): guest range -> place in the stream buffer.
  struct CopiaVertices {
    GLintptr offset = 0;
    uint64_t segmento = 0;
  };
  std::unordered_map<uint64_t, CopiaVertices> copias_;
  uint32_t sincronizacion_copias_ = 0;
  // The pass (the framebuffer the draws go to) and the deferred sky.
  GLuint fbo_pase_ = 0;
  uint64_t dibujos_en_pase_ = 0;
  bool cielo_aplazado_ = true;
  bool cielo_pendiente_ = false;
  DibujoGles cielo_;
  uint64_t cielo_segmento_ = 0;
  uint64_t cielo_al_aplazar_ = 0;
  uint32_t cielo_guardia_ = kCieloMirando;
  uint32_t cielo_guardia_fotogramas_ = 0;
  uint32_t cielo_guardia_con_uno_ = 0;
  uint32_t cielo_guardia_en_fotograma_ = 0;
  uint32_t cielo_guardia_max_ = 0;
  uint64_t cielo_vistos_ = 0;
  uint64_t cielo_aplazados_ = 0;
  uint64_t cielo_perdidos_ = 0;
  std::array<uint64_t, kCieloMotivos> cielo_emitidos_{};
  uint64_t cielo_posicion_suma_ = 0;
  // nfsmw_nativo_gles_etapas.
  bool cronometrar_ = false;
  uint64_t marca_ = 0;
  std::array<uint64_t, kEtapas> etapas_ns_{};
  uint64_t fotogramas_cronometrados_ = 0;
  uint64_t dibujados_al_cronometrar_ = 0;
  // nfsmw_reflejo_visibilidad.
  struct ConsultaVisibilidad {
    GLuint consulta = 0;
    bool testigo = false;
  };
  bool medir_visibilidad_ = false;
  bool testigo_pendiente_ = false;
  uint64_t testigo_siguiente_ = 0;
  std::vector<ConsultaVisibilidad> visibilidad_pendientes_;
  std::vector<GLuint> visibilidad_libres_;
  // Texture registers (nfsmw_nativo_gles_memo_texturas).
  std::array<MemoTextura, 16> memo_texturas_{};
  uint64_t memo_generacion_ = 0;

  // Constants in the stream buffer.
  uint64_t constantes_vs_generacion_ = UINT64_MAX;
  uint64_t constantes_vs_segmento_ = 0;
  GLintptr constantes_vs_offset_ = 0;
  uint64_t constantes_ps_generacion_ = UINT64_MAX;
  uint64_t constantes_ps_segmento_ = 0;
  GLintptr constantes_ps_offset_ = 0;
  uint32_t compartidas_previas_[kPalabrasCompartidas] = {};
  bool compartidas_validas_ = false;
  uint64_t compartidas_segmento_ = 0;
  GLintptr compartidas_offset_ = 0;

  // Programs, framebuffers, textures and samplers.
  std::unordered_map<uint64_t, gles::ShaderGlsl> glsl_;
  std::unordered_map<uint64_t, GLuint> shaders_gl_;
  std::unordered_map<uint64_t, ProgramaGles> programas_;
  std::unordered_map<uint64_t, FramebufferGles> fbos_;
  std::unordered_map<uint64_t, TexturaGles> texturas_;
  std::unordered_map<uint32_t, GLuint> samplers_;
  std::unordered_map<const EntradaShader*, uint64_t> huellas_spirv_;
  std::vector<uint8_t> binario_;
  std::vector<uint8_t> datos_;
  std::vector<uint8_t> temporal_;
  uint64_t bytes_texturas_ = 0;
  uint64_t ram_mb_ = 0;
  uint32_t limite_mb_ = 0;
  uint64_t intento_expulsion_ = 0;
  uint64_t texturas_soltadas_ = 0;

  // Counters.
  uint64_t dibujados_ = 0;
  uint64_t rechazados_ = 0;
  uint64_t subidas_textura_ = 0;
  uint64_t bytes_subidos_ = 0;
  uint64_t bytes_vertices_ = 0;
  uint64_t bytes_indices_ = 0;
  double ms_programas_ = 0.0;
  uint64_t programas_disco_ = 0;
  uint64_t programas_disco_fallidos_ = 0;
  uint64_t programas_compilados_ = 0;
  uint64_t saltados_vegetacion_ = 0;
  uint64_t saltados_invisibles_ = 0;
  uint64_t dibujos_z_temprana_ = 0;
  uint64_t dibujos_contados_ = 0;
  uint64_t aciertos_dedupe_ = 0;
  uint64_t bytes_dedupe_ = 0;
  uint64_t aciertos_memo_ = 0;
  uint64_t consultas_visibilidad_ = 0;
  bool bc1_16_ = true;  // nfsmw_nativo_bc1_16_bits (kInitOnly: read with the settings)
  uint64_t bc1_con_alfa_ = 0;
  uint64_t reflejos_ocultos_ = 0;
  uint32_t errores_gl_ = 0;
  std::unordered_map<uint32_t, uint64_t> causas_;
  std::unordered_set<uint32_t> avisados_;
};

}  // namespace

std::unique_ptr<DibujosGles> DibujosGles::Crear(const rex::ui::gles::GlesProvider* provider,
                                                rex::memory::Memory* memoria, ContextoDestinosGles* contexto) {
  if (!provider || !memoria || !contexto) {
    return nullptr;
  }
  auto dibujos = std::make_unique<DibujosGlesImpl>(provider, memoria, contexto);
  if (!dibujos->Inicializar()) {
    return nullptr;
  }
  return dibujos;
}

}  // namespace nfsmw::nativo
