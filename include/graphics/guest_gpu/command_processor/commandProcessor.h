#pragma once
// ProsperoLayer PS5 emulator - command processor interface (Kyty-compatible)
#include "common/common.h"
#include <cstdint>

namespace Graphics {

// Processes a PM4 command stream (guest DCB/ACB).
// Returns the number of dwords consumed, or 0 on error.
uint32_t CommandProcessorRun(const uint32_t* cmd_buffer, uint32_t size_in_dwords);

} // namespace Graphics
