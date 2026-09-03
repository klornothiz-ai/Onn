// ============================================================================
// ProsperoLayer RDNA2 Core - Guest Execution Integration Test
// ----------------------------------------------------------------------------
// End-to-end: allocates an executable region in the real 16 GB guest arena,
// writes real x86-64 machine code into it, and runs it through the full
// CPUJitEngine::ExecuteGuestFull path. This proves the extended interpreter,
// the VmmMemoryBus bridge, the guest stack, and the ProsperoSyscallDispatcher
// wiring all cooperate against live emulator memory -- not a flat test buffer.
// ============================================================================
#include "cpu/jit_executor.hpp"
#include "cpu/fork_process.hpp"
#include "cpu/prospero_syscalls.hpp"
#include "memory/virtual_memory_manager.hpp"

#include <cstdint>
#include <iostream>
#include <unistd.h>
#include <vector>

namespace {

using PS5::CPU::CPUJitEngine;
using PS5::Memory::PageProt;
using PS5::Memory::VirtualMemoryManager;

constexpr uint32_t kRWX = static_cast<uint32_t>(PageProt::Read) |
                          static_cast<uint32_t>(PageProt::Write) |
                          static_cast<uint32_t>(PageProt::Exec);

int g_failures = 0;
int g_checks = 0;
bool Check(bool v, const char* e, int line) {
    ++g_checks;
    if (!v) { ++g_failures; std::cerr << "  [FAIL] line " << line << ": " << e << '\n'; }
    return v;
}
#define CHECK(e) Check((e), #e, __LINE__)

uint64_t MapCode(uint64_t gva, const std::vector<uint8_t>& code) {
    auto& vmm = VirtualMemoryManager::Instance();
    const uint64_t base = vmm.AllocateVirtual(gva, 0x10000, kRWX);
    if (base == 0) return 0;
    if (!vmm.CopyToGuest(base, code.data(), code.size(),
                         static_cast<uint32_t>(PageProt::Write))) {
        return 0;
    }
    return base;
}

// A guest function that computes (arg0 + arg1) * 2 using real control flow.
//   mov rax, rdi
//   add rax, rsi
//   shl rax, 1
//   ret
bool TestArithmeticGuest() {
    std::cout << "[Integration] (a+b)*2 in guest arena\n";
    std::vector<uint8_t> code = {
        0x48, 0x89, 0xF8, // mov rax, rdi
        0x48, 0x01, 0xF0, // add rax, rsi
        0x48, 0xD1, 0xE0, // shl rax, 1
        0xC3,             // ret
    };
    const uint64_t entry = MapCode(0x1000200000ULL, code);
    CHECK(entry != 0);
    const uint64_t result = CPUJitEngine::Instance().ExecuteGuestFull(entry, 20, 1);
    CHECK(result == 42);
    VirtualMemoryManager::Instance().FreeVirtual(entry, 0x10000);
    return true;
}

// A guest loop that sums 1..arg0 -> arg0*(arg0+1)/2. For arg0=100 -> 5050.
//   xor eax, eax        sum = 0
//   mov ecx, edi        i = arg0
// top:
//   add eax, ecx
//   dec ecx
//   jnz top
//   ret
bool TestLoopGuest() {
    std::cout << "[Integration] loop sum 1..N in guest arena\n";
    std::vector<uint8_t> code = {
        0x31, 0xC0,       // xor eax, eax
        0x89, 0xF9,       // mov ecx, edi
        0x01, 0xC8,       // add eax, ecx
        0xFF, 0xC9,       // dec ecx
        0x75, 0xFA,       // jnz -6
        0xC3,             // ret
    };
    const uint64_t entry = MapCode(0x1000210000ULL, code);
    CHECK(entry != 0);
    const uint64_t result = CPUJitEngine::Instance().ExecuteGuestFull(entry, 100, 0);
    CHECK(result == 5050);
    VirtualMemoryManager::Instance().FreeVirtual(entry, 0x10000);
    return true;
}

// A guest that invokes a real Prospero syscall (getpid, #20) and returns it.
//   mov eax, 20
//   syscall
//   ret
bool TestSyscallGuest() {
    std::cout << "[Integration] guest syscall getpid dispatch\n";
    std::vector<uint8_t> code = {
        0xB8, 0x14, 0x00, 0x00, 0x00, // mov eax, 20
        0x0F, 0x05,                   // syscall
        0xC3,                         // ret
    };
    const uint64_t entry = MapCode(0x1000220000ULL, code);
    CHECK(entry != 0);
    const uint64_t before = CPUJitEngine::Instance().GetInterceptedSyscallCount();
    const uint64_t result = CPUJitEngine::Instance().ExecuteGuestFull(entry, 0, 0);
    // Round 18: getpid returns the GUEST's stable process id (the round-17
    // handler leaked the HOST pid); the main thread reports the main pid.
    CHECK(result == 0x11000u);
    CHECK(CPUJitEngine::Instance().GetInterceptedSyscallCount() == before + 1);
    VirtualMemoryManager::Instance().FreeVirtual(entry, 0x10000);
    return true;
}

// A guest that writes to its own stack and reads it back, proving the private
// guest stack and memory operands work through the VMM bus.
//   mov qword [rsp-8], rdi
//   mov rax, qword [rsp-8]
//   ret
bool TestStackGuest() {
    std::cout << "[Integration] guest stack round-trip\n";
    std::vector<uint8_t> code = {
        0x48, 0x89, 0x7C, 0x24, 0xF8, // mov [rsp-8], rdi
        0x48, 0x8B, 0x44, 0x24, 0xF8, // mov rax, [rsp-8]
        0xC3,
    };
    const uint64_t entry = MapCode(0x1000230000ULL, code);
    CHECK(entry != 0);
    const uint64_t result = CPUJitEngine::Instance().ExecuteGuestFull(entry, 0xCAFEBABE, 0);
    CHECK(result == 0xCAFEBABE);
    VirtualMemoryManager::Instance().FreeVirtual(entry, 0x10000);
    return true;
}

// Execution must refuse a non-executable entry point (fail-closed).
bool TestNonExecutableRefused() {
    std::cout << "[Integration] non-executable entry refused\n";
    auto& vmm = VirtualMemoryManager::Instance();
    const uint32_t rw = static_cast<uint32_t>(PageProt::Read) |
                        static_cast<uint32_t>(PageProt::Write);
    const uint64_t base = vmm.AllocateVirtual(0x1000240000ULL, 0x10000, rw);
    CHECK(base != 0);
    const uint64_t result = CPUJitEngine::Instance().ExecuteGuestFull(base, 0, 0);
    CHECK(result == 0);
    vmm.FreeVirtual(base, 0x10000);
    return true;
}

} // namespace

int main() {
    std::cout << "=== Guest Execution Integration Test Suite ===\n";
    VirtualMemoryManager::Instance().InitializeArena();
    TestArithmeticGuest();
    TestLoopGuest();
    TestSyscallGuest();
    TestStackGuest();
    TestNonExecutableRefused();

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    if (g_failures == 0) {
        std::cout << ">> [PASS] Guest execution integration fully verified.\n";
        return 0;
    }
    std::cerr << ">> [FAIL] " << g_failures << " check(s) failed.\n";
    return 1;
}
