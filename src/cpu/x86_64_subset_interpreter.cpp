#include "cpu/x86_64_subset_interpreter.hpp"

#include <algorithm>
#include <cstring>

namespace PS5::CPU {
namespace {

enum class DecodedOpcode {
    Nop,
    Ret,
    Syscall,
    MovRegReg,
    AddRegReg,
    MovRegImm32,
    MovRegImm64,
};

struct DecodedInstruction {
    DecodedOpcode opcode{};
    size_t length{0};
    uint8_t dst{0};
    uint8_t src{0};
    uint64_t immediate{0};
};

enum class DecodeStatus {
    Ok,
    Truncated,
    Unsupported,
};

DecodeStatus DecodeOne(std::span<const uint8_t> code, size_t offset,
                       DecodedInstruction* instruction) {
    if (instruction == nullptr || offset >= code.size()) {
        return DecodeStatus::Truncated;
    }

    const auto remaining = code.size() - offset;
    const uint8_t first = code[offset];

    if (first == 0x90) {
        *instruction = {DecodedOpcode::Nop, 1, 0, 0, 0};
        return DecodeStatus::Ok;
    }
    if (first == 0xc3) {
        *instruction = {DecodedOpcode::Ret, 1, 0, 0, 0};
        return DecodeStatus::Ok;
    }
    if (first == 0x0f) {
        if (remaining < 2) {
            return DecodeStatus::Truncated;
        }
        if (code[offset + 1] == 0x05) {
            *instruction = {DecodedOpcode::Syscall, 2, 0, 0, 0};
            return DecodeStatus::Ok;
        }
        return DecodeStatus::Unsupported;
    }

    // Stage one only accepts REX.W without register-extension bits.
    if (first != 0x48) {
        return DecodeStatus::Unsupported;
    }
    if (remaining < 2) {
        return DecodeStatus::Truncated;
    }

    const uint8_t opcode = code[offset + 1];
    if (opcode >= 0xb8 && opcode <= 0xbf) {
        if (remaining < 10) {
            return DecodeStatus::Truncated;
        }
        uint64_t immediate = 0;
        std::memcpy(&immediate, code.data() + offset + 2, sizeof(immediate));
        *instruction = {DecodedOpcode::MovRegImm64, 10,
                        static_cast<uint8_t>(opcode - 0xb8), 0, immediate};
        return DecodeStatus::Ok;
    }

    if (remaining < 3) {
        return DecodeStatus::Truncated;
    }

    const uint8_t modrm = code[offset + 2];
    const uint8_t mod = modrm >> 6;
    const uint8_t reg = (modrm >> 3) & 0x07;
    const uint8_t rm = modrm & 0x07;
    if (mod != 3) {
        return DecodeStatus::Unsupported;
    }

    switch (opcode) {
        case 0x89: // mov r/m64, r64
            *instruction = {DecodedOpcode::MovRegReg, 3, rm, reg, 0};
            return DecodeStatus::Ok;
        case 0x01: // add r/m64, r64
            *instruction = {DecodedOpcode::AddRegReg, 3, rm, reg, 0};
            return DecodeStatus::Ok;
        case 0xc7: { // mov r/m64, imm32 (/0)
            if (reg != 0) {
                return DecodeStatus::Unsupported;
            }
            if (remaining < 7) {
                return DecodeStatus::Truncated;
            }
            int32_t immediate = 0;
            std::memcpy(&immediate, code.data() + offset + 3, sizeof(immediate));
            *instruction = {DecodedOpcode::MovRegImm32, 7, rm, 0,
                            static_cast<uint64_t>(static_cast<int64_t>(immediate))};
            return DecodeStatus::Ok;
        }
        default:
            return DecodeStatus::Unsupported;
    }
}

GuestExecutionStatus DecodeStatusToExecutionStatus(DecodeStatus status) {
    return status == DecodeStatus::Truncated ? GuestExecutionStatus::TruncatedInstruction
                                              : GuestExecutionStatus::UnsupportedInstruction;
}

} // namespace

GuestProgramInfo X86_64SubsetInterpreter::Inspect(std::span<const uint8_t> code,
                                                   size_t instruction_limit) {
    GuestProgramInfo result{};
    size_t offset = 0;

    for (size_t count = 0; count < instruction_limit; ++count) {
        DecodedInstruction instruction{};
        const auto decode_status = DecodeOne(code, offset, &instruction);
        if (decode_status != DecodeStatus::Ok) {
            result.status = DecodeStatusToExecutionStatus(decode_status);
            result.code_size = offset;
            result.instruction_count = count;
            return result;
        }

        result.contains_syscall = result.contains_syscall || instruction.opcode == DecodedOpcode::Syscall;
        offset += instruction.length;
        result.code_size = offset;
        result.instruction_count = count + 1;
        if (instruction.opcode == DecodedOpcode::Ret) {
            result.status = GuestExecutionStatus::Completed;
            return result;
        }
    }

    result.status = GuestExecutionStatus::InstructionLimit;
    return result;
}

GuestExecutionResult X86_64SubsetInterpreter::Execute(std::span<const uint8_t> code,
                                                       uint64_t entry_gva,
                                                       GuestRegisterState registers,
                                                       const GuestSyscallHandler& syscall_handler,
                                                       size_t instruction_limit) {
    GuestExecutionResult result{};
    result.registers = registers;
    result.registers.rip = entry_gva;

    size_t offset = 0;
    for (size_t count = 0; count < instruction_limit; ++count) {
        DecodedInstruction instruction{};
        const auto decode_status = DecodeOne(code, offset, &instruction);
        if (decode_status != DecodeStatus::Ok) {
            result.status = DecodeStatusToExecutionStatus(decode_status);
            result.fault_rip = entry_gva + offset;
            result.executed_instructions = count;
            return result;
        }

        result.executed_instructions = count + 1;
        result.registers.rip = entry_gva + offset + instruction.length;
        switch (instruction.opcode) {
            case DecodedOpcode::Nop:
                break;
            case DecodedOpcode::Ret:
                result.status = GuestExecutionStatus::Completed;
                return result;
            case DecodedOpcode::Syscall:
                if (!syscall_handler || !syscall_handler(result.registers)) {
                    result.status = GuestExecutionStatus::DeniedSyscall;
                    result.fault_rip = entry_gva + offset;
                    return result;
                }
                break;
            case DecodedOpcode::MovRegReg:
                result.registers.gpr[instruction.dst] = result.registers.gpr[instruction.src];
                break;
            case DecodedOpcode::AddRegReg:
                result.registers.gpr[instruction.dst] += result.registers.gpr[instruction.src];
                break;
            case DecodedOpcode::MovRegImm32:
            case DecodedOpcode::MovRegImm64:
                result.registers.gpr[instruction.dst] = instruction.immediate;
                break;
        }
        offset += instruction.length;
    }

    result.status = GuestExecutionStatus::InstructionLimit;
    result.fault_rip = entry_gva + offset;
    return result;
}

} // namespace PS5::CPU
