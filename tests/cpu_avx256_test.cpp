// ============================================================================
// ProsperoLayer RDNA2 Core - AVX.256 test (round 18)
// ----------------------------------------------------------------------------
// Round 16 modelled AVX.128 VEX only (AVX.256 failed closed: "128-bit
// register file"). Round 18 adds the real 256-bit register file -- YMM i is
// {xmm[i] (low 128), ymm_hi[i] (high 128)} -- and executes L=1 forms of the
// modelled VEX opcodes:
//
//   A. vaddps/vsubps/vmulps/vdivps/vminps/vmaxps over 8 float lanes,
//   B. vaddpd over 4 double lanes,
//   C. vandps/vxorps/vpand/vpor/vpxor over 32 bytes,
//   D. vmovdqu/vmovdqa ymm <-> 32-byte memory (incl. unaligned),
//   E. the AVX.128 register-write rule (VEX.128 ops zero the upper half),
//   F. vzeroupper / vzeroall,
//   G. vpshufd ymm (per-128-bit-lane shuffle),
//   H. vpshufb ymm (per-lane),
//   I. vptest ymm (ZF/CF across 32 bytes),
//   J. vsqrtps ymm,
//   K. fail-closed: a scalar VEX op with L=1 is an invalid encoding.
// ============================================================================
#include "cpu/x86_64_interpreter.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

using namespace PS5::CPU;

int g_failures = 0;
int g_checks = 0;
bool Check(bool v, const char* e, int line) {
    ++g_checks;
    if (!v) { ++g_failures; std::cerr << "  [FAIL] line " << line << ": " << e << '\n'; }
    return v;
}
#define CHECK(e) Check((e), #e, __LINE__)

constexpr uint64_t kBase = 0x2000000000ULL;
constexpr uint64_t kStop = 0x0000FEEDDEAD0000ULL;

struct FlatMemoryBus final : GuestMemoryBus {
    std::vector<uint8_t> mem;
    uint64_t base;
    FlatMemoryBus(uint64_t b, size_t bytes) : mem(bytes, 0), base(b) {}
    bool Read(uint64_t a, void* d, size_t n) override {
        if (a < base || a - base + n > mem.size()) return false;
        std::memcpy(d, mem.data() + (a - base), n);
        return true;
    }
    bool Write(uint64_t a, const void* s, size_t n) override {
        if (a < base || a - base + n > mem.size()) return false;
        std::memcpy(mem.data() + (a - base), s, n);
        return true;
    }
    void LoadBlob(uint64_t a, const std::vector<uint8_t>& code) {
        Write(a, code.data(), code.size());
    }
};

struct Asm {
    std::vector<uint8_t> code;
    std::vector<size_t> imm64_at;
    void b(uint8_t v) { code.push_back(v); }
    void imm64_slot() {
        imm64_at.push_back(code.size());
        for (int i = 0; i < 8; ++i) b(0);
    }
    void patch(const std::vector<uint64_t>& values) {
        for (size_t i = 0; i < imm64_at.size() && i < values.size(); ++i) {
            std::memcpy(&code[imm64_at[i]], &values[i], 8);
        }
    }
};

struct Machine {
    FlatMemoryBus bus{kBase, 0x40000};
    CpuState cpu{};

    Machine() {
        cpu.rip = kBase;
        uint64_t rsp = (kBase + 0x30000) & ~15ull;
        rsp -= 8;
        bus.Write(rsp, &kStop, 8);
        cpu.gpr[RSP] = rsp;
    }
    RunResult run(const std::vector<uint8_t>& code, size_t limit = 100000) {
        bus.LoadBlob(kBase, code);
        X86Interpreter interp(cpu, bus);
        return interp.Run(limit, kStop);
    }
};

template <typename T>
T memread(FlatMemoryBus& bus, uint64_t addr) {
    T v{};
    bus.Read(addr, &v, sizeof(T));
    return v;
}

// VEX.256 C5 encodings: C5 [R~ vvvv~ L pp] opcode modrm
// (R~=1, vvvv chosen per case; L=1 in the second byte.)

} // namespace

int main() {
    std::cout << "[avx256] round 18: the 256-bit register file\n";

    const uint64_t SRC = kBase + 0x13000;    // 32-byte source A
    const uint64_t SRC2 = kBase + 0x13040;   // 32-byte source B
    const uint64_t DST = kBase + 0x13100;    // result slot

    // =====================================================================
    // A: vaddps ymm0, ymm1, ymm2 + vsubps/vmulps/vdivps/vminps/vmaxps.
    // =====================================================================
    std::cout << "[avx256] A: packed float arithmetic (8 lanes)\n";
    {
        Machine m;
        const float a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        const float b[8] = {8, 7, 6, 5, 4, 3, 2, 1};
        m.bus.Write(SRC, a, 32);
        m.bus.Write(SRC2, b, 32);

        Asm s;
        s.b(0x48); s.b(0xB8); s.imm64_slot();               // mov rax,<SRC>
        s.b(0x48); s.b(0xB9); s.imm64_slot();               // mov rcx,<SRC2>
        s.b(0x48); s.b(0xBA); s.imm64_slot();               // mov rdx,<DST>
        // vmovdqu ymm1,[rax]      C5 FE 6F 08   (vvvv=0, L=1, no pp)
        s.b(0xC5); s.b(0xFC); s.b(0x6F); s.b(0x08);
        // vmovdqu ymm2,[rcx]      C5 FE 6F 11
        s.b(0xC5); s.b(0xFC); s.b(0x6F); s.b(0x11);
        // vaddps ymm0,ymm1,ymm2   C5 F4 58 C2   (vvvv=1)
        s.b(0xC5); s.b(0xF4); s.b(0x58); s.b(0xC2);
        // vmovdqu [rdx],ymm0      C5 FE 7F 02
        s.b(0xC5); s.b(0xFC); s.b(0x7F); s.b(0x02);
        s.b(0xC3);
        s.patch({SRC, SRC2, DST});
        const RunResult res = m.run(s.code);
        CHECK(res.status == ExecStatus::Returned);
        float got[8] = {};
        m.bus.Read(DST, got, 32);
        for (int i = 0; i < 8; ++i) CHECK(got[i] == a[i] + b[i]);

        // vsubps ymm3,ymm1,ymm2  (C5 E4 5C DA: vvvv=1, reg=3, rm=2)
        Machine m2;
        m2.bus.Write(SRC, a, 32);
        m2.bus.Write(SRC2, b, 32);
        Asm s2;
        s2.b(0x48); s2.b(0xB8); s2.imm64_slot();
        s2.b(0x48); s2.b(0xB9); s2.imm64_slot();
        s2.b(0x48); s2.b(0xBA); s2.imm64_slot();
        s2.b(0xC5); s2.b(0xFC); s2.b(0x6F); s2.b(0x08);   // vmovdqu ymm1,[rax]
        s2.b(0xC5); s2.b(0xFC); s2.b(0x6F); s2.b(0x11);   // vmovdqu ymm2,[rcx]
        s2.b(0xC5); s2.b(0xF4); s2.b(0x5C); s2.b(0xDA);   // vsubps ymm3,ymm1,ymm2
        s2.b(0xC5); s2.b(0xFC); s2.b(0x7F); s2.b(0x1A);   // vmovdqu [rdx],ymm3
        s2.b(0xC3);
        s2.patch({SRC, SRC2, DST});
        CHECK(m2.run(s2.code).status == ExecStatus::Returned);
        float got2[8] = {};
        m2.bus.Read(DST, got2, 32);
        for (int i = 0; i < 8; ++i) CHECK(got2[i] == a[i] - b[i]);

        // vmulps + vminps + vmaxps + vdivps on one program
        Machine m3;
        m3.bus.Write(SRC, a, 32);
        m3.bus.Write(SRC2, b, 32);
        Asm s3;
        s3.b(0x48); s3.b(0xB8); s3.imm64_slot();
        s3.b(0x48); s3.b(0xB9); s3.imm64_slot();
        s3.b(0x48); s3.b(0xBA); s3.imm64_slot();
        s3.b(0x48); s3.b(0xBF); s3.imm64_slot();           // mov rdi,<DST+0x80>
        s3.b(0xC5); s3.b(0xFC); s3.b(0x6F); s3.b(0x08);    // ymm1 = [rax]
        s3.b(0xC5); s3.b(0xFC); s3.b(0x6F); s3.b(0x11);    // ymm2 = [rcx]
        s3.b(0xC5); s3.b(0xF4); s3.b(0x59); s3.b(0xC2);    // ymm0 = ymm1*ymm2
        s3.b(0xC5); s3.b(0xFC); s3.b(0x7F); s3.b(0x02);    // [rdx] = ymm0
        s3.b(0xC5); s3.b(0xF4); s3.b(0x5F); s3.b(0xC2);    // ymm0 = max(ymm1,ymm2)
        s3.b(0xC5); s3.b(0xFC); s3.b(0x7F); s3.b(0x07);    // [rdi] = ymm0 (max)
        s3.b(0xC5); s3.b(0xF4); s3.b(0x5D); s3.b(0xC2);    // ymm0 = min(ymm1,ymm2)
        s3.b(0xC5); s3.b(0xFC); s3.b(0x7F); s3.b(0x42); s3.b(0x40); // [rdx+0x40] = min
        s3.b(0xC5); s3.b(0xF4); s3.b(0x5E); s3.b(0xC2);    // ymm0 = ymm1/ymm2
        s3.b(0xC5); s3.b(0xFC); s3.b(0x7F); s3.b(0x47); s3.b(0x40); // [rdi+0x40] = ymm0
        s3.b(0xC3);
        s3.patch({SRC, SRC2, DST, DST + 0x80});
        CHECK(m3.run(s3.code).status == ExecStatus::Returned);
        float mul[8] = {}, mxv[8] = {}, mn[8] = {}, dv[8] = {};
        m3.bus.Read(DST, mul, 32);
        m3.bus.Read(DST + 0x40, mn, 32);
        m3.bus.Read(DST + 0x80, mxv, 32);
        m3.bus.Read(DST + 0xC0, dv, 32);
        for (int i = 0; i < 8; ++i) {
            CHECK(std::fabs(mul[i] - a[i] * b[i]) < 1e-6f);
            CHECK(std::fabs(mn[i] - (a[i] < b[i] ? a[i] : b[i])) < 1e-6f);
            CHECK(std::fabs(mxv[i] - (a[i] > b[i] ? a[i] : b[i])) < 1e-6f);
            CHECK(std::fabs(dv[i] - a[i] / b[i]) < 1e-6f);
        }
    }

    // =====================================================================
    // B: vaddpd ymm (4 double lanes).
    // =====================================================================
    std::cout << "[avx256] B: packed double arithmetic\n";
    {
        Machine m;
        const double a[4] = {1.5, 2.5, 3.5, 4.5};
        const double b[4] = {10.25, 20.25, 30.25, 40.25};
        m.bus.Write(SRC, a, 32);
        m.bus.Write(SRC2, b, 32);
        Asm s;
        s.b(0x48); s.b(0xB8); s.imm64_slot();
        s.b(0x48); s.b(0xB9); s.imm64_slot();
        s.b(0x48); s.b(0xBA); s.imm64_slot();
        s.b(0xC5); s.b(0xFD); s.b(0x6F); s.b(0x08);   // vmovapd ymm1,[rax]
        s.b(0xC5); s.b(0xFD); s.b(0x6F); s.b(0x11);   // vmovapd ymm2,[rcx]
        s.b(0xC5); s.b(0xF5); s.b(0x58); s.b(0xC2);   // vaddpd ymm0,ymm1,ymm2
        s.b(0xC5); s.b(0xFD); s.b(0x7F); s.b(0x02);   // vmovapd [rdx],ymm0
        s.b(0xC3);
        s.patch({SRC, SRC2, DST});
        CHECK(m.run(s.code).status == ExecStatus::Returned);
        double got[4] = {};
        m.bus.Read(DST, got, 32);
        for (int i = 0; i < 4; ++i) CHECK(got[i] == a[i] + b[i]);
    }

    // =====================================================================
    // C: bitwise 256-bit ops.
    // =====================================================================
    std::cout << "[avx256] C: bitwise\n";
    {
        Machine m;
        const uint64_t a[4] = {0xF0F0F0F0F0F0F0F0ull, 0x0F0F0F0F0F0F0F0Full,
                               0xAAAAAAAAAAAAAAAAull, 0x5555555555555555ull};
        const uint64_t b[4] = {0xFF00FF00FF00FF00ull, 0x00FF00FF00FF00FFull,
                               0xCCCCCCCCCCCCCCCCull, 0x3333333333333333ull};
        m.bus.Write(SRC, a, 32);
        m.bus.Write(SRC2, b, 32);
        Asm s;
        s.b(0x48); s.b(0xB8); s.imm64_slot();
        s.b(0x48); s.b(0xB9); s.imm64_slot();
        s.b(0x48); s.b(0xBA); s.imm64_slot();
        s.b(0x48); s.b(0xBF); s.imm64_slot();            // mov rdi, DST+0x40
        s.b(0xC5); s.b(0xFC); s.b(0x6F); s.b(0x08);      // vmovdqu ymm1,[rax]
        s.b(0xC5); s.b(0xFC); s.b(0x6F); s.b(0x11);      // vmovdqu ymm2,[rcx]
        s.b(0xC5); s.b(0xF5); s.b(0x54); s.b(0xC2);      // vandps ymm0,ymm1,ymm2
        s.b(0xC5); s.b(0xFC); s.b(0x7F); s.b(0x02);      // [rdx] = and
        s.b(0xC5); s.b(0xF5); s.b(0x57); s.b(0xC2);      // vxorps ymm0,ymm1,ymm2
        s.b(0xC5); s.b(0xFC); s.b(0x7F); s.b(0x07);      // [rdi] = xor
        s.b(0xC3);
        s.patch({SRC, SRC2, DST, DST + 0x40});
        CHECK(m.run(s.code).status == ExecStatus::Returned);
        uint64_t andv[4] = {}, xorv[4] = {};
        m.bus.Read(DST, andv, 32);
        m.bus.Read(DST + 0x40, xorv, 32);
        for (int i = 0; i < 4; ++i) {
            CHECK(andv[i] == (a[i] & b[i]));
            CHECK(xorv[i] == (a[i] ^ b[i]));
        }
    }

    // =====================================================================
    // E: the AVX.128 register-write rule -- a VEX.128 op writing a register
    //    ZEROES its upper half (and vzeroupper does the same explicitly).
    // =====================================================================
    std::cout << "[avx256] E: upper-half zeroing rules\n";
    {
        Machine m;
        const float a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        m.bus.Write(SRC, a, 32);
        Asm s;
        s.b(0x48); s.b(0xB8); s.imm64_slot();            // mov rax,<SRC>
        s.b(0x48); s.b(0xBA); s.imm64_slot();            // mov rdx,<DST>
        s.b(0xC5); s.b(0xFC); s.b(0x6F); s.b(0x08);      // vmovdqu ymm1,[rax]
        s.b(0xC5); s.b(0xF0); s.b(0x58); s.b(0xC9);      // vaddps xmm1,xmm1,xmm1 (128!)
        s.b(0xC5); s.b(0xFC); s.b(0x7F); s.b(0x0A);      // vmovdqu [rdx],ymm1
        s.b(0xC3);
        s.patch({SRC, DST});
        CHECK(m.run(s.code).status == ExecStatus::Returned);
        float got[8] = {};
        m.bus.Read(DST, got, 32);
        CHECK(got[0] == 2.0f && got[3] == 8.0f);         // low half doubled
        CHECK(got[4] == 0.0f && got[7] == 0.0f);         // upper half zeroed
    }

    // =====================================================================
    // F: vzeroupper / vzeroall.
    // =====================================================================
    std::cout << "[avx256] F: vzeroupper/vzeroall\n";
    {
        Machine m;
        const float a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        m.bus.Write(SRC, a, 32);
        Asm s;
        s.b(0x48); s.b(0xB8); s.imm64_slot();
        s.b(0x48); s.b(0xBA); s.imm64_slot();
        s.b(0xC5); s.b(0xFC); s.b(0x6F); s.b(0x08);      // ymm1 = [rax] (full)
        s.b(0xC5); s.b(0xF8); s.b(0x77);                 // vzeroupper
        s.b(0xC5); s.b(0xFC); s.b(0x7F); s.b(0x0A);      // [rdx] = ymm1
        s.b(0xC3);
        s.patch({SRC, DST});
        CHECK(m.run(s.code).status == ExecStatus::Returned);
        float got[8] = {};
        m.bus.Read(DST, got, 32);
        CHECK(got[0] == 1.0f && got[3] == 4.0f);         // low preserved
        CHECK(got[4] == 0.0f && got[7] == 0.0f);         // upper cleared

        Machine m2;
        m2.bus.Write(SRC, a, 32);
        Asm s2;
        s2.b(0x48); s2.b(0xB8); s2.imm64_slot();
        s2.b(0x48); s2.b(0xBA); s2.imm64_slot();
        s2.b(0xC5); s2.b(0xFC); s2.b(0x6F); s2.b(0x08);  // ymm1 = [rax]
        s2.b(0xC5); s2.b(0xFC); s2.b(0x77);              // vzeroall (VEX.256)
        s2.b(0xC5); s2.b(0xFC); s2.b(0x7F); s2.b(0x0A);  // [rdx] = ymm1
        s2.b(0xC3);
        s2.patch({SRC, DST});
        CHECK(m2.run(s2.code).status == ExecStatus::Returned);
        float got2[8] = {};
        m2.bus.Read(DST, got2, 32);
        for (int i = 0; i < 8; ++i) CHECK(got2[i] == 0.0f);
    }

    // =====================================================================
    // G: vpshufd ymm -- the shuffle applies per 128-bit lane.
    // =====================================================================
    std::cout << "[avx256] G: vpshufd per-lane shuffle\n";
    {
        Machine m;
        const uint32_t a[8] = {0xA0, 0xA1, 0xA2, 0xA3, 0xB0, 0xB1, 0xB2, 0xB3};
        m.bus.Write(SRC, a, 32);
        Asm s;
        s.b(0x48); s.b(0xB8); s.imm64_slot();
        s.b(0x48); s.b(0xBA); s.imm64_slot();
        s.b(0xC5); s.b(0xFC); s.b(0x6F); s.b(0x08);      // ymm1 = [rax]
        // vpshufd ymm0, ymm1, 0x1B (3,2,1,0 per lane)  C5 FD 70 C1 1B
        s.b(0xC5); s.b(0xFD); s.b(0x70); s.b(0xC1); s.b(0x1B);
        s.b(0xC5); s.b(0xFC); s.b(0x7F); s.b(0x02);      // [rdx] = ymm0
        s.b(0xC3);
        s.patch({SRC, DST});
        CHECK(m.run(s.code).status == ExecStatus::Returned);
        uint32_t got[8] = {};
        m.bus.Read(DST, got, 32);
        CHECK(got[0] == 0xA3 && got[1] == 0xA2 && got[2] == 0xA1 && got[3] == 0xA0);
        CHECK(got[4] == 0xB3 && got[5] == 0xB2 && got[6] == 0xB1 && got[7] == 0xB0);
    }

    // =====================================================================
    // I: vptest ymm -- ZF/CF over 32 bytes.
    // =====================================================================
    std::cout << "[avx256] I: vptest\n";
    {
        Machine m;
        const uint64_t a[4] = {1, 2, 3, 4};
        const uint64_t b[4] = {1, 0, 0, 0};
        m.bus.Write(SRC, a, 32);
        m.bus.Write(SRC2, b, 32);
        Asm s;
        s.b(0x48); s.b(0xB8); s.imm64_slot();
        s.b(0x48); s.b(0xB9); s.imm64_slot();
        s.b(0x48); s.b(0xBA); s.imm64_slot();            // rdx = flag slot
        // vptest ymm1, [rcx] against a LOADED ymm: load both first
        s.b(0xC5); s.b(0xFC); s.b(0x6F); s.b(0x08);      // ymm1 = [rax]
        s.b(0xC5); s.b(0xFC); s.b(0x6F); s.b(0x19);      // ymm3 = [rcx]
        // vptest ymm1, ymm3   C4 E2 7D 17 CB (C4: R=1 X=0 B=0? build explicitly)
        // C4 E2 7D 17 CB: m-mmmm=00010 (0F38), W=0, vvvv=1, L=1, pp=0
        s.b(0xC4); s.b(0xE2); s.b(0x7D); s.b(0x17); s.b(0xCB);
        s.b(0x0F); s.b(0x94); s.b(0xC3);                 // sete bl (ZF)
        s.b(0x88); s.b(0x1A);                            // mov [rdx], bl
        s.b(0xC3);
        s.patch({SRC, SRC2, DST});
        CHECK(m.run(s.code).status == ExecStatus::Returned);
        // (a & b) != 0 -> ZF=0 -> sete gives 0
        CHECK(memread<uint8_t>(m.bus, DST) == 0);

        // Fully disjoint masks: (a & b) == 0 -> ZF=1; (a & ~b) != 0 -> CF=0
        Machine m2;
        const uint64_t c[4] = {0, 0, 0, 0};
        const uint64_t d[4] = {5, 6, 7, 8};
        m2.bus.Write(SRC, c, 32);
        m2.bus.Write(SRC2, d, 32);
        Asm s2;
        s2.b(0x48); s2.b(0xB8); s2.imm64_slot();
        s2.b(0x48); s2.b(0xB9); s2.imm64_slot();
        s2.b(0x48); s2.b(0xBA); s2.imm64_slot();
        s2.b(0xC5); s2.b(0xFC); s2.b(0x6F); s2.b(0x08);  // ymm1 = [rax] (zeros)
        s2.b(0xC5); s2.b(0xFC); s2.b(0x6F); s2.b(0x19);  // ymm3 = [rcx]
        s2.b(0xC4); s2.b(0xE2); s2.b(0x7D); s2.b(0x17); s2.b(0xCB); // vptest
        s2.b(0x0F); s2.b(0x94); s2.b(0xC3);              // sete bl
        s2.b(0x88); s2.b(0x1A);
        s2.b(0xC3);
        s2.patch({SRC, SRC2, DST});
        CHECK(m2.run(s2.code).status == ExecStatus::Returned);
        CHECK(memread<uint8_t>(m2.bus, DST) == 1);       // ZF=1 (and == 0)
    }

    // =====================================================================
    // J: vsqrtps ymm (8 lanes).
    // =====================================================================
    std::cout << "[avx256] J: vsqrtps\n";
    {
        Machine m;
        const float a[8] = {1, 4, 9, 16, 25, 36, 49, 64};
        m.bus.Write(SRC, a, 32);
        Asm s;
        s.b(0x48); s.b(0xB8); s.imm64_slot();
        s.b(0x48); s.b(0xBA); s.imm64_slot();
        s.b(0xC5); s.b(0xFC); s.b(0x6F); s.b(0x08);      // ymm1 = [rax]
        // vsqrtps ymm0, ymm1   C5 FC 51 C1
        s.b(0xC5); s.b(0xFC); s.b(0x51); s.b(0xC1);
        s.b(0xC5); s.b(0xFC); s.b(0x7F); s.b(0x02);      // [rdx] = ymm0
        s.b(0xC3);
        s.patch({SRC, DST});
        CHECK(m.run(s.code).status == ExecStatus::Returned);
        float got[8] = {};
        m.bus.Read(DST, got, 32);
        for (int i = 0; i < 8; ++i) CHECK(got[i] == std::sqrt(a[i]));
    }

    // =====================================================================
    // K: fail-closed -- a scalar VEX op with L=1 is an invalid encoding.
    // =====================================================================
    std::cout << "[avx256] K: scalar L=1 rejected\n";
    {
        Machine m;
        Asm s;
        // vaddss ymm0, ymm1, ymm2 (C5 F2 58 C2: pp=2 scalar + L=1)
        s.b(0xC5); s.b(0xF6); s.b(0x58); s.b(0xC2);
        s.b(0xC3);
        const RunResult res = m.run(s.code, 10);
        CHECK(res.status == ExecStatus::UnsupportedOpcode);
    }

    std::cout << "[avx256] " << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
