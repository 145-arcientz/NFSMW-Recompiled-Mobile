// clang++ -std=c++20 tools/tests/texturas_bc_test.cpp -o texturas_bc_test
#include "../../app/src/nfsmw_texturas_bc.h"
#include "../../app/src/nfsmw_gpu_compatibilidad.h"
#include <cstdio>
#include <cstdlib>

using nfsmw::bc::Formato;
void Check(bool ok, const char* message) {
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

void IndicesAlfa(uint8_t* bloque) {
  uint64_t indices = 0;
  for (uint32_t i = 0; i < 16; ++i) indices |= uint64_t(i % 8) << (i * 3);
  for (uint32_t i = 0; i < 6; ++i) bloque[i + 2] = uint8_t(indices >> (i * 8));
}

int main() {
  // All four color selectors, including the two interpolants.
  const uint8_t bc1[] = {0x00, 0xF8, 0x1F, 0x00, 0xE4, 0xE4, 0xE4, 0xE4};
  uint8_t pixels[64]{};
  nfsmw::bc::Bloque(Formato::BC1, bc1, pixels);
  const uint8_t colors[4][4] = {{255,0,0,255}, {0,0,255,255}, {170,0,85,255}, {85,0,170,255}};
  for (uint32_t i = 0; i < 16; ++i)
    Check(std::equal(pixels + i * 4, pixels + i * 4 + 4, colors[i % 4]), "BC1 colors/selectors");
  uint8_t transparent[] = {0,0,255,255,255,255,255,255};
  nfsmw::bc::Bloque(Formato::BC1, transparent, pixels);
  Check(std::all_of(pixels, pixels + 64, [](auto c) { return c == 0; }), "BC1 transparent index");

  // BC2 always uses four opaque color entries, even with reversed endpoints.
  uint8_t bc2[16] = {0x10,0x32,0x54,0x76,0x98,0xBA,0xDC,0xFE};
  std::copy_n(transparent, 8, bc2 + 8);
  nfsmw::bc::Bloque(Formato::BC2, bc2, pixels);
  for (uint32_t i = 0; i < 16; ++i) {
    Check(pixels[i*4] == 170 && pixels[i*4+1] == 170 && pixels[i*4+2] == 170, "BC2 reversed colors");
    Check(pixels[i*4+3] == i * 17, "BC2 four-bit alpha order");
  }

  // Alpha selectors cross byte boundaries. Test both BC3/4 alpha tables.
  uint8_t bc3[16] = {210,0};
  IndicesAlfa(bc3);
  std::copy_n(transparent, 8, bc3 + 8);
  nfsmw::bc::Bloque(Formato::BC3, bc3, pixels);
  const uint8_t alpha8[] = {210,0,180,150,120,90,60,30};
  for (uint32_t i = 0; i < 16; ++i) {
    Check(pixels[i*4+3] == alpha8[i%8] && pixels[i*4] == 170, "BC3 alpha and opaque colors");
  }
  uint8_t bc4[8] = {0,200};
  IndicesAlfa(bc4);
  nfsmw::bc::Bloque(Formato::BC4, bc4, pixels);
  const uint8_t alpha6[] = {0,200,40,80,120,160,0,255};
  for (uint32_t i = 0; i < 16; ++i) Check(pixels[i] == alpha6[i%8], "BC4 six-step alpha");
  uint8_t bc5[16] = {0,200};
  IndicesAlfa(bc5);
  bc5[8] = 210;
  IndicesAlfa(bc5 + 8);
  nfsmw::bc::Bloque(Formato::BC5, bc5, pixels);
  for (uint32_t i = 0; i < 16; ++i) {
    Check(pixels[i*2] == alpha6[i%8] && pixels[i*2+1] == alpha8[i%8], "BC5 independent RG channels");
  }

  // A 7x5 image needs four input blocks. Edge texels must be clipped,
  // and each input block must go to its spatial quadrant.
  std::vector<uint8_t> input(32, 0), output;
  for (uint32_t i = 0; i < 4; ++i) input[i * 8] = uint8_t(10 + i);
  std::array<uint32_t, 16> offsets{};
  Check(nfsmw::bc::Convertir(Formato::BC4, input, 7,5,1,1,output,offsets), "odd dimensions decode");
  Check(output.size() == 35, "odd dimensions output size");
  for (uint32_t y = 0; y < 5; ++y) for (uint32_t x = 0; x < 7; ++x)
    Check(output[y*7+x] == 10 + (y/4)*2 + x/4, "edge block placement");

  // Six cubemap faces, four mips (8x4, 4x2, 2x1, 1x1).
  // Each mip/face has a distinct solid value; tiny mips remain single BC blocks.
  input.clear();
  for (uint32_t n = 0; n < 4; ++n) {
    const uint32_t w = std::max(8u >> n,1u), h = std::max(4u >> n,1u);
    for (uint32_t face = 0; face < 6; ++face) {
      for (uint32_t b = 0; b < ((w+3)/4)*((h+3)/4); ++b) {
        const uint8_t block[8] = {uint8_t(20 + n*6 + face),0,0,0,0,0,0,0};
        input.insert(input.end(), block, block+8);
      }
    }
  }
  const auto original = input;
  Check(nfsmw::bc::Convertir(Formato::BC4,input,8,4,6,4,output,offsets), "cubemap/mip decode");
  Check(input == original, "input must stay untouched");
  for (uint32_t n = 0; n < 4; ++n) {
    const uint32_t area = std::max(8u >> n,1u) * std::max(4u >> n,1u);
    Check(offsets[n] % 4 == 0, "Vulkan mip offset alignment");
    for (uint32_t face = 0; face < 6; ++face) for (uint32_t i = 0; i < area; ++i)
      Check(output[offsets[n] + area*face + i] == 20 + n*6 + face, "mip/face ordering");
  }
  // A one-byte mip followed by another one-byte mip needs padding.
  input.assign(32,0);
  Check(nfsmw::bc::Convertir(Formato::BC4,input,4,4,1,4,output,offsets), "tiny mip decode");
  Check(offsets[3] == 24 && output.size() == 25, "tiny R8 mip must align to four bytes");
  const auto previous_output = output;
  const auto previous_offsets = offsets;
  input.pop_back();
  Check(!nfsmw::bc::Convertir(Formato::BC4,input,4,4,1,4,output,offsets), "reject truncated input");
  Check(output == previous_output && offsets == previous_offsets, "failed decode must preserve layout/data");
  Check(!nfsmw::bc::Convertir(Formato::BC1,{},UINT32_MAX,UINT32_MAX,6,16,output,offsets), "reject size overflow");
  Check(!nfsmw::bc::Convertir(Formato::BC1,{},0,4,1,1,output,offsets), "reject empty dimensions");
  Check(!nfsmw::bc::Convertir(Formato::BC1,{},4,4,1,17,output,offsets), "reject excess mips");

  using nfsmw::compatibilidad::OclusionGpu;
  Check(!OclusionGpu("auto",true,0x13B5), "automatic Mali protection");
  Check(OclusionGpu("auto",true,0x5143), "Adreno keeps queries");
  Check(OclusionGpu("auto",false,0x13B5), "other platforms keep queries");
  Check(!OclusionGpu("off",false,0x5143), "manual safety on any GPU");
  Check(OclusionGpu("on",true,0x13B5), "explicit experimental override");
  std::puts("PASS: BC1-5 colors/alpha, edges, mip/cubemap layout, alignment, invalid input and GPU policy");
}
