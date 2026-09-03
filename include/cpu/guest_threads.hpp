#pragma once
// ============================================================================
// ProsperoLayer RDNA2 Core - Real guest threads on host threads (round 15)
// ----------------------------------------------------------------------------
// The documented "deliberately next" step: thr_new used to run the tiny
// fail-closed subset executor on a host-mmap'd stack. This subsystem gives
// guest threads the real thing:
//
//   * a VMM-allocated per-thread stack (guest-visible, protection-checked)
//   * a per-thread TLS block (fresh copy of the main module's PT_TLS template,
//     self-pointer at fs:[0]) -- the same layout GuestLauncher uses
//   * the FULL extended X86Interpreter (all modelled ISA: ALU/SSE/control
//     flow) with a per-thread CpuState
//   * ProsperoSyscallDispatcher syscalls, with thr_exit / exit intercepted
//     per-thread so a guest thread terminates exactly where the guest asked
//   * join / detach with exit-code propagation
//
// Concurrency model: each guest thread runs on its own host thread through
// the shared mutex-protected VirtualMemoryManager; stack + TLS are allocated
// on the SPAWNING thread so VMM writes are serialized before the thread
// starts executing.
// ============================================================================

#include "cpu/x86_64_interpreter.hpp"
#include "memory/virtual_memory_manager.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace PS5::CPU {

struct GuestThreadRecord {
    uint32_t tid{0};
    std::string name;
    std::atomic<int> exit_code{0};
    std::atomic<bool> finished{false};
    std::atomic<bool> detached{false};
    std::atomic<bool> exit_was_syscall{false};
    uint64_t stack_gva{0};
    size_t stack_size{0};
    uint64_t tls_gva{0};
    uint64_t entry_gva{0};
    size_t executed_instructions{0};
    ExecStatus stop_status{ExecStatus::Running};
    std::shared_ptr<std::thread> host_worker;
};

class GuestThreadManager {
public:
    static GuestThreadManager& Instance();

    // Installs the per-thread TLS allocator (the main module's PT_TLS
    // template). Called by GuestLauncher::Boot. Returning 0 means "no TLS
    // template" (fs_base stays 0, matching pre-TLS behaviour).
    void SetTlsAllocator(std::function<uint64_t()> allocator);

    // Spawns a REAL guest thread: allocates its VMM stack + TLS block, then
    // starts a host thread that executes `entry_gva(arg)` on the full
    // interpreter until the guest returns, calls thr_exit, or exhausts the
    // instruction budget. Returns the new tid (0 on allocation failure).
    uint32_t SpawnThread(uint64_t entry_gva, uint64_t arg,
                         const char* name = "guest-thread",
                         size_t stack_size = 1 * 1024 * 1024,
                         size_t instruction_limit = 100000000);

    // Waits for a spawned thread to finish. Returns true when the thread was
    // joined and (optionally) delivers its exit code. timeout_us < 0 waits
    // forever; 0 polls.
    bool JoinThread(uint32_t tid, int64_t timeout_us, int* exit_code = nullptr);

    // Marks a thread detached: its record is reclaimed once it finishes and
    // JoinThread will refuse it afterwards.
    bool DetachThread(uint32_t tid);

    // tid of the calling host thread if it is a managed guest thread,
    // 0 otherwise.
    uint32_t CurrentTid() const;

    // Number of threads that have been spawned and not yet reaped/detached
    // (includes still-running and finished-but-unjoined threads).
    size_t TrackedThreadCount() const;
    size_t RunningThreadCount() const;

    // Tids of all tracked (not yet joined/detached-reaped) threads.
    std::vector<uint32_t> ThreadIds() const;

    // Records thr_exit(code) for the calling managed thread and returns
    // false so the interpreter unwinds. Non-managed callers return true
    // (the dispatcher's generic thr_exit handler applies).
    bool HandleThreadExit(int code);

    // True when the calling thread is a managed guest thread.
    static bool InGuestThread();

    ~GuestThreadManager();

    GuestThreadManager(const GuestThreadManager&) = delete;
    GuestThreadManager& operator=(const GuestThreadManager&) = delete;

private:
    GuestThreadManager();

    void ThreadBody(std::shared_ptr<GuestThreadRecord> rec, uint64_t arg,
                    size_t instruction_limit);

    // Round 20: shared completion path for the interpreter and direct paths.
    void FinishThread(std::shared_ptr<GuestThreadRecord> rec,
                      Memory::VirtualMemoryManager& vmm);

    mutable std::mutex m_mutex;
    std::condition_variable m_join_cv;
    std::unordered_map<uint32_t, std::shared_ptr<GuestThreadRecord>> m_threads;
    std::vector<std::shared_ptr<GuestThreadRecord>> m_reapable;
    std::function<uint64_t()> m_tls_allocator;
    uint32_t m_next_tid{1};
    bool m_tls_allocator_set{false};

    static thread_local GuestThreadRecord* t_current;
};

} // namespace PS5::CPU
