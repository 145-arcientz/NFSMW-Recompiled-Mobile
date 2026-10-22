#pragma once
/*
 * OpenGL ES 3.1/3.2 graphics provider over EGL (Android), for the native renderer's GLES backend
 * (docs/motor-nativo-gles.md in NFSMW-Recompiled).
 *
 * GL objects belong to a context and a context is current on one thread at a time, so the provider keeps
 * a root context that is never made current and that every other one shares objects with: the
 * presenter's (made current by whichever thread paints) and one per rendering thread (CrearContextoHilo).
 * Textures, buffers, programs and sync objects are shared; framebuffers and vertex arrays are not.
 */

#include <cstdint>
#include <memory>
#include <string>

#include <EGL/egl.h>

#include <rex/ui/graphics_provider.h>

namespace rex::ui::gles {

class GlesProvider final : public GraphicsProvider {
 public:
  struct Capacidades {
    uint32_t version = 0;  // 31 or 32
    bool clip_control = false;          // GL_EXT_clip_control
    bool timer_query = false;           // GL_EXT_disjoint_timer_query
    bool buffer_storage = false;        // GL_EXT_buffer_storage
    bool copy_image = false;            // core in 3.2, GL_EXT_copy_image in 3.1
    bool texture_compression_s3tc = false;  // GL_EXT_texture_compression_s3tc (BC1-3)
    bool texture_compression_rgtc = false;  // GL_EXT_texture_compression_rgtc (BC4-5)
    bool texture_norm16 = false;            // GL_EXT_texture_norm16 (R16, RG16, RGBA16 unorm)
    bool float_linear = false;              // GL_OES_texture_float_linear (filtering of 32-bit floats)
    bool mirror_clamp = false;              // GL_EXT_texture_mirror_clamp_to_edge
    float anisotropia_max = 0.0f;           // GL_EXT_texture_filter_anisotropic; 0 = without it
    int32_t max_texture_size = 0;
    int32_t max_combined_texture_units = 0;
    int32_t max_vertex_output_components = 0;
    int32_t max_uniform_block_size = 0;
    int32_t uniform_buffer_offset_alignment = 0;
    std::string renderer;
    std::string vendor;
    std::string version_texto;
  };

  // One context per thread other than the presenter's, shared with it. It is created current on no thread:
  // HacerActual on the thread that will use it, before any GL call there.
  class ContextoHilo {
   public:
    ~ContextoHilo();
    ContextoHilo(const ContextoHilo&) = delete;
    ContextoHilo& operator=(const ContextoHilo&) = delete;
    bool HacerActual();
    void Soltar();

   private:
    friend class GlesProvider;
    ContextoHilo(EGLDisplay display, EGLContext contexto, EGLSurface superficie)
        : display_(display), contexto_(contexto), superficie_(superficie) {}
    EGLDisplay display_;
    EGLContext contexto_;
    EGLSurface superficie_;  // 1x1 pbuffer, or EGL_NO_SURFACE with EGL_KHR_surfaceless_context
  };

  // nullptr, with the cause in the log, if there is no usable OpenGL ES 3.1 or newer.
  static std::unique_ptr<GlesProvider> Create();
  ~GlesProvider() override;

  std::unique_ptr<Presenter> CreatePresenter(
      Presenter::HostGpuLossCallback host_gpu_loss_callback =
          Presenter::FatalErrorHostGpuLossCallback) override;
  // Not yet: without an immediate drawer the app does not set up the ImGui overlays.
  std::unique_ptr<ImmediateDrawer> CreateImmediateDrawer() override;

  std::unique_ptr<ContextoHilo> CrearContextoHilo() const;
  // A new context shared with the root one (the presenter's), current on no thread. EGL_NO_CONTEXT on failure.
  EGLContext CrearContextoCompartido() const;

  EGLDisplay display() const { return display_; }
  EGLConfig config() const { return config_; }
  const Capacidades& capacidades() const { return capacidades_; }

 private:
  GlesProvider() = default;
  bool Inicializar();
  EGLSurface CrearPbuffer() const;

  EGLDisplay display_ = EGL_NO_DISPLAY;
  EGLConfig config_ = nullptr;
  EGLContext raiz_ = EGL_NO_CONTEXT;
  EGLint version_contexto_ = 0;  // minor version the contexts are created with
  bool sin_superficie_ = false;  // EGL_KHR_surfaceless_context
  Capacidades capacidades_;
};

}  // namespace rex::ui::gles
