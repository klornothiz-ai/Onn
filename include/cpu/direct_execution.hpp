#pragma once
// ============================================================================
// ProsperoLayer RDNA2 Core - DirectExecutionBackend (round 20)
// ----------------------------------------------------------------------------
// The change of execution model the project documentation promised: guest
// x86-64 code runs NATIVELY on the host CPU. The guest and the host share the
// ISA (the PS5 CPU is x86-64 Zen 2), so instead of interpreting every
// instruction, the backend
//
//   * switches the host thread into the guest context (registers, RSP, FS
//     base) through a small assembly trampoline and JUMPS to the guest entry
//     point -- from that moment the code executes at hardware speed inside the
//     VMM's 16 GB arena (the arena already IS a contiguous host mapping at a
//     fixed offset, so guest addresses work unchanged);
//
//   * lets SIGNALS define the only exits:
//       - SIGSEGV on the unmapped stop sentinel  -> clean return,
//       - SIGSEGV anywhere else                  -> guest fault (recorded),
//       - SIGILL on an `ud2` the backend planted -> interception (below),
//       - SIGFPE/SIGTRAP/SIGBUS                  -> guest fault,
//       - SIGALRM                                -> wall-clock budget;
//
//   * INTERCEPTS exactly the instructions a host cannot serve faithfully by
//     rewriting their first two bytes to `ud2` (0F 0B) BEFORE the guest ever
//     reaches them (the same full-decoder block scanner the JIT uses walks
//     every basic block on first entry and patches):
//       - `syscall`  (FreeBSD-9 numbering!) -- serviced through the SAME
//         ProsperoSyscallDispatcher the interpreter uses, NEVER the host
//         kernel,
//       - cpuid / rdtsc / rdtscp -- virtualised through HostModels (a fixed
//         Zen 2 CPU + an invariant 3.5 GHz TSC), identical on every host,
//       - SSE4a / BMI1 / BMI2 / MOVBE / TZCNT/LZCNT -- the Zen 2 extensions
//         that are not on every Intel/AMD host: ALWAYS patched and replayed
//         through the interpreter core, so behaviour is bit-identical
//         everywhere (the "rare unsupported instructions get special
//         handling" rule);
//
//   * keeps the fail-closed contract: every precondition (backend enabled,
//     entry executable, VMM arena ready, budget) declines with a recorded
//     reason and the caller falls back to the interpreter -- exactly like the
//     GPU paths decline to the software rasterizer.
//
// Block discovery is incremental and sound for branches into the middle of
// discovered blocks: site patches live at instruction starts; a trap at a
// block entry restores the original two bytes, scans the block, patches its
// sites and arms a trap at its fall-through successor, then resumes.
//
// Defence in depth: while direct execution is active every entering thread
// installs a seccomp denylist filter (best effort) so the worst-case escape --
// a host syscall executed because two threads raced into a never-executed
// block -- returns EPERM instead of reaching fork/execve/ptrace/etc. The
// guest's own syscalls always go through the dispatcher (they are patched).
// ============================================================================
#include "cpu/x86_64_interpreter.hpp"
#include "memory/virtual_memory_manager.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace PS5::CPU {

// The same return sentinel the interpreter paths push (see
// CPUJitEngine::ExecuteGuestFull / GuestThreadManager::ThreadBody): a clean
// top-level `ret` lands here (an unmapped address) and the backend stops with
// DirectStopReason::Returned.
constexpr uint64_t kDirectStopSentinel = 0x0000FEEDDEAD0000ULL;

enum class DirectStopReason {
    None = 0,
    Returned,          // ret to the stop sentinel
    ThreadExit,        // the syscall handler asked to unwind (thr_exit / exit)
    GuestFault,        // SIGSEGV/SIGBUS at a guest address
    FpuFault,          // SIGFPE
    IllegalInstruction,// SIGILL at something we did not plant
    Trap,              // SIGTRAP (int3)
    Timeout,           // the wall-clock budget expired
    HostError,         // backend misuse / precondition failure (never entered)
};

const char* ToString(DirectStopReason reason);

struct DirectRunStats {
    size_t blocks_discovered{0};     // basic blocks scanned + patched
    size_t sites_patched{0};         // interception sites rewritten to ud2
    size_t syscalls_serviced{0};     // guest syscalls through the dispatcher
    size_t instructions_emulated{0}; // patched instructions replayed
    size_t reentries{0};             // trampoline entries (one per trap)
    size_t faults{0};
};

// How the run ended + diagnostics, mirroring RunResult for the interpreter.
struct DirectRunOutcome {
    DirectStopReason reason{DirectStopReason::None};
    uint64_t fault_gva{0};           // faulting address (guest view)
    uint64_t fault_rip{0};
    std::string message;             // decline reason (HostError) / detail
    DirectRunStats stats;
};

class DirectExecutionBackend {
public:
    static DirectExecutionBackend& Instance();

    // Installs the trap handlers (chained when not executing), lifts the VMM
    // arena to host-executable and arms the seccomp denylist (per-thread, at
    // first entry). Idempotent; false only when the arena is missing.
    bool Enable();
    void Disable();
    bool IsEnabled() const { return m_enabled; }

    // Runs ONE guest function natively. The contract mirrors the interpreter:
    //   * cpu.gpr[RDI]/[RSI] carry arg0/arg1, cpu.rip is the entry, cpu.fs_base
    //     is loaded into the REAL FS base (arch_prctl), cpu.gpr[RSP] MUST point
    //     at the stop sentinel (the caller pushes it, like the interpreter
    //     paths do).
    //   * syscalls (patched `syscall` sites) go through `syscalls` -- the same
    //     std::function<bool(CpuState&, GuestMemoryBus&)> the interpreter uses.
    //     Returning false unwinds (reason ThreadExit; the exit code is RAX).
    //   * returns RAX at the stop; `out` reports how the run ended.
    //   * budget_ms bounds the WHOLE run wall-clock (SIGALRM) so a runaway
    //     guest loop cannot hang the emulator; 0 = no budget.
    // On ANY precondition failure the function declines (reason HostError,
    // message set) and touches nothing.
    uint64_t RunFunction(CpuState& cpu, GuestMemoryBus& bus,
                         const SyscallHandler& syscalls,
                         uint64_t stop_sentinel,
                         uint64_t budget_ms,
                         DirectRunOutcome& out);

    // ---- headless-testable pure pieces -----------------------------------

    // Fetches up to `max_bytes` of guest code at gva into `code` (stopping at
    // the first uncommitted page). Returns false when gva itself is not
    // executable.
    bool FetchCode(uint64_t gva, size_t max_bytes,
                   std::vector<uint8_t>& code) const;

    // The ud2 patch write (2 bytes at gva) with the mprotect elevation dance
    // around it -- code pages are R+X in the guest, so the emulator must
    // briefly lift the page to RWX. Returns false when gva is unmapped.
    bool WriteCodeBytes(uint64_t gva, const uint8_t* bytes, size_t len);

    // Scans the block at gva, patches every interception site inside it
    // (persisting in the registry), arms an entry trap at the block's fall-
    // through successor and returns the scan result. Fail-closed: a decode
    // failure returns the failure status and patches nothing.
    BlockInspectResult DiscoverBlock(uint64_t gva, bool* ok);

    // Replays ONE patched instruction through the interpreter core (the
    // original bytes are re-executed against a scratch bus that serves
    // instruction fetches from the saved bytes and everything else from the
    // REAL guest bus -- so emulation is BY CONSTRUCTION identical to the
    // interpreter). Advances cpu.rip on success; returns false on failure.
    bool EmulatePatchedInstruction(CpuState& cpu, GuestMemoryBus& bus,
                                   uint64_t gva);

    // ---- registry / stats --------------------------------------------------
    size_t PatchedSiteCount() const;
    size_t DiscoveredBlockCount() const;
    bool HasSiteAt(uint64_t gva) const;
    PatchKind SiteKindAt(uint64_t gva) const;
    const DirectRunStats& LifetimeStats() const { return m_lifetime; }

    // Restores every patched byte (sites + armed entry traps) and forgets the
    // registry. Test isolation helper: after this the guest code is bit-
    // identical to what was loaded.
    void ResetAllPatches();

private:
    DirectExecutionBackend() = default;
    DirectExecutionBackend(const DirectExecutionBackend&) = delete;
    DirectExecutionBackend& operator=(const DirectExecutionBackend&) = delete;

    struct SiteRecord {
        uint8_t original[16]{};
        uint8_t length{0};       // full instruction length (>= 2)
        uint8_t patched{2};      // how many leading bytes were replaced
        bool armed_entry{false}; // true = transient block-entry trap
        PatchKind kind{PatchKind::None};
    };

    void EnsureHandlersInstalled();
    bool ArmEntryTrapLocked(uint64_t gva);
    void DisarmEntryTrapLocked(uint64_t gva);
    void RestoreSiteLocked(uint64_t gva, SiteRecord& rec);

    // Round 20: the VMM code-write notifier target -- drops patches + block
    // discovery for a rewritten code range (see Enable()).
    void InvalidateRange(uint64_t gva, size_t size);

    bool m_enabled{false};
    bool m_handlers_installed{false};
    int m_active_runs{0};
    std::string m_fatal_error;   // set when an interception site cannot be patched

    mutable std::mutex m_registry_mutex;
    std::unordered_map<uint64_t, SiteRecord> m_sites;
    std::unordered_set<uint64_t> m_discovered;

    DirectRunStats m_lifetime;
};

} // namespace PS5::CPU
