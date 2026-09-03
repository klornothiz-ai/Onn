#pragma once
// ProsperoLayer PS5 emulator - guest graphics run state (Kyty-compatible)
#include "common/common.h"
#include <cstdint>

namespace Graphics {

// GraphicsRun coordinates guest GPU execution (PM4 stream processing).
// This interface is provided for the guest graphics driver; the concrete
// implementation lives in the renderer backend.
struct GraphicsRunContext {
        uint64_t frame_num{0};
        bool     is_compute{false};
};

} // namespace Graphics
