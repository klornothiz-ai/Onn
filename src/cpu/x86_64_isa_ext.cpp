// ============================================================================
// ProsperoLayer RDNA2 Core - x86-64 ISA extension (round 16)
// ----------------------------------------------------------------------------
// New instruction families executed by the SAME interpreter core:
//   * ExecBitOp  : bt/bts/btr/btc (reg + imm8), bsf/bsr (and the F3-prefixed
//                  tzcnt/lzcnt), popcnt, cmpxchg, xadd
//   * ExecStringOp: movs/stos/lods/scas/cmps with REP/REPE/REPNE + DF
//   * ExecSse3   : 0F 38 / 0F 3A -- SSSE3/SSE4.1 integer SIMD (pshufb,
//                  phaddw/d, pabs*, ptest, pmulld, pcmpeqq, packusdw,
//                  palignr, roundps/pd/ss/sd, pextrd/q, pinsrd/q)
//   * ExecVex    : AVX.128 VEX-decoded forms (C5 2-byte, C4 3-byte) of the
//                  modelled SSE ops -- 3-operand non-destructive semantics.
//                  The VEX bit layout was verified EMPIRICALLY against the
//                  host assembler (gas) encodings:
//                    C5 [R~ v3~ v2~ v1~ v0~ L pp]        (0F map, W=0 implied)
//                    C4 [R~ X~ B~ m4..m0][W~ v3~..v0~ L pp]
//                  AVX.256 (L=1) fails closed: the register file is 128-bit.
//   * InspectBlock: full-decoder basic-block scanner (ret/branch/call/syscall
//                  terminators) replacing the subset scanner in the JIT engine.
// ============================================================================
#include "cpu/x86_64_interpreter.hpp"

#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

namespace PS5::CPU {

// Static mutex serializing all LOCK-prefixed atomic operations (cmpxchg, xadd)
// across guest threads running on parallel host threads. Without this, the
// read-compare-write sequence in the interpreter could be interleaved by
// another guest thread, breaking atomicity guarantees.
static std::mutex s_atomic_mutex;

// ---------------------------------------------------------------------------
// Round 20: the virtual Zen 2 host models (shared by the interpreter and the
// direct execution backend -- see HostModels in the header).
// ---------------------------------------------------------------------------
namespace HostModels {

uint64_t ReadVirtualTsc() {
    // PS5 CPU cores are fixed at 3.5 GHz: ticks = ns * 3.5 = ns * 7 / 2.
    // steady_clock is monotonic and never frequency-scaled, matching an
    // invariant TSC.
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const uint64_t ns =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    return (ns * 7ull) / 2ull;
}

void CpuidModel(uint32_t leaf, uint32_t subleaf,
                uint32_t& eax, uint32_t& ebx, uint32_t& ecx, uint32_t& edx) {
    (void)subleaf;
    eax = ebx = ecx = edx = 0;
    switch (leaf) {
        case 0x00000000:
            eax = 0x00000007;                    // highest standard leaf we model
            ebx = 0x68747541;                    // "Auth"
            edx = 0x69746E65;                    // "enti"
            ecx = 0x444D4163;                    // "cAMD"
            return;
        case 0x00000001:
            // Ryzen 9 3900X: family 0x17, model 0x71, stepping 0.
            eax = 0x00870F10u;
            // EDX: FPU(0), PSE(3), TSC(4), MSR(5), PAE(6), CX8(8), CMOV(15),
            //      PAT(16), PSE36(17), CLFLUSH(19), MMX(23), FXSR(24), SSE(25), SSE2(26)
            edx = (1u<<0)|(1u<<3)|(1u<<4)|(1u<<5)|(1u<<8)|(1u<<15)|(1u<<16)|
                  (1u<<19)|(1u<<23)|(1u<<24)|(1u<<25)|(1u<<26);
            // ECX: SSE3(0), SSSE3(9), SSE4.1(19), SSE4.2(20), POPCNT(23),
            //      AVX(28), RDRAND stays 0 (we do not model it).
            ecx = (1u<<0)|(1u<<9)|(1u<<19)|(1u<<20)|(1u<<23)|(1u<<28);
            return;
        case 0x00000007:
            // Structured feature flags: only sub-leaf 0, and only what we can
            // serve on EVERY host: BMI1(3), AVX2(5), BMI2(8), MOVBE(22).
            if (subleaf == 0) {
                eax = 0;
                ebx = (1u<<3)|(1u<<5)|(1u<<8)|(1u<<22);
            }
            return;
        case 0x80000000:
            eax = 0x80000008;                    // highest extended leaf we model
            return;
        case 0x80000001:
            eax = 0x00870F10u;                   // same family/model as leaf 1
            // EDX: SYSCALL(11), NX(20), LM(29).
            edx = (1u<<11)|(1u<<20)|(1u<<29);
            // ECX: LZCNT/ABM(5), SSE4A(6) -- both emulated on every host.
            ecx = (1u<<5)|(1u<<6);
            return;
        case 0x80000002: {
            // Brand string bytes 0..15: "AMD Ryzen 9 390"
            const char* s = "AMD Ryzen 9 3900X 12-Core Processor            ";
            std::memcpy(&eax, s + 0, 4);
            std::memcpy(&ebx, s + 4, 4);
            std::memcpy(&ecx, s + 8, 4);
            std::memcpy(&edx, s + 12, 4);
            return;
        }
        case 0x80000003: {
            const char* s = "AMD Ryzen 9 3900X 12-Core Processor            ";
            std::memcpy(&eax, s + 16, 4);
            std::memcpy(&ebx, s + 20, 4);
            std::memcpy(&ecx, s + 24, 4);
            std::memcpy(&edx, s + 28, 4);
            return;
        }
        case 0x80000004: {
            const char* s = "AMD Ryzen 9 3900X 12-Core Processor            ";
            std::memcpy(&eax, s + 32, 4);
            std::memcpy(&ebx, s + 36, 4);
            std::memcpy(&ecx, s + 40, 4);
            std::memcpy(&edx, s + 44, 4);
            return;
        }
        case 0x80000007:
            edx = (1u<<8);                       // invariant TSC
            return;
        case 0x80000008:
            eax = 0x30;                          // 48-bit virtual addresses
            return;
        default:
            return;                              // everything else: zeros
    }
}

} // namespace HostModels

// ---------------------------------------------------------------------------
// Round 20: AMD SSE4a. Bit-level semantics verified against QEMU
// (target/i386/ops_sse.h: helper_extrq / helper_insertq):
//   extrq(src, shift, len)  = (src >> shift) & mask(len)          -- low qword
//   insertq(dst, src, shift, len):
//       mask = len ? (1<<len)-1 : ~0
//       result = (dst & ~(mask << shift)) | ((src & mask) << shift)  -- low qword
// Only the destination's LOW 64 bits change (upper qword untouched);
// len==0 selects a full 64-bit mask; len/idx are taken mod 64.
// ---------------------------------------------------------------------------
namespace {

inline uint64_t Sse4aMask(uint32_t len) {
    // len is already reduced mod 64; len==0 means "all 64 bits".
    return len == 0 ? ~0ull : ((1ull << len) - 1ull);
}

} // namespace

bool X86Interpreter::ExecSse4a(uint64_t& rip, const Prefixes& p, uint8_t op2,
                               RunResult& r) {
    auto fault = [&](ExecStatus s, uint64_t addr) { r.status = s; r.fault_addr = addr; };

    const bool p66 = p.opsize;        // 66-prefixed (EXTRQ family)
    const bool pf2 = p.rep == 0xF2;   // F2-prefixed (INSERTQ / MOVNTSD)
    const bool pf3 = p.rep == 0xF3;   // F3-prefixed (MOVNTSS)

    // MOVNTSS/MOVNTSD/movntps/movntpd: 0F 2B /r (memory destination only).
    if (op2 == 0x2B) {
        if (p.rep == 0 && !p.opsize) {
            // movntps m128, xmm: plain 16-byte store.
            uint8_t modrm = 0;
            if (!Fetch8(rip, modrm)) { fault(ExecStatus::DecodeFault, rip); return true; }
            ModRMOperand rm{}; uint8_t reg = 0;
            if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
            if (rm.is_reg) { fault(ExecStatus::UnsupportedOpcode, rip); return true; }
            if (!WriteXmmOperand(rm, p, 16, m_state.xmm[reg])) {
                fault(ExecStatus::MemoryFault, rm.addr); return true;
            }
            return true;
        }
        if (p.opsize && p.rep == 0) {
            // movntpd m128, xmm: plain 16-byte store (same as movntps here).
            uint8_t modrm = 0;
            if (!Fetch8(rip, modrm)) { fault(ExecStatus::DecodeFault, rip); return true; }
            ModRMOperand rm{}; uint8_t reg = 0;
            if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
            if (rm.is_reg) { fault(ExecStatus::UnsupportedOpcode, rip); return true; }
            if (!WriteXmmOperand(rm, p, 16, m_state.xmm[reg])) {
                fault(ExecStatus::MemoryFault, rm.addr); return true;
            }
            return true;
        }
        if (pf3 || pf2) {
            // movntss (4 bytes) / movntsd (8 bytes): the non-temporal hint has
            // no architectural effect -- a plain scalar store.
            const int width = pf3 ? 4 : 8;
            uint8_t modrm = 0;
            if (!Fetch8(rip, modrm)) { fault(ExecStatus::DecodeFault, rip); return true; }
            ModRMOperand rm{}; uint8_t reg = 0;
            if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
            if (rm.is_reg) { fault(ExecStatus::UnsupportedOpcode, rip); return true; }
            if (!m_mem.Write(rm.addr, &m_state.xmm[reg], width)) {
                fault(ExecStatus::MemoryFault, rm.addr); return true;
            }
            return true;
        }
        return false;
    }

    // EXTRQ / INSERTQ: 0F 78 (+2 imm8) and 0F 79 (register form).
    if (op2 != 0x78 && op2 != 0x79) {
        return false;
    }
    const bool is_extrq = p66;       // 66-prefixed: EXTRQ
    const bool is_insertq = pf2;     // F2-prefixed: INSERTQ
    if (!is_extrq && !is_insertq) {
        return false;                // other prefixes: not SSE4a
    }

    uint8_t modrm = 0;
    if (!Fetch8(rip, modrm)) { fault(ExecStatus::DecodeFault, rip); return true; }
    ModRMOperand rm{}; uint8_t reg = 0;
    if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
    if (!rm.is_reg) { fault(ExecStatus::UnsupportedOpcode, rip); return true; }

    const uint8_t dst_i = static_cast<uint8_t>(rm.reg | (p.rex_b ? 0x08 : 0x00));
    CpuState::XmmReg& dst = m_state.xmm[dst_i];

    if (op2 == 0x78) {
        // Immediate form: EXTRQ xmm, len, idx / INSERTQ xmm1, xmm2, len, idx.
        // (QEMU emit.c.inc: length = imm1 & 63, index = imm2 & 63. QEMU's
        // decode_0F78 additionally requires the ModRM reg field == 0 for the
        // 66-prefixed EXTRQ form.)
        if (is_extrq && reg != 0) {
            fault(ExecStatus::UnsupportedOpcode, rip);
            return true;
        }
        uint8_t imm_len = 0, imm_idx = 0;
        if (!Fetch8(rip, imm_len)) { fault(ExecStatus::DecodeFault, rip); return true; }
        if (!Fetch8(rip, imm_idx)) { fault(ExecStatus::DecodeFault, rip); return true; }
        const uint32_t len = imm_len & 63u;
        const uint32_t idx = imm_idx & 63u;
        if (is_extrq) {
            dst.lo = (dst.lo >> idx) & Sse4aMask(len);
        } else {
            const uint8_t src_i = static_cast<uint8_t>(reg | (p.rex_r ? 0x08 : 0x00));
            const uint64_t src = m_state.xmm[src_i].lo;
            const uint64_t mask = Sse4aMask(len);
            dst.lo = (dst.lo & ~(mask << idx)) | ((src & mask) << idx);
        }
        return true;
    }

    // Register form (0F 79): the len/idx come from the SECOND source.
    const uint8_t src_i = static_cast<uint8_t>(reg | (p.rex_r ? 0x08 : 0x00));
    const CpuState::XmmReg& src = m_state.xmm[src_i];
    const auto byte_of = [](const CpuState::XmmReg& v, int b) -> uint32_t {
        const auto* bytes = reinterpret_cast<const uint8_t*>(&v);
        return bytes[b] & 63u;
    };
    if (is_extrq) {
        // QEMU helper_extrq_r: shift = src.B1 & 63, len = src.B0 & 63.
        const uint32_t shift = byte_of(src, 1);
        const uint32_t len = byte_of(src, 0);
        dst.lo = (dst.lo >> shift) & Sse4aMask(len);
    } else {
        // QEMU helper_insertq_r: shift = src.B9 & 63, len = src.B8 & 63.
        const uint32_t shift = byte_of(src, 9);
        const uint32_t len = byte_of(src, 8);
        const uint64_t mask = Sse4aMask(len);
        dst.lo = (dst.lo & ~(mask << shift)) | ((src.lo & mask) << shift);
    }
    return true;
}

// ---------------------------------------------------------------------------
// bt/bts/btr/btc + bsf/bsr/popcnt/tzcnt + cmpxchg + xadd
// ---------------------------------------------------------------------------
bool X86Interpreter::ExecBitOp(uint64_t& rip, const Prefixes& p, uint8_t op2,
                               RunResult& r) {
    auto fault = [&](ExecStatus s, uint64_t addr) { r.status = s; r.fault_addr = addr; };
    const int osz = OperandSize(p);

    // bt/bts/btr/btc r/m, reg (A3/AB/B3/BB) and bt group 0F BA /4../7
    const bool is_bt_reg  = (op2 == 0xA3 || op2 == 0xAB || op2 == 0xB3 || op2 == 0xBB);
    const bool is_bt_imm  = (op2 == 0xBA);
    if (is_bt_reg || is_bt_imm) {
        uint8_t modrm = 0;
        if (!Fetch8(rip, modrm)) { fault(ExecStatus::DecodeFault, rip); return true; }
        ModRMOperand rm{}; uint8_t reg = 0;
        if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
        uint8_t sub = (modrm >> 3) & 0x07;
        uint64_t bitpos = 0;
        if (is_bt_imm) {
            if (sub < 4) return false; // /0../3 are not BT ops here
            uint8_t imm = 0;
            if (!Fetch8(rip, imm)) { fault(ExecStatus::DecodeFault, rip); return true; }
            bitpos = imm;
            sub -= 4; // 4..7 -> bt/bts/btr/btc
        } else {
            const uint64_t regv = (osz == 1) ? ReadRegByte(reg, p.rex) : ReadReg(reg, osz);
            bitpos = regv;
        }
        const int size = (is_bt_reg && rm.is_reg && osz == 8) ? 8 : (rm.is_reg ? osz : 4);
        // For the memory form the bit offset is signed-relative; register form
        // takes bitpos modulo the operand width.
        uint64_t eff_addr = rm.addr;
        uint64_t bit_in_word = bitpos;
        if (!rm.is_reg) {
            // [rm + (bitpos / width) * (width/8)] with signed bitpos
            const int64_t sp = static_cast<int64_t>(static_cast<uint64_t>(bitpos));
            const int64_t word = sp / (size * 8);
            bit_in_word = static_cast<uint64_t>(sp % (size * 8));
            eff_addr = rm.addr + static_cast<uint64_t>(word * (size / 8));
        } else {
            bit_in_word %= (size * 8);
        }
        ModRMOperand target = rm;
        target.addr = eff_addr;
        uint64_t val = 0;
        if (!ReadRM(target, size, p.rex, val)) { fault(ExecStatus::MemoryFault, eff_addr); return true; }
        const uint64_t old = (val >> bit_in_word) & 1;
        SetFlag(Flags::CF, old != 0);
        uint64_t res = val;
        switch (is_bt_imm ? sub : (op2 == 0xA3 ? 0 : op2 == 0xAB ? 1 : op2 == 0xB3 ? 2 : 3)) {
            case 0: break;                                        // bt
            case 1: res = val | (1ull << bit_in_word); break;     // bts
            case 2: res = val & ~(1ull << bit_in_word); break;    // btr
            case 3: res = val ^ (1ull << bit_in_word); break;     // btc
            default: return false;
        }
        if (res != val) {
            if (!WriteRM(target, size, p.rex, res)) { fault(ExecStatus::MemoryFault, eff_addr); return true; }
        }
        return true;
    }

    // bsf/bsr (BC/BD), tzcnt/lzcnt (F3 + BC/BD), popcnt (F3 + B8)
    if (op2 == 0xBC || op2 == 0xBD || (op2 == 0xB8 && p.rep == 0xF3)) {
        uint8_t modrm = 0;
        if (!Fetch8(rip, modrm)) { fault(ExecStatus::DecodeFault, rip); return true; }
        ModRMOperand rm{}; uint8_t reg = 0;
        if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
        uint64_t src = 0;
        if (!ReadRM(rm, osz, p.rex, src)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        const int bits = osz * 8;
        const bool is_popcnt = (op2 == 0xB8);
        uint64_t result = 0;
        if (is_popcnt) {
            uint64_t v = src & Mask(osz);
            while (v) { v &= v - 1; ++result; }
            SetFlag(Flags::ZF, result == 0);
            SetFlag(Flags::CF, false); SetFlag(Flags::OF, false); SetFlag(Flags::SF, false);
        } else if (op2 == 0xBC) { // bsf / tzcnt
            const uint64_t v = src & Mask(osz);
            if (v == 0) {
                if (p.rep == 0xF3) { result = static_cast<uint64_t>(bits); } // tzcnt
                else { SetFlag(Flags::ZF, true); return true; }              // bsf: dst undef
            } else {
                result = 0;
                while (((v >> result) & 1) == 0) ++result;
                SetFlag(Flags::ZF, false);
            }
            if (p.rep == 0xF3) { SetFlag(Flags::ZF, (src & Mask(osz)) == 0); }
        } else { // 0xBD: bsr / lzcnt
            const uint64_t v = src & Mask(osz);
            if (v == 0) {
                if (p.rep == 0xF3) { result = static_cast<uint64_t>(bits); } // lzcnt
                else { SetFlag(Flags::ZF, true); return true; }
            } else {
                result = bits - 1;
                while (((v >> result) & 1) == 0) --result;
                SetFlag(Flags::ZF, false);
            }
            if (p.rep == 0xF3) { SetFlag(Flags::ZF, (src & Mask(osz)) == 0); }
        }
        WriteReg(reg, osz, result & Mask(osz));
        return true;
    }

    // cmpxchg (B0/B1) — when the LOCK prefix is present, the read-compare-write
    // sequence must be atomic across guest threads running on parallel host
    // threads. A static mutex serializes all locked cmpxchg/xadd operations,
    // ensuring the compare-and-swap cannot be interleaved. Without LOCK, the
    // operation is still single-stepped (non-atomic, matching hardware semantics
    // for unlocked cmpxchg).
    if (op2 == 0xB0 || op2 == 0xB1) {
        const int size = (op2 == 0xB0) ? 1 : osz;
        uint8_t modrm = 0;
        if (!Fetch8(rip, modrm)) { fault(ExecStatus::DecodeFault, rip); return true; }
        ModRMOperand rm{}; uint8_t reg = 0;
        if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
        // Serialize the entire read-compare-write under LOCK for atomicity.
        std::unique_lock<std::mutex> lk(s_atomic_mutex, std::defer_lock);
        if (p.lock) lk.lock();
        uint64_t acc = (size == 1) ? ReadRegByte(RAX, p.rex) : ReadReg(RAX, size);
        uint64_t memv = 0;
        if (!ReadRM(rm, size, p.rex, memv)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        const uint64_t src = (size == 1) ? ReadRegByte(reg, p.rex) : ReadReg(reg, size);
        if (acc == memv) {
            if (!WriteRM(rm, size, p.rex, src)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
            SetFlag(Flags::ZF, true);
        } else {
            if (size == 1) WriteRegByte(RAX, p.rex, static_cast<uint8_t>(memv));
            else WriteReg(RAX, size, memv);
            SetFlag(Flags::ZF, false);
        }
        return true;
    }

    // xadd (C0/C1) — same atomicity treatment as cmpxchg under LOCK.
    if (op2 == 0xC0 || op2 == 0xC1) {
        const int size = (op2 == 0xC0) ? 1 : osz;
        uint8_t modrm = 0;
        if (!Fetch8(rip, modrm)) { fault(ExecStatus::DecodeFault, rip); return true; }
        ModRMOperand rm{}; uint8_t reg = 0;
        if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
        std::unique_lock<std::mutex> lk(s_atomic_mutex, std::defer_lock);
        if (p.lock) lk.lock();
        uint64_t memv = 0;
        if (!ReadRM(rm, size, p.rex, memv)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        const uint64_t rv = (size == 1) ? ReadRegByte(reg, p.rex) : ReadReg(reg, size);
        const uint64_t sum = memv + rv;
        if (size == 1) WriteRegByte(reg, p.rex, static_cast<uint8_t>(memv));
        else WriteReg(reg, size, memv);
        if (!WriteRM(rm, size, p.rex, sum)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        UpdateFlagsAdd(memv, rv, sum, size, false, false);
        return true;
    }

    return false; // not one of ours
}

// ---------------------------------------------------------------------------
// string ops + REP/REPE/REPNE
// ---------------------------------------------------------------------------
bool X86Interpreter::ExecStringOp(uint64_t& rip, const Prefixes& p, uint8_t opcode,
                                  RunResult& r) {
    (void)rip; // string ops carry no immediate bytes; rip is already final
    auto fault = [&](ExecStatus s, uint64_t addr) { r.status = s; r.fault_addr = addr; };
    const int osz = OperandSize(p);
    const int esz = (opcode & 1) ? osz : 1;
    const int64_t step = GetFlag(Flags::DF) ? -esz : esz;

    const bool is_cmp = (opcode == 0xA6 || opcode == 0xA7 || opcode == 0xAE || opcode == 0xAF);
    const bool use_rep = p.rep != 0;
    // REP cap per single step: an interpreter "instruction" may loop, but the
    // loop is bounded so a guest bug cannot hang the host thread.
    constexpr uint64_t kMaxRepIters = 16 * 1024 * 1024;
    uint64_t iters = 1;
    if (use_rep) {
        iters = m_state.gpr[RCX] & Mask(osz);
        if (iters == 0) return true;
        if (iters > kMaxRepIters) iters = kMaxRepIters;
    }

    for (uint64_t n = 0; n < iters; ++n) {
        switch (opcode) {
            case 0xA4: case 0xA5: { // movs [rdi], [rsi]
                uint8_t buf[8];
                if (!m_mem.Read(m_state.gpr[RSI], buf, esz)) { fault(ExecStatus::MemoryFault, m_state.gpr[RSI]); return false; }
                if (!m_mem.Write(m_state.gpr[RDI], buf, esz)) { fault(ExecStatus::MemoryFault, m_state.gpr[RDI]); return false; }
                m_state.gpr[RSI] = (m_state.gpr[RSI] + step) & Mask(8);
                m_state.gpr[RDI] = (m_state.gpr[RDI] + step) & Mask(8);
                break;
            }
            case 0xA6: case 0xA7: { // cmps [rsi], [rdi]
                uint64_t a = 0, b = 0;
                if (!m_mem.Read(m_state.gpr[RSI], &a, esz)) { fault(ExecStatus::MemoryFault, m_state.gpr[RSI]); return false; }
                if (!m_mem.Read(m_state.gpr[RDI], &b, esz)) { fault(ExecStatus::MemoryFault, m_state.gpr[RDI]); return false; }
                const uint64_t res = (a & Mask(esz)) - (b & Mask(esz));
                UpdateFlagsSub(a & Mask(esz), b & Mask(esz), res & Mask(esz), esz, false, false);
                m_state.gpr[RSI] = (m_state.gpr[RSI] + step) & Mask(8);
                m_state.gpr[RDI] = (m_state.gpr[RDI] + step) & Mask(8);
                break;
            }
            case 0xAA: case 0xAB: { // stos [rdi], al/ax/eax/rax
                const uint64_t v = ReadReg(RAX, esz) & Mask(esz);
                if (!m_mem.Write(m_state.gpr[RDI], &v, esz)) { fault(ExecStatus::MemoryFault, m_state.gpr[RDI]); return false; }
                m_state.gpr[RDI] = (m_state.gpr[RDI] + step) & Mask(8);
                break;
            }
            case 0xAC: case 0xAD: { // lods al/ax/eax/rax, [rsi]
                uint64_t v = 0;
                if (!m_mem.Read(m_state.gpr[RSI], &v, esz)) { fault(ExecStatus::MemoryFault, m_state.gpr[RSI]); return false; }
                WriteReg(RAX, esz, v & Mask(esz));
                m_state.gpr[RSI] = (m_state.gpr[RSI] + step) & Mask(8);
                break;
            }
            case 0xAE: case 0xAF: { // scas al/ax/eax/rax, [rdi]
                const uint64_t a = ReadReg(RAX, esz) & Mask(esz);
                uint64_t b = 0;
                if (!m_mem.Read(m_state.gpr[RDI], &b, esz)) { fault(ExecStatus::MemoryFault, m_state.gpr[RDI]); return false; }
                const uint64_t res = a - (b & Mask(esz));
                UpdateFlagsSub(a, b & Mask(esz), res & Mask(esz), esz, false, false);
                m_state.gpr[RDI] = (m_state.gpr[RDI] + step) & Mask(8);
                break;
            }
            default:
                return false;
        }
        if (use_rep) {
            m_state.gpr[RCX] = (m_state.gpr[RCX] - 1) & Mask(osz);
            if (m_state.gpr[RCX] == 0) break;
            if (is_cmp) {
                // REPE: continue while ZF=1; REPNE: continue while ZF=0.
                const bool zf = GetFlag(Flags::ZF);
                if ((p.rep == 0xF3 && !zf) || (p.rep == 0xF2 && zf)) break;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 0F 38 / 0F 3A: SSSE3 / SSE4.1
// ---------------------------------------------------------------------------
bool X86Interpreter::ExecSse3(uint64_t& rip, const Prefixes& p, bool has_3a,
                              uint8_t op3, RunResult& r) {
    auto fault = [&](ExecStatus s, uint64_t addr) { r.status = s; r.fault_addr = addr; };
    auto rd_xmm = [&](const ModRMOperand& op, CpuState::XmmReg& out) {
        return ReadXmmOperand(op, p, 16, out);
    };
    if (!has_3a) {
        // Round 20: MOVBE (0F38 F0/F1 without F2) -- byte-swapped load/store.
        if ((op3 == 0xF0 || op3 == 0xF1) && p.rep != 0xF2) {
            const int osz = OperandSize(p);
            uint8_t modrm = 0;
            if (!Fetch8(rip, modrm)) { fault(ExecStatus::DecodeFault, rip); return true; }
            ModRMOperand rm{}; uint8_t reg = 0;
            if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
            if (rm.is_reg) { fault(ExecStatus::UnsupportedOpcode, rip); return true; }
            uint64_t raw = 0;
            if (!ReadRM(rm, osz, p.rex, raw)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
            // bswap the loaded/stored value one byte at a time.
            const auto bswap = [&](uint64_t v) {
                uint64_t out = 0;
                for (int i = 0; i < osz; ++i) {
                    out = (out << 8) | ((v >> (i * 8)) & 0xFFull);
                }
                return out;
            };
            if (op3 == 0xF0) {          // movbe r, m: load + swap
                WriteReg(reg, osz, bswap(raw));
            } else {                    // movbe m, r: swap + store
                if (!WriteRM(rm, osz, p.rex, bswap(ReadReg(reg, osz)))) {
                    fault(ExecStatus::MemoryFault, rm.addr); return true;
                }
            }
            return true;
        }
        // Round 20: CRC32 (F2 0F38 F0/F1) -- SSE4.2, the Castagnoli polynomial
        // 0x11EDC6F41 reflected as 0x82F63B78, bit-reflected input/output
        // (the standard crc32c software form). Width: 64-bit with REX.W,
        // 16-bit with 66, otherwise 32-bit.
        if ((op3 == 0xF0 || op3 == 0xF1) && p.rep == 0xF2) {
            uint8_t modrm = 0;
            if (!Fetch8(rip, modrm)) { fault(ExecStatus::DecodeFault, rip); return true; }
            ModRMOperand rm{}; uint8_t reg = 0;
            if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
            const int width = p.rex_w ? 8 : (p.opsize ? 2 : 4);
            uint64_t src = 0;
            if (!ReadRM(rm, width, p.rex, src)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
            uint32_t crc = static_cast<uint32_t>(ReadReg(reg, 4));
            for (int i = 0; i < width; ++i) {
                const uint32_t byte = (src >> (i * 8)) & 0xFFull;
                crc ^= byte;
                for (int k = 0; k < 8; ++k) {
                    crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u)));
                }
            }
            WriteReg(reg, 4, crc);
            return true;
        }
        switch (op3) {
            case 0x00: { // pshufb xmm, r/m (SSSE3): dst bytes shuffled by r/m mask
                uint8_t modrm = 0; if (!Fetch8(rip, modrm)) { fault(ExecStatus::DecodeFault, rip); return true; }
                ModRMOperand rm{}; uint8_t reg = 0;
                if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
                CpuState::XmmReg mask{}; if (!rd_xmm(rm, mask)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                const CpuState::XmmReg data = m_state.xmm[reg];
                CpuState::XmmReg out{};
                const uint8_t* s = reinterpret_cast<const uint8_t*>(&data);
                const uint8_t* m = reinterpret_cast<const uint8_t*>(&mask);
                uint8_t* d = reinterpret_cast<uint8_t*>(&out);
                for (int i = 0; i < 16; ++i) {
                    d[i] = (m[i] & 0x80) ? 0 : s[m[i] & 0x0F];
                }
                m_state.xmm[reg] = out;
                return true;
            }
            case 0x01: { // phaddw (SSSE3)
                uint8_t modrm = 0; if (!Fetch8(rip, modrm)) { fault(ExecStatus::DecodeFault, rip); return true; }
                ModRMOperand rm{}; uint8_t reg = 0;
                if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
                CpuState::XmmReg src{}; if (!rd_xmm(rm, src)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                const CpuState::XmmReg& dst = m_state.xmm[reg];
                const int16_t* s = reinterpret_cast<const int16_t*>(&src);
                const int16_t* d = reinterpret_cast<const int16_t*>(&dst);
                CpuState::XmmReg out{};
                int16_t* o = reinterpret_cast<int16_t*>(&out);
                o[0] = static_cast<int16_t>(d[0] + d[1]); o[1] = static_cast<int16_t>(d[2] + d[3]);
                o[2] = static_cast<int16_t>(d[4] + d[5]); o[3] = static_cast<int16_t>(d[6] + d[7]);
                o[4] = static_cast<int16_t>(s[0] + s[1]); o[5] = static_cast<int16_t>(s[2] + s[3]);
                o[6] = static_cast<int16_t>(s[4] + s[5]); o[7] = static_cast<int16_t>(s[6] + s[7]);
                m_state.xmm[reg] = out;
                return true;
            }
            case 0x02: { // phaddd (SSSE3)
                uint8_t modrm = 0; if (!Fetch8(rip, modrm)) { fault(ExecStatus::DecodeFault, rip); return true; }
                ModRMOperand rm{}; uint8_t reg = 0;
                if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
                CpuState::XmmReg src{}; if (!rd_xmm(rm, src)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                const CpuState::XmmReg& dst = m_state.xmm[reg];
                const int32_t* s = reinterpret_cast<const int32_t*>(&src);
                const int32_t* d = reinterpret_cast<const int32_t*>(&dst);
                CpuState::XmmReg out{};
                int32_t* o = reinterpret_cast<int32_t*>(&out);
                o[0] = d[0] + d[1]; o[1] = d[2] + d[3];
                o[2] = s[0] + s[1]; o[3] = s[2] + s[3];
                m_state.xmm[reg] = out;
                return true;
            }
            case 0x17: { // ptest (SSE4.1): ZF = (dst & src)==0, CF = (dst & ~src)==0
                uint8_t modrm = 0; if (!Fetch8(rip, modrm)) { fault(ExecStatus::DecodeFault, rip); return true; }
                ModRMOperand rm{}; uint8_t reg = 0;
                if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
                CpuState::XmmReg src{}; if (!rd_xmm(rm, src)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                const CpuState::XmmReg& dst = m_state.xmm[reg];
                const uint64_t andv0 = dst.lo & src.lo, andv1 = dst.hi & src.hi;
                const uint64_t andnv0 = dst.lo & ~src.lo, andnv1 = dst.hi & ~src.hi;
                SetFlag(Flags::ZF, (andv0 | andv1) == 0);
                SetFlag(Flags::CF, (andnv0 | andnv1) == 0);
                return true;
            }
            case 0x1C: case 0x1D: case 0x1E: { // pabsb/pabsw/pabsd (SSSE3)
                uint8_t modrm = 0; if (!Fetch8(rip, modrm)) { fault(ExecStatus::DecodeFault, rip); return true; }
                ModRMOperand rm{}; uint8_t reg = 0;
                if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
                CpuState::XmmReg src{}; if (!rd_xmm(rm, src)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                CpuState::XmmReg out{};
                if (op3 == 0x1C) {
                    const int8_t* s = reinterpret_cast<const int8_t*>(&src);
                    uint8_t* o = reinterpret_cast<uint8_t*>(&out);
                    for (int i = 0; i < 16; ++i) o[i] = static_cast<uint8_t>(s[i] < 0 ? -s[i] : s[i]);
                } else if (op3 == 0x1D) {
                    const int16_t* s = reinterpret_cast<const int16_t*>(&src);
                    uint16_t* o = reinterpret_cast<uint16_t*>(&out);
                    for (int i = 0; i < 8; ++i) o[i] = static_cast<uint16_t>(s[i] < 0 ? -s[i] : s[i]);
                } else {
                    const int32_t* s = reinterpret_cast<const int32_t*>(&src);
                    uint32_t* o = reinterpret_cast<uint32_t*>(&out);
                    for (int i = 0; i < 4; ++i) o[i] = static_cast<uint32_t>(s[i] < 0 ? -s[i] : s[i]);
                }
                m_state.xmm[reg] = out;
                return true;
            }
            case 0x29: { // pcmpeqq (SSE4.1)
                uint8_t modrm = 0; if (!Fetch8(rip, modrm)) { fault(ExecStatus::DecodeFault, rip); return true; }
                ModRMOperand rm{}; uint8_t reg = 0;
                if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
                CpuState::XmmReg src{}; if (!rd_xmm(rm, src)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                CpuState::XmmReg& dst = m_state.xmm[reg];
                dst.lo = (dst.lo == src.lo) ? ~0ull : 0;
                dst.hi = (dst.hi == src.hi) ? ~0ull : 0;
                return true;
            }
            case 0x2B: { // packusdw (SSE4.1)
                uint8_t modrm = 0; if (!Fetch8(rip, modrm)) { fault(ExecStatus::DecodeFault, rip); return true; }
                ModRMOperand rm{}; uint8_t reg = 0;
                if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
                CpuState::XmmReg src{}; if (!rd_xmm(rm, src)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                const CpuState::XmmReg& dst = m_state.xmm[reg];
                const int32_t* d = reinterpret_cast<const int32_t*>(&dst);
                const int32_t* s = reinterpret_cast<const int32_t*>(&src);
                CpuState::XmmReg out{};
                uint16_t* o = reinterpret_cast<uint16_t*>(&out);
                auto sat = [](int32_t v) -> uint16_t {
                    if (v < 0) return 0;
                    if (v > 0xFFFF) return 0xFFFF;
                    return static_cast<uint16_t>(v);
                };
                for (int i = 0; i < 4; ++i) o[i] = sat(d[i]);
                for (int i = 0; i < 4; ++i) o[4 + i] = sat(s[i]);
                m_state.xmm[reg] = out;
                return true;
            }
            case 0x40: { // pmulld (SSE4.1)
                uint8_t modrm = 0; if (!Fetch8(rip, modrm)) { fault(ExecStatus::DecodeFault, rip); return true; }
                ModRMOperand rm{}; uint8_t reg = 0;
                if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
                CpuState::XmmReg src{}; if (!rd_xmm(rm, src)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                CpuState::XmmReg& dst = m_state.xmm[reg];
                const int32_t* d = reinterpret_cast<const int32_t*>(&dst);
                const int32_t* s = reinterpret_cast<const int32_t*>(&src);
                CpuState::XmmReg out{};
                int32_t* o = reinterpret_cast<int32_t*>(&out);
                for (int i = 0; i < 4; ++i) o[i] = d[i] * s[i];
                m_state.xmm[reg] = out;
                return true;
            }
            default:
                return false;
        }
    }

    // ---- 0F 3A ----
    switch (op3) {
        case 0x08: case 0x09: { // roundps / roundpd imm8 (SSE4.1)
            uint8_t modrm = 0; if (!Fetch8(rip, modrm)) { fault(ExecStatus::DecodeFault, rip); return true; }
            ModRMOperand rm{}; uint8_t reg = 0;
            if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
            uint8_t imm = 0; if (!Fetch8(rip, imm)) { fault(ExecStatus::DecodeFault, rip); return true; }
            CpuState::XmmReg src{}; if (!rd_xmm(rm, src)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
            CpuState::XmmReg out{};
            const int mode = imm & 0x3;
            for (int lane = 0; lane < 4; ++lane) {
                if (op3 == 0x08) { // ps: 4 floats
                    float f = XmmFloat(src, lane);
                    switch (mode) {
                        case 0: f = std::nearbyint(f); break;
                        case 1: f = std::floor(f); break;
                        case 2: f = std::ceil(f); break;
                        default: f = (f >= 0.0f) ? std::floor(f) : std::ceil(f); break; // trunc
                    }
                    SetXmmFloat(out, lane, f);
                } else { // pd: 2 doubles
                    double d = XmmDouble(src, lane);
                    switch (mode) {
                        case 0: d = std::nearbyint(d); break;
                        case 1: d = std::floor(d); break;
                        case 2: d = std::ceil(d); break;
                        default: d = std::trunc(d); break;
                    }
                    SetXmmDouble(out, lane, d);
                }
            }
            m_state.xmm[reg] = out;
            return true;
        }
        case 0x0F: { // palignr imm8 (SSSE3)
            uint8_t modrm = 0; if (!Fetch8(rip, modrm)) { fault(ExecStatus::DecodeFault, rip); return true; }
            ModRMOperand rm{}; uint8_t reg = 0;
            if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
            uint8_t imm = 0; if (!Fetch8(rip, imm)) { fault(ExecStatus::DecodeFault, rip); return true; }
            CpuState::XmmReg src{}; if (!rd_xmm(rm, src)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
            const CpuState::XmmReg& dst = m_state.xmm[reg];
            // concatenate dst:src (32 bytes), shift right by imm bytes, keep 16
            uint8_t cat[32];
            std::memcpy(cat, &src, 16);
            std::memcpy(cat + 16, &dst, 16);
            CpuState::XmmReg out{};
            uint8_t* o = reinterpret_cast<uint8_t*>(&out);
            if (imm <= 31) {
                std::memcpy(o, cat + imm, 16);
            } else {
                std::memset(o, 0, 16);
            }
            m_state.xmm[reg] = out;
            return true;
        }
        case 0x16: { // pextrd r/m32, xmm, imm8 (SSE4.1)
            uint8_t modrm = 0; if (!Fetch8(rip, modrm)) { fault(ExecStatus::DecodeFault, rip); return true; }
            ModRMOperand rm{}; uint8_t reg = 0;
            if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
            uint8_t imm = 0; if (!Fetch8(rip, imm)) { fault(ExecStatus::DecodeFault, rip); return true; }
            const uint32_t lane = (imm & 3);
            const CpuState::XmmReg& v = m_state.xmm[reg];
            const uint32_t word = static_cast<uint32_t>((lane < 2) ? (v.lo >> (32 * lane)) : (v.hi >> (32 * (lane - 2))));
            if (rm.is_reg) {
                WriteReg(rm.reg, 4, word);
            } else if (!WriteRM(rm, 4, p.rex, word)) {
                fault(ExecStatus::MemoryFault, rm.addr); return true;
            }
            return true;
        }
        case 0x21: { // insertps / pinsrd xmm, r/m32, imm8 (SSE4.1; only the
                     // pinsrd integer form is modelled here)
            if (p.rep != 0) return false; // F2 form is insertps -- unsupported
            uint8_t modrm = 0; if (!Fetch8(rip, modrm)) { fault(ExecStatus::DecodeFault, rip); return true; }
            ModRMOperand rm{}; uint8_t reg = 0;
            if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
            uint8_t imm = 0; if (!Fetch8(rip, imm)) { fault(ExecStatus::DecodeFault, rip); return true; }
            uint64_t v = 0;
            if (!ReadRM(rm, 4, p.rex, v)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
            const uint32_t lane = (imm & 3);
            CpuState::XmmReg& dst = m_state.xmm[reg];
            const uint32_t word = static_cast<uint32_t>(v);
            if (lane < 2) {
                dst.lo = (dst.lo & ~(0xFFFFFFFFull << (32 * lane))) | (static_cast<uint64_t>(word) << (32 * lane));
            } else {
                const uint32_t l = lane - 2;
                dst.hi = (dst.hi & ~(0xFFFFFFFFull << (32 * l))) | (static_cast<uint64_t>(word) << (32 * l));
            }
            return true;
        }
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// legacy-0F integer SIMD (round 16)
// ---------------------------------------------------------------------------
bool X86Interpreter::ExecSseInt(uint64_t& rip, const Prefixes& p, uint8_t op2,
                                RunResult& r) {
    auto fault = [&](ExecStatus s, uint64_t addr) { r.status = s; r.fault_addr = addr; };
    const bool is_int_op =
        ((op2 >= 0x60 && op2 <= 0x76) && op2 != 0x6E && op2 != 0x7E) ||
        op2 == 0x6C || op2 == 0x6D || op2 == 0x6F ||
        op2 == 0x7F || op2 == 0xC4 || op2 == 0xC5 || op2 == 0xD1 || op2 == 0xD2 ||
        op2 == 0xD3 || op2 == 0xD4 || op2 == 0xD5 || op2 == 0xDB || op2 == 0xDF ||
        op2 == 0xE1 || op2 == 0xE2 || op2 == 0xF1 || op2 == 0xF2 || op2 == 0xF3 ||
        op2 == 0xF4 || op2 == 0xF5 || op2 == 0xF8 || op2 == 0xF9 || op2 == 0xFA ||
        op2 == 0xFB || op2 == 0xFC || op2 == 0xFD || op2 == 0xFE;
    if (!is_int_op) {
        return false; // not ours; nothing fetched
    }

    auto rd_src = [&](const ModRMOperand& rm, CpuState::XmmReg& out) {
        return ReadXmmOperand(rm, p, 16, out);
    };
    auto decode = [&](uint8_t& modrm, ModRMOperand& rm, uint8_t& reg) -> bool {
        if (!Fetch8(rip, modrm)) { fault(ExecStatus::DecodeFault, rip); return false; }
        if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return false; }
        return true;
    };

    // --- moves ---
    if (op2 == 0x6F) { // movdqa (66) / movdqu (F3): xmm <- r/m
        uint8_t modrm = 0; ModRMOperand rm{}; uint8_t reg = 0;
        if (!decode(modrm, rm, reg)) return true;
        CpuState::XmmReg src{};
        if (!rd_src(rm, src)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        m_state.xmm[reg] = src;
        return true;
    }
    if (op2 == 0x7F) { // movdqa / movdqu: r/m <- xmm
        uint8_t modrm = 0; ModRMOperand rm{}; uint8_t reg = 0;
        if (!decode(modrm, rm, reg)) return true;
        if (!WriteXmmOperand(rm, p, 16, m_state.xmm[reg])) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        return true;
    }
    if (op2 == 0x70) { // pshufd (66) / pshufhw (F3) / pshuflw (F2)
        uint8_t modrm = 0; ModRMOperand rm{}; uint8_t reg = 0;
        if (!decode(modrm, rm, reg)) return true;
        uint8_t imm = 0;
        if (!Fetch8(rip, imm)) { fault(ExecStatus::DecodeFault, rip); return true; }
        CpuState::XmmReg src{};
        if (!rd_src(rm, src)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        CpuState::XmmReg out{};
        if (p.rep == 0xF3) { // pshufhw: shuffle words 4..7, keep 0..3
            const uint16_t* s = reinterpret_cast<const uint16_t*>(&src);
            uint16_t* o = reinterpret_cast<uint16_t*>(&out);
            for (int i = 0; i < 4; ++i) o[i] = s[i];
            o[4] = s[4 + ((imm >> 0) & 3)]; o[5] = s[4 + ((imm >> 2) & 3)];
            o[6] = s[4 + ((imm >> 4) & 3)]; o[7] = s[4 + ((imm >> 6) & 3)];
        } else if (p.rep == 0xF2) { // pshuflw: shuffle words 0..3, keep 4..7
            const uint16_t* s = reinterpret_cast<const uint16_t*>(&src);
            uint16_t* o = reinterpret_cast<uint16_t*>(&out);
            o[0] = s[(imm >> 0) & 3]; o[1] = s[(imm >> 2) & 3];
            o[2] = s[(imm >> 4) & 3]; o[3] = s[(imm >> 6) & 3];
            for (int i = 4; i < 8; ++i) o[i] = s[i];
        } else { // pshufd: shuffle dwords
            const uint32_t* s = reinterpret_cast<const uint32_t*>(&src);
            uint32_t* o = reinterpret_cast<uint32_t*>(&out);
            o[0] = s[(imm >> 0) & 3]; o[1] = s[(imm >> 2) & 3];
            o[2] = s[(imm >> 4) & 3]; o[3] = s[(imm >> 6) & 3];
        }
        m_state.xmm[reg] = out;
        return true;
    }

    // --- pextrw / pinsrw ---
    if (op2 == 0xC5) { // pextrw r32, xmm, imm8
        uint8_t modrm = 0; ModRMOperand rm{}; uint8_t reg = 0;
        if (!decode(modrm, rm, reg)) return true;
        uint8_t imm = 0;
        if (!Fetch8(rip, imm)) { fault(ExecStatus::DecodeFault, rip); return true; }
        const CpuState::XmmReg& v = m_state.xmm[reg];
        const uint16_t* w = reinterpret_cast<const uint16_t*>(&v);
        const uint16_t sel = w[imm & 7];
        if (rm.is_reg) WriteReg(rm.reg, 4, sel);
        else if (!WriteRM(rm, 4, p.rex, sel)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        return true;
    }
    if (op2 == 0xC4) { // pinsrw xmm, r32/m16, imm8
        uint8_t modrm = 0; ModRMOperand rm{}; uint8_t reg = 0;
        if (!decode(modrm, rm, reg)) return true;
        uint8_t imm = 0;
        if (!Fetch8(rip, imm)) { fault(ExecStatus::DecodeFault, rip); return true; }
        uint64_t v = 0;
        if (!ReadRM(rm, 2, p.rex, v)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        uint16_t* w = reinterpret_cast<uint16_t*>(&m_state.xmm[reg]);
        w[imm & 7] = static_cast<uint16_t>(v);
        return true;
    }

    // --- shift groups ---
    const bool is_shift_imm = (op2 == 0x71 || op2 == 0x72 || op2 == 0x73);
    const bool is_shift_reg = (op2 == 0xD1 || op2 == 0xD2 || op2 == 0xD3 ||
                               op2 == 0xE1 || op2 == 0xE2 ||
                               op2 == 0xF1 || op2 == 0xF2 || op2 == 0xF3);
    if (is_shift_imm || is_shift_reg) {
        uint8_t modrm = 0; ModRMOperand rm{}; uint8_t reg = 0;
        if (!decode(modrm, rm, reg)) return true;
        uint64_t count = 0;
        if (is_shift_imm) {
            uint8_t imm = 0;
            if (!Fetch8(rip, imm)) { fault(ExecStatus::DecodeFault, rip); return true; }
            count = imm;
        } else {
            count = m_state.xmm[reg].lo; // count from the XMM operand
        }
        const uint8_t sub = (modrm >> 3) & 7;
        // op2/sub -> (element bits, operation): ops: 0=sll,1=srl,2=sra,3=bytedq
        struct Op { int elem_bits; int kind; };
        Op op{-1, -1};
        if (op2 == 0x71 || op2 == 0xD1 || op2 == 0xF1) {
            // word forms: /2 sll /4 sra /6 srl (imm); reg: D1=psrlw, F1=psllw, E1=psraw
            if (is_shift_imm) op = {16, (sub == 2) ? 0 : (sub == 4) ? 2 : (sub == 6) ? 1 : -1};
            else op = {16, (op2 == 0xF1) ? 0 : (op2 == 0xE1) ? 2 : 1};
        } else if (op2 == 0x72 || op2 == 0xD2 || op2 == 0xF2) {
            if (is_shift_imm) op = {32, (sub == 2) ? 0 : (sub == 4) ? 2 : (sub == 6) ? 1 : -1};
            else op = {32, (op2 == 0xF2) ? 0 : (op2 == 0xE2) ? 2 : 1};
        } else { // 0x73 / D3 / F3: qword forms + psrldq/pslldq (imm only)
            if (is_shift_imm) {
                if (sub == 2) op = {64, 0};
                else if (sub == 6) op = {64, 1};
                else if (sub == 3) op = {0, 3};  // pslldq (byte shift left)
                else if (sub == 7) op = {0, 4};  // psrldq (byte shift right)
            } else {
                op = {64, (op2 == 0xF3) ? 0 : 1};
            }
        }
        if (op.kind < 0) { fault(ExecStatus::UnsupportedOpcode, rip); return true; }
        CpuState::XmmReg v{};
        if (!rd_src(rm, v)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        CpuState::XmmReg out{};
        if (op.kind == 3 || op.kind == 4) { // byte-granular pslldq/psrldq
            const uint8_t n = static_cast<uint8_t>(count & 0xFF);
            const uint8_t* s = reinterpret_cast<const uint8_t*>(&v);
            uint8_t* o = reinterpret_cast<uint8_t*>(&out);
            if (op.kind == 3) { // shift LEFT by n bytes
                for (int i = 15; i >= 0; --i) o[i] = (i >= n) ? s[i - n] : 0;
            } else {           // shift RIGHT by n bytes
                for (int i = 0; i < 16; ++i) o[i] = (i + n < 16) ? s[i + n] : 0;
            }
        } else {
            const int lanes = 128 / op.elem_bits;
            for (int i = 0; i < lanes; ++i) {
                const uint64_t mask = (op.elem_bits == 64) ? ~0ull : ((1ull << op.elem_bits) - 1);
                const int sh = i * op.elem_bits;
                const uint64_t elem = (sh >= 64) ? (v.hi >> (sh - 64)) : (v.lo >> sh);
                uint64_t e = elem & mask;
                uint64_t res = 0;
                const uint64_t cnt = count;
                if (op.kind == 0) res = (cnt >= (uint64_t)op.elem_bits) ? 0 : ((e << cnt) & mask);
                else if (op.kind == 1) res = (cnt >= (uint64_t)op.elem_bits) ? 0 : (e >> cnt);
                else {
                    const int64_t se = static_cast<int64_t>(e << (64 - op.elem_bits)) >> (64 - op.elem_bits);
                    res = (cnt >= (uint64_t)op.elem_bits)
                        ? static_cast<uint64_t>(se >> (op.elem_bits - 1))
                        : (static_cast<uint64_t>(se >> cnt) & mask);
                }
                if (sh >= 64) out.hi = (out.hi & ~(mask << (sh - 64))) | (res << (sh - 64));
                else out.lo = (out.lo & ~(mask << sh)) | (res << sh);
            }
        }
        if (!WriteXmmOperand(rm, p, 16, out)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        return true;
    }

    // --- 3-operand-style integer ALU (2-operand in SSE encoding) ---
    uint8_t modrm = 0; ModRMOperand rm{}; uint8_t reg = 0;
    if (!decode(modrm, rm, reg)) return true;
    CpuState::XmmReg src{};
    if (!rd_src(rm, src)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
    CpuState::XmmReg dst = m_state.xmm[reg];
    CpuState::XmmReg out{};
    const uint8_t* d8 = reinterpret_cast<const uint8_t*>(&dst);
    const uint8_t* s8 = reinterpret_cast<const uint8_t*>(&src);
    const uint16_t* d16 = reinterpret_cast<const uint16_t*>(&dst);
    const uint16_t* s16 = reinterpret_cast<const uint16_t*>(&src);
    const uint32_t* d32 = reinterpret_cast<const uint32_t*>(&dst);
    const uint32_t* s32 = reinterpret_cast<const uint32_t*>(&src);
    uint8_t* o8 = reinterpret_cast<uint8_t*>(&out);
    uint16_t* o16 = reinterpret_cast<uint16_t*>(&out);
    uint32_t* o32 = reinterpret_cast<uint32_t*>(&out);
    auto sat8 = [](int32_t v) -> uint8_t { return v < 0 ? 0 : (v > 255 ? 255 : v); };
    auto sat8s = [](int32_t v) -> int8_t { return v < -128 ? -128 : (v > 127 ? 127 : v); };
    auto sat16s = [](int32_t v) -> int16_t { return v < -32768 ? -32768 : (v > 32767 ? 32767 : v); };

    switch (op2) {
        case 0x60: case 0x61: case 0x62: { // punpcklbw/lwd/ldq
            const int step = (op2 == 0x60) ? 1 : (op2 == 0x61) ? 2 : 4;
            for (int i = 0; i < 8 / step; ++i) {
                if (step == 1) { o8[2 * i] = d8[i]; o8[2 * i + 1] = s8[i]; }
                else if (step == 2) { o16[2 * i] = d16[i]; o16[2 * i + 1] = s16[i]; }
                else { o32[2 * i] = d32[i]; o32[2 * i + 1] = s32[i]; }
            }
            break;
        }
        case 0x68: case 0x69: case 0x6A: { // punpckhbw/hwd/hdq
            const int step = (op2 == 0x68) ? 1 : (op2 == 0x69) ? 2 : 4;
            for (int i = 0; i < 8 / step; ++i) {
                if (step == 1) { o8[2 * i] = d8[8 + i]; o8[2 * i + 1] = s8[8 + i]; }
                else if (step == 2) { o16[2 * i] = d16[4 + i]; o16[2 * i + 1] = s16[4 + i]; }
                else { o32[2 * i] = d32[2 + i]; o32[2 * i + 1] = s32[2 + i]; }
            }
            break;
        }
        case 0x6C: { // punpcklqdq
            out.lo = dst.lo; out.hi = src.lo; break;
        }
        case 0x6D: { // punpckhqdq
            out.lo = dst.hi; out.hi = src.hi; break;
        }
        case 0x63: { // packsswb
            const int16_t* a = reinterpret_cast<const int16_t*>(&dst);
            const int16_t* b = reinterpret_cast<const int16_t*>(&src);
            for (int i = 0; i < 4; ++i) { o8[i] = static_cast<uint8_t>(sat8s(a[i])); o8[4 + i] = static_cast<uint8_t>(sat8s(b[i])); }
            break;
        }
        case 0x67: { // packuswb
            const int16_t* a = reinterpret_cast<const int16_t*>(&dst);
            const int16_t* b = reinterpret_cast<const int16_t*>(&src);
            for (int i = 0; i < 4; ++i) { o8[i] = sat8(a[i]); o8[4 + i] = sat8(b[i]); }
            break;
        }
        case 0x6B: { // packssdw
            const int32_t* a = reinterpret_cast<const int32_t*>(&dst);
            const int32_t* b = reinterpret_cast<const int32_t*>(&src);
            for (int i = 0; i < 2; ++i) { o16[i] = static_cast<uint16_t>(sat16s(a[i])); o16[2 + i] = static_cast<uint16_t>(sat16s(b[i])); }
            for (int i = 2; i < 4; ++i) { o16[i] = static_cast<uint16_t>(sat16s(a[i])); o16[4 + i] = static_cast<uint16_t>(sat16s(b[i - 2])); }
            break;
        }
        case 0x64: case 0x65: case 0x66: { // pcmpgtb/w/d
            const int n = (op2 == 0x64) ? 16 : (op2 == 0x65) ? 8 : 4;
            for (int i = 0; i < n; ++i) {
                bool gt = false;
                if (n == 16) gt = static_cast<int8_t>(d8[i]) > static_cast<int8_t>(s8[i]);
                else if (n == 8) gt = static_cast<int16_t>(d16[i]) > static_cast<int16_t>(s16[i]);
                else gt = static_cast<int32_t>(d32[i]) > static_cast<int32_t>(s32[i]);
                if (n == 16) o8[i] = gt ? 0xFF : 0;
                else if (n == 8) o16[i] = gt ? 0xFFFF : 0;
                else o32[i] = gt ? 0xFFFFFFFFu : 0;
            }
            break;
        }
        case 0x74: case 0x75: case 0x76: { // pcmpeqb/w/d
            const int n = (op2 == 0x74) ? 16 : (op2 == 0x75) ? 8 : 4;
            for (int i = 0; i < n; ++i) {
                bool eq = false;
                if (n == 16) eq = d8[i] == s8[i];
                else if (n == 8) eq = d16[i] == s16[i];
                else eq = d32[i] == s32[i];
                if (n == 16) o8[i] = eq ? 0xFF : 0;
                else if (n == 8) o16[i] = eq ? 0xFFFF : 0;
                else o32[i] = eq ? 0xFFFFFFFFu : 0;
            }
            break;
        }
        case 0xDB: out.lo = dst.lo & src.lo; out.hi = dst.hi & src.hi; break; // pand
        case 0xDF: out.lo = dst.lo & ~src.lo; out.hi = dst.hi & ~src.hi; break; // pandn
        case 0xEB: out.lo = dst.lo | src.lo; out.hi = dst.hi | src.hi; break; // por
        case 0xD4: out.lo = dst.lo + src.lo; out.hi = dst.hi + src.hi; break; // paddq
        case 0xFB: out.lo = dst.lo - src.lo; out.hi = dst.hi - src.hi; break; // psubq
        case 0xFC: case 0xFD: case 0xFE: { // paddb/w/d
            const int n = (op2 == 0xFC) ? 16 : (op2 == 0xFD) ? 8 : 4;
            for (int i = 0; i < n; ++i) {
                if (n == 16) o8[i] = static_cast<uint8_t>(d8[i] + s8[i]);
                else if (n == 8) o16[i] = static_cast<uint16_t>(d16[i] + s16[i]);
                else o32[i] = d32[i] + s32[i];
            }
            break;
        }
        case 0xF8: case 0xF9: case 0xFA: { // psubb/w/d
            const int n = (op2 == 0xF8) ? 16 : (op2 == 0xF9) ? 8 : 4;
            for (int i = 0; i < n; ++i) {
                if (n == 16) o8[i] = static_cast<uint8_t>(d8[i] - s8[i]);
                else if (n == 8) o16[i] = static_cast<uint16_t>(d16[i] - s16[i]);
                else o32[i] = d32[i] - s32[i];
            }
            break;
        }
        case 0xD5: { // pmullw
            for (int i = 0; i < 8; ++i) {
                o16[i] = static_cast<uint16_t>(static_cast<int16_t>(d16[i]) * static_cast<int16_t>(s16[i]));
            }
            break;
        }
        case 0xF4: { // pmuludq: dword pairs -> qword products
            out.lo = static_cast<uint64_t>(d32[0]) * static_cast<uint64_t>(s32[0]);
            out.hi = static_cast<uint64_t>(d32[2]) * static_cast<uint64_t>(s32[2]);
            break;
        }
        case 0xF5: { // pmaddwd
            for (int i = 0; i < 4; ++i) {
                const int32_t a = static_cast<int16_t>(d16[2 * i]) * static_cast<int16_t>(s16[2 * i]);
                const int32_t b = static_cast<int16_t>(d16[2 * i + 1]) * static_cast<int16_t>(s16[2 * i + 1]);
                o32[i] = static_cast<uint32_t>(a + b);
            }
            break;
        }
        default:
            fault(ExecStatus::UnsupportedOpcode, rip);
            return true;
    }
    m_state.xmm[reg] = out;
    return true;
}


bool X86Interpreter::ExecVex(uint64_t& rip, uint8_t vex_first, RunResult& r) {
    auto fault = [&](ExecStatus s, uint64_t addr) { r.status = s; r.fault_addr = addr; };
    auto commit = [&](uint64_t end_rip) { m_state.rip = end_rip; };

    uint8_t b2 = 0;
    if (!Fetch8(rip, b2)) { fault(ExecStatus::DecodeFault, rip); return true; }

    Prefixes p{};
    p.rex = true; // VEX implies REX-like register access
    uint8_t vreg = 0;
    bool L = false;
    uint8_t map_select = 1; // 0F
    bool vex_w = false;

    if (vex_first == 0xC5) {
        // C5 [R~ v3~ v2~ v1~ v0~ L pp]
        p.rex_r = ((~b2) >> 7) & 1;
        vreg = (~(b2 >> 3)) & 0xF;
        L = ((b2 >> 2) & 1) != 0;
        const uint8_t pp = b2 & 3;
        if (pp == 1) p.opsize = true;        // 66
        else if (pp == 2) p.rep = 0xF3;      // F3
        else if (pp == 3) p.rep = 0xF2;      // F2
    } else {
        // C4 [R~ X~ B~ m4..m0][W~ v3~..v0~ L pp]
        uint8_t b3 = 0;
        if (!Fetch8(rip, b3)) { fault(ExecStatus::DecodeFault, rip); return true; }
        p.rex_r = ((~b2) >> 7) & 1;
        p.rex_x = ((~b2) >> 6) & 1;
        p.rex_b = ((~b2) >> 5) & 1;
        map_select = b2 & 0x1F;
        vex_w = ((b3 >> 7) & 1) != 0; // W is a DIRECT bit (empirically verified vs gas)
        vreg = (~(b3 >> 3)) & 0xF;
        L = ((b3 >> 2) & 1) != 0;
        const uint8_t pp = b3 & 3;
        if (pp == 1) p.opsize = true;
        else if (pp == 2) p.rep = 0xF3;
        else if (pp == 3) p.rep = 0xF2;
    }

    // Round 28: the old blanket "scalar VEX with L=1 is invalid" guard is
    // GONE -- it wrongly rejected legitimate 256-bit instructions that use
    // the 66/F2/F3 prefixes (vhaddps, vaddsubps, vcvttps2dq, vcvtdq2pd,
    // ...). Scalar-vs-vector validity is now decided per-opcode inside
    // ExecSimdFull (VEX_L1_SCALAR table).
    const int vw = L ? 32 : 16;   // round 18: the vector register width in bytes

    uint8_t opcode = 0;
    if (!Fetch8(rip, opcode)) { fault(ExecStatus::DecodeFault, rip); return true; }

    // ---- round 28: the FULL SIMD engine takes priority for the entire VEX
    // maps (AVX/AVX2/FMA, 128- and 256-bit). When the engine declines, the
    // stream is rewound to the opcode position and the legacy VEX subset
    // handlers below run exactly as before.
    {
        const uint64_t rip_at_opcode = rip;
        SimdVexInfo vi{};
        vi.vreg = vreg;
        vi.L = L;
        vi.W = vex_w;
        if (ExecSimdFull(rip, p, opcode, map_select, &vi, r)) {
            if (r.status == ExecStatus::Running) m_state.rip = rip;
            return true;
        }
        rip = rip_at_opcode;
    }

    if (map_select == 1) {
        // ---- 0F map -------------------------------------------------------
        uint8_t modrm = 0;
        auto modrm_ok = [&]() -> bool {
            if (!Fetch8(rip, modrm)) { fault(ExecStatus::DecodeFault, rip); return false; }
            return true;
        };
        // ---- round 18: 256-bit (AVX.256) operand helpers ------------------
        // YMM i = { xmm[i] (low 128), ymm_hi[i] (high 128) }. VEX.128
        // register writes zero the upper half (hardware rule).
        struct YmmPair {
            CpuState::XmmReg lo, hi;
        };
        auto read_v = [&](const ModRMOperand& op, YmmPair& out) -> bool {
            if (vw == 16) return ReadXmmOperand(op, p, 16, out.lo);
            if (op.is_reg) {
                const uint8_t i = static_cast<uint8_t>(op.reg | (p.rex_b ? 0x08 : 0x00));
                out.lo = m_state.xmm[i];
                out.hi = m_state.ymm_hi[i];
                return true;
            }
            return m_mem.Read(op.addr, &out, 32);
        };
        auto write_v = [&](const ModRMOperand& op, const YmmPair& v) -> bool {
            if (vw == 16) {
                if (!WriteXmmOperand(op, p, 16, v.lo)) return false;
                if (op.is_reg) {
                    const uint8_t i = static_cast<uint8_t>(op.reg | (p.rex_b ? 0x08 : 0x00));
                    m_state.ymm_hi[i] = CpuState::XmmReg{};   // AVX.128 zeroes the upper half
                }
                return true;
            }
            if (op.is_reg) {
                const uint8_t i = static_cast<uint8_t>(op.reg | (p.rex_b ? 0x08 : 0x00));
                m_state.xmm[i] = v.lo;
                m_state.ymm_hi[i] = v.hi;
                return true;
            }
            return m_mem.Write(op.addr, &v, 32);
        };
        // vvvv operand read at the full vector width.
        auto read_vvvv = [&](YmmPair& out) -> bool {
            return read_v(ModRMOperand{true, vreg}, out);
        };
        // apply a 128-bit lane op to both halves of a YmmPair.
        auto map_lanes = [&](const YmmPair& a, const YmmPair& b, YmmPair& out,
                             uint64_t (*op128)(uint64_t, uint64_t)) -> void {
            out.lo.lo = op128(a.lo.lo, b.lo.lo);
            out.lo.hi = op128(a.lo.hi, b.lo.hi);
            out.hi.lo = op128(a.hi.lo, b.hi.lo);
            out.hi.hi = op128(a.hi.hi, b.hi.hi);
        };

        // Arithmetic 3-operand: dst = vvvv OP rm (16 or 32 bytes).
        const bool is_float_arith =
            (opcode == 0x58 || opcode == 0x59 || opcode == 0x5C || opcode == 0x5D ||
             opcode == 0x5E || opcode == 0x5F);
        const bool is_float_bitwise = (opcode == 0x54 || opcode == 0x56 || opcode == 0x57);
        const bool is_int_bitwise = (opcode == 0xDB || opcode == 0xEB || opcode == 0xEF);
        if (is_float_arith || is_float_bitwise || is_int_bitwise) {
            if (!modrm_ok()) return true;
            ModRMOperand rm{}; uint8_t reg = 0;
            if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
            YmmPair src1{}; // vvvv operand (read-only)
            if (!read_vvvv(src1)) { fault(ExecStatus::MemoryFault, rip); return true; }
            YmmPair src2{};
            if (!read_v(rm, src2)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
            YmmPair out{};
            if (is_int_bitwise || is_float_bitwise) {
                uint64_t (*f)(uint64_t, uint64_t);
                if (opcode == 0xDB || opcode == 0x54)      f = [](uint64_t a, uint64_t b) { return a & b; };
                else if (opcode == 0xEB || opcode == 0x56) f = [](uint64_t a, uint64_t b) { return a | b; };
                else                                       f = [](uint64_t a, uint64_t b) { return a ^ b; };
                map_lanes(src1, src2, out, f);
            } else {
                // float arithmetic; opsize selects ps (0) vs pd (1)
                const bool dbl = p.opsize;
                const bool packed = (p.rep == 0);  // ss/sd are scalar (F3/F2)
                const int lanes = (dbl ? 2 : 4) * (vw / 16);
                const auto half = [&](const CpuState::XmmReg& a, const CpuState::XmmReg& b,
                                      CpuState::XmmReg& o) {
                    o = a;   // scalar keeps the untouched lanes from src (vvvv)
                    for (int lane = 0; lane < (dbl ? 2 : 4); ++lane) {
                        if (!packed && lane != 0) continue;  // scalar: lane 0 only
                        const double x = dbl ? XmmDouble(a, lane) : static_cast<double>(XmmFloat(a, lane));
                        const double y = dbl ? XmmDouble(b, lane) : static_cast<double>(XmmFloat(b, lane));
                        double res = 0;
                        switch (opcode) {
                            case 0x58: res = x + y; break;
                            case 0x59: res = x * y; break;
                            case 0x5C: res = x - y; break;
                            case 0x5D: res = x < y ? x : y; break;
                            case 0x5E: res = x / y; break;
                            default:   res = x > y ? x : y; break; // 0x5F
                        }
                        if (dbl) SetXmmDouble(o, lane, res);
                        else SetXmmFloat(o, lane, static_cast<float>(res));
                    }
                };
                (void)lanes;
                half(src1.lo, src2.lo, out.lo);
                if (vw == 32 && packed) half(src1.hi, src2.hi, out.hi);
            }
            ModRMOperand dst{true, reg};
            if (!write_v(dst, out)) { fault(ExecStatus::MemoryFault, rip); return true; }
            commit(rip);
            return true;
        }

        // VZEROUPPER / VZEROALL (no modrm).
        if (opcode == 0x77) {
            if (L) {
                // vzeroall: every YMM register fully cleared.
                for (int i = 0; i < 16; ++i) {
                    m_state.xmm[i] = CpuState::XmmReg{};
                    m_state.ymm_hi[i] = CpuState::XmmReg{};
                }
            } else {
                // vzeroupper: only the upper halves.
                for (int i = 0; i < 16; ++i) {
                    m_state.ymm_hi[i] = CpuState::XmmReg{};
                }
            }
            commit(rip);
            return true;
        }

        switch (opcode) {
            case 0x28: case 0x10: { // vmovaps/vmovups ymm, r/m (load / copy)
                if (!modrm_ok()) return true;
                ModRMOperand rm{}; uint8_t reg = 0;
                if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
                YmmPair src{};
                if (!read_v(rm, src)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                ModRMOperand dst{true, reg};
                if (!write_v(dst, src)) { fault(ExecStatus::MemoryFault, rip); return true; }
                commit(rip);
                return true;
            }
            case 0x29: case 0x11: { // vmovaps/vmovups r/m, ymm (store)
                if (!modrm_ok()) return true;
                ModRMOperand rm{}; uint8_t reg = 0;
                if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
                YmmPair src{};
                if (!read_v(ModRMOperand{true, reg}, src)) { fault(ExecStatus::MemoryFault, rip); return true; }
                if (!write_v(rm, src)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                commit(rip);
                return true;
            }
            case 0x6F: { // vmovdqa/vmovdqu ymm, r/m
                if (!modrm_ok()) return true;
                ModRMOperand rm{}; uint8_t reg = 0;
                if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
                YmmPair src{};
                if (!read_v(rm, src)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                ModRMOperand dst{true, reg};
                if (!write_v(dst, src)) { fault(ExecStatus::MemoryFault, rip); return true; }
                commit(rip);
                return true;
            }
            case 0x7F: { // vmovdqa/vmovdqu r/m, ymm
                if (!modrm_ok()) return true;
                ModRMOperand rm{}; uint8_t reg = 0;
                if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
                YmmPair src{};
                if (!read_v(ModRMOperand{true, reg}, src)) { fault(ExecStatus::MemoryFault, rip); return true; }
                if (!write_v(rm, src)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                commit(rip);
                return true;
            }
            case 0x70: { // vpshufd -- per-128-bit-lane shuffle (AVX2 rule)
                if (p.rep != 0) { fault(ExecStatus::UnsupportedOpcode, rip); return true; }
                if (!modrm_ok()) return true;
                ModRMOperand rm{}; uint8_t reg = 0;
                if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
                uint8_t imm = 0; if (!Fetch8(rip, imm)) { fault(ExecStatus::DecodeFault, rip); return true; }
                YmmPair src{};
                if (!read_v(rm, src)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                const auto shuffle = [&](const CpuState::XmmReg& in) {
                    const uint32_t w[4] = {
                        static_cast<uint32_t>(in.lo), static_cast<uint32_t>(in.lo >> 32),
                        static_cast<uint32_t>(in.hi), static_cast<uint32_t>(in.hi >> 32)};
                    CpuState::XmmReg out{};
                    uint32_t* o = reinterpret_cast<uint32_t*>(&out);
                    o[0] = w[(imm >> 0) & 3];
                    o[1] = w[(imm >> 2) & 3];
                    o[2] = w[(imm >> 4) & 3];
                    o[3] = w[(imm >> 6) & 3];
                    return out;
                };
                YmmPair out{};
                out.lo = shuffle(src.lo);
                if (vw == 32) out.hi = shuffle(src.hi);
                ModRMOperand dst{true, reg};
                if (!write_v(dst, out)) { fault(ExecStatus::MemoryFault, rip); return true; }
                commit(rip);
                return true;
            }
            case 0x6E: { // vmovd/vmovq r32/64, xmm (W selects; 128-bit only)
                if (L) { fault(ExecStatus::UnsupportedOpcode, rip); return true; }
                if (!modrm_ok()) return true;
                ModRMOperand rm{}; uint8_t reg = 0;
                if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
                const int width = vex_w ? 8 : 4;
                uint64_t v = 0;
                if (!ReadRM(rm, width, p.rex, v)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                CpuState::XmmReg out{};
                out.lo = v;
                m_state.xmm[reg] = out;
                m_state.ymm_hi[reg] = CpuState::XmmReg{};
                commit(rip);
                return true;
            }
            case 0x7E: { // vmovd/vmovq xmm, r/m (store; W selects q)
                if (L) { fault(ExecStatus::UnsupportedOpcode, rip); return true; }
                if (!modrm_ok()) return true;
                ModRMOperand rm{}; uint8_t reg = 0;
                if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
                const int width = vex_w ? 8 : 4;
                const uint64_t v = m_state.xmm[reg].lo & ((width == 8) ? ~0ull : 0xFFFFFFFFull);
                if (!WriteRM(rm, width, p.rex, v)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                commit(rip);
                return true;
            }
            case 0xD6: { // vmovq r/m64, xmm (128-bit only)
                if (L) { fault(ExecStatus::UnsupportedOpcode, rip); return true; }
                if (!modrm_ok()) return true;
                ModRMOperand rm{}; uint8_t reg = 0;
                if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
                if (!WriteRM(rm, 8, p.rex, m_state.xmm[reg].lo)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                commit(rip);
                return true;
            }
            case 0x51: { // vsqrtps/vsqrtss/vsqrtpd/vsqrtsd
                if (!modrm_ok()) return true;
                ModRMOperand rm{}; uint8_t reg = 0;
                if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
                YmmPair src{};
                if (!read_v(rm, src)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                YmmPair out{};
                const bool dbl = p.opsize;
                const bool scalar = p.rep != 0;
                const auto half = [&](const CpuState::XmmReg& in, CpuState::XmmReg& o) {
                    for (int lane = 0; lane < (dbl ? 2 : 4); ++lane) {
                        if (scalar && lane != 0) continue;
                        if (dbl) SetXmmDouble(o, lane, std::sqrt(XmmDouble(in, lane)));
                        else SetXmmFloat(o, lane, std::sqrt(XmmFloat(in, lane)));
                    }
                    if (scalar) {
                        o.hi = in.hi;
                        if (dbl) o.lo = (o.lo & 0xFFFFFFFFull) | (in.lo & ~0xFFFFFFFFull);
                    }
                };
                half(src.lo, out.lo);
                if (vw == 32 && !scalar) out.hi = [&]() {
                    CpuState::XmmReg o{};
                    for (int lane = 0; lane < (dbl ? 2 : 4); ++lane) {
                        if (dbl) SetXmmDouble(o, lane, std::sqrt(XmmDouble(src.hi, lane)));
                        else SetXmmFloat(o, lane, std::sqrt(XmmFloat(src.hi, lane)));
                    }
                    return o;
                }();
                else if (scalar) {
                    // scalar keeps the upper 128 bits from src (vvvv rule)
                    out.hi = src.lo;
                }
                ModRMOperand dst{true, reg};
                if (!write_v(dst, out)) { fault(ExecStatus::MemoryFault, rip); return true; }
                commit(rip);
                return true;
            }
            default:
                fault(ExecStatus::UnsupportedOpcode, rip);
                return true;
        }
    }

    if (map_select == 2) {
        // ---- 0F 38 map: the opcode byte fetched after the VEX prefix IS the
        // 0F38 opcode (VEX replaces the 0F 38 escape entirely). ----
        const uint8_t op3 = opcode;
        uint8_t modrm = 0;
        if (!Fetch8(rip, modrm)) { fault(ExecStatus::DecodeFault, rip); return true; }
        ModRMOperand rm{}; uint8_t reg = 0;
        if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return true; }
        // ---- round 20: FMA3 (VEX.128/256.66.0F38 98..BF) ----------------------
        // gas ground truth: vfmadd132ps = C4 E2 69 98 /r (map 2, pp = 66,
        // W = 0); the 213/231 orders step the opcode by 0x10; sub / negated-
        // product variants step it by 2/4/6; the ODD opcodes are the scalar
        // (ss/sd) forms and W selects float32 vs float64:
        //   98/99 VFMADD132 ps/ss   9A/9B VFMSUB132   9C/9D VFNMADD132
        //   9E/9F VFNMSUB132        A8..AF the 213 order   B8..BF the 231 order
        // Semantics (SDM): 132: dst = dst*rm + vvvv; 213: dst = dst*vvvv +
        // rm; 231: dst = vvvv*rm + dst; FNM negates the PRODUCT, SUB
        // subtracts the addend. Scalar forms touch lane 0 only.
        if (op3 >= 0x98 && op3 <= 0xBF && p.opsize && p.rep == 0) {
            const uint32_t t = op3 - 0x98u;
            const uint32_t sub_op = t & 0x0Fu;      // 0..7 (8..15: not FMA)
            const uint32_t order = t >> 4u;          // 0=132 1=213 2=231
            if (sub_op <= 7u && order <= 2u) {
                const bool scalar = (sub_op & 1u) != 0u;
                const bool neg_product = (sub_op & 4u) != 0u;   // FNM*
                const bool sub_addend = (sub_op & 2u) != 0u;    // *SUB
                const bool dbl = vex_w;                          // ps vs pd

                auto read_x = [&](const ModRMOperand& op,
                                  CpuState::XmmReg& lo,
                                  CpuState::XmmReg& hi) -> bool {
                    if (op.is_reg) {
                        const uint8_t i = static_cast<uint8_t>(
                            op.reg | (p.rex_b ? 0x08 : 0x00));
                        lo = m_state.xmm[i];
                        hi = m_state.ymm_hi[i];
                        return true;
                    }
                    if (!m_mem.Read(op.addr, &lo, 16)) return false;
                    if (L && !m_mem.Read(op.addr + 16, &hi, 16)) return false;
                    return true;
                };

                CpuState::XmmReg dst_lo{}, dst_hi{}, vvvv_lo{}, vvvv_hi{},
                    rm_lo{}, rm_hi{};
                const uint8_t dst_idx = static_cast<uint8_t>(
                    reg | (p.rex_r ? 0x08 : 0x00));
                dst_lo = m_state.xmm[dst_idx];
                dst_hi = m_state.ymm_hi[dst_idx];
                if (!read_x(ModRMOperand{true, vreg}, vvvv_lo, vvvv_hi)) {
                    fault(ExecStatus::MemoryFault, rip);
                    return true;
                }
                if (!read_x(rm, rm_lo, rm_hi)) {
                    fault(ExecStatus::MemoryFault, rm.addr);
                    return true;
                }

                // The product pair (a,b) and the addend c per the order.
                const CpuState::XmmReg *a_lo, *b_lo, *c_lo;
                const CpuState::XmmReg *a_hi, *b_hi, *c_hi;
                if (order == 0u) {          // 132: dst * rm + vvvv
                    a_lo = &dst_lo; b_lo = &rm_lo; c_lo = &vvvv_lo;
                    a_hi = &dst_hi; b_hi = &rm_hi; c_hi = &vvvv_hi;
                } else if (order == 1u) {   // 213: dst * vvvv + rm
                    a_lo = &dst_lo; b_lo = &vvvv_lo; c_lo = &rm_lo;
                    a_hi = &dst_hi; b_hi = &vvvv_hi; c_hi = &rm_hi;
                } else {                    // 231: vvvv * rm + dst
                    a_lo = &vvvv_lo; b_lo = &rm_lo; c_lo = &dst_lo;
                    a_hi = &vvvv_hi; b_hi = &rm_hi; c_hi = &dst_hi;
                }

                const int lane_count = dbl ? 2 : 4;
                const auto fma_lane = [&](const CpuState::XmmReg& a,
                                          const CpuState::XmmReg& b,
                                          const CpuState::XmmReg& c,
                                          CpuState::XmmReg& out) {
                    out = scalar ? a : CpuState::XmmReg{};   // scalar keeps lanes
                    for (int lane = 0; lane < lane_count; ++lane) {
                        if (scalar && lane != 0) continue;
                        if (dbl) {
                            const double prod = XmmDouble(a, lane) * XmmDouble(b, lane);
                            const double p = neg_product ? -prod : prod;
                            const double r = sub_addend ? p - XmmDouble(c, lane)
                                                        : p + XmmDouble(c, lane);
                            SetXmmDouble(out, lane, r);
                        } else {
                            const double prod = static_cast<double>(XmmFloat(a, lane)) *
                                                static_cast<double>(XmmFloat(b, lane));
                            const double p = neg_product ? -prod : prod;
                            const double r = sub_addend
                                ? p - static_cast<double>(XmmFloat(c, lane))
                                : p + static_cast<double>(XmmFloat(c, lane));
                            SetXmmFloat(out, lane, static_cast<float>(r));
                        }
                    }
                };
                CpuState::XmmReg out_lo{}, out_hi{};
                fma_lane(*a_lo, *b_lo, *c_lo, out_lo);
                if (L && !scalar) fma_lane(*a_hi, *b_hi, *c_hi, out_hi);
                else if (L && scalar) out_hi = *a_hi;  // scalar: upper half kept

                m_state.xmm[dst_idx] = out_lo;
                if (L) m_state.ymm_hi[dst_idx] = out_hi;
                else m_state.ymm_hi[dst_idx] = CpuState::XmmReg{};  // 128-bit
                commit(rip);
                return true;
            }
        }

        if (op3 == 0x00) { // vpshufb: dst = vvvv(data) shuffled by rm(mask), per lane
            struct YmmPair2 { CpuState::XmmReg lo, hi; };
            auto read16 = [&](const ModRMOperand& op, CpuState::XmmReg& o) -> bool {
                if (op.is_reg) {
                    const uint8_t i = static_cast<uint8_t>(op.reg | (p.rex_b ? 0x08 : 0x00));
                    o = m_state.xmm[i];
                    return true;
                }
                return m_mem.Read(op.addr, &o, 16);
            };
            // The vvvv/data + mask operands are full-width for L=1 (AVX2),
            // shuffling per 128-bit lane.
            if (L) {
                CpuState::XmmReg dlo, dhi, mlo, mhi;
                if (!read16(ModRMOperand{true, vreg}, dlo)) { fault(ExecStatus::MemoryFault, rip); return true; }
                dhi = m_state.ymm_hi[vreg];
                if (!read16(rm, mlo)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                if (rm.is_reg) {
                    const uint8_t i = static_cast<uint8_t>(rm.reg | (p.rex_b ? 0x08 : 0x00));
                    mhi = m_state.ymm_hi[i];
                } else {
                    if (!m_mem.Read(rm.addr + 16, &mhi, 16)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                }
                const auto shuf = [](const CpuState::XmmReg& d, const CpuState::XmmReg& msk) {
                    CpuState::XmmReg out{};
                    const uint8_t* s = reinterpret_cast<const uint8_t*>(&d);
                    const uint8_t* m = reinterpret_cast<const uint8_t*>(&msk);
                    uint8_t* o = reinterpret_cast<uint8_t*>(&out);
                    for (int i = 0; i < 16; ++i) o[i] = (m[i] & 0x80) ? 0 : s[m[i] & 0x0F];
                    return out;
                };
                CpuState::XmmReg olo = shuf(dlo, mlo), ohi = shuf(dhi, mhi);
                if (rm.is_reg) {
                    const uint8_t i = static_cast<uint8_t>(rm.reg | (p.rex_b ? 0x08 : 0x00));
                    (void)i;
                }
                ModRMOperand dst{true, reg};
                const uint8_t di = static_cast<uint8_t>(reg | (p.rex_r ? 0x08 : 0x00));
                (void)dst;
                m_state.xmm[di] = olo;
                m_state.ymm_hi[di] = ohi;
                commit(rip);
                return true;
            }
            CpuState::XmmReg data{}, mask{};
            if (!read16(ModRMOperand{true, vreg}, data)) { fault(ExecStatus::MemoryFault, rip); return true; }
            if (!read16(rm, mask)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
            CpuState::XmmReg out{};
            const uint8_t* s = reinterpret_cast<const uint8_t*>(&data);
            const uint8_t* m = reinterpret_cast<const uint8_t*>(&mask);
            uint8_t* o = reinterpret_cast<uint8_t*>(&out);
            for (int i = 0; i < 16; ++i) o[i] = (m[i] & 0x80) ? 0 : s[m[i] & 0x0F];
            m_state.xmm[reg] = out;
            m_state.ymm_hi[reg] = CpuState::XmmReg{};
            commit(rip);
            return true;
        }
        if (op3 == 0x17) { // vptest (32 bytes when L=1)
            struct YmmPair3 { CpuState::XmmReg lo, hi; };
            auto read_full = [&](const ModRMOperand& op, YmmPair3& o) -> bool {
                if (op.is_reg) {
                    const uint8_t i = static_cast<uint8_t>(op.reg | (p.rex_b ? 0x08 : 0x00));
                    o.lo = m_state.xmm[i];
                    o.hi = L ? m_state.ymm_hi[i] : CpuState::XmmReg{};
                    return true;
                }
                if (!m_mem.Read(op.addr, &o.lo, 16)) return false;
                if (L) return m_mem.Read(op.addr + 16, &o.hi, 16);
                return true;
            };
            YmmPair3 src{};
            if (!read_full(rm, src)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
            YmmPair3 dstv{};
            if (!read_full(ModRMOperand{true, reg}, dstv)) { fault(ExecStatus::MemoryFault, rip); return true; }
            const uint64_t and0 = (dstv.lo.lo & src.lo.lo) | (dstv.lo.hi & src.lo.hi);
            const uint64_t and1 = (dstv.hi.lo & src.hi.lo) | (dstv.hi.hi & src.hi.hi);
            const uint64_t andn0 = (dstv.lo.lo & ~src.lo.lo) | (dstv.lo.hi & ~src.lo.hi);
            const uint64_t andn1 = (dstv.hi.lo & ~src.hi.lo) | (dstv.hi.hi & ~src.hi.hi);
            SetFlag(Flags::ZF, (and0 | and1) == 0);
            SetFlag(Flags::CF, (andn0 | andn1) == 0);
            commit(rip);
            return true;
        }
        // ---- round 20: BMI1/BMI2 (VEX 0F38 map). Encodings per the Intel SDM;
        // the operand size comes from the VEX.W bit (32/64-bit), and every op
        // writes the full register width (upper bits zero for 32-bit form).
        // Flags per the SDM: ANDN/BEXTR set ZF from the RESULT and clear
        // CF/OF/SF; BLSI/BLSMSK/BLSR set ZF from the SOURCE being zero;
        // BZHI sets CF=(index<width), ZF=(result==0);
        // MULX/PDEP/PEXT/RORX/SARX/SHLX/SHRX leave flags untouched.
        // All BMI ops require VEX.L = 0 (invalid encoding otherwise). ----
        const bool bmi_shape =
            (op3 == 0xF2 && (p.rep == 0 || p.rep == 0xF2) && !p.opsize) || // ANDN (pp=00 canonical; hardware ignores pp for GPR ops)
            (op3 == 0xF7 && p.rep == 0 && !p.opsize) ||             // BEXTR
            (op3 == 0xF3 && p.rep == 0 && !p.opsize) ||             // BLSR/BLSMSK/BLSI
            (op3 == 0xF6 && p.rep == 0xF2 && !p.opsize) ||          // MULX
            (op3 == 0xF5) ||                                        // BZHI/PDEP/PEXT
            (op3 == 0xF7);                                          // SHLX/SHRX/SARX
        if (bmi_shape) {
            if (L) { fault(ExecStatus::UnsupportedOpcode, rip); return true; }
            const int bsz = vex_w ? 8 : 4;
            const uint8_t sub = (modrm >> 3) & 7;
            uint64_t src1 = 0; // vvvv operand
            src1 = ReadReg(vreg, bsz);
            uint64_t src2 = 0; // r/m operand
            if (!ReadRM(rm, bsz, p.rex, src2)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
            const uint8_t dst_idx = static_cast<uint8_t>(reg | (p.rex_r ? 0x08 : 0x00));

            if (op3 == 0xF2 && (p.rep == 0 || p.rep == 0xF2)) {       // ANDN dst, src1, src2
                const uint64_t res = (~src1) & src2;
                WriteReg(dst_idx, bsz, res);
                SetFlag(Flags::CF, false); SetFlag(Flags::OF, false); SetFlag(Flags::SF, false);
                SetFlag(Flags::ZF, res == 0);
                commit(rip); return true;
            }
            if (op3 == 0xF7 && p.rep == 0 && !p.opsize) {           // BEXTR dst, src, (start<<8)|len
                const uint32_t ctl = static_cast<uint32_t>(src1);
                const uint32_t start = ctl & 0xFFu;
                const uint32_t len = (ctl >> 8) & 0xFFu;
                const uint32_t width = static_cast<uint32_t>(bsz) * 8u;
                uint64_t res = 0;
                if (start < width) {
                    const uint64_t shifted = src2 >> start;
                    const uint64_t mask = (len >= 64ull) ? ~0ull : ((1ull << len) - 1ull);
                    res = shifted & mask;
                }
                WriteReg(dst_idx, bsz, res);
                SetFlag(Flags::CF, false); SetFlag(Flags::OF, false); SetFlag(Flags::SF, false);
                SetFlag(Flags::ZF, res == 0);
                commit(rip); return true;
            }
            if (op3 == 0xF3 && p.rep == 0 && !p.opsize) {           // BLSR(/1) BLSMSK(/2) BLSI(/3)
                uint64_t res = 0;
                if (sub == 1) {            // BLSR: x & (x-1)
                    res = src2 & (src2 - 1);
                } else if (sub == 2) {     // BLSMSK: x ^ (x-1)
                    res = src2 ^ (src2 - 1);
                } else if (sub == 3) {     // BLSI: x & -x
                    res = src2 & (~src2 + 1);
                } else {
                    fault(ExecStatus::UnsupportedOpcode, rip);
                    return true;
                }
                WriteReg(dst_idx, bsz, res);
                SetFlag(Flags::CF, false); SetFlag(Flags::OF, false); SetFlag(Flags::SF, false);
                // SDM: for the BLS* family ZF reflects the SOURCE being zero.
                SetFlag(Flags::ZF, src2 == 0);
                commit(rip); return true;
            }
            if (op3 == 0xF0 && p.rep == 0xF2) {                     // RORX dst, src, imm8
                uint8_t imm = 0;
                if (!Fetch8(rip, imm)) { fault(ExecStatus::DecodeFault, rip); return true; }
                const uint32_t sh = imm & (bsz == 8 ? 63u : 31u);
                uint64_t res = 0;
                if (bsz == 8) {
                    res = (sh == 0) ? src2 : ((src2 >> sh) | (src2 << (64 - sh)));
                } else {
                    const uint32_t v = static_cast<uint32_t>(src2);
                    res = (sh == 0) ? v : ((v >> sh) | (v << (32 - sh)));
                }
                WriteReg(dst_idx, bsz, res);
                commit(rip); return true;
            }
            if (op3 == 0xF5) {
                if (p.rep == 0xF2) {                                // PDEP dst, src, mask
                    uint64_t res = 0;
                    int bit = 0;
                    for (int i = 0; i < bsz * 8; ++i) {
                        if (((src2 >> i) & 1ull) != 0ull) {
                            res |= ((src1 >> bit) & 1ull) << i;
                            ++bit;
                        }
                    }
                    WriteReg(dst_idx, bsz, res);
                    commit(rip); return true;
                }
                if (p.rep == 0xF3) {                                // PEXT dst, src, mask
                    uint64_t res = 0;
                    int bit = 0;
                    for (int i = 0; i < bsz * 8; ++i) {
                        if (((src2 >> i) & 1ull) != 0ull) {
                            res |= ((src1 >> i) & 1ull) << bit;
                            ++bit;
                        }
                    }
                    WriteReg(dst_idx, bsz, res);
                    commit(rip); return true;
                }
                if (p.rep == 0 && !p.opsize) {                      // BZHI dst, src, index(vvvv)
                    const int width = bsz * 8;
                    const uint64_t idx = src1 & 0xFFull;
                    uint64_t res = src2;
                    bool cf = false;
                    if (idx < static_cast<uint64_t>(width)) {
                        res = src2 & ((idx == 0) ? ~0ull : ((1ull << idx) - 1ull));
                    } else {
                        cf = true;                                  // index >= width: full result, CF=1
                    }
                    WriteReg(dst_idx, bsz, res);
                    SetFlag(Flags::CF, cf); SetFlag(Flags::OF, false); SetFlag(Flags::SF, false);
                    SetFlag(Flags::ZF, res == 0);
                    commit(rip); return true;
                }
                fault(ExecStatus::UnsupportedOpcode, rip);
                return true;
            }
            if (op3 == 0xF6 && p.rep == 0xF2) {                     // MULX dst_lo(=reg), dst_hi(=vvvv), src
                const uint64_t a = ReadReg(RDX, bsz);
                const uint64_t b = src2;
                // __extension__ keeps -Wpedantic quiet (GNU 128-bit mul);
                // identical semantics to a 64x64->128 multiply.
                __extension__ const unsigned __int128 prod =
                    static_cast<unsigned __int128>(a) * static_cast<unsigned __int128>(b);
                const uint64_t lo = static_cast<uint64_t>(prod);
                const uint64_t hi = static_cast<uint64_t>(prod >> 64);
                if (bsz == 4) {
                    WriteReg(dst_idx, 4, lo & 0xFFFFFFFFull);
                    WriteReg(vreg, 4, hi & 0xFFFFFFFFull);
                } else {
                    WriteReg(dst_idx, 8, lo);
                    WriteReg(vreg, 8, hi);
                }
                commit(rip); return true;
            }
            if (op3 == 0xF7) {                                      // SHLX/SARX/SHRX dst, src, shift(vvvv)
                const uint64_t sh = src1 & (bsz == 8 ? 63ull : 31ull);
                uint64_t res = 0;
                if (p.rep == 0xF2) {                                // SHRX: logical right
                    res = src2 >> sh;
                } else if (p.rep == 0xF3) {                         // SARX: arithmetic right
                    if (bsz == 8) res = static_cast<uint64_t>(static_cast<int64_t>(src2) >> sh);
                    else res = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(src2)) >> sh) & 0xFFFFFFFFull;
                } else if (p.opsize) {                              // SHLX (pp=66)
                    res = src2 << sh;
                } else {
                    fault(ExecStatus::UnsupportedOpcode, rip);
                    return true;
                }
                WriteReg(dst_idx, bsz, res);
                commit(rip); return true;
            }
        }

        if (map_select == 3) {
            // ---- 0F 3A map (VEX): RORX dst, src, imm8 ------------------------
            // gas ground truth: rorx $4, %ecx, %eax = C4 E3 7B F0 C1 04
            // (map 3, opcode F0, pp = F2). The only BMI2 op outside 0F38.
            if (opcode == 0xF0 && p.rep == 0xF2 && !p.opsize) {
                if (L) { fault(ExecStatus::UnsupportedOpcode, rip); return true; }
                uint8_t modrm = 0;
                if (!Fetch8(rip, modrm)) { fault(ExecStatus::DecodeFault, rip); return true; }
                ModRMOperand rm{}; uint8_t reg = 0;
                if (!DecodeModRM(rip, p, modrm, rm, reg)) {
                    fault(ExecStatus::DecodeFault, rip);
                    return true;
                }
                const int bsz = vex_w ? 8 : 4;
                uint64_t src2 = 0;
                if (!ReadRM(rm, bsz, p.rex, src2)) {
                    fault(ExecStatus::MemoryFault, rm.addr);
                    return true;
                }
                uint8_t imm = 0;
                if (!Fetch8(rip, imm)) { fault(ExecStatus::DecodeFault, rip); return true; }
                const uint32_t sh = imm & (bsz == 8 ? 63u : 31u);
                uint64_t res = 0;
                if (bsz == 8) {
                    res = (sh == 0) ? src2 : ((src2 >> sh) | (src2 << (64 - sh)));
                } else {
                    const uint32_t v = static_cast<uint32_t>(src2);
                    res = (sh == 0) ? v : ((v >> sh) | (v << (32 - sh)));
                }
                WriteReg(static_cast<uint8_t>(reg | (p.rex_r ? 0x08 : 0x00)), bsz, res);
                commit(rip);
                return true;
            }
        }

        fault(ExecStatus::UnsupportedOpcode, rip);
        return true;
    }

    fault(ExecStatus::UnsupportedOpcode, rip);
    return true;
}

// ---------------------------------------------------------------------------
// full-decoder block scanner
// ---------------------------------------------------------------------------
namespace {

// Scan-time bus: instruction fetches (inside the code span) see the REAL
// bytes; every other read succeeds with zeros and every write is discarded,
// so data accesses never fault a scan -- only decode errors can stop it.
class CodeScanBus final : public GuestMemoryBus {
public:
    CodeScanBus(uint64_t base, std::span<const uint8_t> code)
        : m_base(base), m_code(code) {}

    bool Read(uint64_t addr, void* dst, size_t size) override {
        if (addr >= m_base && addr - m_base + size <= m_code.size()) {
            std::memcpy(dst, m_code.data() + (addr - m_base), size);
            return true;
        }
        std::memset(dst, 0, size);
        return true;
    }
    bool Write(uint64_t, const void*, size_t) override { return true; }

private:
    uint64_t m_base;
    std::span<const uint8_t> m_code;
};

} // namespace

BlockInspectResult X86Interpreter::InspectBlock(std::span<const uint8_t> code,
                                                uint64_t guest_rip,
                                                size_t max_instructions,
                                                size_t max_bytes) {
    BlockInspectResult out{};
    CodeScanBus bus(guest_rip, code);
    CpuState cpu{};
    cpu.rip = guest_rip;
    // Nonzero GPRs: avoid div-by-zero style semantic faults on the probe.
    for (auto& g : cpu.gpr) g = 1;
    cpu.gpr[RSP] = guest_rip + code.size() + 2048; // writes are discarded anyway
    X86Interpreter probe(cpu, bus);

    uint64_t rip = guest_rip;
    for (size_t n = 0; n < max_instructions && (rip - guest_rip) < max_bytes; ++n) {
        const size_t off = static_cast<size_t>(rip - guest_rip);
        size_t pfx = 0;
        bool pfx66 = false, pfxF2 = false, pfxF3 = false;
        while (off + pfx < code.size()) {
            const uint8_t b = code[off + pfx];
            if (b == 0x66) { pfx66 = true; ++pfx; continue; }
            if (b == 0xF2) { pfxF2 = true; ++pfx; continue; }
            if (b == 0xF3) { pfxF3 = true; ++pfx; continue; }
            if (b == 0x67 || b == 0xF0 ||
                b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65 ||
                (b >= 0x40 && b <= 0x4F)) {
                ++pfx;
            } else {
                break;
            }
        }
        if (off + pfx >= code.size()) {
            out.status = ExecStatus::DecodeFault;
            out.fault_addr = rip;
            return out;
        }
        const uint8_t op = code[off + pfx];
        const uint8_t op2 = (op == 0x0F && off + pfx + 1 < code.size()) ? code[off + pfx + 1] : 0;
        const uint8_t modrm = (off + pfx + 1 < code.size()) ? code[off + pfx + 1] : 0;
        const uint8_t ff_sub = (modrm >> 3) & 7;

        // Round 20: classify the host-dependent instructions the direct
        // execution backend must intercept (see PatchKind). VEX BMI encodings
        // VERIFIED AGAINST GAS (as --64; objdump -d):
        //   ANDN  = C4 E2 60 F2 C1  (0F38 F2, pp=00 canonical -- hardware
        //                              ignores pp for GPR ops, pp=F2 also decodes)
        //   BEXTR = C4 E2 70 F7 C3  (0F38 F7, pp=00)
        //   BLSR  = C4 E2 60 F3 C9  (0F38 F3, pp=00)
        //   MULX  = C4 E2 6B F6 D9  (0F38 F6, pp=F2)
        //   BZHI  = C4 E2 70 F5 C3  (0F38 F5, pp=00)
        //   PDEP  = C4 E2 63 F5 C1  (0F38 F5, pp=F2)
        //   PEXT  = C4 E2 62 F5 C1  (0F38 F5, pp=F3)
        //   SHLX  = C4 E2 71 F7 C3  (0F38 F7, pp=66)
        //   SARX  = C4 E2 72 F7 C3  (0F38 F7, pp=F3)
        //   SHRX  = C4 E2 73 F7 C3  (0F38 F7, pp=F2)
        //   RORX  = C4 E3 7B F0 C1 04 (0F3A F0, pp=F2 -- MAP 3, not 0F38!)
        PatchKind kind = PatchKind::None;
        if (op == 0x0F) {
            if (op2 == 0x05) kind = PatchKind::Syscall;
            else if (op2 == 0x31) kind = PatchKind::Rdtsc;
            else if (op2 == 0xA2) kind = PatchKind::Cpuid;
            else if (op2 == 0x01 && off + pfx + 2 < code.size() &&
                     code[off + pfx + 2] == 0xF9) kind = PatchKind::Rdtscp;
            else if ((op2 == 0x78 || op2 == 0x79) && (pfx66 || pfxF2)) kind = PatchKind::Sse4a;
            else if (op2 == 0x2B && (pfxF2 || pfxF3)) kind = PatchKind::Sse4a;
            else if ((op2 == 0xBC || op2 == 0xBD) && pfxF3) kind = PatchKind::Tzcnt;
            else if (op2 == 0x38 && !pfxF2 && off + pfx + 2 < code.size()) {
                // legacy 0F38 F0/F1 without F2 = MOVBE (the F2 form is CRC32,
                // which every host this emulator targets serves natively).
                const uint8_t op3 = code[off + pfx + 2];
                if (op3 == 0xF0 || op3 == 0xF1) kind = PatchKind::Movbe;
            }
        } else if (op == 0xC4 && off + pfx + 3 < code.size()) {
            const uint8_t map = code[off + pfx + 1] & 0x1Fu;
            const uint8_t pp = code[off + pfx + 2] & 0x3u;
            const uint8_t vop = code[off + pfx + 3];
            if (map == 2u) {
                if (vop == 0xF2 && (pp == 0u || pp == 3u)) kind = PatchKind::Bmi1;  // andn
                else if (vop == 0xF7 && pp == 0u) kind = PatchKind::Bmi1;         // bextr
                else if (vop == 0xF3 && pp == 0u) kind = PatchKind::Bmi1;         // blsr/smsk/si
                else if (vop == 0xF0 && pp == 3u) kind = PatchKind::Bmi2;         // rorx (0F38 form never emitted, but decodes)
                else if (vop == 0xF5) kind = (pp == 0u) ? PatchKind::Bmi2         // bzhi
                                            : (pp == 2u) ? PatchKind::Bmi2        // pext
                                            : (pp == 3u) ? PatchKind::Bmi2        // pdep
                                                         : PatchKind::None;
                else if (vop == 0xF6 && pp == 3u) kind = PatchKind::Bmi2;         // mulx
                else if (vop == 0xF7) kind = (pp == 1u || pp == 2u || pp == 3u)
                                            ? PatchKind::Bmi2                      // shlx/sarx/shrx
                                            : PatchKind::None;
            } else if (map == 3u) {
                if (vop == 0xF0 && pp == 3u) kind = PatchKind::Bmi2;              // rorx (0F3A)
            }
        }

        const bool is_ret = (op == 0xC3 || op == 0xC2);
        const bool is_call = (op == 0xE8) || (op == 0xFF && ff_sub == 2);
        const bool is_jmp = (op == 0xE9 || op == 0xEB) || (op == 0xFF && ff_sub == 4);
        const bool is_jcc = (op >= 0x70 && op <= 0x7F) ||
                            (op == 0x0F && op2 >= 0x80 && op2 <= 0x8F);
        const bool is_loop = (op >= 0xE0 && op <= 0xE3);
        const bool is_syscall = (op == 0x0F && op2 == 0x05);
        const bool is_hlt = (op == 0xF4);

        // Consume the instruction through the REAL decoder.
        cpu.rip = rip;
        const RunResult step = probe.Step();
        const uint64_t next = cpu.rip;

        out.instruction_count = n + 1;

        // ---- the SEQUENTIAL end of this instruction ---------------------------
        // The decoder's `next` is the BRANCH TARGET for a taken branch: the
        // scan state's zeroed flags take every jnz/jo/etc, so a backward
        // target used to fall into the `rip + pfx + 1` fallback -- ONE BYTE
        // short -- and the successor entry trap got armed INSIDE the branch
        // encoding (round-20 defect: `jnz -8` at offset 14 produced
        // next_rip=15, ud2 over the imm8, and a corrupted stream). The end is
        // now derived from the ENCODING for every control-transfer form:
        // pfx + opcode + operand bytes, with a full ModRM/SIB decode for the
        // indirect call/jmp (0xFF /2 /4) forms. Unknown forms arm nothing.
        const auto insn_end = [&]() -> uint64_t {
            const size_t opc = off + pfx;               // opcode offset in code
            const uint8_t o1 = code[opc];
            const uint8_t o2v = (opc + 1 < code.size()) ? code[opc + 1] : 0;
            size_t len = 0;
            if (o1 == 0xC3) len = pfx + 1;                              // ret
            else if (o1 == 0xC2) len = pfx + 1 + 2;                      // ret imm16
            else if (o1 == 0xE8) len = pfx + 1 + 4;                      // call rel32
            else if (o1 == 0xE9) len = pfx + 1 + 4;                      // jmp rel32
            else if (o1 == 0xEB) len = pfx + 1 + 1;                      // jmp rel8
            else if (o1 >= 0x70 && o1 <= 0x7F) len = pfx + 1 + 1;        // jcc rel8
            else if (o1 >= 0xE0 && o1 <= 0xE3) len = pfx + 1 + 1;        // loop*/jrcxz
            else if (o1 == 0x0F && o2v == 0x05) len = pfx + 2;           // syscall
            else if (o1 == 0x0F && (o2v & 0xF0) == 0x80) len = pfx + 2 + 4; // jcc rel32
            else if (o1 == 0xF4) len = pfx + 1;                          // hlt
            else if (o1 == 0xFF && (ff_sub == 2 || ff_sub == 4)) {
                // call/jmp r/m: prefixes + FF + modrm [+ sib] [+ disp].
                size_t l = pfx + 2;
                const uint8_t mod = (modrm >> 6) & 3;
                const uint8_t rm = modrm & 7;
                if (mod != 3) {
                    if (rm == 4 && off + l < code.size()) {             // SIB byte
                        const uint8_t sib = code[off + l];
                        l += 1;
                        if (mod == 0 && (sib & 7) == 5) l += 4;          // base=101,mod=0: disp32
                    } else if (mod == 0 && rm == 5) {
                        l += 4;                                          // rip-rel disp32
                    } else if (mod == 1) {
                        l += 1;
                    } else if (mod == 2) {
                        l += 4;
                    }
                }
                len = (off + l <= code.size()) ? l : 0;
            }
            if (len == 0) {
                return 0;   // unknown encoding: the caller arms NOTHING
            }
            return rip + len;
        }();

        out.next_rip = (is_syscall || is_ret || is_call || is_jmp || is_jcc ||
                         is_loop || is_hlt)
                            ? insn_end
                            : (next > rip ? next : rip + pfx + 1);
        if (out.next_rip == 0) {
            // An indirect/unknown terminator: still ends the block, but its
            // fall-through is not derivable -- report the conservative end
            // (opcode+modrm minimum) and let the caller see ends_in_branch
            // without a valid successor to arm.
            out.next_rip = rip + pfx + 2;
            out.code_size = static_cast<size_t>(out.next_rip - guest_rip);
        } else {
            out.code_size = static_cast<size_t>(out.next_rip - guest_rip);
        }

        if (kind != PatchKind::None) {
            PatchSiteInfo site{};
            site.offset = static_cast<uint32_t>(off);
            site.length = static_cast<uint32_t>(out.next_rip - rip);
            site.kind = kind;
            out.sites.push_back(site);
        }

        if (is_syscall) {
            out.contains_syscall = true;
            out.ends_in_branch = true;
            return out;
        }
        if (is_ret) { out.ends_in_ret = true; return out; }
        if (is_call) { out.ends_in_call = true; return out; }
        if (is_jmp || is_jcc || is_loop) { out.ends_in_branch = true; return out; }
        if (is_hlt) { out.status = ExecStatus::Halted; return out; }
        if (step.status != ExecStatus::Running || next <= rip) {
            out.status = (step.status != ExecStatus::Running) ? step.status : ExecStatus::DecodeFault;
            out.fault_addr = rip;
            return out;
        }
        rip = next;
    }
    out.status = ExecStatus::InstructionLimit;
    out.next_rip = rip;
    return out;
}

} // namespace PS5::CPU
