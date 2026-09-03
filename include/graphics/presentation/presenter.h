#pragma once
// ProsperoLayer PS5 emulator - presenter interface (Kyty-compatible)
#include "common/common.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include <cstdint>

namespace Graphics {

// The presenter owns the swapchain and provides the render context.
// Kyty's Presenter class is a concrete SDL+Vulkan object; here we re-export
// the RenderContext-based interface so guest code only depends on the ABI.

} // namespace Graphics
