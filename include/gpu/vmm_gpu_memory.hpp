#pragma once
// ============================================================================
// ProsperoLayer RDNA2 Core - VMM-backed GpuGuestMemory
// ----------------------------------------------------------------------------
// Plugs the real 16 GB VirtualMemoryManager arena into the PM4 translator's
// compute path so DISPATCH_DIRECT can read the guest's compute shader + input
// SSBO and write the output SSBO back through the VMM's protection-checked
// copy paths (no raw host pointers, faults rejected).
// ============================================================================

#include "gpu/gpu_guest_memory.hpp"
#include "memory/virtual_memory_manager.hpp"

namespace PS5::GPU {

class VmmGpuMemory final : public GpuGuestMemory {
public:
    explicit VmmGpuMemory(
        PS5::Memory::VirtualMemoryManager& vmm = PS5::Memory::VirtualMemoryManager::Instance())
        : m_vmm(vmm) {}

    bool ReadDwords(uint64_t gva, uint32_t* dst, size_t dwords) override {
        return m_vmm.CopyFromGuest(gva, dst, dwords * sizeof(uint32_t),
                                   static_cast<uint32_t>(PS5::Memory::PageProt::Read));
    }
    bool WriteDwords(uint64_t gva, const uint32_t* src, size_t dwords) override {
        return m_vmm.CopyToGuest(gva, src, dwords * sizeof(uint32_t),
                                 static_cast<uint32_t>(PS5::Memory::PageProt::Write));
    }

private:
    PS5::Memory::VirtualMemoryManager& m_vmm;
};

} // namespace PS5::GPU
