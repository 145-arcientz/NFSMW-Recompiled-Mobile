// nfsmw - native renderer, part C2: what the copies (resolves) need that does not depend on the graphics API.
//
// Shared by the Vulkan render targets (nfsmw_nativo_destinos.cpp) and the OpenGL ES ones
// (nfsmw_nativo_destinos_gles.cpp): the rectangle a copy covers, the address of a texel in a tiled texture and
// the write of a read-back texture to guest memory. The bodies are the ones nfsmw_nativo_destinos.cpp had.

#pragma once

#include "nfsmw_nativo_destinos.h"

#include <rex/graphics/xenos.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace nfsmw::nativo {

// Direct3D 11 16.8 fixed point with rounding, like ui::FloatToD3D11Fixed16p8.
inline int32_t Fijo16p8(float valor) {
  if (!(std::abs(valor) >= 1.0f / 512.0f)) {
    return 0;
  }
  const double escalado = std::clamp(double(valor) * 256.0, -2147483392.0, 2147483392.0);
  return int32_t(std::lround(escalado));
}

// Address of a texel in a 32x32-tiled texture (pipeline/texture/util.cpp:424-436).
inline int32_t DesplazamientoMosaico2D(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_bytes) {
  pitch = (pitch + 31) & ~uint32_t(31);
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_bytes + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bytes;
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// With 4-byte texels, DesplazamientoMosaico2D(x, y, pitch, 2) is
// (((x >> 5) + (y >> 5) * (pitch aligned to 32 >> 5)) << 12) + table[(y & 31) * 32 + (x & 31)]:
// the part inside the 32x32 tile does not depend on the pitch (EscribirTexelsEnGuest).
inline const std::array<uint16_t, 1024>& TablaMosaico2DTexel4() {
  static const std::array<uint16_t, 1024> tabla = [] {
    std::array<uint16_t, 1024> t{};
    for (int32_t y = 0; y < 32; ++y) {
      for (int32_t x = 0; x < 32; ++x) {
        t[size_t(y) * 32 + size_t(x)] = uint16_t(DesplazamientoMosaico2D(x, y, 32, 2));
      }
    }
    return t;
  }();
  return tabla;
}

inline int32_t ExtenderSigno15(uint32_t valor) {
  return int32_t(valor << 17) >> 17;
}

// Fixed color per render target with nfsmw_nativo_diag_borrado: the capture shows which render target reached
// the screen. The same hash as ColorDiagnostico in nfsmw_nativo_destinos.cpp.
inline std::array<float, 4> ColorDiagnosticoRgba(uint32_t base, uint32_t formato, uint32_t pitch) {
  uint32_t h = base * 2654435761u ^ pitch * 2246822519u ^ formato * 3266489917u;
  h ^= h >> 15;
  h *= 2246822519u;
  h ^= h >> 13;
  std::array<float, 4> color{};
  for (uint32_t j = 0; j < 3; ++j) {
    color[j] = float((h >> (j * 8)) & 0xFF) * (1.0f / 255.0f);
  }
  color[3] = 1.0f;
  return color;
}

// Rectangle covered by the copy, as in GetResolveInfo (draw.cpp:787-889). On failure, `causa` is 5 (the copy
// vertices are not 3 2D points) or 6 (empty rectangle), the causes the callers log.
inline bool RectanguloCopia(const RegistrosCopia& reg, uint32_t pitch, rex::memory::Memory* memoria, int32_t& x0,
                            int32_t& y0, int32_t& x1, int32_t& y1, uint32_t& causa) {
  namespace xenos = rex::graphics::xenos;
  const uint32_t tipo = reg.fetch_vertices[0] & 0x3;
  const uint32_t direccion = reg.fetch_vertices[0] >> 2;
  const auto orden = static_cast<xenos::Endian>(reg.fetch_vertices[1] & 0x3);
  const uint32_t tamano = (reg.fetch_vertices[1] >> 2) & 0xFFFFFF;
  if (tipo != uint32_t(xenos::FetchConstantType::kVertex) || tamano != 3 * 2) {
    causa = 5;
    return false;
  }
  const uint8_t* vertices = memoria->TranslatePhysical(direccion * 4);
  const float medio_pixel =
      (reg.pa_su_vtx_cntl & 0x1) == uint32_t(xenos::PixelCenter::kD3DZero) ? 0.5f : 0.0f;
  int32_t fijo[6];
  for (int i = 0; i < 6; ++i) {
    float valor;
    std::memcpy(&valor, vertices + i * 4, sizeof(valor));
    fijo[i] = Fijo16p8(xenos::GpuSwap(valor, orden) + medio_pixel);
  }
  x0 = (std::min({fijo[0], fijo[2], fijo[4]}) + 127) >> 8;
  y0 = (std::min({fijo[1], fijo[3], fijo[5]}) + 127) >> 8;
  x1 = (std::max({fijo[0], fijo[2], fijo[4]}) + 127) >> 8;
  y1 = (std::max({fijo[1], fijo[3], fijo[5]}) + 127) >> 8;

  const int32_t desplazamiento_x = ExtenderSigno15(reg.pa_sc_window_offset & 0x7FFF);
  const int32_t desplazamiento_y = ExtenderSigno15((reg.pa_sc_window_offset >> 16) & 0x7FFF);
  if ((reg.pa_su_sc_mode_cntl >> 16) & 0x1) {  // vtx_window_offset_enable
    x0 += desplazamiento_x;
    y0 += desplazamiento_y;
    x1 += desplazamiento_x;
    y1 += desplazamiento_y;
  }
  // Window scissor (GetScissor without clamping to the pitch).
  int32_t izquierda = int32_t(reg.pa_sc_window_scissor_tl & 0x3FFF);
  int32_t arriba = int32_t((reg.pa_sc_window_scissor_tl >> 16) & 0x3FFF);
  int32_t derecha = int32_t(reg.pa_sc_window_scissor_br & 0x3FFF);
  int32_t abajo = int32_t((reg.pa_sc_window_scissor_br >> 16) & 0x3FFF);
  if (!((reg.pa_sc_window_scissor_tl >> 31) & 0x1)) {  // window_offset_disable
    izquierda += desplazamiento_x;
    arriba += desplazamiento_y;
    derecha += desplazamiento_x;
    abajo += desplazamiento_y;
  }
  izquierda = std::max(izquierda, 0);
  arriba = std::max(arriba, 0);
  derecha = std::max(derecha, izquierda);
  abajo = std::max(abajo, arriba);
  x0 = std::clamp(x0, izquierda, derecha);
  y0 = std::clamp(y0, arriba, abajo);
  x1 = std::clamp(x1, izquierda, derecha);
  y1 = std::clamp(y1, arriba, abajo);
  // D3D9 alinea a 8 (kResolveAlignmentPixels).
  x0 &= ~int32_t(7);
  y0 &= ~int32_t(7);
  x1 = (x1 + 7) & ~int32_t(7);
  y1 = (y1 + 7) & ~int32_t(7);
  const int32_t pitch_alineado = int32_t(pitch & ~uint32_t(7));
  x0 = std::min(x0, pitch_alineado);
  x1 = std::min(x1, pitch_alineado);
  if (x0 >= x1 || y0 >= y1) {
    causa = 6;
    return false;
  }
  return true;
}

// A read-back resolved texture (`datos`, R8G8B8A8 rows of `ancho` texels) to guest memory, in the layout the
// guest reads it with its fetch constant (GpuSwap per word and R8G8B8A8): each texel must give the same
// channels as the resolved texture on the GPU, (B, G, R, A) with copy_dest_swap and (R, G, B, A) without it.
// `base`, `pitch`, `alto_destino` and `info` are RB_COPY_DEST_BASE, the pitch and height of RB_COPY_DEST_PITCH
// and RB_COPY_DEST_INFO; (x0, y0) is the texel of the texture where the rectangle starts.
inline void EscribirTexelsEnGuest(uint8_t* fisica, const uint8_t* datos, uint32_t base_copia, int32_t x0, int32_t y0,
                                  uint32_t ancho, uint32_t alto, uint32_t pitch, uint32_t alto_destino,
                                  uint32_t info) {
  namespace xenos = rex::graphics::xenos;
  const bool intercambio = (info >> 24) & 0x1;
  const uint32_t orden_copia = info & 0x7;  // Endian128: 0-3 as xenos::Endian
  const auto orden = orden_copia <= 3 ? static_cast<xenos::Endian>(orden_copia) : xenos::Endian::k8in32;
  // Each texel is a permutation of its 4 bytes (R/B swap plus GpuSwap): it is computed once with
  // marked bytes. Addresses come from the tile and the tiling table, and the guest's physical memory
  // is contiguous.
  const uint32_t permutacion = xenos::GpuSwap(intercambio ? uint32_t(0x03000102) : uint32_t(0x03020100), orden);
  const uint8_t b0 = uint8_t(permutacion), b1 = uint8_t(permutacion >> 8), b2 = uint8_t(permutacion >> 16),
                b3 = uint8_t(permutacion >> 24);
  const uint64_t base = uint64_t(base_copia & 0x1FFFFFFF);
  const auto& tabla = TablaMosaico2DTexel4();
  const uint64_t baldosas_por_fila = ((pitch + 31) & ~uint32_t(31)) >> 5;
  for (uint32_t j = 0; j < alto; ++j) {
    const uint32_t ty = uint32_t(y0) + j;
    if (alto_destino && ty >= alto_destino) {
      break;
    }
    const uint64_t fila = base + ((uint64_t(ty >> 5) * baldosas_por_fila) << 12);
    const uint16_t* const local = tabla.data() + size_t(ty & 31) * 32;
    const uint8_t* s = datos + size_t(j) * ancho * 4;
    for (uint32_t i = 0; i < ancho; ++i, s += 4) {
      const uint32_t tx = uint32_t(x0) + i;
      if (tx >= pitch) {
        break;
      }
      const uint64_t direccion = fila + (uint64_t(tx >> 5) << 12) + local[tx & 31];
      if (direccion + 4 > 0x20000000) {
        continue;
      }
      const uint8_t t0 = s[b0], t1 = s[b1], t2 = s[b2], t3 = s[b3];
      uint8_t* const d = fisica + direccion;
      d[0] = t0;
      d[1] = t1;
      d[2] = t2;
      d[3] = t3;
    }
  }
}

}  // namespace nfsmw::nativo
