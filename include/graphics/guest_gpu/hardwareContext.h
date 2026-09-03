#pragma once
// ProsperoLayer PS5 emulator - hardware context interface (Kyty-compatible)
#include "common/common.h"
#include <cstdint>

namespace Graphics {

// The hardware context tracks the current GPU register state.
struct HardwareContext {
        uint64_t reg_base{0};
        uint32_t num_regs{0};
};

} // namespace Graphics
