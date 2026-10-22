/*
 * OpenGL ES graphics provider over EGL (see rex/ui/gles/provider.h).
 */

#include <cstring>
#include <string_view>

#include <GLES3/gl32.h>

#include <rex/logging.h>
#include <rex/ui/gles/immediate_drawer.h>
#include <rex/ui/gles/presenter.h>
#include <rex/ui/gles/provider.h>

namespace rex::ui::gles {

namespace {

bool TieneExtension(const char* lista, std::string_view nombre) {
  if (!lista) {
    return false;
  }
  for (std::string_view resto(lista); !resto.empty();) {
    const size_t fin = resto.find(' ');
    if (resto.substr(0, fin) == nombre) {
      return true;
    }
    if (fin == std::string_view::npos) {
      break;
    }
    resto.remove_prefix(fin + 1);
  }
  return false;
}

bool TieneExtensionGl(std::string_view nombre) {
  GLint n = 0;
  glGetIntegerv(GL_NUM_EXTENSIONS, &n);
  for (GLint i = 0; i < n; ++i) {
    const char* e = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, GLuint(i)));
    if (e && nombre == e) {
      return true;
    }
  }
  return false;
}

const char* Texto(const GLubyte* s) { return s ? reinterpret_cast<const char*>(s) : ""; }

}  // namespace

GlesProvider::ContextoHilo::~ContextoHilo() {
  if (eglGetCurrentContext() == contexto_) {
    Soltar();
  }
  if (superficie_ != EGL_NO_SURFACE) {
    eglDestroySurface(display_, superficie_);
  }
  if (contexto_ != EGL_NO_CONTEXT) {
    eglDestroyContext(display_, contexto_);
  }
}

bool GlesProvider::ContextoHilo::HacerActual() {
  if (eglMakeCurrent(display_, superficie_, superficie_, contexto_) != EGL_TRUE) {
    REXLOG_ERROR("GlesProvider: eglMakeCurrent del contexto de hilo fallo ({:#x})", eglGetError());
    return false;
  }
  return true;
}

void GlesProvider::ContextoHilo::Soltar() {
  eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

std::unique_ptr<GlesProvider> GlesProvider::Create() {
  auto provider = std::unique_ptr<GlesProvider>(new GlesProvider());
  if (!provider->Inicializar()) {
    return nullptr;
  }
  return provider;
}

GlesProvider::~GlesProvider() {
  if (display_ == EGL_NO_DISPLAY) {
    return;
  }
  if (raiz_ != EGL_NO_CONTEXT) {
    eglDestroyContext(display_, raiz_);
  }
  // eglTerminate is not called: on Android the display is shared by the whole process, and SDL or the
  // system may be using it.
}

EGLSurface GlesProvider::CrearPbuffer() const {
  const EGLint atributos[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
  return eglCreatePbufferSurface(display_, config_, atributos);
}

bool GlesProvider::Inicializar() {
  display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (display_ == EGL_NO_DISPLAY) {
    REXLOG_ERROR("GlesProvider: no hay display de EGL");
    return false;
  }
  EGLint mayor = 0, menor = 0;
  if (eglInitialize(display_, &mayor, &menor) != EGL_TRUE) {
    REXLOG_ERROR("GlesProvider: eglInitialize fallo ({:#x})", eglGetError());
    display_ = EGL_NO_DISPLAY;
    return false;
  }
  const char* extensiones_egl = eglQueryString(display_, EGL_EXTENSIONS);
  sin_superficie_ = TieneExtension(extensiones_egl, "EGL_KHR_surfaceless_context");

  // RGBA8 window and pbuffer, OpenGL ES 3. No depth or stencil: the renderer draws into its own
  // framebuffers, and the window only receives the final image.
  const EGLint atributos_config[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                     EGL_SURFACE_TYPE,    EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
                                     EGL_RED_SIZE,        8,
                                     EGL_GREEN_SIZE,      8,
                                     EGL_BLUE_SIZE,       8,
                                     EGL_ALPHA_SIZE,      8,
                                     EGL_DEPTH_SIZE,      0,
                                     EGL_STENCIL_SIZE,    0,
                                     EGL_NONE};
  EGLint configs = 0;
  if (eglChooseConfig(display_, atributos_config, &config_, 1, &configs) != EGL_TRUE || configs < 1) {
    REXLOG_ERROR("GlesProvider: no hay configuracion de EGL RGBA8 para OpenGL ES 3 ({:#x})", eglGetError());
    return false;
  }
  if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
    REXLOG_ERROR("GlesProvider: eglBindAPI(EGL_OPENGL_ES_API) fallo");
    return false;
  }
  // 3.2 first, then 3.1.
  for (EGLint m : {2, 1}) {
    const EGLint atributos_contexto[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, m, EGL_NONE};
    raiz_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, atributos_contexto);
    if (raiz_ != EGL_NO_CONTEXT) {
      version_contexto_ = m;
      break;
    }
  }
  if (raiz_ == EGL_NO_CONTEXT) {
    REXLOG_ERROR("GlesProvider: no se pudo crear un contexto de OpenGL ES 3.1 ni 3.2 ({:#x})", eglGetError());
    return false;
  }

  // The capabilities are read with the root context current for a moment, on a pbuffer.
  EGLSurface pbuffer = sin_superficie_ ? EGL_NO_SURFACE : CrearPbuffer();
  if (!sin_superficie_ && pbuffer == EGL_NO_SURFACE) {
    REXLOG_ERROR("GlesProvider: no se pudo crear el pbuffer ({:#x})", eglGetError());
    return false;
  }
  if (eglMakeCurrent(display_, pbuffer, pbuffer, raiz_) != EGL_TRUE) {
    REXLOG_ERROR("GlesProvider: eglMakeCurrent del contexto raiz fallo ({:#x})", eglGetError());
    if (pbuffer != EGL_NO_SURFACE) {
      eglDestroySurface(display_, pbuffer);
    }
    return false;
  }
  Capacidades& c = capacidades_;
  GLint gl_mayor = 0, gl_menor = 0;
  glGetIntegerv(GL_MAJOR_VERSION, &gl_mayor);
  glGetIntegerv(GL_MINOR_VERSION, &gl_menor);
  c.version = uint32_t(gl_mayor * 10 + gl_menor);
  c.renderer = Texto(glGetString(GL_RENDERER));
  c.vendor = Texto(glGetString(GL_VENDOR));
  c.version_texto = Texto(glGetString(GL_VERSION));
  c.clip_control = TieneExtensionGl("GL_EXT_clip_control");
  c.timer_query = TieneExtensionGl("GL_EXT_disjoint_timer_query");
  c.buffer_storage = TieneExtensionGl("GL_EXT_buffer_storage");
  c.copy_image = c.version >= 32 || TieneExtensionGl("GL_EXT_copy_image");
  c.texture_compression_s3tc = TieneExtensionGl("GL_EXT_texture_compression_s3tc");
  c.texture_compression_rgtc = TieneExtensionGl("GL_EXT_texture_compression_rgtc");
  c.texture_norm16 = TieneExtensionGl("GL_EXT_texture_norm16");
  c.float_linear = TieneExtensionGl("GL_OES_texture_float_linear");
  c.mirror_clamp = TieneExtensionGl("GL_EXT_texture_mirror_clamp_to_edge");
  if (TieneExtensionGl("GL_EXT_texture_filter_anisotropic")) {
    glGetFloatv(0x84FF /* GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT */, &c.anisotropia_max);
  }
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &c.max_texture_size);
  glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &c.max_combined_texture_units);
  glGetIntegerv(GL_MAX_VERTEX_OUTPUT_COMPONENTS, &c.max_vertex_output_components);
  glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &c.max_uniform_block_size);
  glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &c.uniform_buffer_offset_alignment);
  eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (pbuffer != EGL_NO_SURFACE) {
    eglDestroySurface(display_, pbuffer);
  }

  REXLOG_INFO("GlesProvider: EGL {}.{}, {} ({}), {}", mayor, menor, c.renderer, c.vendor, c.version_texto);
  REXLOG_INFO(
      "GlesProvider: GLES {}.{}; clip_control {}, timer_query {}, buffer_storage {}, copy_image {}, s3tc {}, "
      "rgtc {}, norm16 {}, float_linear {}, mirror_clamp {}, anisotropia {}; textura max {}, unidades {}, salidas VS "
      "{}, UBO max {} (alineacion {}); sin superficie {}",
      c.version / 10, c.version % 10, c.clip_control, c.timer_query, c.buffer_storage, c.copy_image,
      c.texture_compression_s3tc, c.texture_compression_rgtc, c.texture_norm16, c.float_linear, c.mirror_clamp,
      c.anisotropia_max, c.max_texture_size, c.max_combined_texture_units,
      c.max_vertex_output_components, c.max_uniform_block_size, c.uniform_buffer_offset_alignment,
      sin_superficie_);
  if (c.version < 31) {
    REXLOG_ERROR("GlesProvider: hace falta OpenGL ES 3.1 como minimo");
    return false;
  }
  return true;
}

EGLContext GlesProvider::CrearContextoCompartido() const {
  const EGLint atributos[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, version_contexto_,
                              EGL_NONE};
  EGLContext contexto = eglCreateContext(display_, config_, raiz_, atributos);
  if (contexto == EGL_NO_CONTEXT) {
    REXLOG_ERROR("GlesProvider: no se pudo crear un contexto compartido ({:#x})", eglGetError());
  }
  return contexto;
}

std::unique_ptr<GlesProvider::ContextoHilo> GlesProvider::CrearContextoHilo() const {
  EGLContext contexto = CrearContextoCompartido();
  if (contexto == EGL_NO_CONTEXT) {
    return nullptr;
  }
  EGLSurface superficie = EGL_NO_SURFACE;
  if (!sin_superficie_) {
    superficie = CrearPbuffer();
    if (superficie == EGL_NO_SURFACE) {
      REXLOG_ERROR("GlesProvider: no se pudo crear el pbuffer del contexto de hilo ({:#x})", eglGetError());
      eglDestroyContext(display_, contexto);
      return nullptr;
    }
  }
  return std::unique_ptr<ContextoHilo>(new ContextoHilo(display_, contexto, superficie));
}

std::unique_ptr<ImmediateDrawer> GlesProvider::CreateImmediateDrawer() {
  return GlesImmediateDrawer::Create(*this);
}

std::unique_ptr<Presenter> GlesProvider::CreatePresenter(Presenter::HostGpuLossCallback host_gpu_loss_callback) {
  return GlesPresenter::Create(host_gpu_loss_callback, this);
}

}  // namespace rex::ui::gles
