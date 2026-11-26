// SPDX-License-Identifier: GPL-3.0-only
// The vendored FidelityFX VK backend references its swapchain hook; PEUSSOK never presents
// through FidelityFX (frames go back to the application), so the swapchain sources are not built.
#include <FidelityFX/host/backends/vk/ffx_vk.h>
FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const*){return FFX_ERROR_INVALID_ARGUMENT;}
