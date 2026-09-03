#pragma once
// ProsperoLayer PS5 emulator - tile (micro-tiling) helpers (Kyty-compatible)
#include "common/common.h"
#include <cstdint>
#include <cstddef>

namespace Graphics {

// Micro-tiling mode (matches libvideoout tiling modes).
enum class TileMode : uint32_t {
        Linear    = 0,
        Tiled     = 1,
        MacroTiled = 2,
};

// Computes the memory size required for a tiled surface.
uint64_t TileGetDataSize(TileMode mode, uint32_t width, uint32_t height, uint32_t bytes_per_pixel);
// Converts a linear address to a tiled offset (stub for now, returns offset unchanged).
uint64_t TileLinearToTiled(uint64_t linear_offset);

} // namespace Graphics
