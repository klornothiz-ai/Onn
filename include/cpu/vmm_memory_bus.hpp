#pragma once

// ============================================================================
// VmmMemoryBus - bridges the extended X86Interpreter to the emulator's real
// 16 GB guest virtual memory arena (VirtualMemoryManager). Every guest access
// is protection-checked through the VMM's CopyFromGuest / CopyToGuest paths, so
// the interpreter can run real guest basic blocks against live emulator memory
// without ever dereferencing a raw host pointer.
// ============================================================================

#include "cpu/x86_64_interpreter.hpp"
#include "memory/virtual_memory_manager.hpp"

namespace PS5::CPU {

class VmmMemoryBus final : public GuestMemoryBus {
public:
    explicit VmmMemoryBus(Memory::VirtualMemoryManager& vmm) : m_vmm(vmm) {}

    bool Read(uint64_t addr, void* dst, size_t size) override {
        return m_vmm.CopyFromGuest(addr, dst, size,
                                   static_cast<uint32_t>(Memory::PageProt::Read));
    }

    bool Write(uint64_t addr, const void* src, size_t size) override {
        return m_vmm.CopyToGuest(addr, src, size,
                                 static_cast<uint32_t>(Memory::PageProt::Write));
    }

private:
    Memory::VirtualMemoryManager& m_vmm;
};

} // namespace PS5::CPU
