#pragma once
// ProsperoLayer PS5 emulator - host renderer interface (Kyty-compatible)
#include "common/common.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include <cstdint>

namespace Graphics {

// High-level renderer entry point: submits a frame for presentation.
bool RendererPresent(RenderContext& ctx, uint32_t buffer_index);

} // namespace Graphics
