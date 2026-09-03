// ============================================================================
// ProsperoLayer RDNA2 Core - Full-ISA extension test (round 16)
// ----------------------------------------------------------------------------
// Part A: rotations (rol/ror) + bit ops (bt/bts/btc, popcnt/tzcnt, bsf/bsr) +
//         xadd + cmpxchg
// Part B: string ops with REP/REPE/REPNE (movsq/stosb/scasb/lodsq)
// Part C: SSSE3/SSE4.1 integer SIMD (pshufb, pshufd, pmulld, ptest, roundps)
// Part D: AVX.128 VEX (vmovdqa/vaddps/vmulps/vxorps/vpshufb/vptest) +
//         AVX.256 fail-closed
// Part E: full-decoder block scanner (InspectBlock) terminators + coverage
// ============================================================================
#include "cpu/x86_64_interpreter.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

using PS5::CPU::CpuState;
using PS5::CPU::RunResult;
using PS5::CPU::ExecStatus;
using PS5::CPU::FlatMemoryBus;
using PS5::CPU::RSP;
using PS5::CPU::X86Interpreter;

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

// Assembler with position-tracked 64-bit immediates (patched at the end).
struct Asm {
    std::vector<uint8_t> code;
    std::vector<size_t> imm64_at;

    void b(uint8_t v) { code.push_back(v); }
    void imm32(uint32_t v) { for (int i = 0; i < 4; ++i) b((v >> (8 * i)) & 0xFF); }
    void imm64_slot() { imm64_at.push_back(code.size()); for (int i = 0; i < 8; ++i) b(0); }
    void patch(const std::vector<uint64_t>& values) {
        CHECK(values.size() == imm64_at.size());
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

} // namespace

int main() {
    std::cout << "== Part A: rotations + bit ops + xadd + cmpxchg ==\n";
    {
        Machine m;
        Asm s;
        s.b(0x49); s.b(0xB9); s.imm64_slot();              // mov r9, <data>
        s.b(0xB8); s.imm32(0x89ABCDEF);                    // mov eax,0x89ABCDEF
        s.b(0xC0); s.b(0xC0); s.b(0x04);                   // rol al,4
        s.b(0xC1); s.b(0xC8); s.b(0x08);                   // ror eax,8 -> 0xFE89ABCD
        s.b(0xF3); s.b(0x0F); s.b(0xB8); s.b(0xC8);        // popcnt ecx,eax
        s.b(0xF3); s.b(0x0F); s.b(0xBC); s.b(0xD0);        // tzcnt edx,eax
        s.b(0x41); s.b(0x89); s.b(0x51); s.b(0x10);        // mov [r9+16],edx (before dl clobber)
        s.b(0x0F); s.b(0xBC); s.b(0xF0);                   // bsf esi,eax
        s.b(0x0F); s.b(0xBD); s.b(0xF8);                   // bsr edi,eax
        s.b(0xBB); s.imm32(0x10);                          // mov ebx,0x10
        s.b(0xBD); s.imm32(0x22);                          // mov ebp,0x22
        s.b(0x0F); s.b(0xC1); s.b(0xEB);                   // xadd ebx,ebp
        s.b(0x0F); s.b(0xBA); s.b(0xE0); s.b(0x06);        // bt eax,6 (bit6 of 0xCD = 1)
        s.b(0x0F); s.b(0x92); s.b(0xC2);                   // setc dl
        s.b(0x0F); s.b(0xBA); s.b(0xE8); s.b(0x00);        // bts eax,0 (already 1)
        s.b(0x0F); s.b(0xBA); s.b(0xF8); s.b(0x01);        // btc eax,1 (0->1)
        s.b(0x41); s.b(0x89); s.b(0x01);                   // mov [r9],eax
        s.b(0x41); s.b(0x89); s.b(0x49); s.b(0x08);        // mov [r9+8],ecx
        s.b(0x41); s.b(0x89); s.b(0x71); s.b(0x18);        // mov [r9+24],esi
        s.b(0x41); s.b(0x89); s.b(0x79); s.b(0x20);        // mov [r9+32],edi
        s.b(0x41); s.b(0x89); s.b(0x59); s.b(0x28);        // mov [r9+40],ebx
        s.b(0x41); s.b(0x89); s.b(0x69); s.b(0x30);        // mov [r9+48],ebp
        s.b(0x44); s.b(0x0F); s.b(0xB6); s.b(0xD2);        // movzx r10d,dl (REX.R: r10 dst)
        s.b(0x45); s.b(0x89); s.b(0x51); s.b(0x38);        // mov [r9+56],r10d
        // cmpxchg: eax=0x32 must equal [r9+64]; then [r9+64]=r8d(0x99)
        s.b(0xB8); s.imm32(0x32);                          // mov eax,0x32
        s.b(0x41); s.b(0xB8); s.imm32(0x99);               // mov r8d,0x99
        s.b(0x45); s.b(0x0F); s.b(0xB1); s.b(0x41); s.b(0x40); // cmpxchg [r9+64],r8d
        s.b(0x0F); s.b(0x94); s.b(0xC2);                   // sete dl (ZF after cmpxchg)
        s.b(0x41); s.b(0x88); s.b(0x51); s.b(0x3C);        // mov [r9+60],dl
        s.b(0xC3);
        const uint64_t data = kBase + 0x10000;
        s.patch({data});
        uint32_t preset = 0x32;
        m.bus.Write(data + 64, &preset, 4);
        const RunResult res = m.run(s.code);
        CHECK(res.status == ExecStatus::Returned);
        CHECK(memread<uint32_t>(m.bus, data + 0) == 0xFE89ABCF); // rol/ror/bts/btc
        CHECK(memread<uint32_t>(m.bus, data + 8) == 20);   // popcount(0xFE89ABCD)
        CHECK(memread<uint32_t>(m.bus, data + 16) == 0);   // tzcnt: bit0 set
        CHECK(memread<uint32_t>(m.bus, data + 24) == 0);   // bsf: bit0
        CHECK(memread<uint32_t>(m.bus, data + 32) == 31);  // bsr: bit31
        CHECK(memread<uint32_t>(m.bus, data + 40) == 0x32);// xadd ebx
        CHECK(memread<uint32_t>(m.bus, data + 48) == 0x10);// xadd ebp (old ebx)
        CHECK(memread<uint8_t>(m.bus, data + 56) == 1);    // bt eax,6 -> CF=1
        CHECK(memread<uint32_t>(m.bus, data + 64) == 0x99);// cmpxchg wrote r8d
        CHECK(memread<uint8_t>(m.bus, data + 60) == 1);    // cmpxchg ZF=1
    }

    std::cout << "== Part B: string ops + REP ==\n";
    {
        Machine m;
        const uint64_t src = kBase + 0x11000, dst = kBase + 0x11100;
        const uint64_t dst2 = kBase + 0x11200, outq = kBase + 0x11300;
        const uint64_t pattern[] = {0x1122334455667788ull, 0x99AABBCCDDEEFF00ull};
        m.bus.Write(src, pattern, 16);
        Asm s;
        s.b(0x48); s.b(0xBF); s.imm64_slot();              // mov rdi,<dst>
        s.b(0x48); s.b(0xBE); s.imm64_slot();              // mov rsi,<src>
        s.b(0xB9); s.imm32(2);                             // mov ecx,2
        s.b(0xF3); s.b(0x48); s.b(0xA5);                   // rep movsq
        s.b(0x48); s.b(0xBF); s.imm64_slot();              // mov rdi,<dst2>
        s.b(0xB9); s.imm32(16);                            // mov ecx,16
        s.b(0xB0); s.b(0xAB);                              // mov al,0xAB
        s.b(0xF3); s.b(0xAA);                              // rep stosb
        s.b(0xB9); s.imm32(16);                            // mov ecx,16
        s.b(0x48); s.b(0xBF); s.imm64_slot();              // mov rdi,<dst2> (reset for scasb)
        s.b(0xF2); s.b(0xAE);                              // repne scasb (stop at match)
        s.b(0x48); s.b(0xBE); s.imm64_slot();              // mov rsi,<src>
        s.b(0x48); s.b(0xAD);                              // lodsq
        s.b(0x49); s.b(0xB9); s.imm64_slot();              // mov r9,<outq>
        s.b(0x49); s.b(0x89); s.b(0x01);                   // mov [r9],rax
        s.b(0xC3);
        s.patch({dst, src, dst2, dst2, src, outq});
        const RunResult res = m.run(s.code);
        CHECK(res.status == ExecStatus::Returned);
        CHECK(memread<uint64_t>(m.bus, dst) == 0x1122334455667788ull);
        CHECK(memread<uint64_t>(m.bus, dst + 8) == 0x99AABBCCDDEEFF00ull);
        CHECK(memread<uint8_t>(m.bus, dst2) == 0xAB);
        CHECK(memread<uint8_t>(m.bus, dst2 + 15) == 0xAB);
        CHECK(memread<uint64_t>(m.bus, outq) == 0x1122334455667788ull);
        // repne scasb: al=0xAB matches dst2[0] immediately -> ZF=1 stops the
        // loop after 1 compare: rcx = 16-1 = 15, rdi advanced by 1.
        CHECK(m.cpu.gpr[PS5::CPU::RCX] == 15);
        CHECK(m.cpu.gpr[PS5::CPU::RDI] == dst2 + 1);
    }

    std::cout << "== Part C: SSSE3/SSE4.1 integer SIMD ==\n";
    {
        Machine m;
        const uint64_t srcv = kBase + 0x12000, maska = kBase + 0x12010;
        const uint64_t o1 = kBase + 0x12100, o0 = kBase + 0x12140;
        const uint64_t o2 = kBase + 0x12120, dlo = kBase + 0x12180;
        const uint32_t src_data[4] = {1, 2, 3, 4};
        const float rnd_data[4] = {4.75f, 3.9f, 2.5f, 1.5f};
        // byte-reverse lanes 0..1, zero lanes 2..3 (0x80 = zero)
        const uint8_t mask_data[16] = {3, 2, 1, 0, 7, 6, 5, 4,
                                       0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
        m.bus.Write(srcv, src_data, 16);
        m.bus.Write(maska, mask_data, 16);
        m.bus.Write(o1 + 0x100, rnd_data, 16);

        Asm s;
        s.b(0x48); s.b(0xB8); s.imm64_slot();              // mov rax,<src>
        s.b(0x48); s.b(0xB9); s.imm64_slot();              // mov rcx,<o1>
        s.b(0x48); s.b(0xBA); s.imm64_slot();              // mov rdx,<o0>
        s.b(0x48); s.b(0xBB); s.imm64_slot();              // mov rbx,<mask>
        s.b(0x48); s.b(0xBE); s.imm64_slot();              // mov rsi,<o2>
        s.b(0x48); s.b(0xBF); s.imm64_slot();              // mov rdi,<dlo>
        s.b(0x66); s.b(0x0F); s.b(0x6F); s.b(0x00);        // movdqa xmm0,[rax]
        s.b(0x66); s.b(0x0F); s.b(0x70); s.b(0xC8); s.b(0x1B); // pshufd xmm1,xmm0,0x1B
        s.b(0x0F); s.b(0x29); s.b(0x09);                   // movaps [rcx],xmm1
        s.b(0x66); s.b(0x0F); s.b(0x38); s.b(0x00); s.b(0x03); // pshufb xmm0,[rbx]
        s.b(0x0F); s.b(0x29); s.b(0x02);                   // movaps [rdx],xmm0
        s.b(0x66); s.b(0x0F); s.b(0x6F); s.b(0xD1);        // movdqa xmm2,xmm1
        s.b(0x66); s.b(0x0F); s.b(0x38); s.b(0x40); s.b(0xD1); // pmulld xmm2,xmm1 (squares)
        s.b(0x0F); s.b(0x29); s.b(0x16);                   // movaps [rsi],xmm2
        s.b(0x66); s.b(0x0F); s.b(0x38); s.b(0x17); s.b(0xC9); // ptest xmm1,xmm1
        s.b(0x41); s.b(0x0F); s.b(0x94); s.b(0xC0);        // sete r8b
        s.b(0x66); s.b(0x0F); s.b(0x6F); s.b(0x99); s.b(0x00); s.b(0x01); s.b(0x00); s.b(0x00); // movdqa xmm3,[rcx+0x100]
        s.b(0x66); s.b(0x0F); s.b(0x3A); s.b(0x08); s.b(0xDB); s.b(0x01); // roundps xmm3,xmm3,1(floor)
        s.b(0x0F); s.b(0x29); s.b(0x59); s.b(0x10);        // movaps [rcx+0x10],xmm3
        s.b(0x44); s.b(0x88); s.b(0x07);                   // mov [rdi],r8b
        s.b(0xC3);
        s.patch({srcv, o1, o0, maska, o2, dlo});
        const RunResult res = m.run(s.code);
        CHECK(res.status == ExecStatus::Returned);
        // pshufd 0x1B reverses [1,2,3,4] -> [4,3,2,1]
        CHECK(memread<uint32_t>(m.bus, o1 + 0) == 4);
        CHECK(memread<uint32_t>(m.bus, o1 + 12) == 1);
        // pshufb: dword0 bytes reversed (01 00 00 00 -> 00 00 00 01 = 0x01000000),
        // dword1 reversed likewise, lanes 2-3 zeroed.
        CHECK(memread<uint32_t>(m.bus, o0 + 0) == 0x01000000);
        CHECK(memread<uint32_t>(m.bus, o0 + 4) == 0x02000000);
        CHECK(memread<uint64_t>(m.bus, o0 + 8) == 0);
        // pmulld squares: [16,9,4,1]
        CHECK(memread<uint32_t>(m.bus, o2 + 0) == 16);
        CHECK(memread<uint32_t>(m.bus, o2 + 4) == 9);
        CHECK(memread<uint32_t>(m.bus, o2 + 12) == 1);
        CHECK(memread<uint8_t>(m.bus, dlo) == 0); // ptest nonzero -> ZF=0
        CHECK(memread<float>(m.bus, o1 + 0x10) == 4.0f);  // floor(4.75)
        CHECK(memread<float>(m.bus, o1 + 0x1C) == 1.0f);  // floor(1.5)
    }

    std::cout << "== Part D: AVX.128 VEX ==\n";
    {
        Machine m;
        const uint64_t srcv = kBase + 0x13000;
        const uint64_t o2 = kBase + 0x13100, o3 = kBase + 0x13120, dlo = kBase + 0x13160;
        const float src_f[4] = {1.5f, 2.5f, 3.5f, 4.5f};
        m.bus.Write(srcv, src_f, 16);

        Asm s;
        s.b(0x48); s.b(0xB8); s.imm64_slot();              // mov rax,<src>
        s.b(0x48); s.b(0xBF); s.imm64_slot();              // mov rdi,<o2>
        s.b(0x48); s.b(0xBE); s.imm64_slot();              // mov rsi,<o3>
        s.b(0x48); s.b(0xB9); s.imm64_slot();              // mov rcx,<dlo>
        s.b(0xC5); s.b(0xF9); s.b(0x6F); s.b(0x00);        // vmovdqa xmm0,[rax]
        s.b(0xC5); s.b(0xF8); s.b(0x58); s.b(0xC8);        // vaddps xmm1,xmm0,xmm0 (vvvv=xmm0)
        s.b(0xC5); s.b(0xF0); s.b(0x59); s.b(0xD0);        // vmulps xmm2,xmm1,xmm0 (vvvv=xmm1)
        s.b(0xC5); s.b(0xC8); s.b(0x57); s.b(0xDB);        // vxorps xmm3,xmm3,xmm3 (vvvv=3: v~bar=1100)
        s.b(0xC4); s.b(0xE2); s.b(0x79); s.b(0x17); s.b(0xC9);  // vptest xmm1,xmm1
        s.b(0x0F); s.b(0x94); s.b(0xC2);                   // sete dl
        s.b(0xC5); s.b(0xF9); s.b(0x7F); s.b(0x17);        // vmovdqa [rdi],xmm2
        s.b(0xC5); s.b(0xF9); s.b(0x7F); s.b(0x1E);        // vmovdqa [rsi],xmm3
        s.b(0x88); s.b(0x11);                              // mov [rcx],dl
        s.b(0xC5); s.b(0xF9); s.b(0x6F); s.b(0xE0);        // vmovdqa xmm4,xmm0
        s.b(0xC4); s.b(0xE2); s.b(0x79); s.b(0x00); s.b(0xE1); // vpshufb xmm4,xmm0,xmm1 (modrm=11_100_001)
        s.b(0xC5); s.b(0xF9); s.b(0x7F); s.b(0x66); s.b(0x10); // vmovdqa [rsi+0x10],xmm4
        s.b(0xC5); s.b(0xF4); s.b(0x58); s.b(0xC2);        // vaddps ymm0,ymm1,ymm2 (AVX.256!)
        s.b(0xC5); s.b(0xFD); s.b(0x7F); s.b(0x47); s.b(0x40); // vmovdqu [rdi+0x40],ymm0
        s.b(0xC3);
        s.patch({srcv, o2, o3, dlo});
        const RunResult res = m.run(s.code);
        // Round 18: AVX.256 no longer fails closed -- the 256-bit register
        // file (xmm + ymm_hi) executes it.
        CHECK(res.status != ExecStatus::UnsupportedOpcode);
        // Everything before it ran: xmm2 = (2*src)*src = 2*src^2 (all 4 lanes)
        uint64_t v2bits[2] = {0, 0};
        m.bus.Read(o2, v2bits, 16);
        const float* v2 = reinterpret_cast<const float*>(v2bits);
        CHECK(v2[0] == 2 * 1.5f * 1.5f);
        CHECK(v2[3] == 2 * 4.5f * 4.5f);
        // ymm0 = ymm1 + ymm2 across all 8 lanes; the upper halves were
        // zeroed by the earlier VEX.128 writes (the AVX.128 register-write
        // rule -- round 18 models it).
        float y0[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        m.bus.Read(o2 + 0x40, y0, 32);
        CHECK(y0[0] == 7.5f);
        CHECK(y0[1] == 17.5f);
        CHECK(y0[2] == 31.5f);
        CHECK(y0[3] == 49.5f);
        CHECK(y0[4] == 0.0f && y0[7] == 0.0f);
        CHECK(memread<uint64_t>(m.bus, o3) == 0);   // vxorps zeroing idiom
        CHECK(memread<uint8_t>(m.bus, dlo) == 0);   // vptest(xmm1!=0) -> ZF=0
        // vpshufb xmm4 = xmm0 bytes shuffled by xmm1 (the doubled floats).
        // xmm1 = 2*src = {3,5,7,9} -> bytes lane0: 00 00 40 40 -> low bit clear:
        // every shuffled byte takes src byte index (mask&0xF) since bit7=0.
        const uint64_t v4 = memread<uint64_t>(m.bus, o3 + 0x10);
        const uint8_t* v4b = reinterpret_cast<const uint8_t*>(&v4);
        // xmm0 bytes: 01|00|00|00 (1.5f = 0x3FC00000 LE: 00 00 C0 3F)...
        // lane0 of 1.5f: bytes 00 00 C0 3F; shuffled by xmm1 lane0 bytes
        // (3.0f = 00 00 40 40): out[0]=src[0], out[1]=src[0], out[2]=src[0x40&0xF=0],
        // out[3]=src[0x40&0xF=0] -> 00 00 00 00
        CHECK(v4b[0] == 0x00 && v4b[1] == 0x00 && v4b[2] == 0x00 && v4b[3] == 0x00);
    }

    std::cout << "== Part E: full-decoder block scanner ==\n";
    {
        std::vector<uint8_t> blk1 = {0xB8, 1, 0, 0, 0, 0x83, 0xC0, 0x02, 0xC3};
        auto b = X86Interpreter::InspectBlock(blk1, 0x1000);
        CHECK(b.status == ExecStatus::Running);
        CHECK(b.ends_in_ret);
        CHECK(!b.ends_in_branch);
        CHECK(b.instruction_count == 3);
        CHECK(b.code_size == blk1.size());

        std::vector<uint8_t> blk2 = {0x74, 0x10};
        auto b2 = X86Interpreter::InspectBlock(blk2, 0x1000);
        CHECK(b2.status == ExecStatus::Running);
        CHECK(b2.ends_in_branch);
        CHECK(b2.instruction_count == 1);

        std::vector<uint8_t> blk3 = {0xB8, 1, 0, 0, 0, 0xE8, 0, 0, 0, 0};
        auto b3 = X86Interpreter::InspectBlock(blk3, 0x1000);
        CHECK(b3.status == ExecStatus::Running);
        CHECK(b3.ends_in_call);
        CHECK(b3.instruction_count == 2);

        std::vector<uint8_t> blk4 = {0x0F, 0x05};
        auto b4 = X86Interpreter::InspectBlock(blk4, 0x1000);
        CHECK(b4.status == ExecStatus::Running);
        CHECK(b4.contains_syscall);
        CHECK(b4.ends_in_branch);

        // Round-16 instructions must scan to their terminator.
        std::vector<uint8_t> blk5 = {
            0xC0, 0xC0, 0x04,             // rol al,4
            0xF3, 0x0F, 0xB8, 0xC8,       // popcnt ecx,eax
            0xF3, 0xA4,                   // rep movsb
            0xC5, 0xF0, 0x58, 0xC8,       // vaddps xmm1,xmm0,xmm0
            0xC3};
        auto b5 = X86Interpreter::InspectBlock(blk5, 0x1000);
        CHECK(b5.status == ExecStatus::Running);
        CHECK(b5.ends_in_ret);
        CHECK(b5.instruction_count == 5);
        CHECK(b5.code_size == blk5.size());

        // Real compiler-style blocks (SIB memory + SSE + CL shift).
        std::vector<uint8_t> blk6 = {
            0x48, 0x8B, 0x04, 0x25, 0, 0, 0, 0,  // mov rax, [imm32 sign-ext]
            0x0F, 0x28, 0xC1,                    // movaps xmm0,xmm1
            0xD3, 0xE0,                          // shl eax, cl
            0xEB, 0x00};                         // jmp +0
        auto b6 = X86Interpreter::InspectBlock(blk6, 0x1000);
        CHECK(b6.status == ExecStatus::Running);
        CHECK(b6.ends_in_branch);
        CHECK(b6.instruction_count == 4);
    }

    std::cout << "cpu_full_isa_test: " << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
