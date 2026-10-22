// nfsmw - native renderer, parts C3-C6: what the draws need that does not depend on the graphics API.
//
// Shared by the Vulkan draws (nfsmw_nativo_dibujos.cpp) and the OpenGL ES ones (nfsmw_nativo_dibujos_gles.cpp):
//   - the Xenos mip layout arithmetic, the tiling, the fast untiling and the byte swap (bodies moved as they
//     were from nfsmw_nativo_dibujos.cpp);
//   - the vertex input helpers (XenosRecomp's locations and the g_InputRemap codes);
//   - DescribirTextura and LeerDatosTextura: a texture's layout from its fetch constant and its levels read from
//     guest memory into host layout, with the same rules as PrepararTextura in nfsmw_nativo_dibujos.cpp (which
//     keeps its own copy of that path, interleaved with its hashing threads).

#pragma once

#include "nfsmw_nativo_copias_comun.h"
#include "nfsmw_texturas_bc.h"

#include <rex/graphics/xenos.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace nfsmw::nativo {

constexpr uint32_t kRemapeoIdentidad = 0xFFF;

inline float Flotante(uint32_t valor) {
  return std::bit_cast<float>(valor);
}

// Arithmetic of the Xenos mip level layout (pipeline/texture/util.cpp: GetPackedMipLevel,
// GetPackedMipOffset and GetGuestTextureLayout).
inline uint32_t Log2Techo(uint32_t v) {
  return v <= 1 ? 0 : 32 - uint32_t(std::countl_zero(v - 1));
}

inline uint32_t Log2Suelo(uint32_t v) {
  return v ? 31 - uint32_t(std::countl_zero(v)) : 0;
}

// First level of the packed tail: once the short side is 16 texels or less.
inline uint32_t NivelEmpaquetado(uint32_t ancho, uint32_t alto) {
  const uint32_t l = Log2Techo(std::min(ancho, alto));
  return l > 4 ? l - 4 : 0;
}

// Blocks from the start of the packed tail to a level of a 2D texture; 0 if the level is not packed.
inline void DesplazamientoEmpaquetado(uint32_t ancho, uint32_t alto, uint32_t bloque, uint32_t nivel, uint32_t& x,
                                      uint32_t& y) {
  const uint32_t l2_ancho = Log2Techo(ancho);
  const uint32_t l2_alto = Log2Techo(alto);
  const uint32_t l2 = std::min(l2_ancho, l2_alto);
  x = 0;
  y = 0;
  if (l2 > 4 + nivel) {
    return;
  }
  const uint32_t base = l2 > 4 ? l2 - 4 : 0;
  const uint32_t m = nivel - base;
  if (m < 3) {
    if (l2_ancho > l2_alto) {
      y = 16u >> m;  // wider than tall: levels are stacked vertically
    } else {
      x = 16u >> m;
    }
  } else if (l2_ancho > l2_alto) {
    x = (1u << (l2_ancho - base)) >> (m - 2);
  } else {
    y = (1u << (l2_alto - base)) >> (m - 2);
  }
  x /= bloque;
  y /= bloque;
}

// Fast untiling. During race stutters the ring spent ~30 ms preparing 5 MB of new textures (about 6 ms
// per MB): LeerNivel called DesplazamientoMosaico2D and a variable-size memcpy per block. Here, with the
// block size fixed at compile time:
//   - what depends on the row (y) is computed once per row;
//   - within a 16-byte group of the tiling the blocks are contiguous in the source (only the 4 low bits
//     of micro change), so 16 bytes are copied at once (8 for textures with 1 byte per block).
// It gives exactly the same addresses as DesplazamientoMosaico2D (checked block by block on PC).
template <uint32_t kLog2>
void DesenmosaicarNivel(const uint8_t* origen, uint32_t pitch, uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by,
                        uint32_t bx_host, uint8_t* destino) {
  constexpr uint32_t kBytes = 1u << kLog2;
  constexpr uint32_t kGrupo = (16u >> kLog2) < 8u ? (16u >> kLog2) : 8u;  // blocks contiguous in the source
  const int32_t macros_fila = int32_t(((pitch + 31) & ~uint32_t(31)) >> 5);
  for (uint32_t fila = 0; fila < by; ++fila) {
    const int32_t y = int32_t(oy + fila);
    const int32_t macro_y = (y >> 5) * macros_fila;
    const int32_t micro_y = (y & 0xE) << 2;
    const int32_t y1 = (y & 1) << 4;
    const int32_t y16 = (y & 16) << 7;
    const int32_t y8 = (y & 8) >> 2;
    uint8_t* salida = destino + size_t(fila) * bx_host * kBytes;
    uint32_t columna = 0;
    while (columna < bx) {
      const int32_t x = int32_t(ox + columna);
      const int32_t macro = ((x >> 5) + macro_y) << (kLog2 + 7);
      const int32_t micro = ((x & 7) + micro_y) << kLog2;
      const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + y1;
      const int32_t desplazamiento = ((offset & ~0x1FF) << 3) + y16 + ((offset & 0x1C0) << 2) +
                                     (((y8 + (x >> 3)) & 3) << 6) + (offset & 0x3F);
      if (kGrupo > 1 && (uint32_t(x) % kGrupo) == 0 && columna + kGrupo <= bx) {
        std::memcpy(salida + size_t(columna) * kBytes, origen + desplazamiento, kGrupo * kBytes);
        columna += kGrupo;
      } else {
        std::memcpy(salida + size_t(columna) * kBytes, origen + desplazamiento, kBytes);
        ++columna;
      }
    }
  }
}

// Byte swap of a whole texture in one go (instead of GpuSwap word by word, with the switch on the order
// inside the loop). Same result as GpuSwap: for 16-bit units only k8in16 changes anything; for 32-bit
// units, k8in16, k8in32 and k16in32. It only touches complete units, like the plain loop.
inline void CambiarOrdenBytes(uint8_t* datos, size_t bytes, uint32_t unidad, uint32_t orden) {
  constexpr uint32_t k8in16 = 1, k8in32 = 2, k16in32 = 3;  // xenos::Endian
  size_t n = unidad == 2 ? bytes & ~size_t(1) : bytes & ~size_t(3);
  if ((unidad == 2 && orden != k8in16) || (unidad != 2 && unidad != 4) || orden == 0) {
    return;
  }
  size_t i = 0;
#if defined(__aarch64__)
  for (; i + 16 <= n; i += 16) {
    const uint8x16_t v = vld1q_u8(datos + i);
    uint8x16_t r;
    if (unidad == 2 || orden == k8in16) {
      r = vrev16q_u8(v);
    } else if (orden == k8in32) {
      r = vrev32q_u8(v);
    } else {
      r = vreinterpretq_u8_u16(vrev32q_u16(vreinterpretq_u16_u8(v)));
    }
    vst1q_u8(datos + i, r);
  }
#endif
  if (unidad == 2 || orden == k8in16) {
    for (; i + 2 <= n; i += 2) {
      std::swap(datos[i], datos[i + 1]);
    }
  } else if (orden == k8in32) {
    for (; i + 4 <= n; i += 4) {
      std::swap(datos[i], datos[i + 3]);
      std::swap(datos[i + 1], datos[i + 2]);
    }
  } else if (orden == k16in32) {
    for (; i + 4 <= n; i += 4) {
      std::swap(datos[i], datos[i + 2]);
      std::swap(datos[i + 1], datos[i + 3]);
    }
  }
}

// The same for a 3D texture tiled in 32x32x4 blocks, copied from GetTiledOffset3D
// (graphics/pipeline/texture/util.cpp:438-459). pitch and height in blocks.
inline int32_t DesplazamientoMosaico3D(int32_t x, int32_t y, int32_t z, uint32_t pitch, uint32_t alto,
                                       uint32_t log2_bytes) {
  pitch = (pitch + 31) & ~uint32_t(31);
  alto = (alto + 31) & ~uint32_t(31);
  const int32_t macro_exterior = ((y >> 4) + (z >> 2) * int32_t(alto >> 4)) * int32_t(pitch >> 5);
  const int32_t macro = ((((x >> 5) + macro_exterior) << (log2_bytes + 6)) & 0xFFFFFFF) << 1;
  const int32_t micro = (((x & 7) + ((y & 6) << 2)) << (log2_bytes + 6)) >> 6;
  const int32_t exterior = ((y >> 3) + (z >> 2)) & 1;
  const int32_t offset1 = exterior + ((((x >> 3) + (exterior << 1)) & 3) << 1);
  const int32_t offset2 = ((macro + (micro & ~15)) << 1) + (micro & 15) +
                          ((z & 3) << (log2_bytes + 6)) + ((y & 1) << 4);
  int32_t direccion = (offset1 & 1) << 3;
  direccion += (offset2 >> 6) & 7;
  direccion <<= 3;
  direccion += offset1 & ~1;
  direccion <<= 2;
  direccion += offset2 & ~511;
  direccion <<= 3;
  direccion += offset2 & 63;
  return direccion;
}

// USAGE_LOCATIONS de XenosRecomp (shader_recompiler.cpp).
inline int32_t UbicacionDeUso(uint8_t uso, uint8_t indice) {
  switch (uso) {
    case 0:  // posicion
      return indice == 0 ? 0 : (indice == 1 ? 15 : -1);
    case 3:  // normal
      return indice == 0 ? 1 : -1;
    case 6:  // tangente
      return indice == 0 ? 2 : -1;
    case 7:  // binormal
      return indice == 0 ? 3 : -1;
    case 5:  // texcoord
      return indice < 4 ? 4 + indice : (indice < 8 ? 12 + (indice - 4) : -1);
    case 10:  // color
      return indice == 0 ? 8 : (indice == 1 ? 11 : -1);
    case 2:  // indices de mezcla
      return indice == 0 ? 9 : -1;
    case 1:  // pesos de mezcla
      return indice == 0 ? 10 : -1;
    default:
      return -1;
  }
}

// USAGE_TYPES: these inputs are uint4 in the translated shaders.
inline bool EntradaEntera(uint8_t uso) {
  // Only BLENDINDICES. Since the nfsmw_validado_normales library, normals, tangents and binormals are
  // float4: NFSMW stores them as 16-bit integers (format 26) or as floats, and with uint4 the shader's
  // asfloat gave degenerate directions (seen in the rear-view mirror).
  return uso == 2;
}

inline uint32_t ComponentesVertice(uint32_t formato) {
  switch (formato) {
    case 33:
    case 36:
      return 1;
    case 25:
    case 31:
    case 34:
    case 37:
      return 2;
    case 16:
    case 17:
    case 57:
      return 3;
    default:
      return 4;
  }
}

// Components written by a fetch swizzle (the ones that are not 7), as a 4-bit mask.
inline uint32_t MascaraEscrita(uint32_t swizzle) {
  uint32_t mascara = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (((swizzle >> (i * 3)) & 0x7) != 7) {
      mascara |= 1u << i;
    }
  }
  return mascara;
}

// g_InputRemap code: the SPIR-V writes r[i] = entrada[orig[i]] and the fetch patched by D3D writes
// r[i] = dato[parcheado[i]] (or 0 / 1). For each written component, the host input at orig[i] has to
// come from parcheado[i]. 7 = the same component.
inline uint32_t CodigoRemapeo(uint32_t original, uint32_t parcheado) {
  uint32_t codigo = kRemapeoIdentidad;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t o = (original >> (i * 3)) & 0x7;
    const uint32_t d = (parcheado >> (i * 3)) & 0x7;
    if (o <= 3 && d != 7) {
      codigo = (codigo & ~(uint32_t(0x7) << (o * 3))) | (d << (o * 3));
    }
  }
  return codigo;
}

// --- Textures --------------------------------------------------------------------------------------------------

// Host channel layout of a guest texture format, independent of the API: each backend picks its own format
// for the class. The swizzles are the ones nfsmw_nativo_dibujos.cpp uses (kSwizzleRRRR...).
enum class ClaseTextura : uint8_t {
  kR8, kRG8, kRGBA8, kRGB10A2, kR16, kRG16, kRGBA16, kR16F, kRG16F, kRGBA16F, kR32F, kRG32F, kRGBA32F,
  kBC1, kBC2, kBC3, kBC4, kBC5,
};

constexpr uint16_t kSwizzleTexRRRR = 0;
constexpr uint16_t kSwizzleTexRGGG = (1 << 3) | (1 << 6) | (1 << 9);
constexpr uint16_t kSwizzleTexRGBA = (1 << 3) | (2 << 6) | (3 << 9);
constexpr uint16_t kSwizzleTexBGRA = 2 | (1 << 3) | (0 << 6) | (3 << 9);

struct FormatoGuest {
  ClaseTextura clase = ClaseTextura::kRGBA8;
  uint8_t bloque = 1;        // texels per block side
  uint8_t bytes = 1;         // bytes per block
  uint8_t unidad_orden = 0;  // 0 = no byte swap, else 2 or 4 bytes
  uint16_t swizzle_host = kSwizzleTexRGBA;
};

// The same table as FormatoTexturaDe in nfsmw_nativo_dibujos.cpp (vulkan/texture_cache.cpp:123-397).
inline bool FormatoGuestDe(uint32_t formato, FormatoGuest& f) {
  using C = ClaseTextura;
  switch (formato) {
    case 2:  // k_8
    case 8:  // k_8_A
      f = {C::kR8, 1, 1, 0, kSwizzleTexRRRR};
      return true;
    case 10:  // k_8_8
      f = {C::kRG8, 1, 2, 2, kSwizzleTexRGGG};
      return true;
    case 6:   // k_8_8_8_8
    case 50:  // k_8_8_8_8_AS_16_16_16_16
      f = {C::kRGBA8, 1, 4, 4, kSwizzleTexRGBA};
      return true;
    case 7:   // k_2_10_10_10
    case 54:  // k_2_10_10_10_AS_16_16_16_16
      f = {C::kRGB10A2, 1, 4, 4, kSwizzleTexRGBA};
      return true;
    case 18:  // k_DXT1
    case 51:
      f = {C::kBC1, 4, 8, 2, kSwizzleTexRGBA};
      return true;
    case 19:  // k_DXT2_3
    case 52:
      f = {C::kBC2, 4, 16, 2, kSwizzleTexRGBA};
      return true;
    case 20:  // k_DXT4_5
    case 53:
      f = {C::kBC3, 4, 16, 2, kSwizzleTexRGBA};
      return true;
    case 49:  // k_DXN
      f = {C::kBC5, 4, 16, 2, kSwizzleTexRGGG};
      return true;
    case 59:  // k_DXT5A
      f = {C::kBC4, 4, 8, 2, kSwizzleTexRRRR};
      return true;
    case 24:  // k_16
      f = {C::kR16, 1, 2, 2, kSwizzleTexRRRR};
      return true;
    case 25:  // k_16_16
      f = {C::kRG16, 1, 4, 2, kSwizzleTexRGGG};
      return true;
    case 26:  // k_16_16_16_16
      f = {C::kRGBA16, 1, 8, 2, kSwizzleTexRGBA};
      return true;
    case 30:  // k_16_FLOAT
      f = {C::kR16F, 1, 2, 2, kSwizzleTexRRRR};
      return true;
    case 31:  // k_16_16_FLOAT
      f = {C::kRG16F, 1, 4, 2, kSwizzleTexRGGG};
      return true;
    case 32:  // k_16_16_16_16_FLOAT
      f = {C::kRGBA16F, 1, 8, 2, kSwizzleTexRGBA};
      return true;
    case 36:  // k_32_FLOAT
      f = {C::kR32F, 1, 4, 4, kSwizzleTexRRRR};
      return true;
    case 37:  // k_32_32_FLOAT
      f = {C::kRG32F, 1, 8, 4, kSwizzleTexRGGG};
      return true;
    case 38:  // k_32_32_32_32_FLOAT
      f = {C::kRGBA32F, 1, 16, 4, kSwizzleTexRGBA};
      return true;
    default:
      return false;
  }
}

// 1-5 for the BC classes, 0 for the rest.
inline uint32_t IndiceBcDe(ClaseTextura clase) {
  switch (clase) {
    case ClaseTextura::kBC1: return 1;
    case ClaseTextura::kBC2: return 2;
    case ClaseTextura::kBC3: return 3;
    case ClaseTextura::kBC4: return 4;
    case ClaseTextura::kBC5: return 5;
    default: return 0;
  }
}

// One mip region from the mip address (GetGuestTextureLayout).
struct RegionMip {
  uint64_t desplazamiento = 0;
  uint64_t fila_bytes = 0;
  uint64_t zancada = 0;
  uint32_t pitch_bloques = 0;
};

// What DescribirTextura gets from a fetch constant.
struct DescripcionTextura {
  enum Dimension : uint8_t { k2D, k3D, kCubo };
  Dimension dimension = k2D;
  uint32_t formato = 0;  // guest format (k_8_8_8_8...)
  FormatoGuest tf;
  uint32_t ancho = 0, alto = 0, fondo = 0;  // fondo: 3D slices, 0 for the rest
  uint32_t capas = 1;                       // 6 for cubemaps
  uint32_t ancho_host = 0, alto_host = 0;   // BC in whole blocks
  uint32_t bloques_x = 0, bloques_y = 0;
  uint32_t pitch_bloques = 0;
  bool mosaico = false;
  uint32_t orden = 0;  // xenos::Endian
  uint32_t swizzle = 0;  // the fetch constant's (12 bits)
  uint64_t dir_base = 0;
  uint64_t dir_mips = 0;
  uint64_t zancada_cara = 0;
  uint32_t niveles = 1;  // host levels
  bool leer_base = true;
  uint32_t nivel_empaquetado = UINT32_MAX;
  uint32_t base_ox = 0, base_oy = 0;
  std::array<RegionMip, 16> regiones{};
  uint64_t extension = 0;       // guest bytes of the base (all layers) from dir_base
  uint64_t extension_mips = 0;  // guest bytes of the mips from dir_mips
};

// The layout of the texture of a fetch constant (k2DOrStacked, 3D and cube), with the rules of PrepararTextura:
// mip levels from GetSubresourcesFromFetchConstant, mip regions and packed tail from GetGuestTextureLayout. False
// if it is not a texture, its dimension (1D) or format is not supported, or it goes past guest memory.
inline bool DescribirTextura(const uint32_t* f, bool con_mips, DescripcionTextura& d) {
  namespace xenos = rex::graphics::xenos;
  constexpr uint64_t kMemoriaFisica = 0x20000000;
  d = DescripcionTextura{};
  if ((f[0] & 0x3) != uint32_t(xenos::FetchConstantType::kTexture)) {
    return false;
  }
  const uint32_t dimension = (f[5] >> 9) & 0x3;
  const bool cubo = dimension == uint32_t(xenos::DataDimension::kCube);
  const bool volumen = dimension == uint32_t(xenos::DataDimension::k3D);
  if (dimension != uint32_t(xenos::DataDimension::k2DOrStacked) && !cubo && !volumen) {
    return false;
  }
  d.dimension = cubo ? DescripcionTextura::kCubo : volumen ? DescripcionTextura::k3D : DescripcionTextura::k2D;
  d.capas = cubo ? 6 : 1;
  d.swizzle = (f[3] >> 1) & 0xFFF;
  d.formato = f[1] & 0x3F;
  if (!FormatoGuestDe(d.formato, d.tf)) {
    return false;
  }
  const FormatoGuest& tf = d.tf;
  d.ancho = volumen ? (f[2] & 0x7FF) + 1 : (f[2] & 0x1FFF) + 1;
  d.alto = volumen ? ((f[2] >> 11) & 0x7FF) + 1 : ((f[2] >> 13) & 0x1FFF) + 1;
  d.fondo = volumen ? ((f[2] >> 22) & 0x3FF) + 1 : 0;
  if (volumen && tf.bloque > 1) {
    return false;  // compressed 3D texture
  }
  d.bloques_x = (d.ancho + tf.bloque - 1) / tf.bloque;
  d.bloques_y = (d.alto + tf.bloque - 1) / tf.bloque;
  d.pitch_bloques = std::max<uint32_t>((((f[0] >> 22) & 0x1FF) << 5) / tf.bloque, 1);
  d.zancada_cara =
      (uint64_t((d.pitch_bloques + 31) & ~uint32_t(31)) * tf.bytes * ((d.bloques_y + 31) & ~uint32_t(31)) + 4095) &
      ~uint64_t(4095);
  d.mosaico = (f[0] >> 31) & 0x1;
  d.orden = (f[1] >> 6) & 0x3;
  d.dir_base = uint64_t((f[1] >> 12) << 12) & 0x1FFFFFFF;
  d.dir_mips = uint64_t((f[5] >> 12) & 0x1FFFF) << 12;
  d.nivel_empaquetado = con_mips && !volumen && ((f[5] >> 11) & 0x1) ? NivelEmpaquetado(d.ancho, d.alto) : UINT32_MAX;
  uint32_t nivel_max = 0;
  if (con_mips && !volumen && d.dir_mips != 0) {
    const uint32_t tam_max = Log2Suelo(std::max(d.ancho, d.alto));
    uint32_t nivel_min = std::min((f[4] >> 2) & 0xFu, tam_max);
    nivel_max = std::max(std::min((f[4] >> 6) & 0xFu, tam_max), nivel_min);
    if (nivel_max != 0) {
      if (d.dir_base == 0) {
        nivel_min = std::max(nivel_min, 1u);
      }
      d.leer_base = nivel_min == 0;
    }
  }
  if (nivel_max != 0) {
    const uint32_t ultima = d.nivel_empaquetado == 0 ? 0 : std::min(nivel_max, d.nivel_empaquetado);
    for (uint32_t s = d.nivel_empaquetado == 0 ? 0 : 1; s <= ultima; ++s) {
      RegionMip& region = d.regiones[s];
      const uint32_t fila_texels = std::max(std::bit_ceil(d.ancho) >> s, 1u);
      const uint32_t filas_texels = std::max(std::bit_ceil(d.alto) >> s, 1u);
      region.pitch_bloques = ((fila_texels + tf.bloque - 1) / tf.bloque + 31) & ~31u;
      region.fila_bytes = uint64_t(region.pitch_bloques) * tf.bytes;
      if (!d.mosaico) {
        region.fila_bytes = (region.fila_bytes + 255) & ~uint64_t(255);
      }
      const uint64_t filas_bloques = uint64_t(((filas_texels + tf.bloque - 1) / tf.bloque + 31) & ~31u);
      region.zancada = (region.fila_bytes * filas_bloques + 4095) & ~uint64_t(4095);
      region.desplazamiento = d.extension_mips;
      d.extension_mips += region.zancada * d.capas;
    }
    if (d.dir_mips + d.extension_mips > kMemoriaFisica) {
      nivel_max = 0;  // the mips would go past memory: base only
      d.extension_mips = 0;
      d.leer_base = true;
    }
  }
  if (d.nivel_empaquetado == 0) {
    DesplazamientoEmpaquetado(d.ancho, d.alto, tf.bloque, 0, d.base_ox, d.base_oy);
  }
  d.ancho_host = tf.bloque > 1 ? (d.ancho + 3) & ~uint32_t(3) : d.ancho;
  d.alto_host = tf.bloque > 1 ? (d.alto + 3) & ~uint32_t(3) : d.alto;
  d.niveles = std::min(nivel_max + 1, Log2Suelo(std::max(d.ancho_host, d.alto_host)) + 1);
  // Extent of the base: tiled, GetTiledAddressUpperBound2D; linear, up to the last block. 3D: the whole volume.
  const uint32_t log2_bloque = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2 : tf.bytes >= 2 ? 1 : 0;
  if (volumen) {
    const uint64_t fila = uint64_t((d.pitch_bloques + 31) & ~uint32_t(31)) * tf.bytes;
    const uint64_t corte = fila * ((d.bloques_y + 31) & ~uint32_t(31));
    d.extension = d.mosaico ? uint64_t(std::max<int64_t>(DesplazamientoMosaico3D(int32_t((d.bloques_x + 31) & ~31u),
                                                                                 int32_t((d.bloques_y + 31) & ~31u),
                                                                                 int32_t((d.fondo + 3) & ~3u),
                                                                                 d.pitch_bloques, d.bloques_y,
                                                                                 log2_bloque),
                                                         0))
                            : corte * d.fondo;
  } else {
    const uint32_t bx = d.bloques_x + d.base_ox;
    const uint32_t by = d.bloques_y + d.base_oy;
    const uint64_t capa =
        d.mosaico ? uint64_t(std::max<int64_t>(DesplazamientoMosaico2D(int32_t((bx - 1) & ~31u),
                                                                       int32_t((by - 1) & ~31u), d.pitch_bloques,
                                                                       log2_bloque),
                                               0)) +
                        (log2_bloque == 0 ? 0xA00u : log2_bloque == 1 ? 0xC00u : (0x400u << log2_bloque))
                  : uint64_t(std::max(d.pitch_bloques, bx)) * tf.bytes * (by - 1) + uint64_t(bx) * tf.bytes;
    d.extension = (d.capas - 1) * d.zancada_cara + capa;
  }
  return d.dir_base + d.extension <= kMemoriaFisica;
}

// One level of a layer, untiled, into host rows of bx_host blocks. False if it would read past memory.
inline bool LeerNivelGuest(const uint8_t* fisica, uint64_t direccion, bool mosaico, uint32_t pitch_bloques,
                           uint64_t fila_bytes, const FormatoGuest& tf, uint32_t ox, uint32_t oy, uint32_t bx,
                           uint32_t by, uint32_t bx_host, uint8_t* destino) {
  constexpr uint64_t kMemoriaFisica = 0x20000000;
  if (!bx || !by) {
    return true;
  }
  const uint32_t log2 = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2 : tf.bytes >= 2 ? 1 : 0;
  const uint64_t extremo =
      mosaico ? uint64_t(std::max<int64_t>(DesplazamientoMosaico2D(int32_t((ox + bx + 31) & ~31u),
                                                                   int32_t((oy + by + 31) & ~31u), pitch_bloques, log2),
                                           0))
              : fila_bytes * (oy + by - 1) + uint64_t(ox + bx) * tf.bytes;
  if (direccion + extremo > kMemoriaFisica) {
    return false;
  }
  const uint8_t* origen = fisica + direccion;
  if (!mosaico) {
    for (uint32_t y = 0; y < by; ++y) {
      std::memcpy(destino + size_t(y) * bx_host * tf.bytes, origen + uint64_t(oy + y) * fila_bytes + uint64_t(ox) * tf.bytes,
                  size_t(bx) * tf.bytes);
    }
    return true;
  }
  if (tf.bytes == (1u << log2)) {
    switch (log2) {
      case 0: DesenmosaicarNivel<0>(origen, pitch_bloques, ox, oy, bx, by, bx_host, destino); break;
      case 1: DesenmosaicarNivel<1>(origen, pitch_bloques, ox, oy, bx, by, bx_host, destino); break;
      case 2: DesenmosaicarNivel<2>(origen, pitch_bloques, ox, oy, bx, by, bx_host, destino); break;
      case 3: DesenmosaicarNivel<3>(origen, pitch_bloques, ox, oy, bx, by, bx_host, destino); break;
      default: DesenmosaicarNivel<4>(origen, pitch_bloques, ox, oy, bx, by, bx_host, destino); break;
    }
    return true;
  }
  for (uint32_t y = 0; y < by; ++y) {
    for (uint32_t x = 0; x < bx; ++x) {
      const int32_t desplazamiento = DesplazamientoMosaico2D(int32_t(ox + x), int32_t(oy + y), pitch_bloques, log2);
      std::memcpy(destino + (size_t(y) * bx_host + x) * tf.bytes, origen + desplazamiento, tf.bytes);
    }
  }
  return true;
}

// All the levels of the texture in host layout: level after level and, in each, layer after layer (3D: slice
// after slice), in guest blocks and host byte order. `desplazamientos[n]` = bytes up to level n. With
// `bc_a_cpu`, BC classes come out decoded (bc::Convertir: RGBA8, R8 for BC4, RG8 for BC5) and the offsets are
// those of the decoded data. False if the base is outside memory; a mip outside memory stays at zero.
/*
 * BC1 decoded on the CPU (RGBA8) packed to 16 bits per texel (nfsmw_nativo_bc1_16_bits): half the memory and the
 * bandwidth of RGBA8 on GPUs without BC (Mali, PowerVR). BC1 keeps 5:6:5 endpoints and two interpolated colors per
 * block, and its alpha is 0 or 255 (punch-through), so 16 bits lose at most the rounding of the interpolated ones.
 *   kRgb565:   GL_UNSIGNED_SHORT_5_6_5, no alpha (every texel opaque);
 *   kRgba5551: GL_UNSIGNED_SHORT_5_5_5_1, alpha in bit 0;
 *   kArgb1555: VK_FORMAT_A1R5G5B5_UNORM_PACK16, alpha in bit 15.
 * In place: the 16-bit texels end up in the first half of `datos`, which is then shrunk. The level offsets halve.
 */
enum class Empaquetado16 { kRgb565, kRgba5551, kArgb1555 };

// Whether any texel of RGBA8 data is not opaque.
inline bool HayAlfa(const std::vector<uint8_t>& datos) {
  for (size_t i = 3; i < datos.size(); i += 4) {
    if (datos[i] != 255) {
      return true;
    }
  }
  return false;
}

inline void EmpaquetarA16(std::vector<uint8_t>& datos, Empaquetado16 e) {
  const size_t texels = datos.size() / 4;
  uint16_t* salida = reinterpret_cast<uint16_t*>(datos.data());
  for (size_t i = 0; i < texels; ++i) {
    const uint8_t* p = datos.data() + i * 4;
    const uint32_t r5 = (uint32_t(p[0]) * 31 + 127) / 255;
    const uint32_t b5 = (uint32_t(p[2]) * 31 + 127) / 255;
    uint16_t v;
    if (e == Empaquetado16::kRgb565) {
      const uint32_t g6 = (uint32_t(p[1]) * 63 + 127) / 255;
      v = uint16_t((r5 << 11) | (g6 << 5) | b5);
    } else {
      const uint32_t g5 = (uint32_t(p[1]) * 31 + 127) / 255;
      const uint32_t a = p[3] >= 128 ? 1 : 0;
      v = e == Empaquetado16::kRgba5551 ? uint16_t((r5 << 11) | (g5 << 6) | (b5 << 1) | a)
                                        : uint16_t((a << 15) | (r5 << 10) | (g5 << 5) | b5);
    }
    // Texel i is written to bytes 2i..2i+1, which were already read (they belong to texel i / 2 <= i).
    std::memcpy(salida + i, &v, sizeof(v));
  }
  datos.resize(texels * 2);
}

inline bool LeerDatosTextura(const uint8_t* fisica, const DescripcionTextura& d, bool bc_a_cpu,
                             std::vector<uint8_t>& datos, std::array<uint32_t, 16>& desplazamientos,
                             std::vector<uint8_t>& temporal) {
  const FormatoGuest& tf = d.tf;
  std::array<size_t, 16> bytes_capa_nivel{};
  size_t bytes_datos = 0;
  const uint32_t cortes = d.fondo ? d.fondo : 1;
  for (uint32_t n = 0; n < d.niveles; ++n) {
    const uint32_t bx_host = (std::max(d.ancho_host >> n, 1u) + tf.bloque - 1) / tf.bloque;
    const uint32_t by_host = (std::max(d.alto_host >> n, 1u) + tf.bloque - 1) / tf.bloque;
    desplazamientos[n] = uint32_t(bytes_datos);
    bytes_capa_nivel[n] = size_t(bx_host) * by_host * tf.bytes;
    bytes_datos += bytes_capa_nivel[n] * d.capas * cortes;
  }
  datos.assign(bytes_datos, 0);
  const uint32_t bloques_x_host = (d.ancho_host + tf.bloque - 1) / tf.bloque;
  const uint32_t bloques_y_host = (d.alto_host + tf.bloque - 1) / tf.bloque;
  if (d.dimension == DescripcionTextura::k3D) {
    const uint32_t log2 = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2 : tf.bytes >= 2 ? 1 : 0;
    const uint64_t fila = uint64_t((d.pitch_bloques + 31) & ~uint32_t(31)) * tf.bytes;
    const uint64_t corte = fila * ((d.bloques_y + 31) & ~uint32_t(31));
    const uint8_t* origen = fisica + d.dir_base;
    const size_t bytes_corte = size_t(bloques_x_host) * bloques_y_host * tf.bytes;
    for (uint32_t z = 0; z < d.fondo; ++z) {
      for (uint32_t y = 0; y < d.bloques_y; ++y) {
        for (uint32_t x = 0; x < d.bloques_x; ++x) {
          const uint64_t desplazamiento =
              d.mosaico ? uint64_t(DesplazamientoMosaico3D(int32_t(x), int32_t(y), int32_t(z), d.pitch_bloques,
                                                           d.bloques_y, log2))
                        : z * corte + y * fila + uint64_t(x) * tf.bytes;
          std::memcpy(datos.data() + bytes_corte * z + (size_t(y) * bloques_x_host + x) * tf.bytes,
                      origen + desplazamiento, tf.bytes);
        }
      }
    }
  } else {
    const size_t bytes_capa = bytes_capa_nivel[0];
    for (uint32_t c = 0; c < (d.leer_base ? d.capas : 0u); ++c) {
      if (!LeerNivelGuest(fisica, d.dir_base + c * d.zancada_cara, d.mosaico, d.pitch_bloques,
                          uint64_t(std::max(d.pitch_bloques, d.bloques_x)) * tf.bytes, tf, d.base_ox, d.base_oy,
                          d.bloques_x, d.bloques_y, bloques_x_host, datos.data() + bytes_capa * c)) {
        return false;
      }
    }
    for (uint32_t n = 1; n < d.niveles; ++n) {
      const uint32_t s = d.nivel_empaquetado == 0 ? 0 : std::min(n, d.nivel_empaquetado);
      uint32_t ox = 0, oy = 0;
      if (n >= d.nivel_empaquetado) {
        DesplazamientoEmpaquetado(d.ancho, d.alto, tf.bloque, n, ox, oy);
      }
      const uint32_t bx_invitado = (std::max(d.ancho >> n, 1u) + tf.bloque - 1) / tf.bloque;
      const uint32_t by_invitado = (std::max(d.alto >> n, 1u) + tf.bloque - 1) / tf.bloque;
      const uint32_t bx_host = (std::max(d.ancho_host >> n, 1u) + tf.bloque - 1) / tf.bloque;
      const uint32_t by_host = (std::max(d.alto_host >> n, 1u) + tf.bloque - 1) / tf.bloque;
      bool ok = true;
      for (uint32_t c = 0; ok && c < d.capas; ++c) {
        ok = LeerNivelGuest(fisica, d.dir_mips + d.regiones[s].desplazamiento + c * d.regiones[s].zancada, d.mosaico,
                            d.regiones[s].pitch_bloques, d.regiones[s].fila_bytes, tf, ox, oy,
                            std::min(bx_invitado, bx_host), std::min(by_invitado, by_host), bx_host,
                            datos.data() + desplazamientos[n] + bytes_capa_nivel[n] * c);
      }
      if (!ok) {
        break;  // the mip stays at zero, as in Vulkan
      }
    }
  }
  if (d.orden != 0 && (tf.unidad_orden == 2 || tf.unidad_orden == 4)) {
    CambiarOrdenBytes(datos.data(), datos.size(), tf.unidad_orden, d.orden);
  }
  const uint32_t indice_bc = IndiceBcDe(tf.clase);
  if (bc_a_cpu && indice_bc) {
    if (!nfsmw::bc::Convertir(static_cast<nfsmw::bc::Formato>(indice_bc), datos, d.ancho_host, d.alto_host, d.capas,
                              d.niveles, temporal, desplazamientos)) {
      return false;
    }
    datos.swap(temporal);
  }
  return true;
}

}  // namespace nfsmw::nativo
