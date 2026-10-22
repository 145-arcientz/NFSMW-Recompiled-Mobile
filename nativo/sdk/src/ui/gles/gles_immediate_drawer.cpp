/*
 * OpenGL ES immediate drawer (see gles/immediate_drawer.h).
 */

#include <rex/ui/gles/immediate_drawer.h>

#include <cstddef>
#include <cstring>

#include <EGL/egl.h>

#include <rex/logging.h>
#include <rex/ui/gles/provider.h>

namespace rex::ui::gles {

namespace {

constexpr char kVertices[] = R"(#version 300 es
layout(location = 0) in vec2 posicion;
layout(location = 1) in vec2 coordenadas;
layout(location = 2) in vec4 color_vertice;
// From the coordinate space (origin at the top-left corner, y downwards) to GL's clip space (y upwards).
uniform vec2 escala;
out vec2 uv;
out vec4 color_variable;
void main() {
  gl_Position = vec4(posicion.x * escala.x - 1.0, 1.0 - posicion.y * escala.y, 0.0, 1.0);
  uv = coordenadas;
  color_variable = color_vertice;
}
)";

constexpr char kPixeles[] = R"(#version 300 es
precision mediump float;
in vec2 uv;
in vec4 color_variable;
uniform sampler2D textura;
uniform bool texturizado;
layout(location = 0) out vec4 color;
void main() {
  color = color_variable;
  if (texturizado) {
    color *= texture(textura, uv);
  }
}
)";

GLuint Compilar(GLenum tipo, const char* fuente) {
  const GLuint shader = glCreateShader(tipo);
  glShaderSource(shader, 1, &fuente, nullptr);
  glCompileShader(shader);
  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char registro[1024] = {};
    glGetShaderInfoLog(shader, sizeof(registro), nullptr, registro);
    REXLOG_ERROR("GlesImmediateDrawer: el shader no compila: {}", registro);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

}  // namespace

GlesImmediateDrawer::GlesImmediateTexture::GlesImmediateTexture(uint32_t width, uint32_t height,
                                                                ImmediateTextureFilter filter, bool is_repeated,
                                                                const uint8_t* data,
                                                                std::shared_ptr<Papelera> papelera)
    : ImmediateTexture(width, height), papelera_(std::move(papelera)), filtro_(filter), repetida_(is_repeated) {
  pixeles_.resize(size_t(width) * height * 4);
  if (data) {
    std::memcpy(pixeles_.data(), data, pixeles_.size());
  }
}

GlesImmediateDrawer::GlesImmediateTexture::~GlesImmediateTexture() {
  if (textura_) {
    std::lock_guard<std::mutex> cerrojo(papelera_->cerrojo);
    papelera_->texturas.push_back(textura_);
  }
}

GLuint GlesImmediateDrawer::GlesImmediateTexture::Textura() {
  if (textura_ || !width || !height) {
    return textura_;
  }
  glGenTextures(1, &textura_);
  glBindTexture(GL_TEXTURE_2D, textura_);
  glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, GLsizei(width), GLsizei(height));
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GLsizei(width), GLsizei(height), GL_RGBA, GL_UNSIGNED_BYTE,
                  pixeles_.data());
  const GLint filtro = filtro_ == ImmediateTextureFilter::kLinear ? GL_LINEAR : GL_NEAREST;
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filtro);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filtro);
  const GLint envoltura = repetida_ ? GL_REPEAT : GL_CLAMP_TO_EDGE;
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, envoltura);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, envoltura);
  pixeles_.clear();
  pixeles_.shrink_to_fit();
  return textura_;
}

std::unique_ptr<GlesImmediateDrawer> GlesImmediateDrawer::Create(const GlesProvider& provider) {
  (void)provider;  // everything is created lazily, inside the first paint
  return std::unique_ptr<GlesImmediateDrawer>(new GlesImmediateDrawer());
}

GlesImmediateDrawer::~GlesImmediateDrawer() {
  // Usually destroyed on the UI thread with no context: the GL objects are left to the context's destruction.
  if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
    return;
  }
  if (programa_) glDeleteProgram(programa_);
  if (vao_) glDeleteVertexArrays(1, &vao_);
  if (vertices_) glDeleteBuffers(1, &vertices_);
  if (indices_) glDeleteBuffers(1, &indices_);
}

std::unique_ptr<ImmediateTexture> GlesImmediateDrawer::CreateTexture(uint32_t width, uint32_t height,
                                                                     ImmediateTextureFilter filter,
                                                                     bool is_repeated, const uint8_t* data) {
  return std::make_unique<GlesImmediateTexture>(width, height, filter, is_repeated, data, papelera_);
}

bool GlesImmediateDrawer::Preparar() {
  if (preparado_ || fallido_) {
    return preparado_;
  }
  const GLuint vs = Compilar(GL_VERTEX_SHADER, kVertices);
  const GLuint ps = vs ? Compilar(GL_FRAGMENT_SHADER, kPixeles) : 0;
  if (!vs || !ps) {
    if (vs) glDeleteShader(vs);
    fallido_ = true;
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
  if (!ok) {
    char registro[1024] = {};
    glGetProgramInfoLog(programa_, sizeof(registro), nullptr, registro);
    REXLOG_ERROR("GlesImmediateDrawer: el programa no enlaza: {}", registro);
    glDeleteProgram(programa_);
    programa_ = 0;
    fallido_ = true;
    return false;
  }
  uniforme_escala_ = glGetUniformLocation(programa_, "escala");
  uniforme_texturizado_ = glGetUniformLocation(programa_, "texturizado");
  glUseProgram(programa_);
  glUniform1i(glGetUniformLocation(programa_, "textura"), 0);
  glUseProgram(0);
  glGenVertexArrays(1, &vao_);
  glGenBuffers(1, &vertices_);
  glGenBuffers(1, &indices_);
  glBindVertexArray(vao_);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glEnableVertexAttribArray(2);
  glBindVertexArray(0);
  preparado_ = true;
  REXLOG_INFO("GlesImmediateDrawer: listo");
  return true;
}

void GlesImmediateDrawer::Begin(UIDrawContext& ui_draw_context, float coordinate_space_width,
                                float coordinate_space_height) {
  ImmediateDrawer::Begin(ui_draw_context, coordinate_space_width, coordinate_space_height);
  dibujando_ = false;
  if (eglGetCurrentContext() == EGL_NO_CONTEXT || !Preparar()) {
    return;
  }
  {
    std::vector<GLuint> borrar;
    {
      std::lock_guard<std::mutex> cerrojo(papelera_->cerrojo);
      borrar.swap(papelera_->texturas);
    }
    if (!borrar.empty()) {
      glDeleteTextures(GLsizei(borrar.size()), borrar.data());
    }
  }
  const uint32_t ancho = ui_draw_context.render_target_width();
  const uint32_t alto = ui_draw_context.render_target_height();
  if (!ancho || !alto) {
    return;
  }
  glViewport(0, 0, GLsizei(ancho), GLsizei(alto));
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_CULL_FACE);
  glEnable(GL_BLEND);
  glBlendEquation(GL_FUNC_ADD);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glEnable(GL_SCISSOR_TEST);
  glUseProgram(programa_);
  glUniform2f(uniforme_escala_, 2.0f / this->coordinate_space_width(), 2.0f / this->coordinate_space_height());
  glBindVertexArray(vao_);
  glActiveTexture(GL_TEXTURE0);
  glBindSampler(0, 0);
  dibujando_ = true;
}

void GlesImmediateDrawer::ApuntarVertices(int base) {
  if (base == base_apuntada_) {
    return;
  }
  // The base vertex goes in the attribute offsets: glDrawElementsBaseVertex is only in GLES 3.2.
  const auto desplazamiento = [base](size_t campo) {
    return reinterpret_cast<const void*>(size_t(base) * sizeof(ImmediateVertex) + campo);
  };
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(ImmediateVertex), desplazamiento(offsetof(ImmediateVertex, x)));
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(ImmediateVertex), desplazamiento(offsetof(ImmediateVertex, u)));
  glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(ImmediateVertex),
                        desplazamiento(offsetof(ImmediateVertex, color)));
  base_apuntada_ = base;
}

void GlesImmediateDrawer::BeginDrawBatch(const ImmediateDrawBatch& batch) {
  if (!dibujando_) {
    return;
  }
  glBindBuffer(GL_ARRAY_BUFFER, vertices_);
  // Orphaning: the driver gives a new allocation if the GPU still reads the previous one.
  glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(sizeof(ImmediateVertex) * size_t(batch.vertex_count)), batch.vertices,
               GL_STREAM_DRAW);
  base_apuntada_ = -1;
  ApuntarVertices(0);
  lote_con_indices_ = batch.indices && batch.index_count > 0;
  if (lote_con_indices_) {
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(sizeof(uint16_t) * size_t(batch.index_count)), batch.indices,
                 GL_STREAM_DRAW);
  }
}

void GlesImmediateDrawer::Draw(const ImmediateDraw& draw) {
  if (!dibujando_ || draw.count <= 0) {
    return;
  }
  uint32_t izquierda, arriba, ancho, alto;
  if (!ScissorToRenderTarget(draw, izquierda, arriba, ancho, alto)) {
    return;
  }
  // The scissor comes from the top-left corner; GL's from the bottom-left one.
  const uint32_t alto_destino = ui_draw_context()->render_target_height();
  glScissor(GLint(izquierda), GLint(alto_destino - arriba - alto), GLsizei(ancho), GLsizei(alto));
  GLuint textura = 0;
  if (draw.texture) {
    textura = static_cast<GlesImmediateTexture*>(draw.texture)->Textura();
  }
  glUniform1i(uniforme_texturizado_, textura != 0);
  glBindTexture(GL_TEXTURE_2D, textura);
  const GLenum modo = draw.primitive_type == ImmediatePrimitiveType::kLines ? GL_LINES : GL_TRIANGLES;
  if (lote_con_indices_) {
    ApuntarVertices(draw.base_vertex);
    glDrawElements(modo, draw.count, GL_UNSIGNED_SHORT,
                   reinterpret_cast<const void*>(size_t(draw.index_offset) * sizeof(uint16_t)));
  } else {
    ApuntarVertices(0);
    glDrawArrays(modo, draw.base_vertex, draw.count);
  }
}

void GlesImmediateDrawer::EndDrawBatch() {}

void GlesImmediateDrawer::End() {
  if (dibujando_) {
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glUseProgram(0);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    dibujando_ = false;
  }
  ImmediateDrawer::End();
}

}  // namespace rex::ui::gles
