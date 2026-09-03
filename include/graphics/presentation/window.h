#pragma once
// ProsperoLayer PS5 emulator - window / presenter entry points (Kyty-compatible)
#include "common/common.h"
#include <cstdint>

namespace Graphics {

class RenderContext;

RenderContext& WindowInit(uint32_t width, uint32_t height);
void WindowShutdown();

} // namespace Graphics
