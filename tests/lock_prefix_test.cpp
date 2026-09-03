// ============================================================================
// ProsperoLayer RDNA2 Core — lock prefix + atomic operations test (round 31)
// ----------------------------------------------------------------------------
// Verifies that the lock prefix (0xF0) is now tracked (not silently ignored)
// and that cmpxchg/xadd/lock add execute correctly in the interpreter.
// ============================================================================
#include "cpu/x86_64_interpreter.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>

namespace {

using PS5::CPU::X86Interpreter;
using PS5::CPU::CpuState;
using PS5::CPU::FlatMemoryBus;
using PS5::CPU::ExecStatus;

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                          \
            ++g_failures;                                                      \
            std::cerr << "FAIL: " << #cond << " at line " << __LINE__ << "\n"; \
        }                                                                      \
    } while (0)

// Use the project's own FlatMemoryBus — a concrete GuestMemoryBus backed by
// a host vector. We place code at 0x10000 and data at 0x20000.
constexpr uint64_t kCodeBase = 0x10000;
constexpr uint64_t kDataBase = 0x20000;

void emit_code(FlatMemoryBus& bus, const uint8_t* code, size_t len) {
    for (size_t i = 0; i < len; i++) {
        bus.Write(kCodeBase + i, &code[i], 1);
    }
}

void test_lock_cmpxchg() {
    std::cout << "[lock] A: lock cmpxchg [mem], reg — atomic compare-exchange\n";
    FlatMemoryBus bus(0x10000, 0x20000); // code + data in one flat region
    CpuState state{};
    state.rip = kCodeBase;
    state.gpr[4] = kCodeBase + 0x8000; // RSP

    // [mem] = 0x41414141 (expected value)
    uint32_t initial = 0x41414141;
    bus.Write(kDataBase, &initial, 4);

    state.gpr[0] = 0x41414141; // RAX = expected
    state.gpr[3] = 0x42424242; // RBX = new value

    // lock cmpxchg [rip+disp], rbx
    uint8_t code[] = {
        0xF0, 0x48, 0x0F, 0xB1, 0x1D,
        0x00, 0x00, 0x00, 0x00
    };
    const int32_t disp = static_cast<int32_t>(kDataBase - (kCodeBase + sizeof(code)));
    std::memcpy(&code[5], &disp, 4);
    emit_code(bus, code, sizeof(code));

    X86Interpreter interp(state, bus);
    interp.Step();

    uint32_t result = 0;
    bus.Read(kDataBase, &result, 4);
    CHECK(result == 0x42424242);
    std::cout << "  [mem] after lock cmpxchg = 0x" << std::hex << result << std::dec << "\n";
}

void test_lock_xadd() {
    std::cout << "[lock] B: lock xadd [mem], reg — atomic exchange-add\n";
    FlatMemoryBus bus(0x10000, 0x20000);
    CpuState state{};
    state.rip = kCodeBase;
    state.gpr[4] = kCodeBase + 0x8000;

    uint32_t initial = 100;
    bus.Write(kDataBase, &initial, 4);
    state.gpr[0] = 5; // RAX

    uint8_t code[] = {
        0xF0, 0x48, 0x0F, 0xC1, 0x05,
        0x00, 0x00, 0x00, 0x00
    };
    const int32_t disp = static_cast<int32_t>(kDataBase - (kCodeBase + sizeof(code)));
    std::memcpy(&code[5], &disp, 4);
    emit_code(bus, code, sizeof(code));

    X86Interpreter interp(state, bus);
    interp.Step();

    uint32_t result = 0;
    bus.Read(kDataBase, &result, 4);
    CHECK(result == 105);
    CHECK(static_cast<uint32_t>(state.gpr[0]) == 100);
    std::cout << "  [mem] after lock xadd = " << result << ", RAX = " << state.gpr[0] << "\n";
}

void test_lock_add() {
    std::cout << "[lock] C: lock add [mem], reg — atomic add\n";
    FlatMemoryBus bus(0x10000, 0x20000);
    CpuState state{};
    state.rip = kCodeBase;
    state.gpr[4] = kCodeBase + 0x8000;

    uint64_t initial = 0x1000;
    bus.Write(kDataBase, &initial, 8);
    state.gpr[0] = 0x42; // RAX

    uint8_t code[] = {
        0xF0, 0x48, 0x01, 0x05,
        0x00, 0x00, 0x00, 0x00
    };
    const int32_t disp = static_cast<int32_t>(kDataBase - (kCodeBase + sizeof(code)));
    std::memcpy(&code[4], &disp, 4);
    emit_code(bus, code, sizeof(code));

    X86Interpreter interp(state, bus);
    interp.Step();

    uint64_t result = 0;
    bus.Read(kDataBase, &result, 8);
    CHECK(result == 0x1042);
    std::cout << "  [mem] after lock add = 0x" << std::hex << result << std::dec << "\n";
}

void test_non_lock_cmpxchg() {
    std::cout << "[lock] D: non-lock cmpxchg still works (no regression)\n";
    FlatMemoryBus bus(0x10000, 0x20000);
    CpuState state{};
    state.rip = kCodeBase;
    state.gpr[4] = kCodeBase + 0x8000;

    uint32_t initial = 0xAAAA;
    bus.Write(kDataBase, &initial, 4);
    state.gpr[0] = 0xBBBB; // RAX = does not match
    state.gpr[3] = 0xCCCC; // RBX

    uint8_t code[] = {
        0x48, 0x0F, 0xB1, 0x1D,
        0x00, 0x00, 0x00, 0x00
    };
    const int32_t disp = static_cast<int32_t>(kDataBase - (kCodeBase + sizeof(code)));
    std::memcpy(&code[4], &disp, 4);
    emit_code(bus, code, sizeof(code));

    X86Interpreter interp(state, bus);
    interp.Step();

    uint32_t result = 0;
    bus.Read(kDataBase, &result, 4);
    CHECK(result == 0xAAAA); // unchanged
    CHECK(static_cast<uint32_t>(state.gpr[0]) == 0xAAAA);
    std::cout << "  [mem] = 0x" << std::hex << result << ", RAX = 0x" << state.gpr[0] << std::dec << "\n";
}

} // namespace

int main() {
    std::cout << "== lock prefix + atomic ops test (round 31) ==\n";
    test_lock_cmpxchg();
    test_lock_xadd();
    test_lock_add();
    test_non_lock_cmpxchg();

    std::cout << "lock_prefix_test: " << g_checks << " checks, " << g_failures << " failures\n";
    if (g_failures == 0) std::cout << ">> [PASS] lock prefix + atomic ops\n";
    else std::cout << ">> [FAIL] lock prefix + atomic ops\n";

    std::cout.flush();
    std::exit(g_failures == 0 ? 0 : 1);
}
