// hle_trampoline.hpp — guest-callable trampolines for host HLE functions.
//
// Round 30. Real PS5 eboots call their imports through PLT/GOT slots; the
// runtime linker skips JUMP_SLOT/GLOB_DAT relocations for undefined symbols
// because no provider module exists — the slots keep their link-time
// resolver addresses and the first `call [got]` jumps into unmapped space
// (observed live on the real 254 MB Minecraft eboot: guest-fault at
// rip=0xb7b1546, the lazy-binding PLT stub, missing its load bias).
//
// The missing piece is HLE dynamic linking: every host HLE function gets a
// small EXECUTABLE stub inside the guest arena:
//
//     mov  eax, <stub id + magic base>     ; b8 xx xx xx xx
//     syscall                              ; 0f 34   (both engines intercept)
//     ret                                  ; c3
//
// Both execution engines route `syscall` through the SAME service path
// (the interpreter decodes it; the direct backend rewrites the site to a
// trap). The dispatcher recognizes the magic number range and calls the
// registered host function with the guest's current register arguments —
// the SysV ABI (rdi, rsi, rdx, rcx, r8, r9) is already in place, so the
// HLE function runs with the exact arguments the guest passed.
//
// On the interpreter the host function executes as C++ inside the step
// loop; on the direct backend it runs NATIVELY at full speed (its own
// libc calls are real host calls, untouched by the guest interception).
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace PS5::CPU {

// Magic syscall-number base for HLE trampoline calls. FreeBSD/Prospero
// syscall numbers are small (< 1000), so this range can never collide.
// Bit 30 (the x32-ABI flag on Linux x86-64) MUST stay clear: a leaked
// stub syscall with that bit set makes the host kernel raise SIGSYS and
// kill the process instead of returning ENOSYS.
constexpr uint64_t kHleSyscallBase = 0x2CE00000ull;
constexpr uint32_t kHleMaxStubs = 8192;
constexpr uint64_t kHleStubSize = 16;

class HleTrampolines {
public:
    static HleTrampolines& Instance();

    // True when a syscall number is an HLE trampoline call.
    static bool IsHleCall(uint64_t syscall_nr) {
        return syscall_nr >= kHleSyscallBase &&
               syscall_nr < kHleSyscallBase + kHleMaxStubs;
    }

    // Returns a guest-callable executable stub GVA for a host function
    // (one stub per function, cached). 0 on failure (VMM exhausted).
    // `name` (optional) is recorded for the HLE call trace.
    uint64_t StubFor(const void* host_func, const char* name = nullptr);

    // Executes the host function with the guest-passed arguments.
    // Returns the function result; 0 when the number is not a stub.
    uint64_t Dispatch(uint64_t syscall_nr, uint64_t a0, uint64_t a1,
                      uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) const;

    size_t StubCount() const { return m_funcs.size(); }
    uint64_t region_base() const { return m_region_base; }

    // The direct backend registers this at Enable() time: freshly emitted
    // stubs are reached through indirect jumps, so their syscall sites must
    // be pre-discovered (rewritten to interception traps) instead of
    // running natively into the seccomp guard. Kept as a plain hook so the
    // trampoline module links without the backend (interpreter-only tests).
    using StubPreDiscoverHook = void (*)(uint64_t stub_gva);
    static void SetStubPreDiscoverHook(StubPreDiscoverHook hook);

private:
    bool EnsureRegion();
    uint64_t EmitStub(uint32_t id);

    std::unordered_map<const void*, uint32_t> m_by_func;
    std::vector<const void*> m_funcs;
    std::vector<std::string> m_names;
    uint64_t m_region_base = 0;
    uint32_t m_emitted = 0;
};

} // namespace PS5::CPU
