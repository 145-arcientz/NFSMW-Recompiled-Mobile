#pragma once
/*
 * OpenGL ES immediate drawer: the Dear ImGui overlays (XAM dialogs such as the profile name keyboard, the debug
 * and settings overlays, achievement toasts) over the presenter's output.
 *
 * Drawing happens inside GlesPresenter::PaintAndPresentImpl, with the paint context current and the window
 * surface bound. Textures, however, are created and destroyed on the UI thread, which has no GL context: a
 * texture keeps a copy of its pixels and is uploaded the first time it is drawn, and destroying one only queues
 * its GL name, which the next Begin deletes.
 */

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <GLES3/gl32.h>

#include <rex/ui/immediate_drawer.h>

namespace rex::ui::gles {

class GlesProvider;

class GlesImmediateDrawer final : public ImmediateDrawer {
 public:
  // GL names whose deletion is pending: shared with the textures, which may outlive the drawer.
  struct Papelera {
    std::mutex cerrojo;
    std::vector<GLuint> texturas;
  };

  class GlesImmediateTexture final : public ImmediateTexture {
   public:
    GlesImmediateTexture(uint32_t width, uint32_t height, ImmediateTextureFilter filter, bool is_repeated,
                         const uint8_t* data, std::shared_ptr<Papelera> papelera);
    ~GlesImmediateTexture() override;

    // With a context current: creates and uploads the texture the first time.
    GLuint Textura();

   private:
    std::shared_ptr<Papelera> papelera_;
    ImmediateTextureFilter filtro_;
    bool repetida_;
    std::vector<uint8_t> pixeles_;
    GLuint textura_ = 0;
  };

  static std::unique_ptr<GlesImmediateDrawer> Create(const GlesProvider& provider);
  ~GlesImmediateDrawer() override;

  std::unique_ptr<ImmediateTexture> CreateTexture(uint32_t width, uint32_t height, ImmediateTextureFilter filter,
                                                  bool is_repeated, const uint8_t* data) override;

  void Begin(UIDrawContext& ui_draw_context, float coordinate_space_width,
             float coordinate_space_height) override;
  void BeginDrawBatch(const ImmediateDrawBatch& batch) override;
  void Draw(const ImmediateDraw& draw) override;
  void EndDrawBatch() override;
  void End() override;

 private:
  GlesImmediateDrawer() : papelera_(std::make_shared<Papelera>()) {}
  bool Preparar();  // with the paint context current
  void ApuntarVertices(int base);

  std::shared_ptr<Papelera> papelera_;
  bool preparado_ = false;
  bool fallido_ = false;
  bool dibujando_ = false;  // between a Begin that could draw and its End
  bool lote_con_indices_ = false;
  int base_apuntada_ = -1;
  GLuint programa_ = 0;
  GLint uniforme_escala_ = -1;
  GLint uniforme_texturizado_ = -1;
  GLuint vao_ = 0;
  GLuint vertices_ = 0;
  GLuint indices_ = 0;
};

}  // namespace rex::ui::gles
