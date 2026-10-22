// nfsmw - native renderer, part C2 on OpenGL ES (see the header).
//
// How it maps to the Vulkan version (nfsmw_nativo_destinos.cpp):
//   - A render target is a GL_RGBA8 or GL_DEPTH24_STENCIL8 texture per (base, format, pitch), as tall as the
//     Vulkan one. A resolved texture is another one per destination address.
//   - The copy is glCopyImageSubData, or a linear glBlitFramebuffer when the scene is larger than the texture
//     (internal resolution above the front buffer's), the same cases as vkCmdCopyImage and vkCmdBlitImage.
//   - Clears go through glClearBuffer on a scratch framebuffer. glClearBuffer obeys the color, depth and
//     stencil masks and the scissor, which the draws change: they are reset before every clear.
//   - Rows keep the D3D orientation: texture row 0 is the top of the image, and GL numbers framebuffer rows
//     the same way as Vulkan for the same NDC (see nfsmw_gles_shaders.h). Copy and blit rectangles are the
//     Vulkan ones as they are; only the presenter flips the final image.
//   - Small resolved textures (exposure) are read back with a pixel pack buffer and a fence, and written to
//     guest memory when the fence has signaled (EscribirLecturas), without waiting for the GPU.
//   - The output applies the game's gamma ramp and the post-processing with the same shader as
//     nfsmw_salida_rampa_gamma.frag (without FXAA yet).

#include "nfsmw_nativo_destinos_gles.h"

#include "nfsmw_ajustes_graficos.h"
#include "nfsmw_nativo_copias_comun.h"
#include "nfsmw_nativo_dibujos_gles.h"
#include "nfsmw_reflejo_demanda.h"

#include <EGL/egl.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl32.h>

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/ui/gles/presenter.h>
#include <rex/ui/gles/provider.h>
#include <rex/ui/presenter.h>

#include <algorithm>
#include <chrono>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

REXCVAR_DECLARE(bool, nfsmw_nativo_diag_borrado);
REXCVAR_DECLARE(bool, nfsmw_nativo_rampa_gamma);
REXCVAR_DECLARE(int32_t, nfsmw_nativo_leer_resueltas_texels);
REXCVAR_DECLARE(int32_t, nfsmw_nativo_lecturas_cada);

REXCVAR_DECLARE(bool, nfsmw_nativo_gles_etapas);

namespace nfsmw::nativo {
namespace {

// nfsmw_nativo_gles_etapas: adds the time of a scope to a counter (nothing without one).
struct Cronometro {
  uint64_t* destino;
  std::chrono::steady_clock::time_point inicio;
  explicit Cronometro(uint64_t* d) : destino(d), inicio(d ? std::chrono::steady_clock::now() : decltype(inicio){}) {}
  ~Cronometro() {
    if (destino) {
      *destino += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - inicio)
                               .count());
    }
  }
};

namespace xenos = rex::graphics::xenos;
using rex::ui::gles::GlesPresenter;

constexpr uint32_t kAltoMaximoDestino = 2048;

struct ResueltaGles {
  ImagenGles imagen;
  uint32_t formato_guest = 0;
  bool intercambio_rb = false;
};

// A read-back in flight: its pack buffer, the fence after the glReadPixels and where it goes in the guest.
struct LecturaGles {
  GLuint pbo = 0;
  uint32_t capacidad = 0;
  GLsync valla = nullptr;
  uint32_t base = 0;  // RB_COPY_DEST_BASE
  int32_t x0 = 0;
  int32_t y0 = 0;
  uint32_t ancho = 0;
  uint32_t alto = 0;
  uint32_t pitch = 0;
  uint32_t alto_destino = 0;
  uint32_t info = 0;  // RB_COPY_DEST_INFO
};

// Full-screen triangle; the output keeps the source's rows (both in the D3D orientation).
constexpr const char* kVerticesSalida = R"(#version 310 es
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// nfsmw_salida_rampa_gamma.frag. The specialization constants are uniforms here.
constexpr const char* kPixelesSalida = R"(#version 310 es
precision highp float;
precision highp int;
uniform highp sampler2D origen;
uniform bool exacta;
uniform bool graduacion;
uniform bool fxaa;
uniform ivec2 desplazamiento;
uniform vec2 inverso;
layout(std140, binding = 0) uniform Rampa {
  vec4 entradas[256];
  vec4 mezcla;   // saturacion, vibracion, 1 / gamma
  vec4 efectos;  // vineta, lineas
} rampa;
layout(location = 0) out vec4 color;
const vec3 kPesosLuma = vec3(0.299, 0.587, 0.114);
float Luma(vec2 uv) {
  return dot(textureLod(origen, uv, 0.0).rgb, kPesosLuma);
}
// FXAA at normal quality, like the SDK's (edge threshold 0.166, minimum 0.0833 and an 8-texel reach).
vec3 Fxaa(vec2 uv) {
  vec2 paso = 1.0 / vec2(textureSize(origen, 0));
  vec3 rgb_centro = textureLod(origen, uv, 0.0).rgb;
  float luma_centro = dot(rgb_centro, kPesosLuma);
  float luma_no = Luma(uv + vec2(-paso.x, -paso.y));
  float luma_ne = Luma(uv + vec2(paso.x, -paso.y));
  float luma_so = Luma(uv + vec2(-paso.x, paso.y));
  float luma_se = Luma(uv + vec2(paso.x, paso.y));
  float luma_min = min(luma_centro, min(min(luma_no, luma_ne), min(luma_so, luma_se)));
  float luma_max = max(luma_centro, max(max(luma_no, luma_ne), max(luma_so, luma_se)));
  if (luma_max - luma_min < max(0.0833, luma_max * 0.166)) {
    return rgb_centro;
  }
  vec2 direccion = vec2(-((luma_no + luma_ne) - (luma_so + luma_se)), (luma_no + luma_so) - (luma_ne + luma_se));
  float reduccion = max((luma_no + luma_ne + luma_so + luma_se) * (0.25 * 0.125), 1.0 / 128.0);
  float inversa_min = 1.0 / (min(abs(direccion.x), abs(direccion.y)) + reduccion);
  direccion = clamp(direccion * inversa_min, vec2(-8.0), vec2(8.0)) * paso;
  vec3 rgb_a = 0.5 * (textureLod(origen, uv + direccion * (1.0 / 3.0 - 0.5), 0.0).rgb +
                      textureLod(origen, uv + direccion * (2.0 / 3.0 - 0.5), 0.0).rgb);
  vec3 rgb_b = rgb_a * 0.5 + 0.25 * (textureLod(origen, uv + direccion * -0.5, 0.0).rgb +
                                     textureLod(origen, uv + direccion * 0.5, 0.0).rgb);
  float luma_b = dot(rgb_b, kPesosLuma);
  return (luma_b < luma_min || luma_b > luma_max) ? rgb_a : rgb_b;
}
void main() {
  ivec2 pixel = ivec2(gl_FragCoord.xy) - desplazamiento;
  vec3 texel;
  if (fxaa) {
    texel = Fxaa((vec2(uvec2(pixel)) + 0.5) * inverso);
  } else if (exacta) {
    texel = texelFetch(origen, pixel, 0).rgb;
  } else {
    texel = textureLod(origen, (vec2(uvec2(pixel)) + 0.5) * inverso, 0.0).rgb;
  }
  uvec3 i = uvec3(texel * 255.0 + 0.5);
  vec3 c = vec3(rampa.entradas[i.r].r, rampa.entradas[i.g].g, rampa.entradas[i.b].b);
  if (graduacion) {
    float luma = dot(max(c, 0.0), vec3(0.2126, 0.7152, 0.0722));
    c = mix(vec3(luma), c, rampa.mezcla.x);
    float saturado = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
    c = mix(vec3(luma), c, 1.0 + rampa.mezcla.y * (1.0 - clamp(saturado, 0.0, 1.0)));
    c = pow(max(c, vec3(0.0001)), vec3(rampa.mezcla.z));
    vec2 uv = (vec2(uvec2(pixel)) + 0.5) * inverso;
    float v = rampa.efectos.x;
    c *= (1.0 - v * max(0.0, 1.0 - uv.y / 0.42)) * (1.0 - v * max(0.0, 1.0 - (1.0 - uv.y) / 0.42)) *
         (1.0 - v * max(0.0, 1.0 - uv.x / 0.42)) * (1.0 - v * max(0.0, 1.0 - (1.0 - uv.x) / 0.42));
    if ((uint(pixel.y) % 3u) == 0u) {
      c *= 1.0 - rampa.efectos.y * 0.7;
    }
    c = clamp(c, 0.0, 1.0);
  }
  color = vec4(c, 1.0);
}
)";

GLuint CompilarShader(GLenum tipo, const char* fuente) {
  GLuint shader = glCreateShader(tipo);
  glShaderSource(shader, 1, &fuente, nullptr);
  glCompileShader(shader);
  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (ok != GL_TRUE) {
    char registro[2048] = {};
    glGetShaderInfoLog(shader, sizeof(registro) - 1, nullptr, registro);
    REXLOG_ERROR("[nativo] C2 GLES: el shader de salida no compila: {}", registro);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

class DestinosGles final : public DestinosNativos, public ContextoDestinosGles {
 public:
  DestinosGles(const rex::ui::gles::GlesProvider* provider, rex::memory::Memory* memoria)
      : provider_(provider), memoria_(memoria) {}

  ~DestinosGles() override {
    // On the ring thread with its context current (SistemaGraficoNativo destroys it there).
    dibujos_.reset();  // its framebuffers point to these images
    for (auto& [clave, imagen] : destinos_) Destruir(imagen);
    for (auto& [clave, imagen] : profundidades_) Destruir(imagen);
    for (auto& [clave, resuelta] : resueltas_) Destruir(resuelta.imagen);
    for (LecturaGles& l : lecturas_pendientes_) DestruirLectura(l);
    for (LecturaGles& l : lecturas_libres_) DestruirLectura(l);
    for (FramebufferSalida& f : framebuffers_salida_) {
      if (f.fbo) glDeleteFramebuffers(1, &f.fbo);
    }
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (programa_) glDeleteProgram(programa_);
    if (ubo_rampa_) glDeleteBuffers(1, &ubo_rampa_);
    if (sampler_) glDeleteSamplers(1, &sampler_);
    if (consulta_activa_.consulta) {
      glEndQuery(GL_ANY_SAMPLES_PASSED);
      consultas_pendientes_.push_back(consulta_activa_);
    }
    for (const ConsultaOclusionGles& c : consultas_pendientes_) {
      consultas_libres_.push_back(c.consulta);
      if (c.contador) contadores_libres_.push_back(c.contador);
    }
    if (!contadores_libres_.empty()) glDeleteBuffers(GLsizei(contadores_libres_.size()), contadores_libres_.data());
    if (tiempo_activo_) {
      glEndQuery(kTiempoTranscurrido);
      tiempos_libres_.push_back(tiempo_activo_);
    }
    tiempos_libres_.insert(tiempos_libres_.end(), tiempos_pendientes_.begin(), tiempos_pendientes_.end());
    if (!consultas_libres_.empty()) glDeleteQueries(GLsizei(consultas_libres_.size()), consultas_libres_.data());
    if (!tiempos_libres_.empty()) glDeleteQueries(GLsizei(tiempos_libres_.size()), tiempos_libres_.data());
  }

  bool Inicializar() {
    {
      // Sample counters for the occlusion queries: atomic counters in the fragment stage (optional in GLES 3.1).
      GLint contadores = 0, buferes = 0;
      glGetIntegerv(GL_MAX_FRAGMENT_ATOMIC_COUNTERS, &contadores);
      glGetIntegerv(GL_MAX_FRAGMENT_ATOMIC_COUNTER_BUFFERS, &buferes);
      contadores_ = contadores > 0 && buferes > 0;
      REXLOG_INFO("[nativo] C2 GLES oclusion: {}", contadores_ ? "cuenta las muestras con contadores atomicos"
                                                              : "solo si o no (el driver no tiene contadores atomicos "
                                                                "en los pixel shaders)");
    }
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &ubo_rampa_);
    glBindBuffer(GL_UNIFORM_BUFFER, ubo_rampa_);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(rampa_valores_), nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    glGenSamplers(1, &sampler_);
    glSamplerParameteri(sampler_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(sampler_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(sampler_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(sampler_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const GLuint vs = CompilarShader(GL_VERTEX_SHADER, kVerticesSalida);
    const GLuint ps = CompilarShader(GL_FRAGMENT_SHADER, kPixelesSalida);
    if (!vs || !ps) {
      glDeleteShader(vs);
      glDeleteShader(ps);
      return false;
    }
    programa_ = glCreateProgram();
    glAttachShader(programa_, vs);
    glAttachShader(programa_, ps);
    glLinkProgram(programa_);
    glDeleteShader(vs);
    glDeleteShader(ps);
    GLint ok = GL_FALSE;
    glGetProgramiv(programa_, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
      char registro[2048] = {};
      glGetProgramInfoLog(programa_, sizeof(registro) - 1, nullptr, registro);
      REXLOG_ERROR("[nativo] C2 GLES: el programa de salida no enlaza: {}", registro);
      return false;
    }
    uniforme_exacta_ = glGetUniformLocation(programa_, "exacta");
    uniforme_graduacion_ = glGetUniformLocation(programa_, "graduacion");
    uniforme_fxaa_ = glGetUniformLocation(programa_, "fxaa");
    uniforme_desplazamiento_ = glGetUniformLocation(programa_, "desplazamiento");
    uniforme_inverso_ = glGetUniformLocation(programa_, "inverso");
    glUseProgram(programa_);
    glUniform1i(glGetUniformLocation(programa_, "origen"), 0);
    glUseProgram(0);
    rampa_gamma_ = REXCVAR_GET(nfsmw_nativo_rampa_gamma);
    for (uint32_t i = 0; i < 256; ++i) {
      for (uint32_t c = 0; c < 3; ++c) {
        rampa_juego_[i][c] = uint16_t((i << 2) | (i >> 6));  // identity until the game loads its ramp
      }
    }
    RecalcularRampa();
    if (const GLenum error = glGetError(); error != GL_NO_ERROR) {
      REXLOG_ERROR("[nativo] C2 GLES: error de GL al inicializar ({:#x})", error);
      return false;
    }
    // Parts C3-C6. Without them the copies and the output still work (the screen shows the clears).
    dibujos_ = DibujosGles::Crear(provider_, memoria_, this);
    if (!dibujos_) {
      REXLOG_ERROR("[nativo] C2 GLES: no se pudieron crear los dibujos (C3-C6): solo copias y borrados");
    }
    REXLOG_INFO("[nativo] C2 GLES: destinos de render sobre OpenGL ES {}.{} ({}); rampa de gamma {}",
                provider_->capacidades().version / 10, provider_->capacidades().version % 10,
                provider_->capacidades().renderer, rampa_gamma_ ? "SI" : "no");
    return true;
  }

  // --- DestinosNativos -------------------------------------------------------------------------------------

  bool Copiar(const RegistrosCopia& reg) override {
    Cronometro cronometro(cronometrar_ ? &ns_copias_ : nullptr);
    ++copias_cronometradas_;
    // The draws' pass ends here (the deferred sky goes out before the copy reads the target), and copies and
    // clears bind other framebuffers and open the masks and the scissor.
    if (dibujos_) {
      dibujos_->TerminarPase();
      dibujos_->EstadoPerdido();
    }
    const uint32_t control = reg.rb_copy_control;
    const uint32_t origen = control & 0x7;
    const bool borrar_color = (control >> 8) & 0x1;
    const uint32_t comando = (control >> 20) & 0x3;
    const bool copiar =
        comando == uint32_t(xenos::CopyCommand::kRaw) || comando == uint32_t(xenos::CopyCommand::kConvert);
    const uint32_t pitch = reg.rb_surface_info & 0x3FFF;
    const uint32_t msaa = (reg.rb_surface_info >> 16) & 0x3;
    if (msaa != uint32_t(xenos::MsaaSamples::k1X) && avisados_.insert(1).second) {
      REXLOG_INFO("[nativo] C2 GLES: destino con MSAA: se usa con 1 muestra");
    }
    if (origen >= xenos::kMaxColorRenderTargets) {
      // From depth: the copy goes to a resolved texture with the host depth format. With a depth source no
      // color is cleared (IsClearingColor, graphics/util/draw.h:534-538).
      const bool borrara = (control >> 9) & 0x1;
      if (copiar) {
        CopiarProfundidad(reg, pitch);
      }
      if (borrara) {
        BorrarProfundidad(reg, pitch);
      }
      return true;
    }
    const uint32_t info_color = reg.rb_color_info[origen];
    const uint32_t formato_color = (info_color >> 16) & 0xF;
    if (formato_color != uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8) &&
        formato_color != uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)) {
      return Rechazar(100 + formato_color, "formato de destino de render todavia no soportado");
    }
    int32_t x0, y0, x1, y1;
    if (!Rectangulo(reg, pitch, x0, y0, x1, y1)) {
      return false;
    }
    ImagenGles* destino_render = ObtenerDestino(info_color & 0xFFF, formato_color, pitch);
    if (!destino_render) {
      return false;
    }
    Preparar(*destino_render);
    if (copiar) {
      const uint32_t info_destino = reg.rb_copy_dest_info;
      const uint32_t formato_destino = (info_destino >> 7) & 0x3F;
      if ((info_destino >> 3) & 0x1) {
        Rechazar(3, "copia a textura 3D o array: todavia no");
      } else if (formato_destino != uint32_t(xenos::ColorFormat::k_8_8_8_8) &&
                 formato_destino != uint32_t(xenos::ColorFormat::k_8_8_8_8_A) &&
                 formato_destino != uint32_t(xenos::ColorFormat::k_8_8_8_8_AS_16_16_16_16)) {
        Rechazar(200 + formato_destino, "formato de copia todavia no soportado");
      } else {
        CopiarColor(reg, *destino_render, x0, y0, x1, y1, info_destino, formato_destino);
      }
    }
    if (borrar_color) {
      std::array<float, 4> color{};
      if (REXCVAR_GET(nfsmw_nativo_diag_borrado)) {
        color = ColorDiagnosticoRgba(info_color & 0xFFF, formato_color, pitch);
      } else {
        const uint64_t valor = uint64_t(reg.rb_color_clear) | (uint64_t(reg.rb_color_clear_lo) << 32);
        for (uint32_t j = 0; j < 4; ++j) {
          color[j] = float((valor >> (j * 8)) & 0xFF) * (1.0f / 255.0f);
        }
      }
      BorrarColor(*destino_render, color.data());
      ++borrados_;
    }
    if ((control >> 9) & 0x1) {
      BorrarProfundidad(reg, pitch);
    }
    return true;
  }

  bool Presentar(rex::ui::Presenter* presentador, const TexturaSwap& swap, uint32_t ancho,
                 uint32_t alto) override {
    if (dibujos_) {
      dibujos_->TerminarPase();  // before anything reads the frame or measures it
    }
    InformeEtapas();
    Cronometro cronometro(cronometrar_ ? &ns_swaps_ : nullptr);
    nfsmw::reflejo_demanda::AnotarSwap();  // nfsmw_reflejo_bajo_demanda
    EscribirLecturas(false);
    MedirGpu();
    if (dibujos_) {
      dibujos_->EstadoPerdido();  // the output pass and the presenter's paint change GL state
      dibujos_->FinDeFotograma();
    }
    if (!presentador) {
      return false;
    }
    const uint32_t base = (swap.dword[1] & 0xFFFFF000) & 0x1FFFFFFF;
    auto it = resueltas_.find(base);
    if (it == resueltas_.end() || !it->second.imagen.preparada) {
      Rechazar(4, "Swap sin textura resuelta: se pinta el color de prueba");
      return false;
    }
    ResueltaGles& resuelta = it->second;
    const uint32_t w = std::min(ancho ? ancho : resuelta.imagen.ancho, resuelta.imagen.ancho);
    const uint32_t h = std::min(alto ? alto : resuelta.imagen.alto, resuelta.imagen.alto);
    // The display proportion: 16:9, or the front buffer's own when it is wider (nfsmw_proporcion_pantalla).
    const bool mas_ancha = uint64_t(w) * 9 > uint64_t(h) * 16 + h;
    bool pintado = false;
    presentador->RefreshGuestOutput(w, h, mas_ancha ? w : 1280, mas_ancha ? h : 720,
                                    [&](rex::ui::Presenter::GuestOutputRefreshContext& base_contexto) {
                                      auto& contexto =
                                          static_cast<GlesPresenter::GlesGuestOutputRefreshContext&>(base_contexto);
                                      pintado = PintarSalida(contexto, resuelta.imagen, w, h);
                                      return pintado;
                                    });
    if (pintado) {
      ++presentados_;
    }
    return pintado;
  }

  void RampaGamma(const std::array<std::array<uint16_t, 3>, 256>& rampa) override {
    rampa_juego_ = rampa;
    RecalcularRampa();
  }

  /*
   * The game's occlusion queries (the sun flare). GLES only has GL_ANY_SAMPLES_PASSED, which says whether any
   * fragment passed and not how many. So each query also has a sample counter: a 4-byte atomic counter buffer that
   * the query's draws add 1 to per fragment that passes, with a program variant that tests depth before shading and
   * counts at the end of the shader (kVarianteContar in nfsmw_nativo_dibujos_gles.cpp). That is the count of the
   * Xbox 360, and the flare fades as the sun gets covered, as in Vulkan.
   *
   * Without counters in the fragment stage (GL_MAX_FRAGMENT_ATOMIC_COUNTERS = 0), or for a query with a draw that
   * cannot count, the yes or no of GL_ANY_SAMPLES_PASSED: a visible structure counts kMuestrasVisibles (the count
   * the system fakes for "fully visible") and a hidden one 0, and the flare switches on and off without fading. If
   * the counters ever say 0 where the query says visible, they are turned off for the session.
   *
   * As in Vulkan, Terminar returns the last result measured for that structure, from an earlier frame, read without
   * waiting.
   */
  static constexpr uint64_t kMuestrasVisibles = 1000;

  void EmpezarOclusion(uint32_t base) override {
    // A deferred sky emitted inside the game's query would add its samples to it.
    if (dibujos_) {
      dibujos_->TerminarPase();
    }
    if (consulta_activa_.consulta) {
      CerrarConsulta();
    }
    GLuint consulta = 0;
    if (!consultas_libres_.empty()) {
      consulta = consultas_libres_.back();
      consultas_libres_.pop_back();
    } else {
      glGenQueries(1, &consulta);
    }
    GLuint contador = 0;
    if (contadores_) {
      if (!contadores_libres_.empty()) {
        contador = contadores_libres_.back();
        contadores_libres_.pop_back();
      } else {
        glGenBuffers(1, &contador);
        glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, contador);
        glBufferData(GL_ATOMIC_COUNTER_BUFFER, sizeof(uint32_t), nullptr, GL_DYNAMIC_READ);
      }
      const uint32_t cero = 0;
      glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, contador);
      glBufferSubData(GL_ATOMIC_COUNTER_BUFFER, 0, sizeof(cero), &cero);
      glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, 0);  // the generic binding: the draws' indexed binding 0 stays
    }
    glBeginQuery(GL_ANY_SAMPLES_PASSED, consulta);
    consulta_activa_ = {consulta, base, contador, contador != 0};
    ++oclusiones_empezadas_;
  }
  bool OclusionGpuPermitida() const override { return true; }
  bool OclusionAbierta() const override { return consulta_activa_.consulta != 0; }
  GLuint ContadorOclusion() const override {
    return consulta_activa_.consulta && consulta_activa_.exacta ? consulta_activa_.contador : 0;
  }
  void OclusionSinContar() override { consulta_activa_.exacta = false; }

  void CerrarConsulta() {
    glEndQuery(GL_ANY_SAMPLES_PASSED);
    if (consulta_activa_.contador) {
      // The shaders' atomic writes must be visible to the buffer read in RecogerOclusiones.
      glMemoryBarrier(GL_ATOMIC_COUNTER_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    }
    consultas_pendientes_.push_back(consulta_activa_);
    consulta_activa_ = {};
  }

  bool TerminarOclusion(uint32_t base, uint64_t& muestras) override {
    if (consulta_activa_.consulta && consulta_activa_.base == base) {
      CerrarConsulta();
    }
    RecogerOclusiones();
    const auto it = resultados_oclusion_.find(base);
    if (it == resultados_oclusion_.end()) {
      muestras = 0;
      return false;
    }
    muestras = it->second;
    ++oclusiones_publicadas_;
    oclusiones_muestras_ += muestras;
    return true;
  }
  void EstadisticasOclusion(uint64_t valores[5]) const override {
    valores[0] = oclusiones_empezadas_;
    valores[1] = 0;
    valores[2] = oclusiones_publicadas_;
    valores[3] = oclusiones_muestras_;
    valores[4] = oclusiones_contadas_;
  }

  // The results already available, in order, without waiting for the GPU.
  void RecogerOclusiones() {
    size_t hechas = 0;
    for (; hechas < consultas_pendientes_.size(); ++hechas) {
      const ConsultaOclusionGles& c = consultas_pendientes_[hechas];
      GLuint disponible = GL_FALSE;
      glGetQueryObjectuiv(c.consulta, GL_QUERY_RESULT_AVAILABLE, &disponible);
      if (!disponible) {
        break;
      }
      GLuint pasa = 0;
      glGetQueryObjectuiv(c.consulta, GL_QUERY_RESULT, &pasa);
      uint64_t muestras = pasa ? kMuestrasVisibles : 0;
      if (c.contador && c.exacta && contadores_ && pasa) {
        // The query has finished, and with it its draws: the counter is read without waiting.
        glBindBuffer(GL_COPY_READ_BUFFER, c.contador);
        const auto* valor = static_cast<const uint32_t*>(
            glMapBufferRange(GL_COPY_READ_BUFFER, 0, sizeof(uint32_t), GL_MAP_READ_BIT));
        const uint32_t contadas = valor ? *valor : 0;
        glUnmapBuffer(GL_COPY_READ_BUFFER);
        glBindBuffer(GL_COPY_READ_BUFFER, 0);
        if (contadas) {
          muestras = contadas;
          ++oclusiones_contadas_;
        } else if (++contadores_en_cero_ >= 8) {
          // Visible for the query and 0 for the counter: the driver does not count. Yes or no from now on.
          contadores_ = false;
          REXLOG_WARN("[nativo] C2 GLES oclusion: los contadores de muestras dan 0 con la consulta visible: se apagan "
                      "(el destello se enciende y se apaga sin fundido)");
        }
      }
      resultados_oclusion_[c.base] = muestras;
      consultas_libres_.push_back(c.consulta);
      if (c.contador) {
        contadores_libres_.push_back(c.contador);
      }
    }
    consultas_pendientes_.erase(consultas_pendientes_.begin(), consultas_pendientes_.begin() + ptrdiff_t(hechas));
    // A query the GPU never finishes must not grow the list without limit.
    while (consultas_pendientes_.size() > 64) {
      glDeleteQueries(1, &consultas_pendientes_.front().consulta);
      if (consultas_pendientes_.front().contador) {
        glDeleteBuffers(1, &consultas_pendientes_.front().contador);
      }
      consultas_pendientes_.erase(consultas_pendientes_.begin());
    }
  }

  // Parts C3-C6 (nfsmw_nativo_dibujos_gles.cpp).
  bool Dibujar(const PeticionDibujo& peticion) override {
    if (!dibujos_) {
      ++dibujos_rechazados_;
      return false;
    }
    return dibujos_->Dibujar(peticion);
  }
  EstadisticasDibujos EstadisticasDeDibujos() const override {
    if (dibujos_) {
      return dibujos_->Estadisticas();
    }
    EstadisticasDibujos e;
    e.rechazados = dibujos_rechazados_;
    return e;
  }

  // --- ContextoDestinosGles (C3-C6) -------------------------------------------------------------------------

  ImagenGles* DestinoColor(uint32_t base, uint32_t formato, uint32_t pitch) override {
    ImagenGles* imagen = ObtenerDestino(base, formato, pitch);
    if (imagen) {
      Preparar(*imagen);
    }
    return imagen;
  }

  ImagenGles* DestinoProfundidad(uint32_t base, uint32_t formato, uint32_t pitch) override {
    ImagenGles* imagen = ObtenerProfundidad(base, formato, pitch);
    if (imagen) {
      Preparar(*imagen);
    }
    return imagen;
  }

  const ImagenGles* TexturaResuelta(uint32_t direccion) override {
    const auto it = resueltas_.find(direccion);
    if (it == resueltas_.end() || !it->second.imagen.preparada) {
      return nullptr;
    }
    // nfsmw_reflejo_bajo_demanda: the road reflection is read. The draws look it up at least once per frame
    // (their per-register cache does not last longer than a frame or a C2 copy), which is all it needs.
    if (direccion == nfsmw::reflejo_demanda::kDireccion) {
      nfsmw::reflejo_demanda::AnotarLectura();
    }
    return &it->second.imagen;
  }
  void EsperarSubidas() override {}

  uint64_t CopiasProfundidad() const override { return copias_profundidad_; }
  uint64_t BorradosProfundidad() const override { return borrados_profundidad_; }
  void Estadisticas(uint64_t& copias, uint64_t& borrados, uint64_t& presentados,
                    uint64_t& rechazos) const override {
    copias = copias_;
    borrados = borrados_;
    presentados = presentados_;
    rechazos = rechazos_;
  }
  void TiempoGpu(uint64_t& nanosegundos, uint64_t& trabajos) const override {
    nanosegundos = gpu_ns_;
    trabajos = gpu_fotogramas_;
  }
  uint64_t MarcaGpuFinalNs() const override { return 0; }
  uint64_t TrabajosMarcasPrecisas() const override { return 0; }
  void EsperasGpu(uint64_t& veces, uint64_t& nanosegundos) const override {
    veces = 0;
    nanosegundos = 0;
  }
  void DuracionTrabajosGpu(uint64_t& veces, uint64_t& nanosegundos) const override {
    veces = 0;
    nanosegundos = 0;
  }
  void CosteGrabar(uint64_t coste[6]) const override { std::fill(coste, coste + 6, 0); }
  void TiempoGpuPorCategoria(std::array<uint64_t, kGpuCategorias>& nanosegundos) const override {
    nanosegundos.fill(0);
  }
  void EstadisticasPipeline(std::array<uint64_t, kGpuCategorias>& fragmentos,
                            std::array<uint64_t, kGpuCategorias>& vertices,
                            std::array<uint64_t, kGpuCategorias>& primitivas) const override {
    fragmentos.fill(0);
    vertices.fill(0);
    primitivas.fill(0);
  }
  void CopiasPorTamano(std::array<uint64_t, 4>& copias, std::array<uint64_t, 4>& pixeles) const override {
    copias.fill(0);
    pixeles.fill(0);
  }
  void EstadisticasPorShader(std::vector<uint64_t>& fragmentos, std::vector<uint64_t>& dibujos,
                             uint64_t& fotogramas) const override {
    fragmentos.clear();
    dibujos.clear();
    fotogramas = 0;
  }
  void IntervalosEntreSwaps(std::array<uint64_t, kCubetasSwap>& cubetas, uint64_t& solapes,
                            double& peor_ms) const override {
    cubetas.fill(0);
    solapes = 0;
    peor_ms = 0.0;
  }
  void CostePresentar(uint64_t coste[12]) const override { std::fill(coste, coste + 12, 0); }

 private:
  struct FramebufferSalida {
    GLuint fbo = 0;
    uint64_t version = 0;
  };

  bool Rechazar(uint32_t causa, const char* texto) {
    ++rechazos_;
    if (avisados_.insert(causa).second) {
      REXLOG_WARN("[nativo] C2 GLES: {} (causa {})", texto, causa);
    }
    return false;
  }

  bool Rectangulo(const RegistrosCopia& reg, uint32_t pitch, int32_t& x0, int32_t& y0, int32_t& x1, int32_t& y1) {
    uint32_t causa = 0;
    if (!RectanguloCopia(reg, pitch, memoria_, x0, y0, x1, y1, causa)) {
      return Rechazar(causa, causa == 5 ? "vertices de la copia en un formato no soportado" : "rectangulo de copia vacio");
    }
    return true;
  }

  // --- Images ----------------------------------------------------------------------------------------------

  // Logs the pending GL errors with where they were seen (the first 32; all of them are counted).
  void ComprobarGl(const char* donde) {
    for (GLenum error = glGetError(); error != GL_NO_ERROR; error = glGetError()) {
      if (++errores_gl_ <= 32) {
        REXLOG_WARN("[nativo] C2 GLES: error de GL {:#x} {} (error {})", error, donde, errores_gl_);
      }
    }
  }

  bool Crear(ImagenGles& imagen, uint32_t ancho, uint32_t alto, bool profundidad) {
    ComprobarGl("antes de crear una textura");
    glGenTextures(1, &imagen.textura);
    glBindTexture(GL_TEXTURE_2D, imagen.textura);
    glTexStorage2D(GL_TEXTURE_2D, 1, profundidad ? GL_DEPTH24_STENCIL8 : GL_RGBA8, GLsizei(ancho), GLsizei(alto));
    // Sampled with texelFetch or through the draws' sampler objects: nearest and clamped by default here.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (const GLenum error = glGetError(); error != GL_NO_ERROR) {
      REXLOG_ERROR("[nativo] C2 GLES: no se pudo crear una textura de {}x{} ({}): error {:#x}", ancho, alto,
                   profundidad ? "profundidad" : "color", error);
      glDeleteTextures(1, &imagen.textura);
      imagen = ImagenGles{};
      return false;
    }
    imagen.ancho = ancho;
    imagen.alto = alto;
    imagen.profundidad = profundidad;
    imagen.preparada = false;
    return true;
  }

  void Destruir(ImagenGles& imagen) {
    if (imagen.fbo) {
      glDeleteFramebuffers(1, &imagen.fbo);
    }
    if (imagen.textura) {
      glDeleteTextures(1, &imagen.textura);
    }
    imagen = ImagenGles{};
  }

  // Binds the image's own framebuffer (color, or depth and stencil) to that target.
  static void Adjuntar(GLenum objetivo, const ImagenGles& imagen) {
    if (!imagen.fbo) {
      glGenFramebuffers(1, &imagen.fbo);
      glBindFramebuffer(objetivo, imagen.fbo);
      glFramebufferTexture2D(objetivo, imagen.profundidad ? GL_DEPTH_STENCIL_ATTACHMENT : GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, imagen.textura, 0);
      return;
    }
    glBindFramebuffer(objetivo, imagen.fbo);
  }

  // glClearBuffer and glBlitFramebuffer obey the masks and the scissor, which the draws leave as they like.
  static void EstadoParaBorrar() {
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
  }

  void BorrarColor(ImagenGles& imagen, const float* color) {
    Adjuntar(GL_DRAW_FRAMEBUFFER, imagen);
    EstadoParaBorrar();
    glClearBufferfv(GL_COLOR, 0, color);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  }

  void BorrarProfundidadStencil(ImagenGles& imagen, float profundidad, int32_t stencil) {
    Adjuntar(GL_DRAW_FRAMEBUFFER, imagen);
    EstadoParaBorrar();
    glClearBufferfi(GL_DEPTH_STENCIL, 0, profundidad, stencil);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  }

  // First use: cleared to zero (color) or to the far plane (depth), like Preparar in Vulkan.
  void Preparar(ImagenGles& imagen) {
    if (imagen.preparada) {
      return;
    }
    if (imagen.profundidad) {
      BorrarProfundidadStencil(imagen, 1.0f, 0);
    } else {
      const float cero[4] = {};
      BorrarColor(imagen, cero);
    }
    imagen.preparada = true;
  }

  ImagenGles* ObtenerDestino(uint32_t base, uint32_t formato, uint32_t pitch) {
    if (!pitch) {
      Rechazar(7, "destino de render con pitch 0");
      return nullptr;
    }
    const uint64_t clave = (uint64_t(base) << 20) | (uint64_t(formato) << 16) | pitch;
    auto it = destinos_.find(clave);
    if (it != destinos_.end()) {
      return &it->second;
    }
    // The same height as in Vulkan (see ObtenerDestino there): from the pitch, at least 720.
    const uint32_t alto = std::min(kAltoMaximoDestino, std::max<uint32_t>(720, (pitch + 15) & ~15u));
    ImagenGles imagen;
    if (!Crear(imagen, pitch, alto, false)) {
      Rechazar(8, "no se pudo crear un destino de render");
      return nullptr;
    }
    REXLOG_INFO("[nativo] C2 GLES: destino de render base {:03X}, formato {}, {}x{}", base, formato, pitch, alto);
    return &destinos_.emplace(clave, imagen).first->second;
  }

  ImagenGles* ObtenerProfundidad(uint32_t base, uint32_t formato, uint32_t pitch) {
    if (!pitch) {
      Rechazar(11, "sin destino de profundidad (pitch 0)");
      return nullptr;
    }
    const uint64_t clave = (uint64_t(base) << 20) | (uint64_t(formato) << 16) | pitch;
    auto it = profundidades_.find(clave);
    if (it != profundidades_.end()) {
      return &it->second;
    }
    const uint32_t alto = std::min(kAltoMaximoDestino, std::max<uint32_t>(720, (pitch + 15) & ~15u));
    ImagenGles imagen;
    if (!Crear(imagen, pitch, alto, true)) {
      Rechazar(12, "no se pudo crear un destino de profundidad");
      return nullptr;
    }
    REXLOG_INFO("[nativo] C2 GLES: destino de profundidad base {:03X}, formato {}, {}x{}", base, formato, pitch,
                alto);
    return &profundidades_.emplace(clave, imagen).first->second;
  }

  ResueltaGles* ObtenerResuelta(uint32_t base, uint32_t ancho, uint32_t alto, uint32_t formato, bool intercambio_rb,
                                bool profundidad) {
    if (!ancho || !alto) {
      Rechazar(9, "copia a una textura de tamano 0");
      return nullptr;
    }
    auto it = resueltas_.find(base);
    if (it != resueltas_.end()) {
      if (it->second.imagen.ancho == ancho && it->second.imagen.alto == alto &&
          it->second.imagen.profundidad == profundidad) {
        it->second.formato_guest = formato;
        it->second.intercambio_rb = intercambio_rb;
        it->second.imagen.intercambio_rb = intercambio_rb;
        return &it->second;
      }
      // Another size or format at the same address. GL keeps the old texture alive while queued commands use it.
      Destruir(it->second.imagen);
      resueltas_.erase(it);
    }
    ResueltaGles resuelta;
    if (!Crear(resuelta.imagen, ancho, alto, profundidad)) {
      Rechazar(10, "no se pudo crear una textura resuelta");
      return nullptr;
    }
    resuelta.formato_guest = formato;
    resuelta.intercambio_rb = intercambio_rb;
    resuelta.imagen.intercambio_rb = intercambio_rb;
    REXLOG_INFO("[nativo] C2 GLES: textura resuelta en {:08X}, {}x{}, formato {}", base, ancho, alto, formato);
    return &resueltas_.emplace(base, resuelta).first->second;
  }

  // --- Copies ----------------------------------------------------------------------------------------------

  void CopiarColor(const RegistrosCopia& reg, ImagenGles& destino_render, int32_t x0, int32_t y0, int32_t x1,
                   int32_t y1, uint32_t info_destino, uint32_t formato_destino) {
    const uint32_t pitch_destino = reg.rb_copy_dest_pitch & 0x3FFF;
    const uint32_t alto_destino = (reg.rb_copy_dest_pitch >> 16) & 0x3FFF;
    // 4 bytes per texel: base in multiples of 32 texels (GetResolveInfo).
    const uint32_t base_x = uint32_t(x0) & ~uint32_t(31);
    const uint32_t base_y = uint32_t(y0) & ~uint32_t(31);
    const uint32_t base = (reg.rb_copy_dest_base +
                           uint32_t(DesplazamientoMosaico2D(int32_t(base_x), int32_t(base_y), pitch_destino, 2))) &
                          0x1FFFFFFF;
    const uint32_t dx = uint32_t(x0) - base_x;
    const uint32_t dy = uint32_t(y0) - base_y;
    // The image takes the copy's width when the pitch is only that width rounded up to 32 (see Vulkan).
    const uint32_t ancho_copia = uint32_t(x1 - x0);
    const uint32_t ancho_imagen =
        x0 == 0 && ancho_copia < pitch_destino && ((ancho_copia + 31) & ~uint32_t(31)) == pitch_destino
            ? ancho_copia
            : pitch_destino;
    ResueltaGles* resuelta =
        ObtenerResuelta(base, ancho_imagen, alto_destino, formato_destino, (info_destino >> 24) & 0x1, false);
    if (!resuelta) {
      return;
    }
    // nfsmw_reflejo_bajo_demanda. Without this its guard saw no copy of the reflection in its first frames and
    // switched itself off: with GLES the reflection was drawn in every frame.
    if (base == nfsmw::reflejo_demanda::kDireccion) {
      nfsmw::reflejo_demanda::AnotarCopia();
    }
    Preparar(resuelta->imagen);
    // What the game asks for and what fits in the render target and in the texture.
    const uint32_t pedido_ancho = std::min(uint32_t(x1 - x0), destino_render.ancho - std::min<uint32_t>(x0, destino_render.ancho));
    const uint32_t pedido_alto = std::min(uint32_t(y1 - y0), destino_render.alto - std::min<uint32_t>(y0, destino_render.alto));
    const uint32_t cabe_ancho = resuelta->imagen.ancho > dx ? resuelta->imagen.ancho - dx : 0;
    const uint32_t cabe_alto = resuelta->imagen.alto > dy ? resuelta->imagen.alto - dy : 0;
    const uint32_t ancho = std::min(pedido_ancho, cabe_ancho);
    const uint32_t alto = std::min(pedido_alto, cabe_alto);
    if (!pedido_ancho || !pedido_alto || !cabe_ancho || !cabe_alto) {
      return;
    }
    // A scene larger than the texture (internal resolution above the front buffer's) is shrunk with a linear
    // blit, as in Vulkan; a mismatch of a few pixels is cropped.
    const bool encoger = pedido_ancho * 4 >= cabe_ancho * 5 || pedido_alto * 4 >= cabe_alto * 5;
    if (encoger) {
      Adjuntar(GL_READ_FRAMEBUFFER, destino_render);
      Adjuntar(GL_DRAW_FRAMEBUFFER, resuelta->imagen);
      EstadoParaBorrar();
      glBlitFramebuffer(x0, y0, x0 + GLint(pedido_ancho), y0 + GLint(pedido_alto), GLint(dx), GLint(dy),
                        GLint(dx + cabe_ancho), GLint(dy + cabe_alto), GL_COLOR_BUFFER_BIT, GL_LINEAR);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
      ++copias_;
      if (++reducciones_ <= 4) {
        REXLOG_INFO("[resolucion] GLES: la escena de {}x{} se encoge a {}x{} al resolverla", pedido_ancho,
                    pedido_alto, cabe_ancho, cabe_alto);
      }
      LeerResuelta(reg, *resuelta, x0, y0, dx, dy, cabe_ancho, cabe_alto);
      return;
    }
    glCopyImageSubData(destino_render.textura, GL_TEXTURE_2D, 0, x0, y0, 0, resuelta->imagen.textura, GL_TEXTURE_2D,
                       0, GLint(dx), GLint(dy), 0, GLsizei(ancho), GLsizei(alto), 1);
    ComprobarGl("en una copia de color");
    ++copias_;
    LeerResuelta(reg, *resuelta, x0, y0, dx, dy, ancho, alto);
  }

  // Depth copy: the rectangle of the depth render target to a resolved texture of the same format, at the
  // base GetResolveInfo computes (4 bytes per texel, like k_24_8).
  void CopiarProfundidad(const RegistrosCopia& reg, uint32_t pitch) {
    const uint32_t info_destino = reg.rb_copy_dest_info;
    if ((info_destino >> 3) & 0x1) {
      Rechazar(3, "copia a textura 3D o array: todavia no");
      return;
    }
    int32_t x0, y0, x1, y1;
    if (!Rectangulo(reg, pitch, x0, y0, x1, y1)) {
      return;
    }
    const uint32_t info = reg.rb_depth_info;
    ++copias_profundidad_;
    ImagenGles* profundidad = ObtenerProfundidad(info & 0xFFF, (info >> 16) & 0x1, pitch);
    if (!profundidad) {
      return;
    }
    Preparar(*profundidad);
    const uint32_t pitch_destino = reg.rb_copy_dest_pitch & 0x3FFF;
    const uint32_t alto_destino = (reg.rb_copy_dest_pitch >> 16) & 0x3FFF;
    const uint32_t base_x = uint32_t(x0) & ~uint32_t(31);
    const uint32_t base_y = uint32_t(y0) & ~uint32_t(31);
    const uint32_t base = (reg.rb_copy_dest_base +
                           uint32_t(DesplazamientoMosaico2D(int32_t(base_x), int32_t(base_y), pitch_destino, 2))) &
                          0x1FFFFFFF;
    const uint32_t dx = uint32_t(x0) - base_x;
    const uint32_t dy = uint32_t(y0) - base_y;
    const uint32_t formato_textura = ((info >> 16) & 0x1) ? 23 : 22;  // k_24_8_FLOAT : k_24_8
    ResueltaGles* resuelta = ObtenerResuelta(base, pitch_destino, alto_destino, formato_textura, false, true);
    if (!resuelta) {
      return;
    }
    Preparar(resuelta->imagen);
    if (uint32_t(x0) >= profundidad->ancho || uint32_t(y0) >= profundidad->alto || dx >= resuelta->imagen.ancho ||
        dy >= resuelta->imagen.alto) {
      return;
    }
    const uint32_t ancho =
        std::min({uint32_t(x1 - x0), profundidad->ancho - uint32_t(x0), resuelta->imagen.ancho - dx});
    const uint32_t alto = std::min({uint32_t(y1 - y0), profundidad->alto - uint32_t(y0), resuelta->imagen.alto - dy});
    if (!ancho || !alto) {
      return;
    }
    glCopyImageSubData(profundidad->textura, GL_TEXTURE_2D, 0, x0, y0, 0, resuelta->imagen.textura, GL_TEXTURE_2D, 0,
                       GLint(dx), GLint(dy), 0, GLsizei(ancho), GLsizei(alto), 1);
    ComprobarGl("en una copia de profundidad");
    if (++copias_profundidad_anotadas_ <= 12) {
      REXLOG_INFO("[nativo] C2 GLES diag: copia de profundidad {}: destino {:03X} {}x{} ({},{}) {}x{} -> {:08X} {}x{} "
                  "({},{}); borra despues {}",
                  copias_profundidad_anotadas_, info & 0xFFF, profundidad->ancho, profundidad->alto, x0, y0, ancho,
                  alto, base, resuelta->imagen.ancho, resuelta->imagen.alto, dx, dy, (reg.rb_copy_control >> 9) & 0x1);
    }
    ++copias_;
  }

  void BorrarProfundidad(const RegistrosCopia& reg, uint32_t pitch) {
    const uint32_t info = reg.rb_depth_info;
    ImagenGles* profundidad = ObtenerProfundidad(info & 0xFFF, (info >> 16) & 0x1, pitch);
    if (!profundidad) {
      return;
    }
    profundidad->preparada = true;  // the clear gives it contents
    BorrarProfundidadStencil(*profundidad, float(reg.rb_depth_clear >> 8) / 16777215.0f,
                             int32_t(reg.rb_depth_clear & 0xFF));
    ++borrados_profundidad_;
  }

  // --- Read-backs (nfsmw_nativo_leer_resueltas_texels) -------------------------------------------------------

  void LeerResuelta(const RegistrosCopia& reg, const ResueltaGles& resuelta, int32_t x0, int32_t y0, uint32_t dx,
                    uint32_t dy, uint32_t ancho, uint32_t alto) {
    const int32_t maximo = REXCVAR_GET(nfsmw_nativo_leer_resueltas_texels);
    if (maximo <= 0 || uint64_t(ancho) * alto > uint64_t(maximo) || resuelta.imagen.profundidad) {
      return;
    }
    // nfsmw_nativo_lecturas_cada: exposure changes slowly, the guest can keep the previous one.
    const uint64_t clave_destino = (uint64_t(reg.rb_copy_dest_base) << 28) ^ (uint64_t(ancho) << 14) ^ uint64_t(alto);
    const uint64_t cada = uint64_t(std::max(REXCVAR_GET(nfsmw_nativo_lecturas_cada), 1));
    if (copias_por_destino_[clave_destino]++ % cada != 0) {
      return;
    }
    LecturaGles lectura;
    if (!lecturas_libres_.empty()) {
      lectura = lecturas_libres_.back();
      lecturas_libres_.pop_back();
    }
    const uint32_t bytes = ancho * alto * 4;
    if (!lectura.pbo) {
      glGenBuffers(1, &lectura.pbo);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, lectura.pbo);
    if (lectura.capacidad < bytes) {
      glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
      lectura.capacidad = bytes;
    }
    Adjuntar(GL_READ_FRAMEBUFFER, resuelta.imagen);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(GLint(dx), GLint(dy), GLsizei(ancho), GLsizei(alto), GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    lectura.valla = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    lectura.base = reg.rb_copy_dest_base;
    lectura.x0 = x0;
    lectura.y0 = y0;
    lectura.ancho = ancho;
    lectura.alto = alto;
    lectura.pitch = reg.rb_copy_dest_pitch & 0x3FFF;
    lectura.alto_destino = (reg.rb_copy_dest_pitch >> 16) & 0x3FFF;
    lectura.info = reg.rb_copy_dest_info;
    lecturas_pendientes_.push_back(lectura);
    if (lecturas_hechas_++ == 0) {
      REXLOG_INFO("[nativo] C2 GLES: lectura de vuelta de texturas resueltas de hasta {} texels (la primera: "
                  "{:08X}, {}x{})",
                  maximo, reg.rb_copy_dest_base, ancho, alto);
    }
  }

  // The read-backs whose fence has signaled go to guest memory, in order. With `esperar`, all of them.
  void EscribirLecturas(bool esperar) {
    size_t hechas = 0;
    for (; hechas < lecturas_pendientes_.size(); ++hechas) {
      LecturaGles& l = lecturas_pendientes_[hechas];
      const GLenum estado =
          glClientWaitSync(l.valla, GL_SYNC_FLUSH_COMMANDS_BIT, esperar ? GLuint64(1000000000) : GLuint64(0));
      if (estado == GL_TIMEOUT_EXPIRED) {
        break;  // the following ones were issued later
      }
      glDeleteSync(l.valla);
      l.valla = nullptr;
      if (estado != GL_WAIT_FAILED) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, l.pbo);
        const void* datos = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, GLsizeiptr(l.ancho) * l.alto * 4, GL_MAP_READ_BIT);
        if (datos) {
          EscribirTexelsEnGuest(memoria_->TranslatePhysical(0), static_cast<const uint8_t*>(datos), l.base, l.x0,
                                l.y0, l.ancho, l.alto, l.pitch, l.alto_destino, l.info);
          glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
          ++lecturas_escritas_;
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
      }
      lecturas_libres_.push_back(l);
    }
    lecturas_pendientes_.erase(lecturas_pendientes_.begin(), lecturas_pendientes_.begin() + ptrdiff_t(hechas));
  }

  void DestruirLectura(LecturaGles& lectura) {
    if (lectura.valla) glDeleteSync(lectura.valla);
    if (lectura.pbo) glDeleteBuffers(1, &lectura.pbo);
    lectura = LecturaGles{};
  }

  // --- nfsmw_nativo_gles_etapas: C2's share of the ring --------------------------------------------------------

  void InformeEtapas() {
    const bool cronometrar = REXCVAR_GET(nfsmw_nativo_gles_etapas);
    if (cronometrar != cronometrar_) {
      cronometrar_ = cronometrar;
      ns_copias_ = ns_swaps_ = 0;
      copias_cronometradas_ = swaps_cronometrados_ = 0;
    }
    if (!cronometrar_ || ++swaps_cronometrados_ < 600) {
      return;
    }
    const double swaps = double(swaps_cronometrados_);
    REXLOG_INFO("[nativo] C2 GLES etapas: copias y borrados {:.2f} ms por fotograma ({:.1f} por fotograma, {:.0f} us "
                "cada una), Swap {:.2f} ms por fotograma",
                double(ns_copias_) / 1e6 / swaps, double(copias_cronometradas_) / swaps,
                copias_cronometradas_ ? double(ns_copias_) / 1000.0 / double(copias_cronometradas_) : 0.0,
                double(ns_swaps_) / 1e6 / swaps);
    ns_copias_ = ns_swaps_ = 0;
    copias_cronometradas_ = swaps_cronometrados_ = 0;
  }

  // --- GPU time ------------------------------------------------------------------------------------------------
  //
  // With GL_EXT_disjoint_timer_query, one GL_TIME_ELAPSED query from Swap to Swap: the GPU time of each frame's
  // work (TiempoGpu, the "GPU ms por Swap" of the C2 report). Up to four in flight; a result is dropped if the GPU
  // was disjoint (frequency change, context switch) meanwhile.

  static constexpr GLenum kTiempoTranscurrido = 0x88BF;  // GL_TIME_ELAPSED_EXT
  static constexpr GLenum kGpuDisjunta = 0x8FBB;        // GL_GPU_DISJOINT_EXT
  using FnResultado64 = void(GL_APIENTRYP)(GLuint, GLenum, GLuint64*);

  void MedirGpu() {
    if (!provider_->capacidades().timer_query) {
      return;
    }
    if (!resultado64_) {
      resultado64_ = reinterpret_cast<FnResultado64>(eglGetProcAddress("glGetQueryObjectui64vEXT"));
      if (!resultado64_) {
        return;
      }
    }
    if (tiempo_activo_) {
      glEndQuery(kTiempoTranscurrido);
      tiempos_pendientes_.push_back(tiempo_activo_);
      tiempo_activo_ = 0;
    }
    GLint disjunta = 0;
    glGetIntegerv(kGpuDisjunta, &disjunta);
    while (!tiempos_pendientes_.empty()) {
      const GLuint q = tiempos_pendientes_.front();
      GLuint disponible = GL_FALSE;
      glGetQueryObjectuiv(q, GL_QUERY_RESULT_AVAILABLE, &disponible);
      if (!disponible) {
        break;
      }
      GLuint64 ns = 0;
      resultado64_(q, GL_QUERY_RESULT, &ns);
      if (!disjunta) {
        gpu_ns_ += ns;
        ++gpu_fotogramas_;
      }
      tiempos_libres_.push_back(q);
      tiempos_pendientes_.erase(tiempos_pendientes_.begin());
    }
    if (tiempos_pendientes_.size() >= 4) {
      return;  // the GPU is far behind: skip measuring this frame
    }
    GLuint q = 0;
    if (!tiempos_libres_.empty()) {
      q = tiempos_libres_.back();
      tiempos_libres_.pop_back();
    } else {
      glGenQueries(1, &q);
    }
    glBeginQuery(kTiempoTranscurrido, q);
    tiempo_activo_ = q;
  }

  // --- Output ----------------------------------------------------------------------------------------------

  // The same table as RecalcularRampa in nfsmw_nativo_destinos.cpp: the game's ramp followed by the
  // single-channel post-processing (GoldenEye-Recomp's formula and order).
  void RecalcularRampa() {
    const nfsmw::ajustes::Posproceso& p = posproceso_;
    graduacion_ = p.activo && (p.saturacion != 1.0f || p.vibracion != 0.0f || p.vineta > 0.0f || p.lineas > 0.0f);
    const float tinte[3] = {p.tinte_r, p.tinte_g, p.tinte_b};
    const float inversa_gamma = p.gamma > 0.0f ? 1.0f / p.gamma : 1.0f;
    for (uint32_t i = 0; i < 256; ++i) {
      for (uint32_t c = 0; c < 3; ++c) {
        float x = rampa_gamma_ ? float(rampa_juego_[i][c] & 0x3FF) / 1023.0f : float(i) / 255.0f;
        if (p.activo) {
          x += c == 0 ? p.temperatura * 0.10f : c == 2 ? -p.temperatura * 0.10f : 0.0f;
          x += p.brillo;
          x = (x - 0.5f) * p.contraste + 0.5f;
          x = x + (x * tinte[c] - x) * p.tinte;
          if (!graduacion_) {
            x = std::clamp(std::pow(std::max(x, 0.0001f), inversa_gamma), 0.0f, 1.0f);
          }
        }
        rampa_valores_[i * 4 + c] = x;
      }
      rampa_valores_[i * 4 + 3] = 1.0f;
    }
    const size_t extra = 256 * 4;
    rampa_valores_[extra] = p.saturacion;
    rampa_valores_[extra + 1] = p.vibracion;
    rampa_valores_[extra + 2] = inversa_gamma;
    rampa_valores_[extra + 3] = 0.0f;
    rampa_valores_[extra + 4] = p.vineta;
    rampa_valores_[extra + 5] = p.lineas;
    rampa_valores_[extra + 6] = 0.0f;
    rampa_valores_[extra + 7] = 0.0f;
    ++version_rampa_;
  }

  bool PintarSalida(GlesPresenter::GlesGuestOutputRefreshContext& contexto, const ImagenGles& origen, uint32_t ancho,
                    uint32_t alto) {
    if (const uint64_t version = nfsmw::ajustes::VersionPosproceso(); version != version_posproceso_) {
      version_posproceso_ = version;
      posproceso_ = nfsmw::ajustes::LeerPosproceso();
      RecalcularRampa();
    }
    if (version_rampa_subida_ != version_rampa_) {
      glBindBuffer(GL_UNIFORM_BUFFER, ubo_rampa_);
      glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(rampa_valores_), rampa_valores_.data());
      glBindBuffer(GL_UNIFORM_BUFFER, 0);
      version_rampa_subida_ = version_rampa_;
    }
    GLuint fbo = 0;
    for (const FramebufferSalida& f : framebuffers_salida_) {
      if (f.fbo && f.version == contexto.version()) {
        fbo = f.fbo;
      }
    }
    if (!fbo) {
      FramebufferSalida& f = framebuffers_salida_[siguiente_framebuffer_];
      siguiente_framebuffer_ = (siguiente_framebuffer_ + 1) % framebuffers_salida_.size();
      if (!f.fbo) {
        glGenFramebuffers(1, &f.fbo);
      }
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, f.fbo);
      glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, contexto.texture(), 0);
      if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        return Rechazar(13, "el framebuffer de la salida esta incompleto");
      }
      f.version = contexto.version();
      fbo = f.fbo;
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    glViewport(0, 0, GLsizei(ancho), GLsizei(alto));
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(programa_);
    glUniform1i(uniforme_exacta_, ancho == origen.ancho && alto == origen.alto);
    glUniform1i(uniforme_graduacion_, graduacion_);
    // nfsmw_antialiasing = fxaa: smooths the game image's edges before the table (partly replaces the 360's MSAA).
    const bool fxaa = nfsmw::ajustes::AntialiasingFxaa();
    if (fxaa != fxaa_anotado_) {
      fxaa_anotado_ = fxaa;
      REXLOG_INFO("[nativo] C2 GLES: antialiasing en la salida: {}", fxaa ? "FXAA" : "ninguno");
    }
    glUniform1i(uniforme_fxaa_, fxaa);
    glUniform2i(uniforme_desplazamiento_, 0, 0);
    glUniform2f(uniforme_inverso_, 1.0f / float(ancho), 1.0f / float(alto));
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo_rampa_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, origen.textura);
    glBindSampler(0, sampler_);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glBindSampler(0, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    contexto.SetIs8bpc(true);
    return true;
  }

  const rex::ui::gles::GlesProvider* provider_;
  rex::memory::Memory* memoria_;
  std::unique_ptr<DibujosGles> dibujos_;

  std::unordered_map<uint64_t, ImagenGles> destinos_;
  std::unordered_map<uint64_t, ImagenGles> profundidades_;
  std::unordered_map<uint32_t, ResueltaGles> resueltas_;

  std::vector<LecturaGles> lecturas_pendientes_;
  std::vector<LecturaGles> lecturas_libres_;
  std::unordered_map<uint64_t, uint64_t> copias_por_destino_;
  uint64_t lecturas_hechas_ = 0;
  uint64_t lecturas_escritas_ = 0;

  // Output.
  GLuint programa_ = 0;
  GLuint vao_ = 0;
  GLuint ubo_rampa_ = 0;
  GLuint sampler_ = 0;
  GLint uniforme_exacta_ = -1;
  GLint uniforme_graduacion_ = -1;
  GLint uniforme_fxaa_ = -1;
  bool fxaa_anotado_ = false;
  GLint uniforme_desplazamiento_ = -1;
  GLint uniforme_inverso_ = -1;
  std::array<FramebufferSalida, 3> framebuffers_salida_{};
  size_t siguiente_framebuffer_ = 0;
  bool rampa_gamma_ = true;
  bool graduacion_ = false;
  std::array<std::array<uint16_t, 3>, 256> rampa_juego_{};
  std::array<float, 256 * 4 + 8> rampa_valores_{};
  uint64_t version_rampa_ = 0;
  uint64_t version_rampa_subida_ = ~uint64_t(0);
  nfsmw::ajustes::Posproceso posproceso_;
  uint64_t version_posproceso_ = ~uint64_t(0);

  // Counters (C2 report).
  uint64_t copias_ = 0;
  uint64_t borrados_ = 0;
  uint64_t presentados_ = 0;
  uint64_t rechazos_ = 0;
  uint64_t copias_profundidad_ = 0;
  uint64_t borrados_profundidad_ = 0;
  uint64_t reducciones_ = 0;
  // Occlusion and GPU time.
  struct ConsultaOclusionGles {
    GLuint consulta = 0;
    uint32_t base = 0;
    GLuint contador = 0;  // sample counter (0: yes or no only)
    bool exacta = false;  // every draw of the query counted its samples
  };
  bool contadores_ = false;  // atomic counters in the fragment stage
  // nfsmw_nativo_gles_etapas.
  bool cronometrar_ = false;
  uint64_t ns_copias_ = 0;
  uint64_t ns_swaps_ = 0;
  uint64_t copias_cronometradas_ = 0;
  uint64_t swaps_cronometrados_ = 0;
  std::vector<GLuint> contadores_libres_;
  uint32_t contadores_en_cero_ = 0;
  uint64_t oclusiones_contadas_ = 0;
  ConsultaOclusionGles consulta_activa_;
  std::vector<ConsultaOclusionGles> consultas_pendientes_;
  std::vector<GLuint> consultas_libres_;
  std::unordered_map<uint32_t, uint64_t> resultados_oclusion_;
  uint64_t oclusiones_empezadas_ = 0;
  uint64_t oclusiones_publicadas_ = 0;
  uint64_t oclusiones_muestras_ = 0;
  FnResultado64 resultado64_ = nullptr;
  GLuint tiempo_activo_ = 0;
  std::vector<GLuint> tiempos_pendientes_;
  std::vector<GLuint> tiempos_libres_;
  uint64_t gpu_ns_ = 0;
  uint64_t gpu_fotogramas_ = 0;
  uint64_t errores_gl_ = 0;
  uint32_t copias_profundidad_anotadas_ = 0;
  uint64_t dibujos_rechazados_ = 0;
  std::unordered_set<uint32_t> avisados_;
};

}  // namespace

std::unique_ptr<DestinosNativos> CrearDestinosGles(const rex::ui::gles::GlesProvider* provider,
                                                   rex::memory::Memory* memoria) {
  if (!provider || !memoria) {
    return nullptr;
  }
  auto destinos = std::make_unique<DestinosGles>(provider, memoria);
  if (!destinos->Inicializar()) {
    return nullptr;
  }
  return destinos;
}

}  // namespace nfsmw::nativo
