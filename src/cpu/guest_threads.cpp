// ============================================================================
// ProsperoLayer RDNA2 Core - Real guest threads on host threads (round 15)
// ============================================================================
#include "cpu/guest_threads.hpp"

#include "cpu/prospero_syscalls.hpp"
#include "cpu/vmm_memory_bus.hpp"
#include "cpu/jit_executor.hpp"
#include "cpu/direct_execution.hpp"
#include "memory/virtual_memory_manager.hpp"

#include <chrono>

namespace PS5::CPU {

using Memory::PageProt;
using Memory::VirtualMemoryManager;

thread_local GuestThreadRecord* GuestThreadManager::t_current = nullptr;

namespace {
constexpr uint32_t kRwProt = static_cast<uint32_t>(PageProt::Read) |
                             static_cast<uint32_t>(PageProt::Write);
// A sentinel return address that never maps: a top-level `ret` unwinds here
// and stops the thread cleanly instead of faulting through guest memory.
constexpr uint64_t kStopSentinel = 0x0000FEEDDEAD0000ULL;
} // namespace

GuestThreadManager& GuestThreadManager::Instance() {
    static GuestThreadManager inst;
    return inst;
}

GuestThreadManager::GuestThreadManager() = default;

GuestThreadManager::~GuestThreadManager() {
    std::vector<std::shared_ptr<std::thread>> workers;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& [tid, rec] : m_threads) {
            if (rec && rec->host_worker && rec->host_worker->joinable()) {
                workers.push_back(rec->host_worker);
            }
        }
    }
    for (auto& w : workers) {
        w->join();
    }
}

void GuestThreadManager::SetTlsAllocator(std::function<uint64_t()> allocator) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_tls_allocator = std::move(allocator);
    m_tls_allocator_set = true;
}

bool GuestThreadManager::InGuestThread() {
    return t_current != nullptr;
}

uint32_t GuestThreadManager::CurrentTid() const {
    return t_current ? t_current->tid : 0;
}

bool GuestThreadManager::HandleThreadExit(int code) {
    if (t_current == nullptr) {
        return false;
    }
    t_current->exit_code.store(code, std::memory_order_release);
    t_current->exit_was_syscall.store(true, std::memory_order_release);
    return true;
}

uint32_t GuestThreadManager::SpawnThread(uint64_t entry_gva, uint64_t arg,
                                          const char* name, size_t stack_size,
                                          size_t instruction_limit) {
    auto& vmm = VirtualMemoryManager::Instance();
    if (!vmm.IsGvaMapped(entry_gva) || !vmm.IsGvaExecutable(entry_gva)) {
        return 0;
    }
    if (stack_size == 0) {
        stack_size = 1 * 1024 * 1024;
    }

    // Everything the thread needs is allocated HERE, on the spawning thread,
    // so the VMM is only ever mutated serially during setup.
    const uint64_t stack_base = vmm.AllocateVirtual(0, stack_size, kRwProt);
    if (stack_base == 0) {
        return 0;
    }

    uint64_t tls_gva = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_tls_allocator_set && m_tls_allocator) {
            tls_gva = m_tls_allocator();
        }
    }

    auto rec = std::make_shared<GuestThreadRecord>();
    rec->entry_gva = entry_gva;
    rec->stack_gva = stack_base;
    rec->stack_size = stack_size;
    rec->tls_gva = tls_gva;
    rec->name = name != nullptr ? name : "guest-thread";

    uint32_t tid = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        tid = m_next_tid++;
        rec->tid = tid;
        m_threads[tid] = rec;
    }

    auto worker = std::make_shared<std::thread>(
        [this, rec, arg, instruction_limit]() { ThreadBody(rec, arg, instruction_limit); });
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        rec->host_worker = worker;
    }
    return tid;
}

void GuestThreadManager::ThreadBody(std::shared_ptr<GuestThreadRecord> rec,
                                     uint64_t arg, size_t instruction_limit) {
    t_current = rec.get();
    struct CurrentGuard {
        ~CurrentGuard() { t_current = nullptr; }
    } guard;

    auto& vmm = VirtualMemoryManager::Instance();
    VmmMemoryBus bus(vmm);

    CpuState cpu{};
    cpu.rip = rec->entry_gva;
    cpu.gpr[RDI] = arg;
    cpu.gpr[RSI] = 0;
    cpu.fs_base = rec->tls_gva;

    // Per-thread guest stack: top-aligned, a stop sentinel pushed so a
    // top-level `ret` unwinds cleanly.
    uint64_t rsp = rec->stack_gva + rec->stack_size - 256;
    rsp &= ~15ULL;
    rsp -= 8;
    bus.Write(rsp, &kStopSentinel, sizeof(kStopSentinel));
    cpu.gpr[RSP] = rsp;

    X86Interpreter interp(cpu, bus);
    interp.SetSyscallHandler([rec](CpuState& s, GuestMemoryBus&) {
        // x86-64 syscall ABI: number in rax, args rdi/rsi/rdx/r10/r8/r9.
        // thr_exit / exit terminate THIS thread instead of running through.
        const uint64_t nr = s.gpr[RAX];
        if (nr == static_cast<uint64_t>(ProsperoSyscall::SC_SYS_thr_exit)) {
            const int code = static_cast<int>(s.gpr[RDI] & 0xFFu);
            GuestThreadManager::Instance().HandleThreadExit(code);
            return false; // unwind the interpreter loop
        }
        if (nr == static_cast<uint64_t>(ProsperoSyscall::SC_SYS_exit)) {
            // Process-wide exit: record the code, still terminate this thread.
            SyscallContext ctx{};
        ctx.cpu = &s;
            ctx.rax = s.gpr[RAX];
            ctx.rdi = s.gpr[RDI];
            ctx.rsi = s.gpr[RSI];
            ctx.rdx = s.gpr[RDX];
            ctx.rcx = s.gpr[R10];
            ctx.r8 = s.gpr[R8];
            ctx.r9 = s.gpr[R9];
            s.gpr[RAX] = ProsperoSyscallDispatcher::Instance().Dispatch(ctx);
            GuestThreadManager::Instance().HandleThreadExit(
                static_cast<int>(s.gpr[RDI] & 0xFFu));
            return false;
        }
        SyscallContext ctx{};
        ctx.cpu = &s;
        ctx.rax = s.gpr[RAX];
        ctx.rdi = s.gpr[RDI];
        ctx.rsi = s.gpr[RSI];
        ctx.rdx = s.gpr[RDX];
        ctx.rcx = s.gpr[R10];
        ctx.r8 = s.gpr[R8];
        ctx.r9 = s.gpr[R9];
        s.gpr[RAX] = ProsperoSyscallDispatcher::Instance().Dispatch(ctx);
        return true;
    });

    // Round 20: when the engine-wide backend switch selects direct execution,
    // run the thread's guest code NATIVELY first; any decline (backend off,
    // non-executable entry, unpatchable site, timeout) falls back to the
    // interpreter with identical guest-visible semantics (fail-closed).
    if (CPUJitEngine::Instance().GetExecutionBackend() ==
        GuestExecutionBackend::Direct) {
        DirectRunOutcome outcome{};
        const uint64_t direct_result = DirectExecutionBackend::Instance().RunFunction(
            cpu, bus, interp.GetSyscallHandlerForDirect(), kStopSentinel,
            /*budget_ms=*/10000, outcome);
        const bool clean = outcome.reason == DirectStopReason::Returned ||
                           outcome.reason == DirectStopReason::ThreadExit;
        if (clean) {
            rec->executed_instructions = 0;  // native run: no per-instruction count
            rec->stop_status = outcome.reason == DirectStopReason::Returned
                ? ExecStatus::Returned
                : ExecStatus::SyscallDenied;
            if (!rec->exit_was_syscall.load(std::memory_order_acquire)) {
                rec->exit_code.store(
                    static_cast<int>((outcome.reason == DirectStopReason::Returned
                          ? direct_result : cpu.gpr[RAX]) & 0x7FFFFFFFull),
                    std::memory_order_release);
            }
            FinishThread(rec, vmm);
            return;
        }
        // Declined: the interpreter below runs the same code unchanged.
    }

    const RunResult result = interp.Run(instruction_limit, kStopSentinel);
    rec->executed_instructions = result.executed;
    rec->stop_status = result.status;
    if (!rec->exit_was_syscall.load(std::memory_order_acquire)) {
        // Natural return from the entry function: exit code = RAX.
        rec->exit_code.store(static_cast<int>(cpu.gpr[RAX] & 0x7FFFFFFFull),
                             std::memory_order_release);
    }

    FinishThread(rec, vmm);
}

// Shared completion path (both the interpreter and the round-20 direct path
// end here): reclaim the guest stack + TLS block, publish the exit state and
// wake joiners.
void GuestThreadManager::FinishThread(std::shared_ptr<GuestThreadRecord> rec,
                                       VirtualMemoryManager& vmm) {
    // Reclaim the stack. TLS stays if a late joiner wants to inspect it --
    // no: keep the model tight, free both; records keep the GVs for
    // diagnostics only.
    vmm.FreeVirtual(rec->stack_gva, rec->stack_size);
    if (rec->tls_gva != 0) {
        constexpr size_t kTlsBlockSize = 0x1000; // matches AllocateThreadTls granularity floor
        vmm.FreeVirtual(rec->tls_gva, kTlsBlockSize);
    }

    rec->finished.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (rec->detached.load(std::memory_order_acquire)) {
            m_threads.erase(rec->tid);
            m_reapable.push_back(rec);
        }
    }
    m_join_cv.notify_all();
}

bool GuestThreadManager::JoinThread(uint32_t tid, int64_t timeout_us, int* exit_code) {
    std::unique_lock<std::mutex> lock(m_mutex);
    const auto it = m_threads.find(tid);
    if (it == m_threads.end() || !it->second) {
        return false;
    }
    const auto rec = it->second;
    if (rec->detached.load(std::memory_order_acquire)) {
        return false;
    }
    bool done;
    if (timeout_us < 0) {
        m_join_cv.wait(lock, [&] { return rec->finished.load(); });
        done = true;
    } else {
        done = m_join_cv.wait_for(lock, std::chrono::microseconds(timeout_us),
                                   [&] { return rec->finished.load(); });
    }
    if (!done) {
        return false;
    }
    if (exit_code != nullptr) {
        *exit_code = rec->exit_code.load(std::memory_order_acquire);
    }
    // Reap the record + join the host worker so the thread resource is gone.
    auto worker = rec->host_worker;
    m_threads.erase(tid);
    lock.unlock();
    if (worker && worker->joinable() && worker->get_id() != std::this_thread::get_id()) {
        worker->join();
    }
    return true;
}

bool GuestThreadManager::DetachThread(uint32_t tid) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_threads.find(tid);
    if (it == m_threads.end() || !it->second) {
        return false;
    }
    if (it->second->detached.exchange(true) == true) {
        return false;
    }
    if (it->second->finished.load()) {
        m_threads.erase(tid);
    }
    return true;
}

size_t GuestThreadManager::TrackedThreadCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_threads.size();
}

size_t GuestThreadManager::RunningThreadCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t n = 0;
    for (const auto& [tid, rec] : m_threads) {
        if (rec && !rec->finished.load()) {
            ++n;
        }
    }
    return n;
}

std::vector<uint32_t> GuestThreadManager::ThreadIds() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<uint32_t> ids;
    ids.reserve(m_threads.size());
    for (const auto& [tid, rec] : m_threads) {
        ids.push_back(tid);
    }
    return ids;
}

} // namespace PS5::CPU
