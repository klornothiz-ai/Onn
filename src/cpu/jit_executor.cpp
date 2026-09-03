#include "cpu/jit_executor.hpp"
#include "cpu/x86_64_interpreter.hpp"
#include "cpu/vmm_memory_bus.hpp"
#include "cpu/prospero_syscalls.hpp"
#include "cpu/hle_trampoline.hpp"
#include "cpu/guest_threads.hpp"

#include <limits>
#include <unistd.h>

namespace PS5::CPU {
namespace {

constexpr size_t kMaximumGuestBlockBytes = 64 * 1024;

} // namespace

CPUJitEngine& CPUJitEngine::Instance() {
    static CPUJitEngine instance;
    return instance;
}

std::vector<uint8_t> CPUJitEngine::FetchGuestCode(uint64_t guest_rip, bool* fetch_fault) const {
    if (fetch_fault != nullptr) {
        *fetch_fault = false;
    }

    std::vector<uint8_t> code;
    code.reserve(kMaximumGuestBlockBytes);

    auto& vmm = Memory::VirtualMemoryManager::Instance();
    const uint32_t required_prot = static_cast<uint32_t>(Memory::PageProt::Read) |
                                   static_cast<uint32_t>(Memory::PageProt::Exec);
    for (size_t offset = 0; offset < kMaximumGuestBlockBytes; ++offset) {
        if (guest_rip > std::numeric_limits<uint64_t>::max() - offset) {
            if (fetch_fault != nullptr) {
                *fetch_fault = true;
            }
            break;
        }
        uint8_t byte = 0;
        if (!vmm.CopyFromGuest(guest_rip + offset, &byte, sizeof(byte), required_prot)) {
            if (fetch_fault != nullptr) {
                *fetch_fault = true;
            }
            break;
        }
        code.push_back(byte);
    }
    return code;
}

JITBasicBlock* CPUJitEngine::CompileBasicBlock(uint64_t guest_rip) {
    std::lock_guard<std::mutex> lock(m_jit_mutex);
    const auto cached = m_block_cache.find(guest_rip);
    if (cached != m_block_cache.end()) {
        return cached->second.get();
    }

    auto& vmm = Memory::VirtualMemoryManager::Instance();
    if (!vmm.IsGvaMapped(guest_rip) || !vmm.IsGvaExecutable(guest_rip)) {
        return nullptr;
    }

    bool fetch_fault = false;
    const auto code = FetchGuestCode(guest_rip, &fetch_fault);
    if (code.empty()) {
        return nullptr;
    }

    // Round 16: the block cache is now populated through the FULL x86-64
    // decoder (the same ISA coverage as the extended interpreter): every
    // instruction the interpreter can execute decodes here, and the block
    // ends at the first ret / branch / call / syscall / hlt -- the classic
    // JIT basic-block discipline. (The fail-closed subset scanner only knew
    // NOP/RET/SYSCALL/MOV/ADD, so most real guest blocks were rejected.)
    const auto program = X86Interpreter::InspectBlock(
        std::span<const uint8_t>(code.data(), code.size()), guest_rip);
    if (program.status != ExecStatus::Running) {
        return nullptr;
    }

    auto block = std::make_unique<JITBasicBlock>();
    block->guest_rip = guest_rip;
    block->code_size = program.code_size;
    block->instruction_count = program.instruction_count;
    block->contains_syscall = program.contains_syscall;
    block->sites = program.sites;
    block->is_compiled = true;

    auto* result = block.get();
    m_block_cache.emplace(guest_rip, std::move(block));
    m_total_blocks_compiled.fetch_add(1, std::memory_order_relaxed);
    return result;
}

GuestExecutionResult CPUJitEngine::ExecuteGuestCodeChecked(uint64_t entry_gva,
                                                            uint64_t arg0,
                                                            uint64_t arg1) {
    GuestExecutionResult result{};
    auto& vmm = Memory::VirtualMemoryManager::Instance();
    if (!vmm.IsGvaMapped(entry_gva) || !vmm.IsGvaExecutable(entry_gva)) {
        result.status = GuestExecutionStatus::InvalidEntry;
        result.fault_rip = entry_gva;
        return result;
    }

    bool fetch_fault = false;
    const auto code = FetchGuestCode(entry_gva, &fetch_fault);
    if (code.empty()) {
        result.status = GuestExecutionStatus::FetchFault;
        result.fault_rip = entry_gva;
        return result;
    }

    const auto program = X86_64SubsetInterpreter::Inspect(code);
    if (program.status != GuestExecutionStatus::Completed) {
        // A fetch failure only supersedes an incomplete instruction; an earlier semantic
        // rejection must remain visible so no syscall is performed before it is reported.
        result.status = (fetch_fault && program.status == GuestExecutionStatus::TruncatedInstruction)
            ? GuestExecutionStatus::FetchFault
            : program.status;
        result.fault_rip = entry_gva + program.code_size;
        result.executed_instructions = program.instruction_count;
        return result;
    }

    GuestRegisterState registers{};
    registers.gpr[7] = arg0; // rdi
    registers.gpr[6] = arg1; // rsi

    const auto safe_syscall_handler = [this](GuestRegisterState& state) {
        // The standalone execution boundary exposes only a side-effect-free identity syscall.
        if (state.gpr[0] != 20U) {
            return false;
        }
        state.gpr[0] = static_cast<uint64_t>(getpid());
        m_intercepted_syscalls.fetch_add(1, std::memory_order_relaxed);
        return true;
    };

    result = X86_64SubsetInterpreter::Execute(
        std::span<const uint8_t>(code.data(), program.code_size), entry_gva, registers,
        safe_syscall_handler);
    if (fetch_fault && result.status != GuestExecutionStatus::Completed) {
        result.status = GuestExecutionStatus::FetchFault;
    }
    return result;
}

uint64_t CPUJitEngine::ExecuteGuestCode(uint64_t entry_gva, uint64_t arg0, uint64_t arg1) {
    const auto result = ExecuteGuestCodeChecked(entry_gva, arg0, arg1);
    return result.status == GuestExecutionStatus::Completed ? result.registers.gpr[0] : 0;
}

uint64_t CPUJitEngine::ExecuteGuestFull(uint64_t entry_gva, uint64_t arg0,
                                        uint64_t arg1, size_t instruction_limit,
                                        uint64_t fs_base) {
    auto& vmm = Memory::VirtualMemoryManager::Instance();
    if (!vmm.IsGvaMapped(entry_gva) || !vmm.IsGvaExecutable(entry_gva)) {
        return 0;
    }

    VmmMemoryBus bus(vmm);
    CpuState cpu{};
    cpu.rip = entry_gva;
    cpu.gpr[RDI] = arg0;
    cpu.gpr[RSI] = arg1;
    // Round 11: thread pointer for the FS segment (TLS). Zero keeps the
    // previous behaviour for every existing caller.
    cpu.fs_base = fs_base;

    // Provide a private guest stack and a return sentinel so a top-level `ret`
    // stops execution cleanly rather than faulting off the end of the block.
    constexpr uint64_t kStopSentinel = 0x0000FEEDDEAD0000ULL;
    const size_t stack_size = 1 * 1024 * 1024;
    const uint64_t stack_base = vmm.AllocateVirtual(
        0, stack_size,
        static_cast<uint32_t>(Memory::PageProt::Read) |
            static_cast<uint32_t>(Memory::PageProt::Write));
    if (stack_base == 0) {
        return 0;
    }
    uint64_t rsp = stack_base + stack_size - 64;
    rsp -= 8;
    bus.Write(rsp, &kStopSentinel, 8);
    cpu.gpr[RSP] = rsp;

    m_last_direct = DirectRunOutcome{};
    m_thread_exit_requested = false;

    // Shared syscall servicing (the SAME contract for both engines):
    // x86-64 syscall ABI -- number in rax, args rdi/rsi/rdx/r10/r8/r9;
    // thr_exit terminates THIS execution unwind-style.
    auto service_syscall = [this](CpuState& s, GuestMemoryBus&) {
        const uint64_t nr = s.gpr[RAX];
        // Round 30: HLE trampoline calls — the guest jumped to a stub that
        // loaded the magic number into rax. The host function receives the
        // guest's own argument registers (SysV), so this is the direct
        // equivalent of a PLT call into the HLE implementation.
        if (PS5::CPU::HleTrampolines::IsHleCall(nr)) {
            s.gpr[RAX] = PS5::CPU::HleTrampolines::Instance().Dispatch(
                nr, s.gpr[RDI], s.gpr[RSI], s.gpr[RDX], s.gpr[RCX],
                s.gpr[R8], s.gpr[R9]);
            m_intercepted_syscalls.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        SyscallContext ctx{};
        ctx.cpu = &s;
        ctx.rax = nr;
        ctx.rdi = s.gpr[RDI];
        ctx.rsi = s.gpr[RSI];
        ctx.rdx = s.gpr[RDX];
        ctx.rcx = s.gpr[R10];
        ctx.r8 = s.gpr[R8];
        ctx.r9 = s.gpr[R9];
        if (nr == static_cast<uint64_t>(ProsperoSyscall::SC_SYS_thr_exit)) {
            m_last_thread_exit_code = static_cast<int>(s.gpr[RDI] & 0xFFu);
            m_thread_exit_requested = true;
            return false;
        }
        const uint64_t ret = ProsperoSyscallDispatcher::Instance().Dispatch(ctx);
        s.gpr[RAX] = ret;
        m_intercepted_syscalls.fetch_add(1, std::memory_order_relaxed);
        return true;
    };

    // ---- Round 20: the direct execution path --------------------------------
    // Try the native backend first when selected; ANY decline (not enabled,
    // missing arena, non-executable entry, unpatchable site, timeout) falls
    // back to the interpreter below -- the guest-visible contract is the
    // same either way (fail-closed).
    if (m_backend == GuestExecutionBackend::Direct) {
        auto& direct = DirectExecutionBackend::Instance();
        DirectRunOutcome outcome{};
        const uint64_t direct_result = direct.RunFunction(
            cpu, bus, service_syscall, kStopSentinel,
            /*budget_ms=*/10000, outcome);
        m_last_direct = outcome;
        const bool clean = outcome.reason == DirectStopReason::Returned ||
                           outcome.reason == DirectStopReason::ThreadExit;
        if (clean) {
            vmm.FreeVirtual(stack_base, stack_size);
            if (outcome.reason == DirectStopReason::ThreadExit &&
                m_thread_exit_requested) {
                return static_cast<uint64_t>(
                    static_cast<unsigned>(m_last_thread_exit_code));
            }
            return direct_result;
        }
        // Decline recorded in m_last_direct; the interpreter runs unchanged.
    }

    X86Interpreter interp(cpu, bus);
    interp.SetSyscallHandler(service_syscall);

    const RunResult result = interp.Run(instruction_limit, kStopSentinel);
    vmm.FreeVirtual(stack_base, stack_size);

    if (result.status == ExecStatus::Returned ||
        result.status == ExecStatus::Halted) {
        return cpu.gpr[RAX];
    }
    if (result.status == ExecStatus::SyscallDenied && m_thread_exit_requested) {
        return static_cast<uint64_t>(static_cast<unsigned>(m_last_thread_exit_code));
    }
    // Round 30: honest fault telemetry — real PS5 eboots fault for concrete
    // reasons (unmapped import slot, missing HLE side effect). Reporting the
    // RIP + faulting GVA turns a silent exit-0 into a debuggable fact.
    std::fprintf(stderr, "[guest-run] status=%d executed=%zu rip=0x%llx rax=0x%llx\n",
                 static_cast<int>(result.status), result.executed,
                 static_cast<unsigned long long>(cpu.rip),
                 static_cast<unsigned long long>(cpu.gpr[RAX]));
    if (result.status == ExecStatus::MemoryFault ||
        result.status == ExecStatus::DecodeFault) {
        m_last_fault_rip = cpu.rip;
        m_last_fault_addr = result.fault_addr;
        std::fprintf(stderr,
                     "[guest-fault] status=%s rip=0x%llx fault_addr=0x%llx "
                     "after %zu guest instructions\n",
                     result.status == ExecStatus::MemoryFault ? "memory" : "decode",
                     static_cast<unsigned long long>(m_last_fault_rip),
                     static_cast<unsigned long long>(m_last_fault_addr),
                     result.executed);
    }
    return 0;
}

int CPUJitEngine::LastThreadExitCode() const {
    return m_last_thread_exit_code;
}

size_t CPUJitEngine::GetCachedBlockCount() const {
    std::lock_guard<std::mutex> lock(m_jit_mutex);
    return m_block_cache.size();
}

bool CPUJitEngine::ChainBlocks(uint64_t from_rip, uint64_t to_rip) {
    std::lock_guard<std::mutex> lock(m_jit_mutex);
    const auto from_it = m_block_cache.find(from_rip);
    const auto to_it = m_block_cache.find(to_rip);
    if (from_it == m_block_cache.end() || to_it == m_block_cache.end()) {
        // Fail-closed: either block is missing, so the caller falls back to
        // normal cache lookup.
        return false;
    }

    auto* from_block = from_it->second.get();
    auto* to_block = to_it->second.get();
    // Clear any existing reverse link on the previous successor.
    if (from_block->next_block_rip != 0 &&
        from_block->next_block_rip != to_rip) {
        const auto old_it = m_block_cache.find(from_block->next_block_rip);
        if (old_it != m_block_cache.end() &&
            old_it->second->prev_block_rip == from_rip) {
            old_it->second->prev_block_rip = 0;
        }
    }
    from_block->next_block_rip = to_rip;
    from_block->chain_count++;
    to_block->prev_block_rip = from_rip;
    m_total_chains_created.fetch_add(1, std::memory_order_relaxed);
    return true;
}

JITBasicBlock* CPUJitEngine::GetChainedBlock(uint64_t rip) {
    std::lock_guard<std::mutex> lock(m_jit_mutex);
    const auto it = m_block_cache.find(rip);
    if (it == m_block_cache.end()) {
        return nullptr;
    }
    JITBasicBlock* block = it->second.get();
    if (block->next_block_rip == 0) {
        return nullptr;
    }
    const auto next_it = m_block_cache.find(block->next_block_rip);
    if (next_it == m_block_cache.end()) {
        // Stale chain target: clear it and fall back to normal lookup.
        block->next_block_rip = 0;
        return nullptr;
    }
    m_total_chain_hits.fetch_add(1, std::memory_order_relaxed);
    return next_it->second.get();
}

void CPUJitEngine::InvalidateBlock(uint64_t rip) {
    std::lock_guard<std::mutex> lock(m_jit_mutex);
    const auto it = m_block_cache.find(rip);
    if (it == m_block_cache.end()) {
        return;
    }
    JITBasicBlock* block = it->second.get();
    // Clear the forward chain and the successor's reverse link.
    if (block->next_block_rip != 0) {
        const auto next_it = m_block_cache.find(block->next_block_rip);
        if (next_it != m_block_cache.end() &&
            next_it->second->prev_block_rip == rip) {
            next_it->second->prev_block_rip = 0;
        }
    }
    // Clear any predecessor's forward chain into this block.
    if (block->prev_block_rip != 0) {
        const auto prev_it = m_block_cache.find(block->prev_block_rip);
        if (prev_it != m_block_cache.end() &&
            prev_it->second->next_block_rip == rip) {
            prev_it->second->next_block_rip = 0;
        }
    }
    m_block_cache.erase(it);
}

} // namespace PS5::CPU
