#pragma once
// ProsperoLayer PS5 emulator - guest GPU definitions (Kyty-compatible)
#include "common/common.h"
#include <cstdint>

namespace Graphics {

// Guest GPU register space & hardware constants.
constexpr uint64_t GPU_REGISTER_BASE      = 0x0000000080000000ull;
constexpr uint64_t GPU_REGISTER_SPACE_SIZE = 0x100000ull;

// Framebuffer / scanout formats (VideoOut pixel formats).
enum class PixelFormat : uint32_t {
        A8R8G8B8 = 0x00,
        R8G8B8A8 = 0x01,
        B8G8R8A8 = 0x02,
        R5G6B5   = 0x03,
        R8G8B8   = 0x04,
        A16R16G16B16 = 0x05,
};

} // namespace Graphics
