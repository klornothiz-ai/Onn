#pragma once
// ProsperoLayer PS5 emulator - host memory access helpers (Kyty-compatible)
#include "common/common.h"
#include <cstdint>
#include <cstddef>

namespace Graphics {

// Checks whether a guest address is readable from host context.
bool HostMemoryIsReadable(uint64_t guest_addr);
// Reads a byte from guest memory (returns 0 if unmapped).
uint8_t HostMemoryReadByte(uint64_t guest_addr);
// Writes a byte to guest memory (no-op if unmapped).
void HostMemoryWriteByte(uint64_t guest_addr, uint8_t value);
// Maps a guest address range to a host pointer (nullptr if unmapped).
void* HostMemoryMap(uint64_t guest_addr, size_t size);
// Copies from guest memory to host buffer.
bool HostMemoryRead(uint64_t guest_addr, void* dst, size_t size);
// Copies from host buffer to guest memory.
bool HostMemoryWrite(uint64_t guest_addr, const void* src, size_t size);

} // namespace Graphics
