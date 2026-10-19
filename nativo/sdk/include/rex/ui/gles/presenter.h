#pragma once
/*
 * OpenGL ES presenter (see gles/provider.h).
 *
 * The guest output is a mailbox of three GL_RGB10_A2 textures, like VulkanPresenter's
 * A2B10G10R10 images. Who refreshes it (the native renderer's ring thread) draws into the texture from its
 * own context, shared with the presenter's: that thread must have a ContextoHilo current. Synchronization
 * between the two contexts is done with sync objects, on the GPU (glWaitSync): the refresh leaves one
 * after writing, which the paint waits for before sampling, and the paint leaves one after sampling,
 * which the next refresh of that texture waits for before writing.
 *
 * Painting happens on the presenter's own thread or on the UI thread, never on both at once (the base
 * class guarantees it): the paint context is made current with the window surface for each paint and
 * released afterwards, so it can change threads.
 *
 * Not yet: UI drawers (ImGui), guest output capture and the FidelityFX effects (only bilinear).
 */

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <android/native_window.h>

#include <rex/ui/presenter.h>

namespace rex::ui::gles {

class GlesProvider;

class GlesPresenter final : public Presenter {
 public:
  static constexpr GLenum kGuestOutputFormat = GL_RGB10_A2;

  class GlesGuestOutputRefreshContext final : public GuestOutputRefreshContext {
   public:
    GlesGuestOutputRefreshContext(bool& is_8bpc_out_ref, GLuint texture, uint32_t width, uint32_t height,
                                  uint64_t version)
        : GuestOutputRefreshContext(is_8bpc_out_ref),
          texture_(texture),
          width_(width),
          height_(height),
          version_(version) {}

    // GL_TEXTURE_2D, kGuestOutputFormat, a single level. Row 0 is the top of the image (the D3D
    // orientation the renderer keeps): the presenter flips it when painting to the window.
    GLuint texture() const { return texture_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    // Changes when the texture is recreated: framebuffers of the refresher's context can be cached by it.
    uint64_t version() const { return version_; }

   private:
    GLuint texture_;
    uint32_t width_;
    uint32_t height_;
    uint64_t version_;
  };

  static std::unique_ptr<GlesPresenter> Create(HostGpuLossCallback host_gpu_loss_callback,
                                               const GlesProvider* provider);
  ~GlesPresenter() override;

  Surface::TypeFlags GetSupportedSurfaceTypes() const override;
  bool CaptureGuestOutput(RawImage& image_out) override;

 protected:
  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(
      Surface& new_surface, uint32_t new_surface_width, uint32_t new_surface_height, bool was_paintable,
      bool& is_vsync_implicit_out) override;
  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override;
  bool RefreshGuestOutputImpl(uint32_t mailbox_index, uint32_t frontbuffer_width,
                              uint32_t frontbuffer_height,
                              std::function<bool(GuestOutputRefreshContext& context)> refresher,
                              bool& is_8bpc_out_ref) override;
  PaintResult PaintAndPresentImpl(bool execute_ui_drawers) override;

 private:
  struct ImagenSalida {
    GLuint textura = 0;
    uint32_t ancho = 0;
    uint32_t alto = 0;
    uint64_t version = 0;
    bool escrita_alguna_vez = false;
    // Under sincronizacion_: the producer's (after writing) and the consumer's (after sampling).
    GLsync escrita = nullptr;
    GLsync leida = nullptr;
  };

  GlesPresenter(HostGpuLossCallback host_gpu_loss_callback, const GlesProvider* provider)
      : Presenter(host_gpu_loss_callback), provider_(provider) {}
  static PaintResult ResultadoDeError(EGLint error);
  bool InicializarIndependienteDeSuperficie();
  bool PrepararPintado();  // with the paint context current
  void DestruirObjetosDePintado();

  const GlesProvider* provider_;
  EGLContext contexto_pintar_ = EGL_NO_CONTEXT;
  EGLSurface superficie_ = EGL_NO_SURFACE;
  ANativeWindow* ventana_ = nullptr;
  uint32_t superficie_ancho_ = 0;
  uint32_t superficie_alto_ = 0;
  bool intervalo_puesto_ = false;  // eglSwapInterval for this surface

  // Objects of the paint context.
  GLuint programa_ = 0;
  GLuint vao_ = 0;
  GLuint sampler_ = 0;

  std::mutex sincronizacion_;
  std::array<ImagenSalida, kGuestOutputMailboxSize> salidas_;
  uint64_t siguiente_version_ = 1;
};

}  // namespace rex::ui::gles
