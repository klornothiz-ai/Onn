#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

namespace PS5::CPU {

// This interpreter is deliberately small and fail-closed. It is not a general x86-64 emulator.
enum class GuestExecutionStatus {
    Completed,
    InvalidEntry,
    FetchFault,
    TruncatedInstruction,
    UnsupportedInstruction,
    DeniedSyscall,
    InstructionLimit,
};

struct GuestRegisterState {
    // x86-64 register order: rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8-r15.
    std::array<uint64_t, 16> gpr{};
    uint64_t rip{0};
};

struct GuestExecutionResult {
    GuestExecutionStatus status{GuestExecutionStatus::InvalidEntry};
    GuestRegisterState registers{};
    uint64_t fault_rip{0};
    size_t executed_instructions{0};
};

struct GuestProgramInfo {
    GuestExecutionStatus status{GuestExecutionStatus::InvalidEntry};
    size_t code_size{0};
    size_t instruction_count{0};
    bool contains_syscall{false};
};

// Return false to deny the syscall. A successful handler writes its result to rax.
using GuestSyscallHandler = std::function<bool(GuestRegisterState&)>;

class X86_64SubsetInterpreter {
public:
    static GuestProgramInfo Inspect(std::span<const uint8_t> code,
                                    size_t instruction_limit = 1024);

    static GuestExecutionResult Execute(std::span<const uint8_t> code,
                                        uint64_t entry_gva,
                                        GuestRegisterState initial_registers,
                                        const GuestSyscallHandler& syscall_handler,
                                        size_t instruction_limit = 1024);
};

} // namespace PS5::CPU
