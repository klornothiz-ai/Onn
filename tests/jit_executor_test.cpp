#include "cpu/jit_executor.hpp"
#include "memory/virtual_memory_manager.hpp"

#include <array>
#include <cstdint>
#include <iostream>

namespace {

using PS5::CPU::CPUJitEngine;
using PS5::CPU::GuestExecutionStatus;
using PS5::Memory::PageProt;
using PS5::Memory::VirtualMemoryManager;

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

bool InterpreterBackedJitDoesNotEmitHostCode() {
    constexpr uint64_t kGuestEntry = 0x1000c00000ULL;
    constexpr uint32_t kReadWrite = static_cast<uint32_t>(PageProt::Read) |
                                    static_cast<uint32_t>(PageProt::Write);
    constexpr uint32_t kReadExecute = static_cast<uint32_t>(PageProt::Read) |
                                      static_cast<uint32_t>(PageProt::Exec);
    const std::array<uint8_t, 7> code = {
        0x48, 0x89, 0xf8, // mov rax, rdi
        0x48, 0x01, 0xf0, // add rax, rsi
        0xc3,
    };

    auto& vmm = VirtualMemoryManager::Instance();
    CHECK(vmm.AllocateVirtual(kGuestEntry, 4096, kReadWrite) == kGuestEntry);
    CHECK(vmm.CopyToGuest(kGuestEntry, code.data(), code.size()));
    CHECK(vmm.ProtectVirtual(kGuestEntry, 4096, kReadExecute));

    auto& engine = CPUJitEngine::Instance();
    const auto* block = engine.CompileBasicBlock(kGuestEntry);
    CHECK(block != nullptr);
    CHECK(block->host_code_ptr == nullptr);
    CHECK(block->code_size == code.size());
    CHECK(block->instruction_count == 3);
    CHECK(engine.GetTotalJitAllocated() == 0);

    const auto result = engine.ExecuteGuestCodeChecked(kGuestEntry, 1200, 345);
    CHECK(result.status == GuestExecutionStatus::Completed);
    CHECK(result.registers.gpr[0] == 1545);
    CHECK(vmm.FreeVirtual(kGuestEntry, 4096));
    return true;
}

bool PreflightRejectsBeforeAnySyscallIsHandled() {
    constexpr uint64_t kGuestEntry = 0x1000c01000ULL;
    constexpr uint32_t kReadWrite = static_cast<uint32_t>(PageProt::Read) |
                                    static_cast<uint32_t>(PageProt::Write);
    constexpr uint32_t kReadExecute = static_cast<uint32_t>(PageProt::Read) |
                                      static_cast<uint32_t>(PageProt::Exec);
    const std::array<uint8_t, 13> code = {
        0x48, 0xc7, 0xc0, 0x14, 0x00, 0x00, 0x00, // mov rax, 20
        0x0f, 0x05,                               // syscall
        0x48, 0x8b, 0x00,                         // unsupported memory operand
        0xc3,
    };

    auto& vmm = VirtualMemoryManager::Instance();
    CHECK(vmm.AllocateVirtual(kGuestEntry, 4096, kReadWrite) == kGuestEntry);
    CHECK(vmm.CopyToGuest(kGuestEntry, code.data(), code.size()));
    CHECK(vmm.ProtectVirtual(kGuestEntry, 4096, kReadExecute));

    auto& engine = CPUJitEngine::Instance();
    const uint64_t intercepted_before = engine.GetInterceptedSyscallCount();
    const auto result = engine.ExecuteGuestCodeChecked(kGuestEntry);
    CHECK(result.status == GuestExecutionStatus::UnsupportedInstruction);
    CHECK(engine.GetInterceptedSyscallCount() == intercepted_before);
    CHECK(vmm.FreeVirtual(kGuestEntry, 4096));
    return true;
}

struct TestCase {
    const char* name;
    bool (*function)();
};

} // namespace

int main() {
    const TestCase tests[] = {
        {"InterpreterBackedJitDoesNotEmitHostCode", InterpreterBackedJitDoesNotEmitHostCode},
        {"PreflightRejectsBeforeAnySyscallIsHandled", PreflightRejectsBeforeAnySyscallIsHandled},
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
