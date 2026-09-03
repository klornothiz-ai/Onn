#include "cpu/x86_64_subset_interpreter.hpp"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using PS5::CPU::GuestExecutionStatus;
using PS5::CPU::GuestRegisterState;
using PS5::CPU::X86_64SubsetInterpreter;

bool Check(bool value, const char* expression, int line) {
    if (!value) {
        std::cerr << "check failed at line " << line << ": " << expression << '\n';
    }
    return value;
}

#define CHECK(expression) \
    do { \
        if (!Check((expression), #expression, __LINE__)) return false; \
    } while (false)

bool ArithmeticProgramCompletes() {
    const std::vector<uint8_t> code = {
        0x48, 0x89, 0xf8, // mov rax, rdi
        0x48, 0x01, 0xf0, // add rax, rsi
        0xc3,             // ret
    };
    GuestRegisterState registers{};
    registers.gpr[7] = 1200;
    registers.gpr[6] = 345;

    const auto inspected = X86_64SubsetInterpreter::Inspect(code);
    CHECK(inspected.status == GuestExecutionStatus::Completed);
    CHECK(inspected.code_size == code.size());
    CHECK(inspected.instruction_count == 3);
    CHECK(!inspected.contains_syscall);

    const auto result = X86_64SubsetInterpreter::Execute(code, 0x100000, registers, {});
    CHECK(result.status == GuestExecutionStatus::Completed);
    CHECK(result.registers.gpr[0] == 1545);
    CHECK(result.executed_instructions == 3);
    return true;
}

bool SyscallRequiresAnExplicitHandler() {
    const std::vector<uint8_t> code = {
        0x48, 0xc7, 0xc0, 0x14, 0x00, 0x00, 0x00, // mov rax, 20
        0x0f, 0x05,                               // syscall
        0xc3,
    };

    bool called = false;
    const auto result = X86_64SubsetInterpreter::Execute(
        code, 0x200000, {}, [&called](GuestRegisterState& state) {
            called = true;
            if (state.gpr[0] != 20) {
                return false;
            }
            state.gpr[0] = 4242;
            return true;
        });
    CHECK(result.status == GuestExecutionStatus::Completed);
    CHECK(called);
    CHECK(result.registers.gpr[0] == 4242);

    const auto denied = X86_64SubsetInterpreter::Execute(code, 0x200000, {}, {});
    CHECK(denied.status == GuestExecutionStatus::DeniedSyscall);
    return true;
}

bool UnsafeOrIncompleteInstructionsFailClosed() {
    const std::vector<uint8_t> memory_operand = {0x48, 0x8b, 0x00, 0xc3};
    const auto unsupported = X86_64SubsetInterpreter::Execute(memory_operand, 0x300000, {}, {});
    CHECK(unsupported.status == GuestExecutionStatus::UnsupportedInstruction);
    CHECK(unsupported.fault_rip == 0x300000);

    const std::vector<uint8_t> truncated = {0x48, 0xb8, 0x01};
    const auto incomplete = X86_64SubsetInterpreter::Execute(truncated, 0x300100, {}, {});
    CHECK(incomplete.status == GuestExecutionStatus::TruncatedInstruction);
    CHECK(incomplete.fault_rip == 0x300100);
    return true;
}

bool InstructionBudgetStopsRunawayCode() {
    const std::vector<uint8_t> code = {0x90, 0x90, 0x90, 0x90};
    const auto result = X86_64SubsetInterpreter::Execute(code, 0x400000, {}, {}, 3);
    CHECK(result.status == GuestExecutionStatus::InstructionLimit);
    CHECK(result.executed_instructions == 3);
    return true;
}

struct TestCase {
    const char* name;
    bool (*function)();
};

} // namespace

int main() {
    const TestCase tests[] = {
        {"ArithmeticProgramCompletes", ArithmeticProgramCompletes},
        {"SyscallRequiresAnExplicitHandler", SyscallRequiresAnExplicitHandler},
        {"UnsafeOrIncompleteInstructionsFailClosed", UnsafeOrIncompleteInstructionsFailClosed},
        {"InstructionBudgetStopsRunawayCode", InstructionBudgetStopsRunawayCode},
    };

    size_t passed = 0;
    for (const auto& test : tests) {
        const bool success = test.function();
        std::cout << (success ? "[PASS] " : "[FAIL] ") << test.name << '\n';
        passed += success ? 1 : 0;
    }
    std::cout << passed << '/' << std::size(tests) << " tests passed\n";
    return passed == std::size(tests) ? 0 : 1;
}
