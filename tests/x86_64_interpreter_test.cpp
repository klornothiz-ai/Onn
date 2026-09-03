// ============================================================================
// ProsperoLayer RDNA2 Core - Extended x86-64 Interpreter unit tests
// ----------------------------------------------------------------------------
// Assembles real machine code, runs it through X86Interpreter over a flat
// guest memory bus, and checks register/flag/memory results against the true
// x86-64 semantics. Self-contained; no gtest dependency.
// ============================================================================
#include "cpu/x86_64_interpreter.hpp"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using namespace PS5::CPU;

int g_failures = 0;
int g_checks = 0;

bool Check(bool value, const char* expr, int line) {
    ++g_checks;
    if (!value) {
        ++g_failures;
        std::cerr << "  [FAIL] line " << line << ": " << expr << '\n';
    }
    return value;
}
#define CHECK(e) Check((e), #e, __LINE__)

// Load code at code_base, set rsp near top of memory, push a sentinel return
// address (stop_rip) and run.
struct Harness {
    static constexpr uint64_t kBase = 0x1000000;
    static constexpr size_t   kSize = 0x100000; // 1 MiB
    static constexpr uint64_t kStop = 0xDEADBEEF00; // sentinel return target
    FlatMemoryBus mem{kBase, kSize};
    CpuState cpu{};

    explicit Harness(std::vector<uint8_t> code, uint64_t code_off = 0x1000) {
        mem.LoadBlob(kBase + code_off, code);
        cpu.rip = kBase + code_off;
        cpu.gpr[RSP] = kBase + kSize - 0x1000; // stack somewhere high
        // push the sentinel so a `ret` returns to kStop and Run() stops cleanly
        cpu.gpr[RSP] -= 8;
        mem.Write(cpu.gpr[RSP], &kStop, 8);
    }

    RunResult Run(size_t limit = 100000) {
        X86Interpreter interp(cpu, mem);
        return interp.Run(limit, kStop);
    }
};

// mov rax, imm64 ; add rax, rbx ; ret
bool TestAddAndReturn() {
    std::cout << "[Test] add + ret\n";
    std::vector<uint8_t> code = {
        0x48, 0xC7, 0xC0, 0x0A, 0x00, 0x00, 0x00, // mov rax, 10
        0x48, 0xC7, 0xC3, 0x20, 0x00, 0x00, 0x00, // mov rbx, 32
        0x48, 0x01, 0xD8,                         // add rax, rbx
        0xC3,                                      // ret
    };
    Harness h(code);
    auto r = h.Run();
    CHECK(r.status == ExecStatus::Returned);
    CHECK(h.cpu.gpr[RAX] == 42);
    return true;
}

// A real loop: sum 1..10 with a conditional branch.
//   xor eax, eax        ; sum = 0
//   mov ecx, 10         ; i = 10
// loop:
//   add eax, ecx
//   dec ecx
//   jnz loop
//   ret
bool TestLoopSum() {
    std::cout << "[Test] loop sum 1..10\n";
    std::vector<uint8_t> code = {
        0x31, 0xC0,             // xor eax, eax
        0xB9, 0x0A, 0x00, 0x00, 0x00, // mov ecx, 10
        // loop:
        0x01, 0xC8,             // add eax, ecx
        0xFF, 0xC9,             // dec ecx
        0x75, 0xFA,             // jnz loop (-6)
        0xC3,                   // ret
    };
    Harness h(code);
    auto r = h.Run();
    CHECK(r.status == ExecStatus::Returned);
    CHECK(h.cpu.gpr[RAX] == 55); // 10+9+...+1
    return true;
}

// call/ret with a helper that doubles rdi.
//   mov edi, 21
//   call double
//   ret
// double:
//   mov eax, edi
//   add eax, edi
//   ret
bool TestCallRet() {
    std::cout << "[Test] call/ret\n";
    std::vector<uint8_t> code = {
        0xBF, 0x15, 0x00, 0x00, 0x00, // mov edi, 21
        0xE8, 0x01, 0x00, 0x00, 0x00, // call +1 (to double)
        0xC3,                         // ret (returns to sentinel)
        // double:
        0x89, 0xF8,                   // mov eax, edi
        0x01, 0xF8,                   // add eax, edi
        0xC3,                         // ret
    };
    Harness h(code);
    auto r = h.Run();
    CHECK(r.status == ExecStatus::Returned);
    CHECK(h.cpu.gpr[RAX] == 42);
    return true;
}

// Memory store/load through [base+disp]:
//   mov qword [rsp-8], 0x1234
//   mov rax, qword [rsp-8]
//   ret
bool TestMemoryRoundTrip() {
    std::cout << "[Test] memory store/load\n";
    std::vector<uint8_t> code = {
        0x48, 0xC7, 0x44, 0x24, 0xF8, 0x34, 0x12, 0x00, 0x00, // mov qword [rsp-8], 0x1234
        0x48, 0x8B, 0x44, 0x24, 0xF8,                         // mov rax, [rsp-8]
        0xC3,
    };
    Harness h(code);
    auto r = h.Run();
    CHECK(r.status == ExecStatus::Returned);
    CHECK(h.cpu.gpr[RAX] == 0x1234);
    return true;
}

// SIB addressing: array sum via [rbx + rcx*4].
//   store 0..3 into an array on the stack, then read element 2.
bool TestSibIndexing() {
    std::cout << "[Test] SIB [base+index*4]\n";
    std::vector<uint8_t> code = {
        // lea rbx, [rsp-32]
        0x48, 0x8D, 0x5C, 0x24, 0xE0,
        // mov dword [rbx + 0*4], 100  -> use rcx=0
        0x31, 0xC9,                         // xor ecx,ecx
        0xC7, 0x04, 0x8B, 0x64, 0x00, 0x00, 0x00, // mov dword [rbx+rcx*4], 100
        // rcx=2
        0xB9, 0x02, 0x00, 0x00, 0x00,       // mov ecx, 2
        0xC7, 0x04, 0x8B, 0x07, 0x00, 0x00, 0x00, // mov dword [rbx+rcx*4], 7
        // read element 2 back into eax
        0x8B, 0x04, 0x8B,                   // mov eax, [rbx+rcx*4]
        0xC3,
    };
    Harness h(code);
    auto r = h.Run();
    CHECK(r.status == ExecStatus::Returned);
    CHECK(h.cpu.gpr[RAX] == 7);
    return true;
}

// Flags: cmp then setcc.
//   mov eax, 5 ; cmp eax, 10 ; setl bl ; ret  -> bl == 1 (5 < 10 signed)
bool TestCmpSetcc() {
    std::cout << "[Test] cmp + setl\n";
    std::vector<uint8_t> code = {
        0xB8, 0x05, 0x00, 0x00, 0x00, // mov eax, 5
        0x83, 0xF8, 0x0A,             // cmp eax, 10
        0x0F, 0x9C, 0xC3,             // setl bl
        0xC3,
    };
    Harness h(code);
    auto r = h.Run();
    CHECK(r.status == ExecStatus::Returned);
    CHECK((h.cpu.gpr[RBX] & 0xFF) == 1);
    return true;
}

// movzx / movsx correctness.
//   mov eax, 0x80 ; movsx ecx, al ; movzx edx, al ; ret
//   ecx == 0xFFFFFF80 (as 32-bit, sign extended), edx == 0x80
bool TestMovzxMovsx() {
    std::cout << "[Test] movzx/movsx\n";
    std::vector<uint8_t> code = {
        0xB8, 0x80, 0x00, 0x00, 0x00, // mov eax, 0x80
        0x0F, 0xBE, 0xC8,             // movsx ecx, al
        0x0F, 0xB6, 0xD0,             // movzx edx, al
        0xC3,
    };
    Harness h(code);
    auto r = h.Run();
    CHECK(r.status == ExecStatus::Returned);
    CHECK((h.cpu.gpr[RCX] & 0xFFFFFFFF) == 0xFFFFFF80);
    CHECK((h.cpu.gpr[RDX] & 0xFFFFFFFF) == 0x80);
    return true;
}

// imul (two operand) and shift.
//   mov eax, 6 ; imul eax, eax, 7 ; shl eax, 1 ; ret -> 84
bool TestImulShift() {
    std::cout << "[Test] imul + shl\n";
    std::vector<uint8_t> code = {
        0xB8, 0x06, 0x00, 0x00, 0x00, // mov eax, 6
        0x6B, 0xC0, 0x07,             // imul eax, eax, 7
        0xD1, 0xE0,                   // shl eax, 1
        0xC3,
    };
    Harness h(code);
    auto r = h.Run();
    CHECK(r.status == ExecStatus::Returned);
    CHECK((h.cpu.gpr[RAX] & 0xFFFFFFFF) == 84);
    return true;
}

// push/pop preserve values across a scratch.
//   mov rax, 0x1111 ; push rax ; mov rax, 0x2222 ; pop rbx ; ret
//   rax=0x2222, rbx=0x1111
bool TestPushPop() {
    std::cout << "[Test] push/pop\n";
    std::vector<uint8_t> code = {
        0x48, 0xC7, 0xC0, 0x11, 0x11, 0x00, 0x00, // mov rax, 0x1111
        0x50,                                      // push rax
        0x48, 0xC7, 0xC0, 0x22, 0x22, 0x00, 0x00, // mov rax, 0x2222
        0x5B,                                      // pop rbx
        0xC3,
    };
    Harness h(code);
    auto r = h.Run();
    CHECK(r.status == ExecStatus::Returned);
    CHECK(h.cpu.gpr[RAX] == 0x2222);
    CHECK(h.cpu.gpr[RBX] == 0x1111);
    return true;
}

// A real syscall dispatch: mov eax, 42 ; syscall ; ret. Handler returns 7.
bool TestSyscall() {
    std::cout << "[Test] syscall handler\n";
    std::vector<uint8_t> code = {
        0xB8, 0x2A, 0x00, 0x00, 0x00, // mov eax, 42
        0x0F, 0x05,                   // syscall
        0xC3,
    };
    Harness h(code);
    X86Interpreter interp(h.cpu, h.mem);
    bool seen = false;
    interp.SetSyscallHandler([&](CpuState& s, GuestMemoryBus&) {
        seen = (s.gpr[RAX] == 42);
        s.gpr[RAX] = 7; // return value
        return true;
    });
    auto r = interp.Run(1000, Harness::kStop);
    CHECK(r.status == ExecStatus::Returned);
    CHECK(seen);
    CHECK(h.cpu.gpr[RAX] == 7);
    return true;
}

// Fibonacci(10) computed with a real loop and two registers — exercises
// add, xchg-free rotation via a temp, dec, jnz. Expected fib(10)=55.
bool TestFibonacci() {
    std::cout << "[Test] fibonacci(10)\n";
    //   xor eax,eax        a=0
    //   mov ebx,1          b=1
    //   mov ecx,10         n=10
    // top:
    //   mov edx,eax        t=a
    //   add edx,ebx        t=a+b
    //   mov eax,ebx        a=b
    //   mov ebx,edx        b=t
    //   dec ecx
    //   jnz top
    //   ret               (result in eax)
    std::vector<uint8_t> code = {
        0x31, 0xC0,             // xor eax,eax
        0xBB, 0x01, 0x00, 0x00, 0x00, // mov ebx,1
        0xB9, 0x0A, 0x00, 0x00, 0x00, // mov ecx,10
        // top:
        0x89, 0xC2,             // mov edx,eax
        0x01, 0xDA,             // add edx,ebx
        0x89, 0xD8,             // mov eax,ebx
        0x89, 0xD3,             // mov ebx,edx
        0xFF, 0xC9,             // dec ecx
        0x75, 0xF4,             // jnz top (-12)
        0xC3,
    };
    Harness h(code);
    auto r = h.Run();
    CHECK(r.status == ExecStatus::Returned);
    CHECK((h.cpu.gpr[RAX] & 0xFFFFFFFF) == 55);
    return true;
}

// Memory-fault safety: a store to an unmapped address must fault, not crash.
bool TestMemoryFault() {
    std::cout << "[Test] memory fault safety\n";
    std::vector<uint8_t> code = {
        0x48, 0xC7, 0xC0, 0x00, 0x00, 0x00, 0x00, // mov rax, 0
        0x48, 0x89, 0x18,                         // mov [rax], rbx  -> addr 0 unmapped
        0xC3,
    };
    Harness h(code);
    auto r = h.Run();
    CHECK(r.status == ExecStatus::MemoryFault);
    return true;
}

} // namespace

int main() {
    std::cout << "=== Extended x86-64 Interpreter Test Suite ===\n";
    TestAddAndReturn();
    TestLoopSum();
    TestCallRet();
    TestMemoryRoundTrip();
    TestSibIndexing();
    TestCmpSetcc();
    TestMovzxMovsx();
    TestImulShift();
    TestPushPop();
    TestSyscall();
    TestFibonacci();
    TestMemoryFault();

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    if (g_failures == 0) {
        std::cout << ">> [PASS] Extended x86-64 interpreter fully verified.\n";
        return 0;
    }
    std::cerr << ">> [FAIL] " << g_failures << " check(s) failed.\n";
    return 1;
}
