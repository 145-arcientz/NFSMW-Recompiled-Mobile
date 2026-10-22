#pragma once
// nfsmw - native renderer: half precision in the pixel shaders (nfsmw_nativo_media_precision).
//
// Mali and Adreno do twice the arithmetic at 16 bits and need half the registers, but the library's SPIR-V asks for
// 32 bits everywhere. This marks with RelaxedPrecision the color math: the results of sampling a texture and every
// float operation that comes from them. Vulkan drivers then compute those at 16 bits, and SPIRV-Cross writes them as
// mediump for OpenGL ES. Whatever ends up as a texture coordinate, a depth reference or gl_FragDepth stays at 32 bits,
// and so does everything that does not come from a texture (positions, fog, constants).

#include <cstdint>
#include <vector>

namespace nfsmw::nativo {

// Marks the pixel shader `spirv` in place. Returns how many results were marked (0: unchanged, also for SPIR-V it
// does not understand).
uint32_t RelajarPrecision(std::vector<uint32_t>& spirv);

}  // namespace nfsmw::nativo
