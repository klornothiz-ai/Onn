// ============================================================================
// ProsperoLayer RDNA2 Core - Real fork/wait4 model (round 18)
// ============================================================================
#include "cpu/fork_process.hpp"
#include "cpu/prospero_syscalls.hpp"

#include <chrono>
#include <cstring>

namespace PS5::CPU {

// A private address space for a fork child: an eager copy of the parent's
// committed regions. Reads and writes both hit the copy (full isolation).
class ForkSnapshotBus final : public GuestMemoryBus {
public:
    explicit ForkSnapshotBus(std::vector<PS5::Memory::CommittedRegion> regions)
        : m_regions(std::move(regions)) {}

    bool Read(uint64_t addr, void* dst, size_t size) override {
        for (const auto& r : m_regions) {
            if (addr >= r.gva && addr - r.gva + size <= r.size) {
                std::memcpy(dst, r.bytes.data() + (addr - r.gva), size);
                return true;
            }
        }
        return false;
    }
    bool Write(uint64_t addr, const void* src, size_t size) override {
        for (auto& r : m_regions) {
            if (addr >= r.gva && addr - r.gva + size <= r.size) {
                std::memcpy(r.bytes.data() + (addr - r.gva), src, size);
                return true;
            }
        }
        return false;
    }

private:
    std::vector<PS5::Memory::CommittedRegion> m_regions;
};

namespace {

thread_local ForkChildRecord* t_current_child = nullptr;

} // namespace

ForkProcessManager& ForkProcessManager::Instance() {
    static ForkProcessManager instance;
    return instance;
}

ForkProcessManager::ForkProcessManager() = default;

ForkProcessManager::~ForkProcessManager() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& rec : m_children) {
        if (rec->host_worker && rec->host_worker->joinable()) {
            // Children are bounded by their instruction limits; a still-live
            // child at teardown is joined so the host thread ends cleanly.
            rec->host_worker->join();
        }
    }
}

uint32_t ForkProcessManager::ForkFrom(const CpuState& parent,
                                      size_t instruction_limit) {
    // 1) Snapshot the whole committed address space.
    auto regions = PS5::Memory::VirtualMemoryManager::Instance()
                       .SnapshotCommitted();
    if (regions.empty()) {
        return 0;
    }
    auto bus = std::make_shared<ForkSnapshotBus>(std::move(regions));

    // 2) The child's state = the parent's, with the fork() return value 0.
    //    (m_state.rip at syscall-callback time is already the instruction
    //    AFTER the syscall -- exactly the resume point.)
    CpuState child = parent;
    child.gpr[RAX] = 0;

    // 3) Register + spawn the child host thread.
    auto rec = std::make_shared<ForkChildRecord>();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        rec->pid = m_next_pid++;
        m_children.push_back(rec);
    }
    rec->host_worker = std::make_shared<std::thread>(
        [this, rec, child, bus, instruction_limit]() {
            ChildBody(rec, child, bus, instruction_limit);
        });
    return rec->pid;
}

void ForkProcessManager::ChildBody(std::shared_ptr<ForkChildRecord> rec,
                                   CpuState state,
                                   std::shared_ptr<ForkSnapshotBus> bus,
                                   size_t instruction_limit) {
    t_current_child = rec.get();

    X86Interpreter interp(state, *bus);
    interp.SetSyscallHandler([&](CpuState& s, GuestMemoryBus& mem)
                                 -> bool {
        const uint64_t nr = s.gpr[RAX];
        // exit(2) / thr_exit(431): terminate exactly this child.
        if (nr == static_cast<uint64_t>(ProsperoSyscall::SC_SYS_exit) ||
            nr == static_cast<uint64_t>(ProsperoSyscall::SC_SYS_thr_exit)) {
            rec->exit_code.store(static_cast<int>(s.gpr[RDI] & 0xFFu));
            rec->finished.store(true);
            return false;   // unwind the interpreter loop
        }
        // Everything else routes through the shared dispatcher. (Documented
        // boundary: syscalls that touch guest memory operate on the PARENT
        // address space; the child's own loads/stores stay isolated.)
        SyscallContext ctx{};
        ctx.rax = nr;
        ctx.rdi = s.gpr[RDI];
        ctx.rsi = s.gpr[RSI];
        ctx.rdx = s.gpr[RDX];
        ctx.rcx = s.gpr[R10];
        ctx.r8 = s.gpr[R8];
        ctx.r9 = s.gpr[R9];
        ctx.cpu = &s;
        s.gpr[RAX] = ProsperoSyscallDispatcher::Instance().Dispatch(ctx);
        (void)mem;
        return true;
    });

    constexpr uint64_t kStopSentinel = 0x0000FEEDDEAD0000ULL;
    const RunResult result = interp.Run(instruction_limit, kStopSentinel);
    rec->stop_status = result.status;
    if (!rec->finished.load()) {
        // Natural return / fault / limit: the exit code is rax (the round-15
        // top-level-ret convention) or -1 on abnormal stops.
        rec->exit_code.store(result.status == ExecStatus::Running
                                 ? static_cast<int>(state.gpr[RAX] & 0xFFu)
                                 : -1);
        rec->finished.store(true);
    }
    t_current_child = nullptr;
}

bool ForkProcessManager::WaitChild(int32_t pid_wanted, uint32_t options,
                                   int32_t& out_pid, int32_t& out_status,
                                   int64_t timeout_us) {
    constexpr uint32_t kWnohang = 1;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::microseconds(timeout_us);
    for (;;) {
        std::shared_ptr<ForkChildRecord> done;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& rec : m_children) {
                if (rec->reaped || !rec->finished.load()) {
                    continue;
                }
                if (pid_wanted != -1 &&
                    rec->pid != static_cast<uint32_t>(pid_wanted)) {
                    continue;
                }
                done = rec;
                break;
            }
        }
        if (done) {
            done->reaped = true;
            out_pid = static_cast<int32_t>(done->pid);
            // FreeBSD wait status: exit code in the high byte.
            out_status = static_cast<int32_t>(
                static_cast<uint32_t>(done->exit_code.load() & 0xFF) << 8);
            if (done->host_worker && done->host_worker->joinable()) {
                done->host_worker->join();
            }
            return true;
        }
        // No finished child. If no matching unreaped child exists at all
        // the wait fails with ECHILD -- even under WNOHANG (POSIX).
        bool any_unreaped = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& rec : m_children) {
                if (rec->reaped) continue;
                if (pid_wanted != -1 &&
                    rec->pid != static_cast<uint32_t>(pid_wanted)) {
                    continue;
                }
                any_unreaped = true;
                break;
            }
        }
        if (!any_unreaped) {
            return false;   // ECHILD
        }
        // Children exist but none finished: WNOHANG reports 0, a blocking
        // wait parks.
        if ((options & kWnohang) != 0U) {
            out_pid = 0;
            out_status = 0;
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;   // bounded wait exhausted (honest failure)
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

size_t ForkProcessManager::UnreapedChildCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t n = 0;
    for (const auto& rec : m_children) {
        if (!rec->reaped) ++n;
    }
    return n;
}

uint32_t ForkProcessManager::CurrentPid() {
    return t_current_child != nullptr ? t_current_child->pid : MainPidValue;
}

bool ForkProcessManager::InForkChild() {
    return t_current_child != nullptr;
}

} // namespace PS5::CPU
