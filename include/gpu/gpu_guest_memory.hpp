#pragma once
// ============================================================================
// ProsperoLayer RDNA2 Core - GPU guest-memory access interface
// ----------------------------------------------------------------------------
// The PM4 translator needs to read the guest's compute shader bytecode and its
// input SSBO, and write the output SSBO back, at guest virtual addresses the
// command stream supplies via COMPUTE_PGM_* / COMPUTE_USER_DATA_* registers.
//
// This tiny interface abstracts that access so the translator does not depend
// on the VMM directly: the real emulator plugs in a VMM-backed implementer
// (VmmGpuMemory), while tests can plug in a flat in-process buffer.
// ============================================================================

#include <cstddef>
#include <cstdint>

namespace PS5::GPU {

class GpuGuestMemory {
public:
    virtual ~GpuGuestMemory() = default;

    // Copy `dwords` 32-bit words from guest address `gva` into `dst`.
    // Returns false if any part of the range is not readable.
    virtual bool ReadDwords(uint64_t gva, uint32_t* dst, size_t dwords) = 0;

    // Copy `dwords` 32-bit words from `src` to guest address `gva`.
    // Returns false if any part of the range is not writable.
    virtual bool WriteDwords(uint64_t gva, const uint32_t* src, size_t dwords) = 0;
};

} // namespace PS5::GPU
