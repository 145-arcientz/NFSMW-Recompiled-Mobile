// nfsmw - native renderer, parts C3-C6 on OpenGL ES: the ring's draws with the GLES shader library
// (docs/motor-nativo-gles.md in NFSMW-Recompiled).
//
// The same job as DibujosVulkan (nfsmw_nativo_dibujos.cpp) with what is needed to be exact: vertices, indices,
// textures and constants from guest memory, the programs from the library's SPIR-V converted to GLSL ES
// (nfsmw_gles_shaders.cpp) and the Xenos fixed-function state translated with the same tables. None of the
// Vulkan optimizations yet (deferred sky, early Z, vegetation and invisible draw pruning, hashing threads...).
// Everything runs on the ring thread, with its context current.

#pragma once

#include "nfsmw_nativo_dibujos.h"

#include <GLES3/gl32.h>

#include <cstdint>
#include <memory>

namespace rex::memory {
class Memory;
}
namespace rex::ui::gles {
class GlesProvider;
}

namespace nfsmw::nativo {

class ShadersNativos;

// A render target or a resolved texture of the GLES C2 (nfsmw_nativo_destinos_gles.cpp).
struct ImagenGles {
  GLuint textura = 0;
  uint32_t ancho = 0;
  uint32_t alto = 0;
  bool profundidad = false;
  bool preparada = false;  // already cleared once (like ImagenNativa::preparada)
  // Resolved texture with RB_COPY_DEST_INFO.copy_dest_swap: the guest sees it with red and blue swapped
  // relative to the render target it comes from (ImagenNativa::intercambio_rb).
  bool intercambio_rb = false;
  // Its own framebuffer, with it as the only attachment, created the first time it is needed. Re-attaching a
  // shared scratch framebuffer on every clear and copy cost 150 us of CPU per copy on the Mali-G57 (the driver
  // revalidates the framebuffer on every attachment change).
  mutable GLuint fbo = 0;
  // GL swizzle last set on it by the draws (packed; UINT32_MAX = none yet). Kept here and not by texture name,
  // which GL may give again to another texture after this one is deleted.
  mutable uint32_t swizzle = UINT32_MAX;
};

// What the GLES C2 provides to the draws (the GLES counterpart of ContextoDestinos).
class ContextoDestinosGles {
 public:
  virtual ~ContextoDestinosGles() = default;
  // Render targets, cleared on first use. nullptr on failure.
  virtual ImagenGles* DestinoColor(uint32_t base, uint32_t formato, uint32_t pitch) = 0;
  virtual ImagenGles* DestinoProfundidad(uint32_t base, uint32_t formato, uint32_t pitch) = 0;
  // Texture resolved by C2 at that physical address, or nullptr.
  virtual const ImagenGles* TexturaResuelta(uint32_t direccion) = 0;
  // A game occlusion query is open: the draws must not be pruned, and they may count their samples.
  virtual bool OclusionAbierta() const = 0;
  // The open query's sample counter (an atomic counter buffer, for binding 0), or 0 if it does not count samples.
  virtual GLuint ContadorOclusion() const = 0;
  // A draw of the open query could not count its samples: the query gives yes or no.
  virtual void OclusionSinContar() = 0;
};

class DibujosGles {
 public:
  // nullptr if it could not be set up (the reason is logged). On the ring thread, with its context current.
  static std::unique_ptr<DibujosGles> Crear(const rex::ui::gles::GlesProvider* provider, rex::memory::Memory* memoria,
                                            ContextoDestinosGles* contexto);
  virtual ~DibujosGles() = default;

  virtual bool Dibujar(const PeticionDibujo& peticion) = 0;
  // C2 has changed GL state behind the draws' back (framebuffer, masks, scissor, bound textures): the next
  // draw sets everything again.
  virtual void EstadoPerdido() = 0;
  // The current pass ends (C2 is about to copy, clear or present, or a game occlusion query is about to begin):
  // what the draws are holding back (the deferred sky) goes out now. Call it before touching GL state.
  virtual void TerminarPase() = 0;
  // At every Swap: frame boundary of the stream buffer and of the texture checks.
  virtual void FinDeFotograma() = 0;
  virtual EstadisticasDibujos Estadisticas() const = 0;
};

}  // namespace nfsmw::nativo
