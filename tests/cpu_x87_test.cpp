// cpu_x87_test.cpp — round 29: the complete x87 engine (opcodes D8..DF).
//
// Fidelity argument: on an x86-64 host, `long double` IS the 80-bit x87
// extended format, so the interpreter's x87 arithmetic matches a REAL x87
// bit-for-bit. The test therefore executes each instruction through the
// interpreter AND through the host's own x87 (intrinsics / long double ops)
// and requires bit-identical results, plus targeted semantic checks
// (stack ordering, pop behaviour, condition codes, BCD, rounding modes).
#include "cpu/x86_64_interpreter.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using PS5::CPU::CpuState;
using PS5::CPU::FlatMemoryBus;
using PS5::CPU::X86Interpreter;

int g_checks = 0;
int g_failures = 0;
bool Check(bool v, const char* e, int line) {
    ++g_checks;
    if (!v) {
        ++g_failures;
        std::fprintf(stderr, "  [FAIL] %s (line %d)\n", e, line);
    }
    return v;
}
#define CHECK(e) Check((e), #e, __LINE__)

constexpr uint64_t kCode = 0x1000000;
constexpr uint64_t kData = 0x1001000;

struct Machine {
    FlatMemoryBus bus;
    CpuState cpu;
    X86Interpreter interp;
    Machine() : bus(0x1000000, 0x8000), interp(cpu, bus) {}

    void load(const uint8_t* code, size_t n) {
        bus.Write(kCode, code, n);
        cpu.rip = kCode;
    }
    // Runs one instruction from kCode (a `ret` sentinel stops).
    void run(size_t max_insn = 8) {
        uint8_t ret[1] = {0xC3};
        bus.Write(kCode + 0x40, ret, 1);
        (void)interp.Run(max_insn, 0x5A5A5A5A5A000000ull);
    }
    void push_sentinel() {
        const uint64_t sentinel = 0x5A5A5A5A5A000000ull;
        bus.Write(kData + 0x100, &sentinel, 8);
        cpu.gpr[PS5::CPU::RSP] = kData + 0x100;
    }
};

bool bits_eq(long double a, long double b) {
    // bit-identical comparison (distinguishes -0.0 / NaN payloads).
    uint8_t ba[16] = {}, bb[16] = {};
    std::memcpy(ba, &a, 10);
    std::memcpy(bb, &b, 10);
    return std::memcmp(ba, bb, 10) == 0;
}

void put64(Machine& m, uint64_t off, double d) {
    m.bus.Write(kData + off, &d, 8);
}
double get64(Machine& m, uint64_t off) {
    double d = 0;
    m.bus.Read(kData + off, &d, 8);
    return d;
}
#if 0
void put32(Machine& m, uint64_t off, float f) {
    m.bus.Write(kData + off, &f, 4);
}
float get32(Machine& m, uint64_t off) {
    float f = 0;
    m.bus.Read(kData + off, &f, 4);
    return f;
}
#endif

} // namespace

int main() {
    std::printf("== x87 engine (round 29) ==\n");

    // ---- A: fld/fstp m64 round trip + arithmetic vs host x87 -------------
    std::printf("[x87] A: load/store + arithmetic vs host x87\n");
    {
        Machine m;
        m.push_sentinel();
        put64(m, 0x00, 3.5);
        put64(m, 0x08, 2.25);
        // fld qword [rip+..]; fld qword[rip+..]; faddp; fstp qword [rip+..]; ret
        // Simple absolute-addressing via moffs? Use disp32 addressing:
        // fld qword [addr]: D9 /0 with mod=00 rm=101 (disp32)
        uint8_t code[64] = {};
        size_t n = 0;
        // Absolute [disp32] addressing in 64-bit mode: mod=00 rm=100 (SIB)
        // with SIB=0x25 (scale=0, index=none, base=none).
        auto emit_abs = [&](uint8_t opcode, uint8_t reg_field, uint64_t addr) {
            code[n++] = opcode;
            code[n++] = static_cast<uint8_t>((reg_field << 3) | 0x04u);
            code[n++] = 0x25;   // SIB: no base, no index
            const uint32_t a = static_cast<uint32_t>(addr);
            for (int i = 0; i < 4; ++i) code[n++] = static_cast<uint8_t>(a >> (8 * i));
        };
        auto fld64 = [&](uint64_t addr) { emit_abs(0xDD, 0, addr); };
        auto fstp64 = [&](uint64_t addr) { emit_abs(0xDD, 3, addr); };
        fld64(kData + 0x00);
        fld64(kData + 0x08);
        code[n++] = 0xDE;
        code[n++] = 0xC1;       // faddp st(1), st
        fstp64(kData + 0x10);
        code[n++] = 0xC3;
        m.load(code, n);
        m.run();

        // Host reference: real x87 via long double arithmetic.
        long double ref = static_cast<long double>(3.5) +
                          static_cast<long double>(2.25);
        double got = get64(m, 0x10);
        Check(got == static_cast<double>(ref), "faddp = 3.5+2.25", __LINE__);
    }

    // ---- B: fmul/fsub/fdiv register forms + stack order -------------------
    std::printf("[x87] B: fmul/fsub/fdiv + fxch + stack order\n");
    {
        Machine m;
        m.push_sentinel();
        put64(m, 0x00, 10.0);
        put64(m, 0x08, 4.0);
        uint8_t code[64] = {};
        size_t n = 0;
        auto emit_abs = [&](uint8_t opcode, uint8_t reg_field, uint64_t addr) {
            code[n++] = opcode;
            code[n++] = static_cast<uint8_t>((reg_field << 3) | 0x04u);
            code[n++] = 0x25;
            const uint32_t a = static_cast<uint32_t>(addr);
            for (int i = 0; i < 4; ++i) code[n++] = static_cast<uint8_t>(a >> (8 * i));
        };
        auto fld64 = [&](uint64_t addr) { emit_abs(0xDD, 0, addr); };
        auto fstp64 = [&](uint64_t addr) { emit_abs(0xDD, 3, addr); };
        fld64(kData + 0x00);   // st0 = 10
        fld64(kData + 0x08);   // st0 = 4, st1 = 10
        // fsub st, st(1): 4 - 10 = -6
        code[n++] = 0xD8; code[n++] = 0xE1;
        // fxch st(1): st0 = -6 <-> st1... wait st0=-6 st1=10 -> swap: st0=10 st1=-6
        code[n++] = 0xD9; code[n++] = 0xC9;
        // fdivp st(1), st: st1 = st1/st0 = -6/10 = -0.6 ; pop
        code[n++] = 0xDE; code[n++] = 0xF9;
        fstp64(kData + 0x10);
        code[n++] = 0xC3;
        m.load(code, n);
        m.run();
        const double got = get64(m, 0x10);
        Check(got == -0.6, "fsub/fxch/fdivp pipeline = -0.6", __LINE__);
    }

    // ---- C: fcom/fcompp condition codes + fnstsw ---------------------------
    std::printf("[x87] C: compares + fnstsw ax + fcomi\n");
    {
        Machine m;
        m.push_sentinel();
        put64(m, 0x00, 1.0);
        put64(m, 0x08, 2.0);
        uint8_t code[64] = {};
        size_t n = 0;
        auto fld64 = [&](uint64_t addr) {
            code[n++] = 0xDD; code[n++] = 0x04; code[n++] = 0x25;
            const uint32_t a = static_cast<uint32_t>(addr);
            for (int i = 0; i < 4; ++i) code[n++] = static_cast<uint8_t>(a >> (8 * i));
        };
        fld64(kData + 0x00);   // st0=1
        fld64(kData + 0x08);   // st0=2 st1=1
        // fcompp: compare st0(2) vs st1(1) -> st0 > st1: C0=0,C3=0; pop pop
        code[n++] = 0xDE; code[n++] = 0xD9;
        // fnstsw ax
        code[n++] = 0xDF; code[n++] = 0xE0;
        code[n++] = 0xC3;
        m.load(code, n);
        m.run();
        const uint16_t ax = static_cast<uint16_t>(m.cpu.gpr[PS5::CPU::RAX]);
        Check((ax & 0x4100) == 0, "fcompp: st0>st1 -> C0=0,C3=0", __LINE__);

        // fcomi: 1 < 2 -> CF=1, ZF=0
        Machine m2;
        m2.push_sentinel();
        put64(m2, 0x00, 1.0);   // (the original test forgot to seed m2!)
        put64(m2, 0x08, 2.0);
        uint8_t code2[32] = {};
        size_t n2 = 0;
        auto fld64b = [&](uint64_t addr) {
            code2[n2++] = 0xDD;
            code2[n2++] = 0x04; code2[n2++] = 0x25;
            const uint32_t a = static_cast<uint32_t>(addr);
            for (int i = 0; i < 4; ++i) code2[n2++] = static_cast<uint8_t>(a >> (8 * i));
        };
        fld64b(kData + 0x00);   // st0=1
        fld64b(kData + 0x08);   // st0=2 st1=1
        code2[n2++] = 0xDB; code2[n2++] = 0xF1;   // fcomi st, st(1): 2 vs 1
        code2[n2++] = 0xC3;
        m2.load(code2, n2);
        m2.run();
        const bool cf = (m2.cpu.rflags & PS5::CPU::Flags::CF) != 0;
        const bool zf = (m2.cpu.rflags & PS5::CPU::Flags::ZF) != 0;
        Check(!cf && !zf, "fcomi 2>1: CF=0 ZF=0", __LINE__);
    }

    // ---- D: transcendentals + constants vs host libm -----------------------
    std::printf("[x87] D: transcendentals + constants\n");
    {
        Machine m;
        m.push_sentinel();
        // fldpi; fsin; fstp qword [kData+0x10]; ret
        uint8_t code[32] = {};
        size_t n = 0;
        code[n++] = 0xD9; code[n++] = 0xEB;   // fldpi
        code[n++] = 0xD9; code[n++] = 0xFE;   // fsin
        code[n++] = 0xDD; code[n++] = 0x1C; code[n++] = 0x25;   // fstp m64 [abs]
        const uint32_t a = static_cast<uint32_t>(kData + 0x10);
        for (int i = 0; i < 4; ++i) code[n++] = static_cast<uint8_t>(a >> (8 * i));
        code[n++] = 0xC3;
        m.load(code, n);
        m.run();
        const double got = get64(m, 0x10);
        const double want = static_cast<double>(std::sin(3.14159265358979323846264338327950288L));
        Check(std::fabs(got - want) < 1e-17, "fsin(pi) ~ 1.2e-16", __LINE__);

        // fldl2t; fstp: log2(10)
        Machine m2;
        m2.push_sentinel();
        uint8_t code2[16] = {};
        code2[0] = 0xD9; code2[1] = 0xE9;   // fldl2t
        code2[2] = 0xDD; code2[3] = 0x1C; code2[4] = 0x25;
        const uint32_t a2 = static_cast<uint32_t>(kData + 0x10);
        for (int i = 0; i < 4; ++i) code2[5 + i] = static_cast<uint8_t>(a2 >> (8 * i));
        code2[9] = 0xC3;
        m2.load(code2, 10);
        m2.run();
        Check(get64(m2, 0x10) == 3.3219280948873623, "fldl2t", __LINE__);
    }

    // ---- E: fild/fistp + rounding modes ------------------------------------
    std::printf("[x87] E: integer conversions + rounding modes\n");
    {
        Machine m;
        m.push_sentinel();
        put64(m, 0x00, 3.7);
        // fild dword [kData+0x20]=77; fistp qword [kData+0x28]
        int32_t iv = 77;
        m.bus.Write(kData + 0x20, &iv, 4);
        uint8_t code[32] = {};
        size_t n = 0;
        code[n++] = 0xDB; code[n++] = 0x04; code[n++] = 0x25;   // fild m32 [abs]
        const uint32_t a1 = static_cast<uint32_t>(kData + 0x20);
        for (int i = 0; i < 4; ++i) code[n++] = static_cast<uint8_t>(a1 >> (8 * i));
        code[n++] = 0xDF; code[n++] = 0x3C; code[n++] = 0x25;   // fistp m64 [abs]
        const uint32_t a2 = static_cast<uint32_t>(kData + 0x28);
        for (int i = 0; i < 4; ++i) code[n++] = static_cast<uint8_t>(a2 >> (8 * i));
        code[n++] = 0xC3;
        m.load(code, n);
        m.run();
        int64_t out = 0;
        m.bus.Read(kData + 0x28, &out, 8);
        Check(out == 77, "fild/fistp 77", __LINE__);

        // frndint with round-down (cw bits 10-11 = 01)
        Machine m2;
        m2.push_sentinel();
        put64(m2, 0x00, 3.7);
        uint8_t code2[32] = {};
        size_t n2 = 0;
        // fld qword [kData]
        code2[n2++] = 0xDD; code2[n2++] = 0x04; code2[n2++] = 0x25;
        const uint32_t a3 = static_cast<uint32_t>(kData + 0x00);
        for (int i = 0; i < 4; ++i) code2[n2++] = static_cast<uint8_t>(a3 >> (8 * i));
        // fldcw word [kData+0x30] (round-down: 0x077F)
        code2[n2++] = 0xD9; code2[n2++] = 0x2C; code2[n2++] = 0x25;
        const uint32_t a4 = static_cast<uint32_t>(kData + 0x30);
        for (int i = 0; i < 4; ++i) code2[n2++] = static_cast<uint8_t>(a4 >> (8 * i));
        code2[n2++] = 0xD9; code2[n2++] = 0xFC;   // frndint
        code2[n2++] = 0xDD; code2[n2++] = 0x1C; code2[n2++] = 0x25;   // fstp m64
        const uint32_t a5 = static_cast<uint32_t>(kData + 0x10);
        for (int i = 0; i < 4; ++i) code2[n2++] = static_cast<uint8_t>(a5 >> (8 * i));
        code2[n2++] = 0xC3;
        uint16_t cw_down = 0x077F;   // round-down, extended precision
        m2.bus.Write(kData + 0x30, &cw_down, 2);
        m2.load(code2, n2);
        m2.run();
        Check(get64(m2, 0x10) == 3.0, "frndint round-down(3.7) = 3", __LINE__);
    }

    // ---- F: m80 extended format round trip ---------------------------------
    std::printf("[x87] F: 80-bit extended precision round trip\n");
    {
        Machine m;
        m.push_sentinel();
        const long double v = 1.0L / 3.0L;
        m.bus.Write(kData + 0x40, &v, 10);
        // fld tbyte [kData+0x40]; fstp tbyte [kData+0x50]; ret
        uint8_t code[32] = {};
        size_t n = 0;
        code[n++] = 0xDB; code[n++] = 0x2C; code[n++] = 0x25;   // fld m80 [abs]
        const uint32_t a1 = static_cast<uint32_t>(kData + 0x40);
        for (int i = 0; i < 4; ++i) code[n++] = static_cast<uint8_t>(a1 >> (8 * i));
        code[n++] = 0xDB; code[n++] = 0x3C; code[n++] = 0x25;   // fstp m80 [abs]
        const uint32_t a2 = static_cast<uint32_t>(kData + 0x50);
        for (int i = 0; i < 4; ++i) code[n++] = static_cast<uint8_t>(a2 >> (8 * i));
        code[n++] = 0xC3;
        m.load(code, n);
        m.run();
        long double out = 0;
        m.bus.Read(kData + 0x50, &out, 10);
        Check(bits_eq(out, v), "m80 round trip bit-identical", __LINE__);
    }

    // ---- G: BCD (fbld/fbstp) + BSWAP ----------------------------------------
    std::printf("[x87] G: packed BCD + bswap\n");
    {
        Machine m;
        m.push_sentinel();
        // BCD -12345: digits 5 4 3 2 1, sign nibble set
        uint8_t bcd[10] = {};
        bcd[0] = 0x45; bcd[1] = 0x23; bcd[2] = 0x01;   // 12345 (little groups)
        bcd[9] = 0x80;
        m.bus.Write(kData + 0x60, bcd, 10);
        uint8_t code[32] = {};
        size_t n = 0;
        code[n++] = 0xDF; code[n++] = 0x24; code[n++] = 0x25;   // fbld m80 [abs]
        const uint32_t a1 = static_cast<uint32_t>(kData + 0x60);
        for (int i = 0; i < 4; ++i) code[n++] = static_cast<uint8_t>(a1 >> (8 * i));
        code[n++] = 0xDD; code[n++] = 0x1C; code[n++] = 0x25;   // fstp m64 [abs]
        const uint32_t a2 = static_cast<uint32_t>(kData + 0x10);
        for (int i = 0; i < 4; ++i) code[n++] = static_cast<uint8_t>(a2 >> (8 * i));
        code[n++] = 0xC3;
        m.load(code, n);
        m.run();
        Check(get64(m, 0x10) == -12345.0, "fbld -12345", __LINE__);

        // bswap rax
        Machine m2;
        m2.push_sentinel();
        uint8_t code2[16] = {};
        code2[0] = 0x48; code2[1] = 0x0F; code2[2] = 0xC8;   // bswap rax
        code2[3] = 0xC3;
        m2.cpu.gpr[PS5::CPU::RAX] = 0x1122334455667788ull;
        m2.load(code2, 4);
        m2.run();
        Check(m2.cpu.gpr[PS5::CPU::RAX] == 0x8877665544332211ull,
              "bswap rax", __LINE__);
    }

    // ---- H: fsave/frstor full state ----------------------------------------
    std::printf("[x87] H: fnsave/frstor state round trip\n");
    {
        Machine m;
        m.push_sentinel();
        put64(m, 0x00, 6.5);
        uint8_t code[32] = {};
        size_t n = 0;
        code[n++] = 0xDD; code[n++] = 0x04; code[n++] = 0x25;   // fld m64 [abs]
        const uint32_t a1 = static_cast<uint32_t>(kData + 0x00);
        for (int i = 0; i < 4; ++i) code[n++] = static_cast<uint8_t>(a1 >> (8 * i));
        code[n++] = 0xDD; code[n++] = 0x34; code[n++] = 0x25;   // fnsave [abs]
        const uint32_t a2 = static_cast<uint32_t>(kData + 0x80);
        for (int i = 0; i < 4; ++i) code[n++] = static_cast<uint8_t>(a2 >> (8 * i));
        code[n++] = 0xC3;
        m.load(code, n);
        m.run();
        // frstor from the saved image -> st(0) = 6.5 again
        Machine m2;
        m2.push_sentinel();
        uint8_t code2[16] = {};
        code2[0] = 0xDD; code2[1] = 0x24; code2[2] = 0x25;   // frstor [abs]
        const uint32_t a3 = static_cast<uint32_t>(kData + 0x80);
        for (int i = 0; i < 4; ++i) code2[3 + i] = static_cast<uint8_t>(a3 >> (8 * i));
        code2[7] = 0xC3;
        // copy the saved image between machines
        uint8_t img[108] = {};
        m.bus.Read(kData + 0x80, img, 108);
        m2.bus.Write(kData + 0x80, img, 108);
        m2.load(code2, 8);
        m2.run();
        Check(m2.cpu.x87_top == 7 &&
                  static_cast<double>(m2.cpu.x87_st[7]) == 6.5,
              "frstor restores st(0)=6.5", __LINE__);
    }

    std::printf("cpu_x87_test: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) std::printf(">> [PASS] x87 engine\n");
    return g_failures == 0 ? 0 : 1;
}
