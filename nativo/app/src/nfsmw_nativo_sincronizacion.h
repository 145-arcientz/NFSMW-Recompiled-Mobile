#pragma once

#include <rex/cvar.h>
#include <vulkan/vulkan.h>

REXCVAR_DECLARE(bool, nfsmw_nativo_sincronizacion_gpu);
REXCVAR_DECLARE(bool, nfsmw_nativo_barreras_transferencia);
REXCVAR_DECLARE(bool, nfsmw_nativo_barrera_total);

namespace nfsmw::nativo {

// GENERAL is a layout, not a memory dependency. The native renderer shares
// images between uploads, resolves, reflections and subsequent render passes.
inline constexpr VkPipelineStageFlags kEtapasImagenes =
    VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
inline constexpr VkAccessFlags kAccesosImagenes =
    VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
inline constexpr VkPipelineStageFlags kEtapasPase =
    kEtapasImagenes & ~VK_PIPELINE_STAGE_TRANSFER_BIT;
inline constexpr VkAccessFlags kAccesosPase =
    kAccesosImagenes & ~(VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);

inline void DependenciasImagenes(VkSubpassDependency (&dependencias)[2]) {
  dependencias[0] = {VK_SUBPASS_EXTERNAL, 0, kEtapasImagenes, kEtapasPase,
                     kAccesosImagenes, kAccesosPase, 0};
  dependencias[1] = {0, VK_SUBPASS_EXTERNAL, kEtapasPase, kEtapasImagenes,
                     kAccesosPase, kAccesosImagenes, 0};
}

// Two transfer commands in a row (copy -> clear, copy -> copy, copy -> read to a buffer) are not ordered
// by Vulkan without a barrier between them. Adreno runs them in order anyway; Mali can overlap them, and
// the validation layer saw it on the A22: a render target cleared before its resolve had read it, a
// cubemap written before its own preparatory clear. The render passes already carry their dependencies
// (DependenciasImagenes); this is only for transfer after transfer.
template <typename Funciones>
inline void BarreraTransferencias(const Funciones& dfn, VkCommandBuffer comandos) {
  if (!REXCVAR_GET(nfsmw_nativo_barreras_transferencia)) {
    return;
  }
  VkMemoryBarrier barrera{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrera.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrera.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  dfn.vkCmdPipelineBarrier(comandos, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                           &barrera, 0, nullptr, 0, nullptr);
}

// nfsmw_nativo_barrera_total: every earlier write before every later access, outside a render pass. The
// render passes already declare this as subpass dependencies, but MoltenVK does not turn those into Metal
// synchronization; an explicit barrier it does honour. Only where the cvar is on (iOS by default).
template <typename Funciones>
inline void BarreraTotal(const Funciones& dfn, VkCommandBuffer comandos) {
  if (!REXCVAR_GET(nfsmw_nativo_barrera_total)) {
    return;
  }
  VkMemoryBarrier barrera{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrera.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  barrera.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  dfn.vkCmdPipelineBarrier(comandos, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                           &barrera, 0, nullptr, 0, nullptr);
}

}  // namespace nfsmw::nativo
