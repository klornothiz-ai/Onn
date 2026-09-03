#pragma once

#include "cpu/x86_64_subset_interpreter.hpp"
#include "cpu/direct_execution.hpp"
#include "memory/virtual_memory_manager.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace PS5::CPU {

// Round 20: which engine executes guest code. Interpreter is the historical
// default (every existing caller keeps it); Direct runs the code natively on
// the host CPU through DirectExecutionBackend with identical guest-visible
// behaviour (same syscalls through ProsperoSyscallDispatcher, same virtual
// cpuid/rdtsc, the Zen 2 extensions emulated bit-identically) and falls back
// to the interpreter on ANY direct-mode decline (fail-closed).
enum class GuestExecutionBackend {
    Interpreter,
    Direct,
};

struct JITBasicBlock {
    uint64_t guest_rip{0};
    // Kept for ABI compatibility. The engine never creates host-executable guest code.
    void* host_code_ptr{nullptr};
    size_t code_size{0};
    size_t instruction_count{0};
    bool contains_syscall{false};
    bool is_compiled{false};
    // Round 20: the host-dependent interception sites inside this block
    // (syscall/cpuid/rdtsc/rdtscp/SSE4a/BMI/movbe/tzcnt) -- the same
    // classification the direct backend patches with.
    std::vector<PatchSiteInfo> sites;

    // Basic block chaining: the exit of this block links directly to the
    // entry of the next block, skipping the cache lookup on every
    // transition. 0 means "not chained"; a nonzero value is the guest RIP
    // of the chained successor block.
    uint64_t next_block_rip{0};
    // Reverse link for invalidation: the predecessor block that chains into
    // this one, or 0 when none.
    uint64_t prev_block_rip{0};
    // How many times the chained successor has been taken from this block
    // (statistical only).
    uint64_t chain_count{0};
};

class CPUJitEngine {
public:
    static CPUJitEngine& Instance();
    ~CPUJitEngine() = default;

    JITBasicBlock* CompileBasicBlock(uint64_t guest_rip);
    GuestExecutionResult ExecuteGuestCodeChecked(uint64_t entry_gva,
                                                 uint64_t arg0 = 0,
                                                 uint64_t arg1 = 0);
    uint64_t ExecuteGuestCode(uint64_t entry_gva, uint64_t arg0 = 0, uint64_t arg1 = 0);

    // Full-fidelity execution path: runs the extended X86Interpreter directly
    // over the live guest arena (VirtualMemoryManager), dispatching syscalls to
    // ProsperoSyscallDispatcher. Unlike ExecuteGuestCodeChecked (which only
    // accepts the tiny fail-closed subset), this executes real control flow,
    // ModRM/SIB memory operands, the ALU/flag model, and the stack. rdi/rsi
    // carry arg0/arg1 per the SysV ABI. Returns the value left in rax when the
    // guest returns to the caller-provided sentinel, or 0 on fault.
    uint64_t ExecuteGuestFull(uint64_t entry_gva, uint64_t arg0 = 0,
                              uint64_t arg1 = 0, size_t instruction_limit = 1000000,
                              uint64_t fs_base = 0);

    // Deprecated compatibility shim. Guest execution no longer uses process-wide signals.
    void InitializeSignalHandlers() {}

    size_t GetCachedBlockCount() const;
    size_t GetTotalJitAllocated() const { return 0; }
    uint64_t GetInterceptedSyscallCount() const { return m_intercepted_syscalls.load(); }

    // Basic block chaining: link the block at from_rip so its exit jumps
    // directly to the block at to_rip, skipping the cache lookup. Returns
    // true when the chain was established, false when either block is
    // missing (fail-closed: caller falls back to normal lookup).
    bool ChainBlocks(uint64_t from_rip, uint64_t to_rip);
    // Returns the block chained after the block at rip (i.e. the block at
    // rip's next_block_rip), or nullptr when rip is not cached or not
    // chained (caller does normal lookup).
    JITBasicBlock* GetChainedBlock(uint64_t rip);
    // Invalidate a cached block: clears its chains and removes it from the
    // cache so it is recompiled on next use. Any predecessor chain into it
    // is also cleared.
    void InvalidateBlock(uint64_t rip);

    // Block-chaining statistics.
    uint64_t GetTotalBlocksCompiled() const { return m_total_blocks_compiled.load(); }
    uint64_t GetTotalChainsCreated() const { return m_total_chains_created.load(); }
    uint64_t GetTotalChainHits() const { return m_total_chain_hits.load(); }

    // Round 15: the thr_exit code observed by the last ExecuteGuestFull run
    // (valid when that run ended through the thr_exit unwind path).
    int LastThreadExitCode() const;

    // Round 20: the execution backend switch + the diagnostics of the last
    // direct run (empty reason when the interpreter ran).
    void SetExecutionBackend(GuestExecutionBackend backend) { m_backend = backend; }
    GuestExecutionBackend GetExecutionBackend() const { return m_backend; }
    const DirectRunOutcome& LastDirectOutcome() const { return m_last_direct; }

private:
    CPUJitEngine() = default;

    std::vector<uint8_t> FetchGuestCode(uint64_t guest_rip, bool* fetch_fault) const;

    mutable std::mutex m_jit_mutex;
    std::unordered_map<uint64_t, std::unique_ptr<JITBasicBlock>> m_block_cache;
    std::atomic<uint64_t> m_intercepted_syscalls{0};
    // Block-chaining statistics.
    std::atomic<uint64_t> m_total_blocks_compiled{0};
    std::atomic<uint64_t> m_total_chains_created{0};
    std::atomic<uint64_t> m_total_chain_hits{0};
    int m_last_thread_exit_code{0};
    uint64_t m_last_fault_rip{0};      // round 30: guest-fault telemetry
    uint64_t m_last_fault_addr{0};
    bool m_thread_exit_requested{false};
    GuestExecutionBackend m_backend{GuestExecutionBackend::Interpreter};
    DirectRunOutcome m_last_direct{};
};

} // namespace PS5::CPU
