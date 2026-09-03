// ============================================================================
// ProsperoLayer RDNA2 Core - Extended x86-64 Interpreter implementation
// ============================================================================
#include "cpu/x86_64_interpreter.hpp"

#include <cmath>
#include <cstring>
#include <limits>

namespace PS5::CPU {

// ---------------------------------------------------------------------------
// FlatMemoryBus
// ---------------------------------------------------------------------------
bool FlatMemoryBus::InRange(uint64_t addr, size_t size) const {
    if (addr < m_base) return false;
    const uint64_t off = addr - m_base;
    if (off > m_storage.size()) return false;
    if (size > m_storage.size() - off) return false;
    return true;
}

bool FlatMemoryBus::Read(uint64_t addr, void* dst, size_t size) {
    if (!InRange(addr, size)) return false;
    std::memcpy(dst, m_storage.data() + (addr - m_base), size);
    return true;
}

bool FlatMemoryBus::Write(uint64_t addr, const void* src, size_t size) {
    if (!InRange(addr, size)) return false;
    std::memcpy(m_storage.data() + (addr - m_base), src, size);
    return true;
}

bool FlatMemoryBus::LoadBlob(uint64_t addr, std::span<const uint8_t> bytes) {
    return Write(addr, bytes.data(), bytes.size());
}

// ---------------------------------------------------------------------------
// static helpers
// ---------------------------------------------------------------------------
uint64_t X86Interpreter::Mask(int size) {
    switch (size) {
        case 1: return 0xFFull;
        case 2: return 0xFFFFull;
        case 4: return 0xFFFFFFFFull;
        default: return ~0ull;
    }
}

uint64_t X86Interpreter::SignExtend(uint64_t value, int size) {
    switch (size) {
        case 1: return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(value)));
        case 2: return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(value)));
        case 4: return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(value)));
        default: return value;
    }
}

// ---------------------------------------------------------------------------
// fetch
// ---------------------------------------------------------------------------
bool X86Interpreter::Fetch8(uint64_t& rip, uint8_t& out) {
    if (!m_mem.Read(rip, &out, 1)) return false;
    rip += 1;
    return true;
}
bool X86Interpreter::Fetch16(uint64_t& rip, uint16_t& out) {
    if (!m_mem.Read(rip, &out, 2)) return false;
    rip += 2;
    return true;
}
bool X86Interpreter::Fetch32(uint64_t& rip, uint32_t& out) {
    if (!m_mem.Read(rip, &out, 4)) return false;
    rip += 4;
    return true;
}
bool X86Interpreter::Fetch64(uint64_t& rip, uint64_t& out) {
    if (!m_mem.Read(rip, &out, 8)) return false;
    rip += 8;
    return true;
}

// ---------------------------------------------------------------------------
// operand size + register file
// ---------------------------------------------------------------------------
int X86Interpreter::OperandSize(const Prefixes& p) const {
    if (p.rex_w) return 8;
    if (p.opsize) return 2;
    return 4;
}

uint64_t X86Interpreter::ReadReg(uint8_t idx, int size) const {
    const uint64_t v = m_state.gpr[idx];
    return v & Mask(size);
}

void X86Interpreter::WriteReg(uint8_t idx, int size, uint64_t value) {
    if (size == 4) {
        // 32-bit writes zero-extend into the full 64-bit register.
        m_state.gpr[idx] = value & 0xFFFFFFFFull;
    } else if (size == 8) {
        m_state.gpr[idx] = value;
    } else {
        const uint64_t m = Mask(size);
        m_state.gpr[idx] = (m_state.gpr[idx] & ~m) | (value & m);
    }
}

// Byte register access. Without REX, indices 4..7 map to AH/CH/DH/BH.
uint64_t X86Interpreter::ReadRegByte(uint8_t idx, bool rex) const {
    if (!rex && idx >= 4 && idx <= 7) {
        return (m_state.gpr[idx - 4] >> 8) & 0xFF;
    }
    return m_state.gpr[idx] & 0xFF;
}

void X86Interpreter::WriteRegByte(uint8_t idx, bool rex, uint8_t value) {
    if (!rex && idx >= 4 && idx <= 7) {
        uint64_t& r = m_state.gpr[idx - 4];
        r = (r & ~0xFF00ull) | (static_cast<uint64_t>(value) << 8);
        return;
    }
    uint64_t& r = m_state.gpr[idx];
    r = (r & ~0xFFull) | value;
}

// ---------------------------------------------------------------------------
// ModRM + SIB decoding
// ---------------------------------------------------------------------------
bool X86Interpreter::DecodeModRM(uint64_t& rip, const Prefixes& p, uint8_t modrm,
                                 ModRMOperand& out, uint8_t& reg_field) {
    const uint8_t mod = modrm >> 6;
    reg_field = ((modrm >> 3) & 0x07) | (p.rex_r ? 0x08 : 0x00);
    uint8_t rm = (modrm & 0x07);

    if (mod == 3) {
        out.is_reg = true;
        out.reg = rm | (p.rex_b ? 0x08 : 0x00);
        return true;
    }

    out.is_reg = false;
    uint64_t addr = 0;
    bool rip_relative = false;

    if (rm == 4) {
        // SIB byte follows.
        uint8_t sib = 0;
        if (!Fetch8(rip, sib)) return false;
        const uint8_t scale = sib >> 6;
        const uint8_t index = ((sib >> 3) & 0x07) | (p.rex_x ? 0x08 : 0x00);
        const uint8_t base = (sib & 0x07) | (p.rex_b ? 0x08 : 0x00);

        uint64_t index_val = 0;
        if (((sib >> 3) & 0x07) != 4 || p.rex_x) {
            // index==4 without REX.X means "no index"
            index_val = m_state.gpr[index] << scale;
        }

        if ((sib & 0x07) == 5 && mod == 0) {
            // no base; disp32 follows
            uint32_t disp = 0;
            if (!Fetch32(rip, disp)) return false;
            addr = SignExtend(disp, 4) + index_val;
        } else {
            addr = m_state.gpr[base] + index_val;
        }
    } else if (rm == 5 && mod == 0) {
        // RIP-relative: disp32 added to rip AFTER the full instruction is
        // decoded. We resolve it against the running rip and add disp now,
        // then a later immediate fetch would shift rip; to stay correct we
        // capture disp and fix up below.
        rip_relative = true;
        uint32_t disp = 0;
        if (!Fetch32(rip, disp)) return false;
        addr = SignExtend(disp, 4); // base (rip) added by caller-agnostic fixup
        // We approximate rip-relative against the address right after this
        // displacement. Callers that fetch further immediates are rare for the
        // instructions we model with rip-relative operands.
        addr += rip;
        (void)rip_relative;
    } else {
        const uint8_t base = rm | (p.rex_b ? 0x08 : 0x00);
        addr = m_state.gpr[base];
    }

    if (mod == 1) {
        uint8_t disp = 0;
        if (!Fetch8(rip, disp)) return false;
        addr += SignExtend(disp, 1);
    } else if (mod == 2) {
        uint32_t disp = 0;
        if (!Fetch32(rip, disp)) return false;
        addr += SignExtend(disp, 4);
    }

    // Round 11: an FS segment override rebases the effective address onto the
    // thread pointer (fs_base). This is how guest TLS is addressed
    // (initial-exec model: mov eax, fs:[offset]).
    if (p.seg_fs) {
        addr += m_state.fs_base;
    }

    out.addr = addr;
    return true;
}

bool X86Interpreter::ReadRM(const ModRMOperand& op, int size, bool rex, uint64_t& out) {
    if (op.is_reg) {
        out = (size == 1) ? ReadRegByte(op.reg, rex) : ReadReg(op.reg, size);
        return true;
    }
    uint64_t buf = 0;
    if (!m_mem.Read(op.addr, &buf, static_cast<size_t>(size))) return false;
    out = buf & Mask(size);
    return true;
}

bool X86Interpreter::WriteRM(const ModRMOperand& op, int size, bool rex, uint64_t value) {
    if (op.is_reg) {
        if (size == 1) WriteRegByte(op.reg, rex, static_cast<uint8_t>(value));
        else WriteReg(op.reg, size, value);
        return true;
    }
    const uint64_t masked = value & Mask(size);
    return m_mem.Write(op.addr, &masked, static_cast<size_t>(size));
}

// ---------------------------------------------------------------------------
// flags
// ---------------------------------------------------------------------------
void X86Interpreter::SetFlag(uint64_t mask, bool on) {
    if (on) m_state.rflags |= mask;
    else m_state.rflags &= ~mask;
}
bool X86Interpreter::GetFlag(uint64_t mask) const {
    return (m_state.rflags & mask) != 0;
}

static bool ParityEven(uint8_t v) {
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (v & 1) == 0;
}

void X86Interpreter::UpdateSZP(uint64_t res, int size) {
    const uint64_t m = Mask(size);
    const uint64_t r = res & m;
    SetFlag(Flags::ZF, r == 0);
    const uint64_t sign_bit = 1ull << (size * 8 - 1);
    SetFlag(Flags::SF, (r & sign_bit) != 0);
    SetFlag(Flags::PF, ParityEven(static_cast<uint8_t>(r & 0xFF)));
}

void X86Interpreter::UpdateFlagsLogic(uint64_t res, int size) {
    UpdateSZP(res, size);
    SetFlag(Flags::CF, false);
    SetFlag(Flags::OF, false);
    SetFlag(Flags::AF, false);
}

void X86Interpreter::UpdateFlagsAdd(uint64_t a, uint64_t b, uint64_t res, int size,
                                    bool with_carry, bool carry_in) {
    const uint64_t m = Mask(size);
    const uint64_t r = res & m;
    UpdateSZP(r, size);
    const uint64_t sign = 1ull << (size * 8 - 1);
    // carry out of the top bit
    const uint64_t ci = (with_carry && carry_in) ? 1u : 0u;
    // Use wider reasoning: CF if unsigned sum overflowed
    bool cf;
    if (size == 8) {
        // detect via wraparound
        cf = (r < (a & m)) || (ci && r == (a & m));
    } else {
        const uint64_t full = (a & m) + (b & m) + ci;
        cf = (full & ~m) != 0;
    }
    SetFlag(Flags::CF, cf);
    SetFlag(Flags::AF, (((a ^ b ^ r) & 0x10) != 0));
    const bool of = (~(a ^ b) & (a ^ r) & sign) != 0;
    SetFlag(Flags::OF, of);
}

void X86Interpreter::UpdateFlagsSub(uint64_t a, uint64_t b, uint64_t res, int size,
                                    bool with_borrow, bool borrow_in) {
    const uint64_t m = Mask(size);
    const uint64_t r = res & m;
    UpdateSZP(r, size);
    const uint64_t sign = 1ull << (size * 8 - 1);
    const uint64_t bi = (with_borrow && borrow_in) ? 1u : 0u;
    const bool cf = (a & m) < ((b & m) + bi);
    SetFlag(Flags::CF, cf);
    SetFlag(Flags::AF, (((a ^ b ^ r) & 0x10) != 0));
    const bool of = ((a ^ b) & (a ^ r) & sign) != 0;
    SetFlag(Flags::OF, of);
}

// tttn condition codes (low nibble of jcc/setcc/cmovcc opcodes).
bool X86Interpreter::EvalCondition(uint8_t tttn) const {
    const bool cf = GetFlag(Flags::CF);
    const bool zf = GetFlag(Flags::ZF);
    const bool sf = GetFlag(Flags::SF);
    const bool of = GetFlag(Flags::OF);
    const bool pf = GetFlag(Flags::PF);
    switch (tttn) {
        case 0x0: return of;                 // O
        case 0x1: return !of;                // NO
        case 0x2: return cf;                 // B/C/NAE
        case 0x3: return !cf;                // AE/NB/NC
        case 0x4: return zf;                 // E/Z
        case 0x5: return !zf;                // NE/NZ
        case 0x6: return cf || zf;           // BE/NA
        case 0x7: return !cf && !zf;         // A/NBE
        case 0x8: return sf;                 // S
        case 0x9: return !sf;                // NS
        case 0xA: return pf;                 // P/PE
        case 0xB: return !pf;                // NP/PO
        case 0xC: return sf != of;           // L/NGE
        case 0xD: return sf == of;           // GE/NL
        case 0xE: return zf || (sf != of);   // LE/NG
        case 0xF: return !zf && (sf == of);  // G/NLE
        default: return false;
    }
}

// ---------------------------------------------------------------------------
// main loop
// ---------------------------------------------------------------------------
RunResult X86Interpreter::Run(size_t instruction_limit, uint64_t stop_rip) {
    RunResult r{};
    for (; r.executed < instruction_limit; ++r.executed) {
        if (m_state.rip == stop_rip) {
            r.status = ExecStatus::Returned;
            return r;
        }
        const RunResult step = Step();
        if (step.status != ExecStatus::Running) {
            r.status = step.status;
            r.fault_addr = step.fault_addr;
            r.executed += 1;
            return r;
        }
    }
    r.status = ExecStatus::InstructionLimit;
    return r;
}

RunResult X86Interpreter::Step() {
    RunResult r{};
    r.status = ExecStatus::Running;
    uint64_t rip = m_state.rip;

    Prefixes p{};
    uint8_t byte = 0;

    // --- VEX (round 16): C4/C5 are VEX prefixes in 64-bit mode (they were
    // LES/LDS, invalid in long mode). AVX.256 fails closed (128-bit XMM file
    // only); AVX.128 runs the same semantics as SSE with 3-operand form. ---
    {
        uint64_t probe = rip;
        uint8_t first = 0;
        if (!Fetch8(probe, first)) { r.status = ExecStatus::DecodeFault; r.fault_addr = rip; return r; }
        if (first == 0xC4 || first == 0xC5) {
            rip = probe;
            if (!ExecVex(rip, first, r)) {
                if (r.status == ExecStatus::Running) {
                    r.status = ExecStatus::UnsupportedOpcode;
                }
            }
            return r;
        }
    }

    // --- prefixes ---
    for (;;) {
        if (!Fetch8(rip, byte)) { r.status = ExecStatus::DecodeFault; r.fault_addr = rip; return r; }
        if (byte == 0x66) { p.opsize = true; continue; }
        if (byte == 0x67) { p.addrsize = true; continue; }
        if (byte == 0xF0) { p.lock = true; continue; } // lock: tracked for atomic ops
        if (byte == 0xF2) { p.rep = 0xF2; continue; }
        if (byte == 0xF3) { p.rep = 0xF3; continue; }
        if (byte == 0x2E || byte == 0x36 || byte == 0x3E ||
            byte == 0x26 || byte == 0x65) {
            continue; // CS/SS/DS/ES/GS overrides: flat model, ignored
        }
        if (byte == 0x64) {
            // FS override (round 11): routes memory operands through the
            // thread pointer (CpuState::fs_base) -- the TLS access model.
            p.seg_fs = true;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4F) {
            p.rex = true;
            p.rex_w = (byte & 0x08) != 0;
            p.rex_r = (byte & 0x04) != 0;
            p.rex_x = (byte & 0x02) != 0;
            p.rex_b = (byte & 0x01) != 0;
            continue;
        }
        break;
    }

    const uint8_t opcode = byte;
    const int osz = OperandSize(p);

    auto commit = [&](void) { m_state.rip = rip; };
    auto fault = [&](ExecStatus s, uint64_t addr) { r.status = s; r.fault_addr = addr; };

    // Helper for the standard ALU group form (r/m and r).
    auto alu_rm_r = [&](int size, auto op, bool store, bool reg_is_dest) -> bool {
        uint8_t modrm = 0;
        if (!Fetch8(rip, modrm)) { fault(ExecStatus::DecodeFault, rip); return false; }
        ModRMOperand rm{}; uint8_t reg = 0;
        if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return false; }
        uint64_t rm_val = 0;
        if (!ReadRM(rm, size, p.rex, rm_val)) { fault(ExecStatus::MemoryFault, rm.addr); return false; }
        const uint64_t reg_val = (size == 1) ? ReadRegByte(reg, p.rex) : ReadReg(reg, size);
        uint64_t a, b;
        if (reg_is_dest) { a = reg_val; b = rm_val; } else { a = rm_val; b = reg_val; }
        const uint64_t res = op(a, b);
        if (store) {
            if (reg_is_dest) {
                if (size == 1) WriteRegByte(reg, p.rex, static_cast<uint8_t>(res));
                else WriteReg(reg, size, res);
            } else if (!WriteRM(rm, size, p.rex, res)) { fault(ExecStatus::MemoryFault, rm.addr); return false; }
        }
        return true;
    };

    switch (opcode) {
        // ---- NOP / basic ----
        case 0x90: // nop (also xchg rax,rax)
            commit(); return r;
        case 0xF4: // hlt
            commit(); r.status = ExecStatus::Halted; return r;
        case 0x9B: // fwait -- no pending x87 exceptions are modelled
            commit(); return r;
        // Round 29: the complete x87 engine (interpreter-side model).
        case 0xD8: case 0xD9: case 0xDA: case 0xDB:
        case 0xDC: case 0xDD: case 0xDE: case 0xDF:
            if (ExecX87(rip, p, opcode, r)) {
                if (r.status == ExecStatus::Running) commit();
                return r;
            }
            return r;   // ExecX87 failed-closed: status already set
        case 0xCC: // int3 -> treat as halt for debugging
            commit(); r.status = ExecStatus::Halted; return r;

        // ---- ALU groups: ADD/OR/ADC/SBB/AND/SUB/XOR/CMP ----
        // Each group has 6 forms: 00-05 style. We cover /r r/m,r and r,r/m and
        // AL/eAX,imm plus the group1 (0x80..0x83) immediate forms below.
        case 0x00: case 0x01: case 0x02: case 0x03: { // ADD
            const bool byte_form = (opcode & 1) == 0;
            const bool reg_is_dest = (opcode & 2) != 0;
            const int size = byte_form ? 1 : osz;
            if (!alu_rm_r(size, [&](uint64_t a, uint64_t b){ uint64_t res=a+b; UpdateFlagsAdd(a,b,res,size,false,false); return res; }, true, reg_is_dest)) return r;
            commit(); return r;
        }
        case 0x08: case 0x09: case 0x0A: case 0x0B: { // OR
            const bool byte_form = (opcode & 1) == 0;
            const bool reg_is_dest = (opcode & 2) != 0;
            const int size = byte_form ? 1 : osz;
            if (!alu_rm_r(size, [&](uint64_t a, uint64_t b){ uint64_t res=a|b; UpdateFlagsLogic(res,size); return res; }, true, reg_is_dest)) return r;
            commit(); return r;
        }
        case 0x10: case 0x11: case 0x12: case 0x13: { // ADC
            const bool byte_form = (opcode & 1) == 0;
            const bool reg_is_dest = (opcode & 2) != 0;
            const int size = byte_form ? 1 : osz;
            const bool cin = GetFlag(Flags::CF);
            if (!alu_rm_r(size, [&](uint64_t a, uint64_t b){ uint64_t res=a+b+(cin?1:0); UpdateFlagsAdd(a,b,res,size,true,cin); return res; }, true, reg_is_dest)) return r;
            commit(); return r;
        }
        case 0x18: case 0x19: case 0x1A: case 0x1B: { // SBB
            const bool byte_form = (opcode & 1) == 0;
            const bool reg_is_dest = (opcode & 2) != 0;
            const int size = byte_form ? 1 : osz;
            const bool bin = GetFlag(Flags::CF);
            if (!alu_rm_r(size, [&](uint64_t a, uint64_t b){ uint64_t res=a-b-(bin?1:0); UpdateFlagsSub(a,b,res,size,true,bin); return res; }, true, reg_is_dest)) return r;
            commit(); return r;
        }
        case 0x20: case 0x21: case 0x22: case 0x23: { // AND
            const bool byte_form = (opcode & 1) == 0;
            const bool reg_is_dest = (opcode & 2) != 0;
            const int size = byte_form ? 1 : osz;
            if (!alu_rm_r(size, [&](uint64_t a, uint64_t b){ uint64_t res=a&b; UpdateFlagsLogic(res,size); return res; }, true, reg_is_dest)) return r;
            commit(); return r;
        }
        case 0x28: case 0x29: case 0x2A: case 0x2B: { // SUB
            const bool byte_form = (opcode & 1) == 0;
            const bool reg_is_dest = (opcode & 2) != 0;
            const int size = byte_form ? 1 : osz;
            if (!alu_rm_r(size, [&](uint64_t a, uint64_t b){ uint64_t res=a-b; UpdateFlagsSub(a,b,res,size,false,false); return res; }, true, reg_is_dest)) return r;
            commit(); return r;
        }
        case 0x30: case 0x31: case 0x32: case 0x33: { // XOR
            const bool byte_form = (opcode & 1) == 0;
            const bool reg_is_dest = (opcode & 2) != 0;
            const int size = byte_form ? 1 : osz;
            if (!alu_rm_r(size, [&](uint64_t a, uint64_t b){ uint64_t res=a^b; UpdateFlagsLogic(res,size); return res; }, true, reg_is_dest)) return r;
            commit(); return r;
        }
        case 0x38: case 0x39: case 0x3A: case 0x3B: { // CMP
            const bool byte_form = (opcode & 1) == 0;
            const bool reg_is_dest = (opcode & 2) != 0;
            const int size = byte_form ? 1 : osz;
            if (!alu_rm_r(size, [&](uint64_t a, uint64_t b){ uint64_t res=a-b; UpdateFlagsSub(a,b,res,size,false,false); return res; }, false, reg_is_dest)) return r;
            commit(); return r;
        }

        // ---- AL/eAX, imm forms (04,05,0C,0D,...,3C,3D) ----
        case 0x04: case 0x0C: case 0x14: case 0x1C:
        case 0x24: case 0x2C: case 0x34: case 0x3C: { // <op> AL, imm8
            uint8_t imm = 0;
            if (!Fetch8(rip, imm)) { fault(ExecStatus::DecodeFault, rip); return r; }
            const uint64_t a = ReadRegByte(RAX, p.rex);
            const uint64_t b = imm;
            uint64_t res = 0;
            switch (opcode) {
                case 0x04: res=a+b; UpdateFlagsAdd(a,b,res,1,false,false); WriteRegByte(RAX,p.rex,(uint8_t)res); break;
                case 0x0C: res=a|b; UpdateFlagsLogic(res,1); WriteRegByte(RAX,p.rex,(uint8_t)res); break;
                case 0x14: { bool c=GetFlag(Flags::CF); res=a+b+(c?1:0); UpdateFlagsAdd(a,b,res,1,true,c); WriteRegByte(RAX,p.rex,(uint8_t)res);} break;
                case 0x1C: { bool c=GetFlag(Flags::CF); res=a-b-(c?1:0); UpdateFlagsSub(a,b,res,1,true,c); WriteRegByte(RAX,p.rex,(uint8_t)res);} break;
                case 0x24: res=a&b; UpdateFlagsLogic(res,1); WriteRegByte(RAX,p.rex,(uint8_t)res); break;
                case 0x2C: res=a-b; UpdateFlagsSub(a,b,res,1,false,false); WriteRegByte(RAX,p.rex,(uint8_t)res); break;
                case 0x34: res=a^b; UpdateFlagsLogic(res,1); WriteRegByte(RAX,p.rex,(uint8_t)res); break;
                case 0x3C: res=a-b; UpdateFlagsSub(a,b,res,1,false,false); break; // cmp, no store
            }
            commit(); return r;
        }
        case 0x05: case 0x0D: case 0x15: case 0x1D:
        case 0x25: case 0x2D: case 0x35: case 0x3D: { // <op> eAX, imm32(sx)
            uint32_t imm = 0;
            if (!Fetch32(rip, imm)) { fault(ExecStatus::DecodeFault, rip); return r; }
            const uint64_t a = ReadReg(RAX, osz);
            const uint64_t b = (osz == 8) ? SignExtend(imm, 4) : (imm & Mask(osz));
            uint64_t res = 0;
            switch (opcode) {
                case 0x05: res=a+b; UpdateFlagsAdd(a,b,res,osz,false,false); WriteReg(RAX,osz,res); break;
                case 0x0D: res=a|b; UpdateFlagsLogic(res,osz); WriteReg(RAX,osz,res); break;
                case 0x15: { bool c=GetFlag(Flags::CF); res=a+b+(c?1:0); UpdateFlagsAdd(a,b,res,osz,true,c); WriteReg(RAX,osz,res);} break;
                case 0x1D: { bool c=GetFlag(Flags::CF); res=a-b-(c?1:0); UpdateFlagsSub(a,b,res,osz,true,c); WriteReg(RAX,osz,res);} break;
                case 0x25: res=a&b; UpdateFlagsLogic(res,osz); WriteReg(RAX,osz,res); break;
                case 0x2D: res=a-b; UpdateFlagsSub(a,b,res,osz,false,false); WriteReg(RAX,osz,res); break;
                case 0x35: res=a^b; UpdateFlagsLogic(res,osz); WriteReg(RAX,osz,res); break;
                case 0x3D: res=a-b; UpdateFlagsSub(a,b,res,osz,false,false); break; // cmp
            }
            commit(); return r;
        }

        // ---- group1: 80/81/83 <op> r/m, imm ----
        case 0x80: case 0x81: case 0x83: {
            const int size = (opcode == 0x80) ? 1 : osz;
            uint8_t modrm = 0;
            if (!Fetch8(rip, modrm)) { fault(ExecStatus::DecodeFault, rip); return r; }
            ModRMOperand rm{}; uint8_t reg = 0;
            if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return r; }
            uint64_t imm = 0;
            if (opcode == 0x80) { uint8_t v=0; if(!Fetch8(rip,v)){fault(ExecStatus::DecodeFault,rip);return r;} imm=v; }
            else if (opcode == 0x83) { uint8_t v=0; if(!Fetch8(rip,v)){fault(ExecStatus::DecodeFault,rip);return r;} imm=SignExtend(v,1)&Mask(size); }
            else { uint32_t v=0; if(!Fetch32(rip,v)){fault(ExecStatus::DecodeFault,rip);return r;} imm=(size==8)?(SignExtend(v,4)&Mask(8)):(v&Mask(size)); }
            uint64_t a=0;
            if (!ReadRM(rm, size, p.rex, a)) { fault(ExecStatus::MemoryFault, rm.addr); return r; }
            const uint8_t sub = (modrm >> 3) & 0x07;
            uint64_t res=0; bool store=true;
            switch (sub) {
                case 0: res=a+imm; UpdateFlagsAdd(a,imm,res,size,false,false); break;       // ADD
                case 1: res=a|imm; UpdateFlagsLogic(res,size); break;                       // OR
                case 2: { bool c=GetFlag(Flags::CF); res=a+imm+(c?1:0); UpdateFlagsAdd(a,imm,res,size,true,c);} break; // ADC
                case 3: { bool c=GetFlag(Flags::CF); res=a-imm-(c?1:0); UpdateFlagsSub(a,imm,res,size,true,c);} break; // SBB
                case 4: res=a&imm; UpdateFlagsLogic(res,size); break;                       // AND
                case 5: res=a-imm; UpdateFlagsSub(a,imm,res,size,false,false); break;       // SUB
                case 6: res=a^imm; UpdateFlagsLogic(res,size); break;                       // XOR
                case 7: res=a-imm; UpdateFlagsSub(a,imm,res,size,false,false); store=false; break; // CMP
            }
            if (store && !WriteRM(rm, size, p.rex, res)) { fault(ExecStatus::MemoryFault, rm.addr); return r; }
            commit(); return r;
        }

        // ---- test r/m,r (84/85) ----
        case 0x84: case 0x85: {
            const int size = (opcode == 0x84) ? 1 : osz;
            if (!alu_rm_r(size, [&](uint64_t a, uint64_t b){ uint64_t res=a&b; UpdateFlagsLogic(res,size); return res; }, false, false)) return r;
            commit(); return r;
        }
        // ---- test AL/eAX, imm (A8/A9) ----
        case 0xA8: { uint8_t imm=0; if(!Fetch8(rip,imm)){fault(ExecStatus::DecodeFault,rip);return r;} uint64_t res=ReadRegByte(RAX,p.rex)&imm; UpdateFlagsLogic(res,1); commit(); return r; }
        case 0xA9: { uint32_t imm=0; if(!Fetch32(rip,imm)){fault(ExecStatus::DecodeFault,rip);return r;} uint64_t b=(osz==8)?SignExtend(imm,4):(imm&Mask(osz)); uint64_t res=ReadReg(RAX,osz)&b; UpdateFlagsLogic(res,osz); commit(); return r; }

        // ---- xchg r/m, r (86/87) ----
        case 0x86: case 0x87: {
            const int size = (opcode == 0x86) ? 1 : osz;
            uint8_t modrm=0; if(!Fetch8(rip,modrm)){fault(ExecStatus::DecodeFault,rip);return r;}
            ModRMOperand rm{}; uint8_t reg=0;
            if(!DecodeModRM(rip,p,modrm,rm,reg)){fault(ExecStatus::DecodeFault,rip);return r;}
            uint64_t rmv=0; if(!ReadRM(rm,size,p.rex,rmv)){fault(ExecStatus::MemoryFault,rm.addr);return r;}
            const uint64_t rv = (size==1)?ReadRegByte(reg,p.rex):ReadReg(reg,size);
            if (size==1) WriteRegByte(reg,p.rex,(uint8_t)rmv); else WriteReg(reg,size,rmv);
            if(!WriteRM(rm,size,p.rex,rv)){fault(ExecStatus::MemoryFault,rm.addr);return r;}
            commit(); return r;
        }

        // ---- mov r/m,r and r,r/m (88-8B) ----
        case 0x88: case 0x89: case 0x8A: case 0x8B: {
            const bool byte_form = (opcode & 1) == 0;
            const bool reg_is_dest = (opcode & 2) != 0;
            const int size = byte_form ? 1 : osz;
            uint8_t modrm=0; if(!Fetch8(rip,modrm)){fault(ExecStatus::DecodeFault,rip);return r;}
            ModRMOperand rm{}; uint8_t reg=0;
            if(!DecodeModRM(rip,p,modrm,rm,reg)){fault(ExecStatus::DecodeFault,rip);return r;}
            if (reg_is_dest) {
                uint64_t v=0; if(!ReadRM(rm,size,p.rex,v)){fault(ExecStatus::MemoryFault,rm.addr);return r;}
                if (size==1) WriteRegByte(reg,p.rex,(uint8_t)v); else WriteReg(reg,size,v);
            } else {
                const uint64_t v = (size==1)?ReadRegByte(reg,p.rex):ReadReg(reg,size);
                if(!WriteRM(rm,size,p.rex,v)){fault(ExecStatus::MemoryFault,rm.addr);return r;}
            }
            commit(); return r;
        }

        // ---- lea (8D) ----
        case 0x8D: {
            uint8_t modrm=0; if(!Fetch8(rip,modrm)){fault(ExecStatus::DecodeFault,rip);return r;}
            ModRMOperand rm{}; uint8_t reg=0;
            if(!DecodeModRM(rip,p,modrm,rm,reg)){fault(ExecStatus::DecodeFault,rip);return r;}
            if (rm.is_reg) { r.status=ExecStatus::UnsupportedOpcode; return r; }
            WriteReg(reg, osz, rm.addr & Mask(osz));
            commit(); return r;
        }

        // ---- mov r, imm (B0-B7 byte, B8-BF osz) ----
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7: {
            uint8_t imm=0; if(!Fetch8(rip,imm)){fault(ExecStatus::DecodeFault,rip);return r;}
            const uint8_t reg = (opcode - 0xB0) | (p.rex_b ? 0x08 : 0);
            WriteRegByte(reg, p.rex, imm);
            commit(); return r;
        }
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
            const uint8_t reg = (opcode - 0xB8) | (p.rex_b ? 0x08 : 0);
            if (osz == 8) {
                uint64_t imm=0; if(!Fetch64(rip,imm)){fault(ExecStatus::DecodeFault,rip);return r;}
                WriteReg(reg, 8, imm);
            } else if (osz == 2) {
                uint16_t imm=0; if(!Fetch16(rip,imm)){fault(ExecStatus::DecodeFault,rip);return r;}
                WriteReg(reg, 2, imm);
            } else {
                uint32_t imm=0; if(!Fetch32(rip,imm)){fault(ExecStatus::DecodeFault,rip);return r;}
                WriteReg(reg, 4, imm);
            }
            commit(); return r;
        }
        // ---- mov r/m, imm (C6/C7) ----
        case 0xC6: case 0xC7: {
            const int size = (opcode == 0xC6) ? 1 : osz;
            uint8_t modrm=0; if(!Fetch8(rip,modrm)){fault(ExecStatus::DecodeFault,rip);return r;}
            ModRMOperand rm{}; uint8_t reg=0;
            if(!DecodeModRM(rip,p,modrm,rm,reg)){fault(ExecStatus::DecodeFault,rip);return r;}
            uint64_t imm=0;
            if (size==1){uint8_t v=0; if(!Fetch8(rip,v)){fault(ExecStatus::DecodeFault,rip);return r;} imm=v;}
            else {uint32_t v=0; if(!Fetch32(rip,v)){fault(ExecStatus::DecodeFault,rip);return r;} imm=(size==8)?(SignExtend(v,4)&Mask(8)):(v&Mask(size));}
            if(!WriteRM(rm,size,p.rex,imm)){fault(ExecStatus::MemoryFault,rm.addr);return r;}
            commit(); return r;
        }

        // ---- push/pop r (50-5F) ----
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57: {
            const uint8_t reg = (opcode - 0x50) | (p.rex_b ? 0x08 : 0);
            const int size = p.opsize ? 2 : 8; // default 64 in long mode
            uint64_t rsp = m_state.gpr[RSP] - size;
            const uint64_t v = ReadReg(reg, size);
            if(!m_mem.Write(rsp, &v, size)){fault(ExecStatus::MemoryFault,rsp);return r;}
            m_state.gpr[RSP] = rsp;
            commit(); return r;
        }
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
            const uint8_t reg = (opcode - 0x58) | (p.rex_b ? 0x08 : 0);
            const int size = p.opsize ? 2 : 8;
            uint64_t rsp = m_state.gpr[RSP];
            uint64_t v=0;
            if(!m_mem.Read(rsp, &v, size)){fault(ExecStatus::MemoryFault,rsp);return r;}
            WriteReg(reg, size, v & Mask(size));
            m_state.gpr[RSP] = rsp + size;
            commit(); return r;
        }
        // ---- push imm (68 / 6A) ----
        case 0x68: {
            uint32_t imm=0; if(!Fetch32(rip,imm)){fault(ExecStatus::DecodeFault,rip);return r;}
            const uint64_t v = SignExtend(imm,4);
            uint64_t rsp = m_state.gpr[RSP]-8;
            if(!m_mem.Write(rsp,&v,8)){fault(ExecStatus::MemoryFault,rsp);return r;}
            m_state.gpr[RSP]=rsp; commit(); return r;
        }
        case 0x6A: {
            uint8_t imm=0; if(!Fetch8(rip,imm)){fault(ExecStatus::DecodeFault,rip);return r;}
            const uint64_t v = SignExtend(imm,1);
            uint64_t rsp = m_state.gpr[RSP]-8;
            if(!m_mem.Write(rsp,&v,8)){fault(ExecStatus::MemoryFault,rsp);return r;}
            m_state.gpr[RSP]=rsp; commit(); return r;
        }

        // ---- imul r, r/m, imm (69/6B) ----
        case 0x69: case 0x6B: {
            uint8_t modrm=0; if(!Fetch8(rip,modrm)){fault(ExecStatus::DecodeFault,rip);return r;}
            ModRMOperand rm{}; uint8_t reg=0;
            if(!DecodeModRM(rip,p,modrm,rm,reg)){fault(ExecStatus::DecodeFault,rip);return r;}
            uint64_t src=0; if(!ReadRM(rm,osz,p.rex,src)){fault(ExecStatus::MemoryFault,rm.addr);return r;}
            uint64_t imm=0;
            if (opcode==0x6B){uint8_t v=0; if(!Fetch8(rip,v)){fault(ExecStatus::DecodeFault,rip);return r;} imm=SignExtend(v,1);}
            else {uint32_t v=0; if(!Fetch32(rip,v)){fault(ExecStatus::DecodeFault,rip);return r;} imm=SignExtend(v,4);}
            const int64_t a = (int64_t)SignExtend(src,osz);
            const int64_t b = (int64_t)imm;
            const int64_t res = a*b;
            WriteReg(reg, osz, (uint64_t)res & Mask(osz));
            const bool overflow = (SignExtend((uint64_t)res,osz) != (uint64_t)res);
            SetFlag(Flags::OF, overflow); SetFlag(Flags::CF, overflow);
            commit(); return r;
        }

        // ---- group2 shifts/rotates: C0/C1 (imm8), D0/D1 (by 1), D2/D3 (by CL) ----
        case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
            const bool byte_form = (opcode & 1) == 0;
            const int size = byte_form ? 1 : osz;
            uint8_t modrm=0; if(!Fetch8(rip,modrm)){fault(ExecStatus::DecodeFault,rip);return r;}
            ModRMOperand rm{}; uint8_t reg=0;
            if(!DecodeModRM(rip,p,modrm,rm,reg)){fault(ExecStatus::DecodeFault,rip);return r;}
            uint8_t count=0;
            if (opcode==0xC0||opcode==0xC1){ if(!Fetch8(rip,count)){fault(ExecStatus::DecodeFault,rip);return r;} }
            else if (opcode==0xD0||opcode==0xD1){ count=1; }
            else { count = (uint8_t)(m_state.gpr[RCX] & 0xFF); }
            count &= (size==8)?0x3F:0x1F;
            uint64_t val=0; if(!ReadRM(rm,size,p.rex,val)){fault(ExecStatus::MemoryFault,rm.addr);return r;}
            const uint8_t sub = (modrm>>3)&0x07;
            const uint64_t m = Mask(size);
            uint64_t res = val;
            if (count != 0) {
                switch (sub) {
                    case 0: { // ROL (round 16)
                        const int bits = size * 8;
                        const uint8_t n = count % bits;
                        res = n ? (((val << n) | (val >> (bits - n))) & m) : val;
                        SetFlag(Flags::CF, res & 1);
                        if (n == 1) SetFlag(Flags::OF, (((res >> (bits - 1)) & 1) != (res & 1)));
                        break;
                    }
                    case 1: { // ROR (round 16)
                        const int bits = size * 8;
                        const uint8_t n = count % bits;
                        res = n ? (((val >> n) | (val << (bits - n))) & m) : val;
                        SetFlag(Flags::CF, (res >> (bits - 1)) & 1);
                        if (n == 1) SetFlag(Flags::OF, (((res >> (bits - 1)) ^ (res >> (bits - 2))) & 1) != 0);
                        break;
                    }
                    case 2: { // RCL (round 16)
                        const int bits = size * 8 + 1; // value + CF
                        const uint64_t n = count % bits;
                        uint64_t v = ((uint64_t)(GetFlag(Flags::CF) ? 1 : 0) << (size * 8)) | (val & m);
                        if (n) {
                            const uint64_t rot = ((v << n) | (v >> (bits - n))) & ((1ull << bits) - 1);
                            res = rot & m;
                            SetFlag(Flags::CF, (rot >> (size * 8)) & 1);
                        }
                        break;
                    }
                    case 3: { // RCR (round 16)
                        const int bits = size * 8 + 1;
                        const uint64_t n = count % bits;
                        uint64_t v = ((uint64_t)(GetFlag(Flags::CF) ? 1 : 0) << (size * 8)) | (val & m);
                        if (n) {
                            const uint64_t rot = ((v >> n) | (v << (bits - n))) & ((1ull << bits) - 1);
                            res = rot & m;
                            SetFlag(Flags::CF, (rot >> (size * 8)) & 1);
                        }
                        break;
                    }
                    case 4: case 6: { // SHL / SAL
                        res = (val << count) & m;
                        // Guard against UB: when count > size*8, the shift amount
                        // (size*8 - count) would be negative → UB in C++.
                        // x86 semantics: CF = last bit shifted out = bit(size*8 - count).
                        // If count > size*8, all bits shifted out → CF = 0.
                        bool cf = (count <= size*8) ? (((val >> (size*8 - count)) & 1) != 0) : false;
                        SetFlag(Flags::CF, cf);
                        UpdateSZP(res,size);
                        break;
                    }
                    case 5: { // SHR
                        res = (val & m) >> count;
                        // CF = last bit shifted out = bit(count-1) of the operand.
                        // If count > size*8, all operand bits shifted out → CF = 0.
                        // Use (val & m) to only consider operand-sized bits.
                        bool cf = (count <= size*8) ? ((((val & m) >> (count-1)) & 1) != 0) : false;
                        SetFlag(Flags::CF, cf);
                        UpdateSZP(res,size);
                        break;
                    }
                    case 7: { // SAR
                        const int64_t sv = (int64_t)SignExtend(val,size);
                        res = (uint64_t)(sv >> count) & m;
                        // For SAR, sign-extension fills high bits, so bit(count-1) of
                        // the sign-extended value gives the correct last-bit-shifted-out
                        // even when count > size*8 (it'll be the sign bit).
                        bool cf = ((sv >> (count-1)) & 1) != 0;
                        SetFlag(Flags::CF, cf);
                        UpdateSZP(res,size);
                        break;
                    }
                    default:
                        r.status = ExecStatus::UnsupportedOpcode; return r;
                }
            }
            if(!WriteRM(rm,size,p.rex,res)){fault(ExecStatus::MemoryFault,rm.addr);return r;}
            commit(); return r;
        }

        // ---- group3: F6/F7 test/not/neg/mul/imul/div/idiv ----
        case 0xF6: case 0xF7: {
            const int size = (opcode==0xF6)?1:osz;
            uint8_t modrm=0; if(!Fetch8(rip,modrm)){fault(ExecStatus::DecodeFault,rip);return r;}
            ModRMOperand rm{}; uint8_t reg=0;
            if(!DecodeModRM(rip,p,modrm,rm,reg)){fault(ExecStatus::DecodeFault,rip);return r;}
            const uint8_t sub=(modrm>>3)&0x07;
            uint64_t val=0; if(!ReadRM(rm,size,p.rex,val)){fault(ExecStatus::MemoryFault,rm.addr);return r;}
            const uint64_t m = Mask(size);
            switch (sub) {
                case 0: case 1: { // TEST r/m, imm
                    uint64_t imm=0;
                    if (size==1){uint8_t v=0; if(!Fetch8(rip,v)){fault(ExecStatus::DecodeFault,rip);return r;} imm=v;}
                    else {uint32_t v=0; if(!Fetch32(rip,v)){fault(ExecStatus::DecodeFault,rip);return r;} imm=(size==8)?(SignExtend(v,4)&m):(v&m);}
                    uint64_t res=val&imm; UpdateFlagsLogic(res,size);
                    break;
                }
                case 2: { // NOT
                    if(!WriteRM(rm,size,p.rex,(~val)&m)){fault(ExecStatus::MemoryFault,rm.addr);return r;}
                    break;
                }
                case 3: { // NEG
                    uint64_t res=(0-val)&m;
                    UpdateFlagsSub(0,val,res,size,false,false);
                    SetFlag(Flags::CF, (val&m)!=0);
                    if(!WriteRM(rm,size,p.rex,res)){fault(ExecStatus::MemoryFault,rm.addr);return r;}
                    break;
                }
                case 4: { // MUL (unsigned) rdx:rax = rax * r/m
                    const uint64_t a = ReadReg(RAX,size);
                    __uint128_t prod = (__uint128_t)(a & m) * (uint64_t)(val & m);
                    WriteReg(RAX, size, (uint64_t)prod & m);
                    if (size!=1) WriteReg(RDX, size, (uint64_t)(prod >> (size*8)) & m);
                    else WriteReg(RAX,2,(uint64_t)prod & 0xFFFF); // AX holds full 16
                    const bool hi = ((uint64_t)(prod >> (size*8)) & m) != 0;
                    SetFlag(Flags::CF,hi); SetFlag(Flags::OF,hi);
                    break;
                }
                case 5: { // IMUL (signed)
                    const int64_t a = (int64_t)SignExtend(ReadReg(RAX,size),size);
                    const int64_t b = (int64_t)SignExtend(val,size);
                    __int128_t prod = (__int128_t)a * b;
                    WriteReg(RAX,size,(uint64_t)prod & m);
                    if (size!=1) WriteReg(RDX,size,(uint64_t)((__uint128_t)prod >> (size*8)) & m);
                    const bool of = ((__int128_t)SignExtend((uint64_t)prod,size) != prod);
                    SetFlag(Flags::CF,of); SetFlag(Flags::OF,of);
                    break;
                }
                case 6: { // DIV (unsigned)
                    if ((val & m)==0){ r.status=ExecStatus::UnsupportedOpcode; return r; } // #DE
                    if (size==1){
                        uint16_t dividend=(uint16_t)ReadReg(RAX,2);
                        uint8_t d=(uint8_t)(val&0xFF);
                        WriteRegByte(RAX,p.rex,(uint8_t)(dividend/d));
                        // Remainder goes in AH (index 4 WITHOUT rex), NOT SPL (index 4 WITH rex).
                        // Using p.rex=false ensures index 4 maps to AH, not RSP/SPL.
                        WriteRegByte(RAX|4,false,(uint8_t)(dividend%d)); // AH (not SPL)
                    } else {
                        __uint128_t dividend = ((__uint128_t)ReadReg(RDX,size) << (size*8)) | ReadReg(RAX,size);
                        uint64_t d=val&m;
                        WriteReg(RAX,size,(uint64_t)(dividend/d)&m);
                        WriteReg(RDX,size,(uint64_t)(dividend%d)&m);
                    }
                    break;
                }
                case 7: { // IDIV (signed)
                    if ((val & m)==0){ r.status=ExecStatus::UnsupportedOpcode; return r; }
                    __int128_t dividend = ((__int128_t)(int64_t)SignExtend(ReadReg(RDX,size),size) << (size*8)) | ReadReg(RAX,size);
                    int64_t d=(int64_t)SignExtend(val,size);
                    WriteReg(RAX,size,(uint64_t)(dividend/d)&m);
                    WriteReg(RDX,size,(uint64_t)(dividend%d)&m);
                    break;
                }
            }
            commit(); return r;
        }

        // ---- group5: FF inc/dec/call/jmp/push ----
        case 0xFF: {
            uint8_t modrm=0; if(!Fetch8(rip,modrm)){fault(ExecStatus::DecodeFault,rip);return r;}
            ModRMOperand rm{}; uint8_t reg=0;
            if(!DecodeModRM(rip,p,modrm,rm,reg)){fault(ExecStatus::DecodeFault,rip);return r;}
            const uint8_t sub=(modrm>>3)&0x07;
            switch (sub) {
                case 0: { // INC
                    uint64_t v=0; if(!ReadRM(rm,osz,p.rex,v)){fault(ExecStatus::MemoryFault,rm.addr);return r;}
                    uint64_t res=v+1; bool cf=GetFlag(Flags::CF); UpdateFlagsAdd(v,1,res,osz,false,false); SetFlag(Flags::CF,cf);
                    if(!WriteRM(rm,osz,p.rex,res)){fault(ExecStatus::MemoryFault,rm.addr);return r;} break;
                }
                case 1: { // DEC
                    uint64_t v=0; if(!ReadRM(rm,osz,p.rex,v)){fault(ExecStatus::MemoryFault,rm.addr);return r;}
                    uint64_t res=v-1; bool cf=GetFlag(Flags::CF); UpdateFlagsSub(v,1,res,osz,false,false); SetFlag(Flags::CF,cf);
                    if(!WriteRM(rm,osz,p.rex,res)){fault(ExecStatus::MemoryFault,rm.addr);return r;} break;
                }
                case 2: { // CALL r/m (near, indirect)
                    uint64_t target=0; if(!ReadRM(rm,8,p.rex,target)){fault(ExecStatus::MemoryFault,rm.addr);return r;}
                    uint64_t rsp=m_state.gpr[RSP]-8;
                    if(!m_mem.Write(rsp,&rip,8)){fault(ExecStatus::MemoryFault,rsp);return r;}
                    m_state.gpr[RSP]=rsp; m_state.rip=target; return r;
                }
                case 4: { // JMP r/m (near, indirect)
                    uint64_t target=0; if(!ReadRM(rm,8,p.rex,target)){fault(ExecStatus::MemoryFault,rm.addr);return r;}
                    m_state.rip=target; return r;
                }
                case 6: { // PUSH r/m
                    uint64_t v=0; if(!ReadRM(rm,8,p.rex,v)){fault(ExecStatus::MemoryFault,rm.addr);return r;}
                    uint64_t rsp=m_state.gpr[RSP]-8;
                    if(!m_mem.Write(rsp,&v,8)){fault(ExecStatus::MemoryFault,rsp);return r;}
                    m_state.gpr[RSP]=rsp; commit(); return r;
                }
                default: r.status=ExecStatus::UnsupportedOpcode; return r;
            }
            commit(); return r;
        }

        // ---- group: FE inc/dec r/m8 ----
        case 0xFE: {
            uint8_t modrm=0; if(!Fetch8(rip,modrm)){fault(ExecStatus::DecodeFault,rip);return r;}
            ModRMOperand rm{}; uint8_t reg=0;
            if(!DecodeModRM(rip,p,modrm,rm,reg)){fault(ExecStatus::DecodeFault,rip);return r;}
            const uint8_t sub=(modrm>>3)&0x07;
            uint64_t v=0; if(!ReadRM(rm,1,p.rex,v)){fault(ExecStatus::MemoryFault,rm.addr);return r;}
            uint64_t res = (sub==0)?v+1:v-1;
            bool cf=GetFlag(Flags::CF);
            if (sub==0) UpdateFlagsAdd(v,1,res,1,false,false); else UpdateFlagsSub(v,1,res,1,false,false);
            SetFlag(Flags::CF,cf);
            if(!WriteRM(rm,1,p.rex,res)){fault(ExecStatus::MemoryFault,rm.addr);return r;}
            commit(); return r;
        }

        // ---- leave (C9), pushfq (9C), popfq (9D), sahf (9E), lahf (9F) ----
        case 0xC9: { // leave: rsp = rbp; pop rbp
            uint64_t rsp = m_state.gpr[RSP] = m_state.gpr[RBP];
            uint64_t v = 0;
            if (!m_mem.Read(rsp, &v, 8)) { fault(ExecStatus::MemoryFault, rsp); return r; }
            m_state.gpr[RSP] = rsp + 8;
            m_state.gpr[RBP] = v;
            commit(); return r;
        }
        case 0x9C: { // pushfq
            uint64_t rsp = m_state.gpr[RSP] - 8;
            if (!m_mem.Write(rsp, &m_state.rflags, 8)) { fault(ExecStatus::MemoryFault, rsp); return r; }
            m_state.gpr[RSP] = rsp; commit(); return r;
        }
        case 0x9D: { // popfq
            uint64_t rsp = m_state.gpr[RSP]; uint64_t v = 0;
            if (!m_mem.Read(rsp, &v, 8)) { fault(ExecStatus::MemoryFault, rsp); return r; }
            m_state.gpr[RSP] = rsp + 8;
            m_state.rflags = v;
            commit(); return r;
        }
        case 0x9E: { // sahf: ah -> flags low byte
            const uint8_t ah = (uint8_t)((m_state.gpr[RAX] >> 8) & 0xFF);
            m_state.rflags = (m_state.rflags & ~0xFFull) | ah;
            commit(); return r;
        }
        case 0x9F: { // lahf: flags low byte -> ah
            const uint8_t fl = (uint8_t)(m_state.rflags & 0xFF);
            m_state.gpr[RAX] = (m_state.gpr[RAX] & ~0xFF00ull) | ((uint64_t)fl << 8);
            commit(); return r;
        }

        // ---- string ops with REP/REPE/REPNE (round 16) ----
        case 0xA4: case 0xA5: case 0xA6: case 0xA7:
        case 0xAA: case 0xAB: case 0xAC: case 0xAD:
        case 0xAE: case 0xAF: {
            if (!ExecStringOp(rip, p, opcode, r)) return r;
            commit(); return r;
        }

        // ---- cwde/cdqe (98), cdq/cqo (99) ----
        case 0x98: { // sign-extend AL->AX / AX->EAX / EAX->RAX
            if (osz==8) WriteReg(RAX,8,SignExtend(ReadReg(RAX,4),4));
            else if (osz==2) WriteReg(RAX,2,SignExtend(ReadReg(RAX,1),1));
            else WriteReg(RAX,4,SignExtend(ReadReg(RAX,2),2));
            commit(); return r;
        }
        case 0x99: { // sign-extend RAX into RDX:RAX
            const uint64_t a = ReadReg(RAX,osz);
            const bool neg = (a & (1ull<<(osz*8-1)))!=0;
            WriteReg(RDX,osz, neg?Mask(osz):0);
            commit(); return r;
        }

        // ---- jmp rel8/rel32 (EB/E9) ----
        case 0xEB: { uint8_t d=0; if(!Fetch8(rip,d)){fault(ExecStatus::DecodeFault,rip);return r;} m_state.rip=rip+SignExtend(d,1); return r; }
        case 0xE9: { uint32_t d=0; if(!Fetch32(rip,d)){fault(ExecStatus::DecodeFault,rip);return r;} m_state.rip=rip+SignExtend(d,4); return r; }

        // ---- call rel32 (E8) ----
        case 0xE8: {
            uint32_t d=0; if(!Fetch32(rip,d)){fault(ExecStatus::DecodeFault,rip);return r;}
            const uint64_t ret_addr = rip;
            uint64_t rsp=m_state.gpr[RSP]-8;
            if(!m_mem.Write(rsp,&ret_addr,8)){fault(ExecStatus::MemoryFault,rsp);return r;}
            m_state.gpr[RSP]=rsp;
            m_state.rip = rip + SignExtend(d,4);
            return r;
        }
        // ---- ret (C3) / ret imm16 (C2) ----
        case 0xC3: {
            uint64_t rsp=m_state.gpr[RSP]; uint64_t ret=0;
            if(!m_mem.Read(rsp,&ret,8)){fault(ExecStatus::MemoryFault,rsp);return r;}
            m_state.gpr[RSP]=rsp+8; m_state.rip=ret; return r;
        }
        case 0xC2: {
            uint16_t imm=0; if(!Fetch16(rip,imm)){fault(ExecStatus::DecodeFault,rip);return r;}
            uint64_t rsp=m_state.gpr[RSP]; uint64_t ret=0;
            if(!m_mem.Read(rsp,&ret,8)){fault(ExecStatus::MemoryFault,rsp);return r;}
            m_state.gpr[RSP]=rsp+8+imm; m_state.rip=ret; return r;
        }

        // ---- jcc rel8 (70-7F) ----
        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
            uint8_t d=0; if(!Fetch8(rip,d)){fault(ExecStatus::DecodeFault,rip);return r;}
            if (EvalCondition(opcode & 0x0F)) m_state.rip = rip + SignExtend(d,1);
            else m_state.rip = rip;
            return r;
        }

        // ---- loop / loope / loopne (E0-E2), jrcxz (E3) ----
        case 0xE0: case 0xE1: case 0xE2: {
            uint8_t d=0; if(!Fetch8(rip,d)){fault(ExecStatus::DecodeFault,rip);return r;}
            uint64_t rcx = m_state.gpr[RCX] - 1;
            m_state.gpr[RCX] = rcx;
            bool take = (rcx != 0);
            if (opcode==0xE1) take = take && GetFlag(Flags::ZF);   // loope
            if (opcode==0xE0) take = take && !GetFlag(Flags::ZF);  // loopne
            m_state.rip = take ? (rip + SignExtend(d,1)) : rip;
            return r;
        }
        case 0xE3: {
            uint8_t d=0; if(!Fetch8(rip,d)){fault(ExecStatus::DecodeFault,rip);return r;}
            m_state.rip = (m_state.gpr[RCX]==0) ? (rip + SignExtend(d,1)) : rip;
            return r;
        }

        // ---- syscall (0F 05) and two-byte opcodes ----
        case 0x0F: {
            uint8_t op2=0; if(!Fetch8(rip,op2)){fault(ExecStatus::DecodeFault,rip);return r;}
            // syscall
            if (op2 == 0x05) {
                m_state.rip = rip;
                if (!m_syscall || !m_syscall(m_state, m_mem)) { r.status=ExecStatus::SyscallDenied; return r; }
                return r;
            }
            // Round 20: ud2 -- guest illegal-instruction trap. The direct
            // backend reuses this opcode as its interception marker; the
            // interpreter treats it as a debug stop (same convention as int3).
            if (op2 == 0x0B) {
                m_state.rip = rip;
                r.status = ExecStatus::Halted;
                return r;
            }
            // Round 20: rdtsc -- virtualised (a fixed monotonic 3.5 GHz Zen 2
            // TSC; see HostModels::ReadVirtualTsc).
            if (op2 == 0x31) {
                const uint64_t tsc = HostModels::ReadVirtualTsc();
                WriteReg(RAX, 8, tsc & 0xFFFFFFFFull);
                WriteReg(RDX, 8, tsc >> 32);
                commit(); return r;
            }
            // Round 20: cpuid -- virtualised Zen 2 model (see HostModels).
            if (op2 == 0xA2) {
                uint32_t a=0,b=0,c=0,d=0;
                HostModels::CpuidModel(static_cast<uint32_t>(ReadReg(RAX,4)),
                                       static_cast<uint32_t>(ReadReg(RCX,4)), a,b,c,d);
                WriteReg(RAX,8,a); WriteReg(RBX,8,b); WriteReg(RCX,8,c); WriteReg(RDX,8,d);
                commit(); return r;
            }
            // Round 20: 0F 01 group -- rdtscp is virtualised; every other
            // 0F 01 form reachable from user code (monitor/mwait/swapgs...)
            // stays fail-closed exactly as before.
            if (op2 == 0x01) {
                uint8_t op3=0; if(!Fetch8(rip,op3)){fault(ExecStatus::DecodeFault,rip);return r;}
                if (op3 == 0xF9) { // rdtscp
                    const uint64_t tsc = HostModels::ReadVirtualTsc();
                    WriteReg(RAX, 8, tsc & 0xFFFFFFFFull);
                    WriteReg(RDX, 8, tsc >> 32);
                    WriteReg(RCX, 8, 0);  // IA32_TSC_AUX: single virtual socket
                    commit(); return r;
                }
                r.status = ExecStatus::UnsupportedOpcode; return r;
            }
            // jcc rel32 (0F 80-8F)
            if (op2 >= 0x80 && op2 <= 0x8F) {
                uint32_t d=0; if(!Fetch32(rip,d)){fault(ExecStatus::DecodeFault,rip);return r;}
                if (EvalCondition(op2 & 0x0F)) m_state.rip = rip + SignExtend(d,4);
                else m_state.rip = rip;
                return r;
            }
            // bswap r64/r32/r16 (0F C8-CF) -- round 29 completion.
            if (op2 >= 0xC8 && op2 <= 0xCF) {
                const uint8_t reg = static_cast<uint8_t>(
                    (op2 & 0x07u) | (p.rex_b ? 0x08u : 0x00u));
                const int size = osz;
                const uint64_t v = ReadReg(reg, size);
                uint64_t res = 0;
                if (size == 2) {          // bswap r16 is undefined on Intel;
                    res = ((v & 0xFFu) << 8) | ((v >> 8) & 0xFFu);  // swap bytes
                } else {
                    for (int b = 0; b < size; ++b) {
                        res |= ((v >> (8 * b)) & 0xFFull) << (8 * (size - 1 - b));
                    }
                }
                WriteReg(reg, size, res);
                commit(); return r;
            }
            // setcc r/m8 (0F 90-9F)
            if (op2 >= 0x90 && op2 <= 0x9F) {
                uint8_t modrm=0; if(!Fetch8(rip,modrm)){fault(ExecStatus::DecodeFault,rip);return r;}
                ModRMOperand rm{}; uint8_t reg=0;
                if(!DecodeModRM(rip,p,modrm,rm,reg)){fault(ExecStatus::DecodeFault,rip);return r;}
                const uint8_t val = EvalCondition(op2 & 0x0F) ? 1 : 0;
                if(!WriteRM(rm,1,p.rex,val)){fault(ExecStatus::MemoryFault,rm.addr);return r;}
                commit(); return r;
            }
            // cmovcc r, r/m (0F 40-4F)
            if (op2 >= 0x40 && op2 <= 0x4F) {
                uint8_t modrm=0; if(!Fetch8(rip,modrm)){fault(ExecStatus::DecodeFault,rip);return r;}
                ModRMOperand rm{}; uint8_t reg=0;
                if(!DecodeModRM(rip,p,modrm,rm,reg)){fault(ExecStatus::DecodeFault,rip);return r;}
                uint64_t v=0; if(!ReadRM(rm,osz,p.rex,v)){fault(ExecStatus::MemoryFault,rm.addr);return r;}
                if (EvalCondition(op2 & 0x0F)) WriteReg(reg,osz,v);
                commit(); return r;
            }
            // movzx (0F B6 r/m8, 0F B7 r/m16)
            if (op2 == 0xB6 || op2 == 0xB7) {
                const int ssize = (op2==0xB6)?1:2;
                uint8_t modrm=0; if(!Fetch8(rip,modrm)){fault(ExecStatus::DecodeFault,rip);return r;}
                ModRMOperand rm{}; uint8_t reg=0;
                if(!DecodeModRM(rip,p,modrm,rm,reg)){fault(ExecStatus::DecodeFault,rip);return r;}
                uint64_t v=0; if(!ReadRM(rm,ssize,p.rex,v)){fault(ExecStatus::MemoryFault,rm.addr);return r;}
                WriteReg(reg,osz, v & Mask(ssize));
                commit(); return r;
            }
            // movsx (0F BE r/m8, 0F BF r/m16)
            if (op2 == 0xBE || op2 == 0xBF) {
                const int ssize = (op2==0xBE)?1:2;
                uint8_t modrm=0; if(!Fetch8(rip,modrm)){fault(ExecStatus::DecodeFault,rip);return r;}
                ModRMOperand rm{}; uint8_t reg=0;
                if(!DecodeModRM(rip,p,modrm,rm,reg)){fault(ExecStatus::DecodeFault,rip);return r;}
                uint64_t v=0; if(!ReadRM(rm,ssize,p.rex,v)){fault(ExecStatus::MemoryFault,rm.addr);return r;}
                WriteReg(reg,osz, SignExtend(v,ssize) & Mask(osz));
                commit(); return r;
            }
            // imul r, r/m (0F AF)
            if (op2 == 0xAF) {
                uint8_t modrm=0; if(!Fetch8(rip,modrm)){fault(ExecStatus::DecodeFault,rip);return r;}
                ModRMOperand rm{}; uint8_t reg=0;
                if(!DecodeModRM(rip,p,modrm,rm,reg)){fault(ExecStatus::DecodeFault,rip);return r;}
                uint64_t v=0; if(!ReadRM(rm,osz,p.rex,v)){fault(ExecStatus::MemoryFault,rm.addr);return r;}
                const int64_t a=(int64_t)SignExtend(ReadReg(reg,osz),osz);
                const int64_t b=(int64_t)SignExtend(v,osz);
                const int64_t res=a*b;
                WriteReg(reg,osz,(uint64_t)res & Mask(osz));
                const bool of = (SignExtend((uint64_t)res,osz) != (uint64_t)res);
                SetFlag(Flags::OF,of); SetFlag(Flags::CF,of);
                commit(); return r;
            }
            // 0F 1F nop r/m (multi-byte nop)
            if (op2 == 0x1F) {
                uint8_t modrm=0; if(!Fetch8(rip,modrm)){fault(ExecStatus::DecodeFault,rip);return r;}
                ModRMOperand rm{}; uint8_t reg=0;
                if(!DecodeModRM(rip,p,modrm,rm,reg)){fault(ExecStatus::DecodeFault,rip);return r;}
                commit(); return r;
            }
            // 0F 38 / 0F 3A: the FULL SIMD engine takes priority (round 28);
            // the legacy ExecSse3 subset stays as fallback for anything the
            // engine declines (fail-closed ordering preserved).
            if (op2 == 0x38 || op2 == 0x3A) {
                uint8_t op3=0; if(!Fetch8(rip,op3)){fault(ExecStatus::DecodeFault,rip);return r;}
                const uint64_t rip_before = rip;
                if (ExecSimdFull(rip, p, op3, op2==0x3A ? 3 : 2, nullptr, r)) {
                    if (r.status == ExecStatus::Running) commit();
                    return r;
                }
                rip = rip_before;   // engine declined: rewind and try legacy path
                if (ExecSse3(rip, p, op2==0x3A, op3, r)) {
                    if (r.status == ExecStatus::Running) commit();
                    return r;
                }
                r.status = ExecStatus::UnsupportedOpcode; return r;
            }
            // bt/bts/btr/btc/bsf/bsr/popcnt/cmpxchg/xadd (round 16)
            if (ExecBitOp(rip, p, op2, r)) {
                if (r.status == ExecStatus::Running) commit();
                return r;
            }
            // Round 20: AMD SSE4a (insertq/extrq/movnt* stores) -- must be
            // tried before the generic SSE table (66/F2 0F 78/79/2B forms).
            if (ExecSse4a(rip, p, op2, r)) {
                if (r.status == ExecStatus::Running) commit();
                return r;
            }
            if (r.status != ExecStatus::Running) { return r; }
            // ---- round 28: the FULL SIMD engine covers the complete legacy
            // SSE maps; ExecSse/ExecSseInt remain as fallback for anything
            // the engine declines (no bytes consumed on decline).
            {
                const uint64_t rip_before = rip;
                if (ExecSimdFull(rip, p, op2, 1, nullptr, r)) {
                    if (r.status == ExecStatus::Running) commit();
                    return r;
                }
                rip = rip_before;
            }
            // ---- SSE/SSE2 (round 11): scalar + packed float ops ----
            if (ExecSse(rip, p, op2, r)) {
                if (r.status == ExecStatus::Running) commit();
                return r;
            }
            r.status = ExecStatus::UnsupportedOpcode; return r;
        }

        // ---- xchg eAX, r (91-97) ----
        case 0x91: case 0x92: case 0x93: case 0x94:
        case 0x95: case 0x96: case 0x97: {
            const uint8_t reg = (opcode-0x90) | (p.rex_b?0x08:0);
            const uint64_t tmp = ReadReg(RAX,osz);
            WriteReg(RAX,osz,ReadReg(reg,osz));
            WriteReg(reg,osz,tmp);
            commit(); return r;
        }

        default:
            r.status = ExecStatus::UnsupportedOpcode;
            return r;
    }
}

// ---------------------------------------------------------------------------
// SSE/SSE2 (round 11)
// ---------------------------------------------------------------------------
float X86Interpreter::XmmFloat(const CpuState::XmmReg& v, int lane) {
    uint32_t bits = 0;
    switch (lane) {
        case 0:  bits = static_cast<uint32_t>(v.lo & 0xFFFFFFFFull); break;
        case 1:  bits = static_cast<uint32_t>(v.lo >> 32); break;
        case 2:  bits = static_cast<uint32_t>(v.hi & 0xFFFFFFFFull); break;
        default: bits = static_cast<uint32_t>(v.hi >> 32); break;
    }
    float f = 0.0f;
    std::memcpy(&f, &bits, 4);
    return f;
}

double X86Interpreter::XmmDouble(const CpuState::XmmReg& v, int lane) {
    const uint64_t bits = (lane == 0) ? v.lo : v.hi;
    double d = 0.0;
    std::memcpy(&d, &bits, 8);
    return d;
}

void X86Interpreter::SetXmmFloat(CpuState::XmmReg& v, int lane, float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    const uint64_t word = bits;
    switch (lane) {
        case 0:  v.lo = (v.lo & 0xFFFFFFFF00000000ull) | word; break;
        case 1:  v.lo = (v.lo & 0x00000000FFFFFFFFull) | (word << 32); break;
        case 2:  v.hi = (v.hi & 0xFFFFFFFF00000000ull) | word; break;
        default: v.hi = (v.hi & 0x00000000FFFFFFFFull) | (word << 32); break;
    }
}

void X86Interpreter::SetXmmDouble(CpuState::XmmReg& v, int lane, double d) {
    uint64_t bits = 0;
    std::memcpy(&bits, &d, 8);
    if (lane == 0) v.lo = bits;
    else v.hi = bits;
}

void X86Interpreter::SetComparedFlags(float a, float b) {
    // COMISS/UCOMISS: ZF/PF/CF; SF/OF/AF cleared. (float->double casts are
    // exact, so the double overload does all the real work.)
    SetComparedFlags(static_cast<double>(a), static_cast<double>(b));
}

void X86Interpreter::SetComparedFlags(double a, double b) {
    SetFlag(Flags::SF, false);
    SetFlag(Flags::OF, false);
    SetFlag(Flags::AF, false);
    if (std::isnan(a) || std::isnan(b)) {
        // Unordered: ZF=PF=CF=1.
        SetFlag(Flags::ZF, true);
        SetFlag(Flags::PF, true);
        SetFlag(Flags::CF, true);
    } else if (a < b) {
        SetFlag(Flags::ZF, false);
        SetFlag(Flags::PF, false);
        SetFlag(Flags::CF, true);
    } else if (a > b) {
        SetFlag(Flags::ZF, false);
        SetFlag(Flags::PF, false);
        SetFlag(Flags::CF, false);
    } else {
        SetFlag(Flags::ZF, true);
        SetFlag(Flags::PF, false);
        SetFlag(Flags::CF, false);
    }
}

bool X86Interpreter::ReadXmmOperand(const ModRMOperand& op, const Prefixes& p, int width,
                                    CpuState::XmmReg& out) {
    if (op.is_reg) {
        const uint8_t idx = op.reg | (p.rex_b ? 0x08 : 0x00);
        out = m_state.xmm[idx];
        return true;
    }
    // Memory operand: read only `width` bytes so a scalar access at the end of
    // a mapped page does not fail on the trailing (untouched) lanes.
    out = CpuState::XmmReg{};
    if (width < 16) {
        return m_mem.Read(op.addr, &out.lo, static_cast<size_t>(width));
    }
    return m_mem.Read(op.addr, &out, 16);
}

bool X86Interpreter::WriteXmmOperand(const ModRMOperand& op, const Prefixes& p, int width,
                                     const CpuState::XmmReg& value) {
    if (op.is_reg) {
        const uint8_t idx = op.reg | (p.rex_b ? 0x08 : 0x00);
        if (width >= 16) {
            m_state.xmm[idx] = value;
        } else {
            // Scalar/64-bit stores into a register preserve the upper lanes.
            CpuState::XmmReg dst = m_state.xmm[idx];
            std::memcpy(&dst, &value, static_cast<size_t>(width));
            m_state.xmm[idx] = dst;
        }
        return true;
    }
    return m_mem.Write(op.addr, &value, static_cast<size_t>(width));
}

bool X86Interpreter::ExecSse(uint64_t& rip, const Prefixes& p, uint8_t op2, RunResult& r) {
    // Round 16: integer SIMD on the legacy 0F map runs first (it needs no
    // float-lane interpretation).
    if (ExecSseInt(rip, p, op2, r)) {
        return true;
    }
    // Only handle the opcodes this executor implements; anything else falls
    // back to the caller's UnsupportedOpcode path (fail-closed).
    switch (op2) {
        case 0x10: case 0x11:   // movups/movss/movupd/movsd
        case 0x28: case 0x29:   // movaps/movapd
        case 0x2A:              // cvtsi2ss/cvtsi2sd
        case 0x2C: case 0x2D:   // cvttss2si/cvttsd2si + rounding forms
        case 0x2E: case 0x2F:   // ucomiss/comiss (+d)
        case 0x51:              // sqrt (scalar + packed)
        case 0x54: case 0x56: case 0x57: // and/or/xor (ps/pd)
        case 0x58: case 0x59: case 0x5C: case 0x5D:
        case 0x5E: case 0x5F:   // add/mul/sub/min/div/max
        case 0x5A:              // cvtss2sd/cvtsd2ss/cvtps2pd/cvtpd2ps
        case 0x6E: case 0x7E:   // movd/movq (GP <-> XMM)
        case 0xD6:              // movq r/m64, xmm
        case 0xEF:              // pxor
            break;
        default:
            return false;
    }

    const bool has66 = p.opsize;          // 0x66 -> packed double / qword
    const bool hasF3 = p.rep == 0xF3;     // scalar float
    const bool hasF2 = p.rep == 0xF2;     // scalar double

    // Instructions that never take an operand.
    if (op2 == 0x2E || op2 == 0x2F) {
        // comiss/ucomiss (0F 2E/2F) and comisd/ucomisd (66 0F 2E/2F):
        // reg = XMM source, r/m = XMM/mem source. Compare low element.
        uint8_t modrm = 0;
        if (!Fetch8(rip, modrm)) { r.status = ExecStatus::DecodeFault; r.fault_addr = rip; return true; }
        ModRMOperand rm{}; uint8_t reg = 0;
        if (!DecodeModRM(rip, p, modrm, rm, reg)) { r.status = ExecStatus::DecodeFault; r.fault_addr = rip; return true; }
        const uint8_t dst_idx = reg | (p.rex_r ? 0x08 : 0x00);
        CpuState::XmmReg src{};
        if (!ReadXmmOperand(rm, p, has66 ? 8 : 4, src)) {
            r.status = ExecStatus::MemoryFault; r.fault_addr = rm.addr; return true;
        }
        if (has66) {
            SetComparedFlags(XmmDouble(m_state.xmm[dst_idx], 0), XmmDouble(src, 0));
        } else {
            SetComparedFlags(XmmFloat(m_state.xmm[dst_idx], 0), XmmFloat(src, 0));
        }
        return true;
    }

    // All remaining handled opcodes use a ModRM byte with an XMM register on
    // one side. Decode it once.
    uint8_t modrm = 0;
    if (!Fetch8(rip, modrm)) { r.status = ExecStatus::DecodeFault; r.fault_addr = rip; return true; }
    ModRMOperand rm{}; uint8_t reg = 0;
    if (!DecodeModRM(rip, p, modrm, rm, reg)) { r.status = ExecStatus::DecodeFault; r.fault_addr = rip; return true; }
    const uint8_t dst_idx = reg | (p.rex_r ? 0x08 : 0x00);

    auto fail_mem = [&]() { r.status = ExecStatus::MemoryFault; r.fault_addr = rm.addr; };
    auto fail_dec = [&]() { r.status = ExecStatus::DecodeFault; r.fault_addr = rip; };

    // ---- GP <-> XMM moves (0x6E / 0x7E / 0xD6) ----
    if (op2 == 0x6E || op2 == 0x7E || op2 == 0xD6) {
        const bool qword = has66 || p.rex_w || p.rep == 0xF3;
        const int width = qword ? 8 : 4;
        if (op2 == 0x6E || (op2 == 0x7E && p.rep == 0xF3)) {
            // GP/mem -> XMM (low element, upper lanes zeroed on load).
            uint64_t v = 0;
            if (rm.is_reg) {
                v = ReadReg(rm.reg, width);
            } else if (!m_mem.Read(rm.addr, &v, static_cast<size_t>(width))) {
                fail_mem(); return true;
            }
            CpuState::XmmReg loaded{};
            std::memcpy(&loaded, &v, static_cast<size_t>(width));
            m_state.xmm[dst_idx] = loaded;
        } else {
            // XMM -> GP/mem (0x7E dword, 66 0x7E qword, 66 0xD6 qword store).
            const CpuState::XmmReg src = m_state.xmm[dst_idx];
            if (rm.is_reg) {
                uint64_t v = 0;
                std::memcpy(&v, &src, static_cast<size_t>(width));
                WriteReg(rm.reg, width, v);
            } else if (!m_mem.Write(rm.addr, &src, static_cast<size_t>(width))) {
                fail_mem(); return true;
            }
        }
        return true;
    }

    // ---- pxor (66 0F EF) ----
    if (op2 == 0xEF) {
        CpuState::XmmReg src{};
        if (!ReadXmmOperand(rm, p, 16, src)) { fail_mem(); return true; }
        CpuState::XmmReg res{};
        res.lo = m_state.xmm[dst_idx].lo ^ src.lo;
        res.hi = m_state.xmm[dst_idx].hi ^ src.hi;
        m_state.xmm[dst_idx] = res;
        return true;
    }

    // ---- packed logical (0x54/0x56/0x57) ----
    if (op2 == 0x54 || op2 == 0x56 || op2 == 0x57) {
        CpuState::XmmReg src{};
        if (!ReadXmmOperand(rm, p, 16, src)) { fail_mem(); return true; }
        CpuState::XmmReg res{};
        if (op2 == 0x54) { res.lo = m_state.xmm[dst_idx].lo & src.lo; res.hi = m_state.xmm[dst_idx].hi & src.hi; }
        else if (op2 == 0x56) { res.lo = m_state.xmm[dst_idx].lo | src.lo; res.hi = m_state.xmm[dst_idx].hi | src.hi; }
        else { res.lo = m_state.xmm[dst_idx].lo ^ src.lo; res.hi = m_state.xmm[dst_idx].hi ^ src.hi; }
        m_state.xmm[dst_idx] = res;
        return true;
    }

    // ---- conversions GP -> XMM (0x2A cvtsi2ss/cvtsi2sd) ----
    if (op2 == 0x2A) {
        const int isz = p.rex_w ? 8 : (p.opsize ? 0 : 4);
        if (isz == 0) { fail_dec(); return true; }
        uint64_t v = 0;
        if (rm.is_reg) {
            v = ReadReg(rm.reg, isz);
        } else if (!m_mem.Read(rm.addr, &v, static_cast<size_t>(isz))) {
            fail_mem(); return true;
        }
        const int64_t iv = static_cast<int64_t>(SignExtend(v, isz));
        CpuState::XmmReg out = m_state.xmm[dst_idx];
        if (hasF2) SetXmmDouble(out, 0, static_cast<double>(iv));
        else SetXmmFloat(out, 0, static_cast<float>(iv));
        m_state.xmm[dst_idx] = out;
        return true;
    }

    // ---- conversions XMM -> GP (0x2C truncating, 0x2D rounding) ----
    if (op2 == 0x2C || op2 == 0x2D) {
        CpuState::XmmReg src{};
        if (!ReadXmmOperand(rm, p, hasF2 ? 8 : 4, src)) { fail_mem(); return true; }
        int64_t result = 0;
        if (hasF2) {
            const double d = XmmDouble(src, 0);
            result = (op2 == 0x2C) ? static_cast<int64_t>(d)
                                   : static_cast<int64_t>(std::nearbyint(d));
        } else {
            const float f = XmmFloat(src, 0);
            result = (op2 == 0x2C) ? static_cast<int64_t>(f)
                                   : static_cast<int64_t>(std::nearbyint(f));
        }
        // Invalid conversion -> indefinite value 0x8000... (SDM behaviour).
        if (std::isnan(hasF2 ? XmmDouble(src, 0) : static_cast<double>(XmmFloat(src, 0)))) {
            result = std::numeric_limits<int64_t>::min();
        }
        const int osz_out = p.rex_w ? 8 : 4;
        WriteReg(dst_idx, osz_out, static_cast<uint64_t>(result) & Mask(osz_out));
        return true;
    }

    // ---- movups/movss/movupd/movsd (0x10 load, 0x11 store) ----
    if (op2 == 0x10 || op2 == 0x11) {
        if (!has66 && !hasF2 && !hasF3) {
            // movups/movaps-family full-width move.
            if (op2 == 0x10) {
                CpuState::XmmReg src{};
                if (!ReadXmmOperand(rm, p, 16, src)) { fail_mem(); return true; }
                m_state.xmm[dst_idx] = src;
            } else {
                if (!WriteXmmOperand(rm, p, 16, m_state.xmm[dst_idx])) { fail_mem(); return true; }
            }
            return true;
        }
        const int width = (hasF2 || has66) ? 8 : 4;
        if (op2 == 0x10) {
            CpuState::XmmReg src{};
            if (!ReadXmmOperand(rm, p, width, src)) { fail_mem(); return true; }
            // movsd/movss into a register: copy the low element, preserve upper.
            CpuState::XmmReg dst = m_state.xmm[dst_idx];
            std::memcpy(&dst, &src, static_cast<size_t>(width));
            m_state.xmm[dst_idx] = dst;
        } else {
            if (!WriteXmmOperand(rm, p, width, m_state.xmm[dst_idx])) { fail_mem(); return true; }
        }
        return true;
    }

    // ---- movaps/movapd (0x28/0x29) ----
    if (op2 == 0x28 || op2 == 0x29) {
        if (op2 == 0x28) {
            CpuState::XmmReg src{};
            if (!ReadXmmOperand(rm, p, 16, src)) { fail_mem(); return true; }
            m_state.xmm[dst_idx] = src;
        } else {
            if (!WriteXmmOperand(rm, p, 16, m_state.xmm[dst_idx])) { fail_mem(); return true; }
        }
        return true;
    }

    // ---- cvtss2sd / cvtsd2ss / cvtps2pd / cvtpd2ps (0x5A) ----
    if (op2 == 0x5A) {
        CpuState::XmmReg src{};
        if (!ReadXmmOperand(rm, p, 8, src)) { fail_mem(); return true; }
        CpuState::XmmReg out = m_state.xmm[dst_idx];
        if (hasF3) {
            // cvtss2sd: float lane0 -> double lane0
            SetXmmDouble(out, 0, static_cast<double>(XmmFloat(src, 0)));
        } else if (hasF2) {
            // cvtsd2ss: double lane0 -> float lane0
            SetXmmFloat(out, 0, static_cast<float>(XmmDouble(src, 0)));
        } else if (has66) {
            // cvtpd2ps: two doubles -> two floats in low qword, upper zeroed
            CpuState::XmmReg res{};
            SetXmmFloat(res, 0, static_cast<float>(XmmDouble(src, 0)));
            SetXmmFloat(res, 1, static_cast<float>(XmmDouble(src, 1)));
            out = res;
        } else {
            // cvtps2pd: two floats (low qword) -> two doubles
            CpuState::XmmReg res{};
            SetXmmDouble(res, 0, static_cast<double>(XmmFloat(src, 0)));
            SetXmmDouble(res, 1, static_cast<double>(XmmFloat(src, 1)));
            out = res;
        }
        m_state.xmm[dst_idx] = out;
        return true;
    }

    // ---- sqrt (0x51) ----
    if (op2 == 0x51) {
        CpuState::XmmReg src{};
        if (!ReadXmmOperand(rm, p, 16, src)) { fail_mem(); return true; }
        CpuState::XmmReg out = m_state.xmm[dst_idx];
        if (hasF3) SetXmmFloat(out, 0, std::sqrt(XmmFloat(src, 0)));
        else if (hasF2) SetXmmDouble(out, 0, std::sqrt(XmmDouble(src, 0)));
        else if (has66) { SetXmmDouble(out, 0, std::sqrt(XmmDouble(src, 0))); SetXmmDouble(out, 1, std::sqrt(XmmDouble(src, 1))); }
        else { for (int l = 0; l < 4; ++l) SetXmmFloat(out, l, std::sqrt(XmmFloat(src, l))); }
        m_state.xmm[dst_idx] = out;
        return true;
    }

    // ---- binary arithmetic (0x58/0x59/0x5C/0x5D/0x5E/0x5F) ----
    {
        CpuState::XmmReg src{};
        if (!ReadXmmOperand(rm, p, 16, src)) { fail_mem(); return true; }
        CpuState::XmmReg out = m_state.xmm[dst_idx];
        const CpuState::XmmReg a = m_state.xmm[dst_idx];
        if (hasF3) {
            const float x = XmmFloat(a, 0), y = XmmFloat(src, 0);
            float res = 0.0f;
            switch (op2) {
                case 0x58: res = x + y; break;
                case 0x59: res = x * y; break;
                case 0x5C: res = x - y; break;
                case 0x5D: res = std::min(x, y); break;
                case 0x5E: res = x / y; break;
                case 0x5F: res = std::max(x, y); break;
            }
            SetXmmFloat(out, 0, res);
        } else if (hasF2) {
            const double x = XmmDouble(a, 0), y = XmmDouble(src, 0);
            double res = 0.0;
            switch (op2) {
                case 0x58: res = x + y; break;
                case 0x59: res = x * y; break;
                case 0x5C: res = x - y; break;
                case 0x5D: res = std::min(x, y); break;
                case 0x5E: res = x / y; break;
                case 0x5F: res = std::max(x, y); break;
            }
            SetXmmDouble(out, 0, res);
        } else if (has66) {
            for (int l = 0; l < 2; ++l) {
                const double x = XmmDouble(a, l), y = XmmDouble(src, l);
                double res = 0.0;
                switch (op2) {
                    case 0x58: res = x + y; break;
                    case 0x59: res = x * y; break;
                    case 0x5C: res = x - y; break;
                    case 0x5D: res = std::min(x, y); break;
                    case 0x5E: res = x / y; break;
                    case 0x5F: res = std::max(x, y); break;
                }
                SetXmmDouble(out, l, res);
            }
        } else {
            for (int l = 0; l < 4; ++l) {
                const float x = XmmFloat(a, l), y = XmmFloat(src, l);
                float res = 0.0f;
                switch (op2) {
                    case 0x58: res = x + y; break;
                    case 0x59: res = x * y; break;
                    case 0x5C: res = x - y; break;
                    case 0x5D: res = std::min(x, y); break;
                    case 0x5E: res = x / y; break;
                    case 0x5F: res = std::max(x, y); break;
                }
                SetXmmFloat(out, l, res);
            }
        }
        m_state.xmm[dst_idx] = out;
        return true;
    }
}

} // namespace PS5::CPU
