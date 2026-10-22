// nfsmw - native renderer, part C2 on OpenGL ES: render targets, copies (resolve and clear) and presentation of
// the game's image (docs/motor-nativo-gles.md in NFSMW-Recompiled).
//
// The same DestinosNativos as the Vulkan one (nfsmw_nativo_destinos.cpp), with what is needed to be exact and
// none of its optimizations yet (image swaps instead of copies, deferred copies, scaled shadow map, GPU
// occlusion). Everything runs on the ring thread, which has its GLES context current.

#pragma once

#include "nfsmw_nativo_destinos.h"

#include <memory>

namespace rex::memory {
class Memory;
}
namespace rex::ui::gles {
class GlesProvider;
}

namespace nfsmw::nativo {

// nullptr, with the cause in the log, if it could not be set up. Called on the ring thread with its context
// current, and destroyed on it too (its GL objects belong to that context).
std::unique_ptr<DestinosNativos> CrearDestinosGles(const rex::ui::gles::GlesProvider* provider,
                                                   rex::memory::Memory* memoria);

}  // namespace nfsmw::nativo
