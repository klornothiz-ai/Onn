#pragma once
// ProsperoLayer PS5 emulator - virtual memory helpers (Kyty-compatible)
#include "common/common.h"
#include <cstdint>
#include <cstddef>

namespace Common::VirtualMemory {

enum class Mode : uint32_t {
        ReadOnly  = 0x01,
        ReadWrite = 0x03,
        ReadExecute = 0x05,
        ReadWriteExecute = 0x07,
};

bool AllocFixed(uintptr_t addr, size_t size, Mode mode);
bool Alloc(uintptr_t* addr, size_t size, Mode mode);
bool Free(uintptr_t addr);
bool Protect(uintptr_t addr, size_t size, Mode mode);
bool Query(uintptr_t addr, uintptr_t* start, uintptr_t* end, Mode* mode);
size_t GetPageSize();

} // namespace Common::VirtualMemory
