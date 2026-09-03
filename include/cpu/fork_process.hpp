#pragma once
// ============================================================================
// ProsperoLayer RDNA2 Core - Real fork/wait4 model (round 18)
// ----------------------------------------------------------------------------
// The honest process model the syscall layer was missing (vfork used to fail
// closed and wait4 was a stub):
//
//   * fork(): an EAGER snapshot of the whole committed guest address space
//     (VirtualMemoryManager::SnapshotCommitted) plus the caller's CpuState;
//     the child runs on its own host thread against the SNAPSHOT bus, so its
//     loads/stores are fully isolated from the parent (writes in the child
//     never reach the parent's VMM and vice versa).
//   * The child resumes at the instruction after the syscall with rax = 0;
//     the parent gets the child pid.
//   * wait4() really waits: WNOHANG polls, a blocking wait parks until the
//     child records its exit code (or a bounded timeout), and the FreeBSD
//     wait status (exit_code << 8) is written to the guest status pointer.
//   * getpid()/getppid(): stable process ids (the main image gets one, every
//     fork child its own; a child's getpid reports the child pid).
//
// Documented boundary (kept honest): the child's syscalls that touch guest
// memory through the kernel bridge (read/write/umtx/...) operate on the
// PARENT address space -- only the child's own instruction loads/stores are
// isolated. A child that wants observable effects uses registers + exit().
// ============================================================================

#include "cpu/x86_64_interpreter.hpp"
#include "memory/virtual_memory_manager.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace PS5::CPU {

struct ForkChildRecord {
    uint32_t pid{0};
    std::atomic<int> exit_code{0};
    std::atomic<bool> finished{false};
    bool reaped{false};
    size_t instructions{0};
    ExecStatus stop_status{ExecStatus::Running};
    std::shared_ptr<std::thread> host_worker;
};

class ForkProcessManager {
public:
    static ForkProcessManager& Instance();

    // Snapshot + spawn. Returns the child pid (0 on failure).
    uint32_t ForkFrom(const CpuState& parent,
                      size_t instruction_limit = 50'000'000);

    // wait4 over the fork children. pid_wanted == -1 matches any child.
    // options bit 1 (WNOHANG) polls instead of blocking. Fills out_pid and
    // the raw wait status. Returns false when no (matching, unreaped) child
    // exists at all (ECHILD semantics).
    bool WaitChild(int32_t pid_wanted, uint32_t options, int32_t& out_pid,
                   int32_t& out_status, int64_t timeout_us = 10'000'000);

    size_t UnreapedChildCount() const;

    static constexpr uint32_t MainPidValue = 0x11000;
    static constexpr uint32_t MainParentPidValue = 1;
    static uint32_t MainPid() { return MainPidValue; }
    static uint32_t MainParentPid() { return MainParentPidValue; }
    // getpid for the CALLING thread: fork children report their own pid.
    static uint32_t CurrentPid();
    // True when the calling host thread is a fork child.
    static bool InForkChild();

    ~ForkProcessManager();

    ForkProcessManager(const ForkProcessManager&) = delete;
    ForkProcessManager& operator=(const ForkProcessManager&) = delete;

private:
    ForkProcessManager();

    void ChildBody(std::shared_ptr<ForkChildRecord> rec, CpuState state,
                   std::shared_ptr<class ForkSnapshotBus> bus,
                   size_t instruction_limit);

    mutable std::mutex m_mutex;
    std::vector<std::shared_ptr<ForkChildRecord>> m_children;
    uint32_t m_next_pid{0x2000};
};

} // namespace PS5::CPU
