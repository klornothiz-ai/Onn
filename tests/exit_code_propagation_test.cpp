// ============================================================================
// ProsperoLayer RDNA2 Core - guest exit() propagation regression test
// ----------------------------------------------------------------------------
// Round 34: the guest `exit` syscall set ExitRequested()/ExitCode() on the
// dispatcher but the interpreter kept running through the dead code after the
// syscall and returned 0, so the CLI reported exit=0 even though the guest
// asked to die with 42. ExecuteGuestFull must stop on a guest exit and return
// the guest's code.
// ============================================================================
#include "cpu/jit_executor.hpp"
#include "memory/virtual_memory_manager.hpp"

#include <array>
#include <cstdint>
#include <iostream>

namespace {

using PS5::CPU::CPUJitEngine;
using PS5::Memory::PageProt;
using PS5::Memory::VirtualMemoryManager;

bool RunExit42(bool prefer_direct) {
    constexpr uint64_t kGuestEntry = 0x1000d00000ULL;
    constexpr uint32_t kReadWrite = static_cast<uint32_t>(PageProt::Read) |
                                    static_cast<uint32_t>(PageProt::Write);
    constexpr uint32_t kReadExecute = static_cast<uint32_t>(PageProt::Read) |
                                      static_cast<uint32_t>(PageProt::Exec);
    // mov $42, %edi ; mov $1, %eax (FreeBSD-style exit) ; syscall
    const std::array<uint8_t, 12> code = {
        0xbf, 0x2a, 0x00, 0x00, 0x00,   // mov edi, 42
        0xb8, 0x01, 0x00, 0x00, 0x00,   // mov eax, 1
        0x0f, 0x05,                     // syscall
    };

    auto& vmm = VirtualMemoryManager::Instance();
    if (vmm.AllocateVirtual(kGuestEntry, 4096, kReadWrite) != kGuestEntry) return false;
    const bool copied = vmm.CopyToGuest(kGuestEntry, code.data(), code.size());
    const bool protected_ = vmm.ProtectVirtual(kGuestEntry, 4096, kReadExecute);
    if (!copied || !protected_) {
        vmm.FreeVirtual(kGuestEntry, 4096);
        return false;
    }

    auto& engine = CPUJitEngine::Instance();
    engine.SetExecutionBackend(prefer_direct ? PS5::CPU::GuestExecutionBackend::Direct
                                             : PS5::CPU::GuestExecutionBackend::Interpreter);
    // A generous budget: the run must STOP on the exit syscall, not execute
    // past it. 200k instructions is more than enough for 3 insns.
    const uint64_t ret = engine.ExecuteGuestFull(kGuestEntry, 0, 0, 200000);
    const bool ok = (ret == 42);
    vmm.FreeVirtual(kGuestEntry, 4096);
    return ok;
}

} // namespace

int main() {
    std::cout << "[exit_prop] round 34: guest exit(42) -> returned exit code\n";
    int failures = 0, checks = 0;

    const bool interp = RunExit42(false);
    ++checks;
    if (!interp) { ++failures; std::cerr << "  [FAIL] interpreter path did not return 42\n"; }

    const bool direct = RunExit42(true);
    ++checks;
    if (!direct) { ++failures; std::cerr << "  [FAIL] direct path did not return 42\n"; }

    std::cout << "  interpreter returned 42: " << (interp ? "yes" : "no") << '\n'
              << "  direct returned 42:      " << (direct ? "yes" : "no") << '\n'
              << (failures == 0 ? "PASS" : "FAIL") << ": " << checks
              << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
