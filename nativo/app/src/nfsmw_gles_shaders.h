// nfsmw - OpenGL ES backend: the library's SPIR-V as GLSL ES.
//
// The GLES library (tools/biblioteca_shaders_gles.mjs) is the same SPIR-V as the Vulkan one except for the
// textures: one resource per sampler register (g_Tex2D_<r>, g_Tex3D_<r>, g_TexCube_<r> in sets 0-2 and
// g_Sampler_<r> in set 3, binding = register) instead of descriptor heaps. SPIRV-Cross turns each texture and
// sampler pair into a combined sampler on texture unit dimension * 16 + register, which is where the backend
// binds the register's texture and its sampler object.
//
// The orientation needs no change. The vertex shaders already come with -fvk-invert-y, and GL maps NDC to
// framebuffer rows, gl_FragCoord and texture rows with the same numbers as Vulkan: row 0 is NDC y = -1 in both,
// so the render targets keep the D3D orientation and only the final output to the window is flipped. Depth is
// the exception: Vulkan clips z to [0, 1] and GL to [-1, 1], so without GL_EXT_clip_control the vertex shader
// rewrites z (SPIRV-Cross fixup_clipspace).
//
// Independent of GL so that the whole library can be converted and checked on PC
// (tools/tests/gles_shaders_test.cpp).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace nfsmw::gles {

// Texture units: dimension * kUnidadesPorDimension + register.
inline constexpr uint32_t kUnidadesPorDimension = 16;
inline constexpr uint32_t kDimension2D = 0;
inline constexpr uint32_t kDimension3D = 1;
inline constexpr uint32_t kDimensionCubo = 2;

// Uniform block bindings (set 4 of the SPIR-V): the VS and PS float constants and the shared block.
inline constexpr uint32_t kBloqueVertices = 0;
inline constexpr uint32_t kBloquePixeles = 1;
inline constexpr uint32_t kBloqueCompartidas = 2;

struct OpcionesGlsl {
  uint32_t version = 320;     // GLSL ES version: 310 or 320
  bool clip_control = false;  // GL_EXT_clip_control with GL_ZERO_TO_ONE: z is left as in Vulkan
};

struct SamplerGlsl {
  uint32_t dimension = kDimension2D;
  uint32_t registro = 0;
  uint32_t unidad = 0;
  // Only queried for its size (textureSize): any sampler object will do.
  bool solo_tamano = false;
};

struct ShaderGlsl {
  bool ok = false;
  std::string error;
  // Complete source, with #version. Its specialization constant is SPIRV_CROSS_CONSTANT_ID_0, 0 unless defined
  // (ConEspecializacion).
  std::string fuente;
  std::vector<SamplerGlsl> samplers;
  // Locations of the stage interface (bit = location): PS inputs, or VS outputs that were kept.
  uint64_t ubicaciones = 0;
};

// `salidas_vs`: locations the pixel shader it is linked with reads (its ShaderGlsl::ubicaciones). The translator
// declares every interpolator the microcode exports (up to 22 vec4: TEXCOORD0-15 and COLOR0-5), more than the 16
// that GLES guarantees and Mali offers, so the vertex shader keeps as outputs only those; the rest become
// private variables. Ignored for pixel shaders.
ShaderGlsl ConvertirAGlsl(std::span<const uint32_t> spirv, bool vertices, const OpcionesGlsl& opciones,
                          uint64_t salidas_vs = ~uint64_t(0));

// The source with the specialization constant (g_SpecConstants) set to that value.
std::string ConEspecializacion(const std::string& fuente, uint32_t especializacion);

}  // namespace nfsmw::gles
