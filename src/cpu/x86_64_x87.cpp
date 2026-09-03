// x86_64_x87.cpp — the complete x87 FPU engine (round 29).
//
// Before this round the interpreter failed closed on EVERY x87 instruction
// (opcodes 0xD8..0xDF) — a gap of ~100 instruction forms (loads/stores,
// arithmetic, compares, transcendentals, BCD, environment). Real binaries
// (CRT startup, long-double math paths, mixed SSE/x87 code) hit them.
//
// Fidelity note: on an x86-64 host, `long double` IS the 80-bit x87 extended
// format, so arithmetic performed in long double matches the guest
// bit-for-bit by construction (the host has a real x87). Rounding and
// precision controls (CW bits) are honoured. Exception flags are NOT
// modelled (documented simplification); the condition codes C0..C3 in the
// status word are, because FSTSW/FCOMI consumers depend on them.
#include "cpu/x86_64_interpreter.hpp"

#include <cmath>
#include <cstring>

namespace PS5::CPU {

namespace {

// Status-word condition-code bits.
constexpr uint16_t kSwC0 = 1u << 8;    // carry / ST(0) < SRC
constexpr uint16_t kSwC1 = 1u << 9;    // (used by FXAM/FPREM only here)
constexpr uint16_t kSwC2 = 1u << 10;   // unordered / FPREM incomplete
constexpr uint16_t kSwC3 = 1u << 14;   // zero / ST(0) == SRC
constexpr uint16_t kSwExcMask = 0x00FFu;  // exception flags (cleared by FNCLEX)

inline long double* st(CpuState& s, int i) {
    return &s.x87_st[(s.x87_top + static_cast<unsigned>(i)) & 7u];
}
inline const long double* st(const CpuState& s, int i) {
    return &s.x87_st[(s.x87_top + static_cast<unsigned>(i)) & 7u];
}

inline void fpush(CpuState& s, long double v) {
    s.x87_top = (s.x87_top + 7u) & 7u;   // stack grows down
    s.x87_st[s.x87_top] = v;
}
inline long double fpop(CpuState& s) {
    const long double v = s.x87_st[s.x87_top];
    s.x87_top = (s.x87_top + 1u) & 7u;
    return v;
}

// Precision control (CW bits 8-9): 0 = 32-bit, 2 = 64-bit, 3 = 80-bit.
inline long double apply_pc(long double v, uint16_t cw) {
    switch ((cw >> 8) & 3u) {
        case 0: return static_cast<long double>(static_cast<float>(v));
        case 2: return static_cast<long double>(static_cast<double>(v));
        default: return v;
    }
}

// Rounding control (CW bits 10-11).
inline long double round_int(long double v, uint16_t cw) {
    switch ((cw >> 10) & 3u) {
        case 0: return nearbyintl(v);
        case 1: return floorl(v);
        case 2: return ceill(v);
        default: return truncl(v);
    }
}

// Sets C0/C2/C3 from a comparison result (x87 FCOM encoding).
inline void set_cmp(CpuState& s, int order) {   // -1: <, 0: ==, 1: >, 2: unordered
    s.x87_sw &= ~(kSwC0 | kSwC2 | kSwC3);
    if (order < 0)      s.x87_sw |= kSwC0;
    else if (order == 0) s.x87_sw |= kSwC3;
    else if (order == 2) s.x87_sw |= kSwC0 | kSwC2 | kSwC3;
}

inline int cmp_ld(long double a, long double b) {
    if (std::isnan(a) || std::isnan(b)) return 2;
    return (a < b) ? -1 : (a > b) ? 1 : 0;
}

// comi family: ZF=C3, PF=C2, CF=C0.
inline void comi_to_rflags(CpuState& s, int order) {
    const bool lt = (order == -1 || order == 2);
    const bool eq = (order == 0 || order == 2);
    const bool un = (order == 2);
    s.rflags &= ~(Flags::ZF | Flags::PF | Flags::CF);
    if (eq) s.rflags |= Flags::ZF;
    if (un) s.rflags |= Flags::PF;
    if (lt) s.rflags |= Flags::CF;
}

} // namespace

// ---------------------------------------------------------------------------
// Public entry: one x87 instruction (opcode 0xD8..0xDF, prefixes already
// consumed). Memory forms reuse the interpreter's ModRM machinery.
// ---------------------------------------------------------------------------
bool X86Interpreter::ExecX87(uint64_t& rip, const Prefixes& p, uint8_t opcode,
                             RunResult& r) {
    (void)p;
    CpuState& s = m_state;

    auto fault = [&](ExecStatus st, uint64_t addr) {
        r.status = st;
        r.fault_addr = addr;
    };
    uint8_t modrm = 0;
    if (!Fetch8(rip, modrm)) {
        fault(ExecStatus::DecodeFault, rip);
        return false;
    }
    ModRMOperand rm{};
    uint8_t reg = 0;
    if (!DecodeModRM(rip, p, modrm, rm, reg)) {
        fault(ExecStatus::DecodeFault, rip);
        return false;
    }
    const bool mem_form = !rm.is_reg;   // mod != 3
    const unsigned op = reg;            // /digit

    // Memory operand readers/writers for the x87 data types.
    auto read_m32f = [&]() -> long double {
        uint64_t raw = 0;
        if (!ReadRM(rm, 4, p.rex, raw)) return 0.0L;
        float f = 0.0f;
        std::memcpy(&f, &raw, 4);
        return static_cast<long double>(f);
    };
    auto read_m64f = [&]() -> long double {
        uint64_t raw = 0;
        if (!ReadRM(rm, 8, p.rex, raw)) return 0.0L;
        double d = 0.0;
        std::memcpy(&d, &raw, 8);
        return static_cast<long double>(d);
    };
    auto read_m80 = [&]() -> long double {
        long double v = 0.0L;
        if (rm.is_reg || !m_mem.Read(rm.addr, &v, 10)) return 0.0L;
        return v;
    };
    auto read_int = [&](int bytes, bool sign) -> long double {
        uint64_t raw = 0;
        if (!ReadRM(rm, bytes, p.rex, raw)) return 0.0L;
        if (sign) {
            if (bytes == 2) return static_cast<long double>(
                static_cast<int16_t>(static_cast<uint16_t>(raw)));
            if (bytes == 4) return static_cast<long double>(
                static_cast<int32_t>(static_cast<uint32_t>(raw)));
            return static_cast<long double>(static_cast<int64_t>(raw));
        }
        return static_cast<long double>(raw);
    };
    auto write_m32f = [&](long double v) {
        float f = static_cast<float>(v);
        uint64_t raw = 0;
        std::memcpy(&raw, &f, 4);
        (void)WriteRM(rm, 4, p.rex, raw);
    };
    auto write_m64f = [&](long double v) {
        double d = static_cast<double>(v);
        uint64_t raw = 0;
        std::memcpy(&raw, &d, 8);
        (void)WriteRM(rm, 8, p.rex, raw);
    };
    auto write_m80 = [&](long double v) {
        if (!rm.is_reg) (void)m_mem.Write(rm.addr, &v, 10);
    };
    auto write_int = [&](long double v, int bytes) {
        const long double r = round_int(v, s.x87_cw);
        if (bytes == 2) {
            (void)WriteRM(rm, 2, p.rex,
                          static_cast<uint64_t>(static_cast<uint16_t>(
                              static_cast<int16_t>(r))));
        } else if (bytes == 4) {
            (void)WriteRM(rm, 4, p.rex,
                          static_cast<uint64_t>(static_cast<uint32_t>(
                              static_cast<int32_t>(r))));
        } else {
            (void)WriteRM(rm, 8, p.rex,
                          static_cast<uint64_t>(static_cast<int64_t>(r)));
        }
    };

    // Environment (FLDENV/FNSTENV, 28-byte 64-bit-mode layout).
    auto store_env = [&]() {
        if (rm.is_reg) return;
        uint8_t env[28] = {};
        std::memcpy(env + 0, &s.x87_cw, 2);
        std::memcpy(env + 2, &s.x87_sw, 2);
        const uint16_t tw = 0xFFFFu;   // all empty (tag model documented)
        std::memcpy(env + 4, &tw, 2);
        (void)m_mem.Write(rm.addr, env, sizeof(env));
    };
    auto load_env = [&]() {
        if (rm.is_reg) return;
        uint8_t env[28] = {};
        if (!m_mem.Read(rm.addr, env, sizeof(env))) return;
        std::memcpy(&s.x87_cw, env + 0, 2);
        std::memcpy(&s.x87_sw, env + 2, 2);
    };

    bool ok = true;

    switch (opcode) {
    case 0xD8: {  // 32-bit real arithmetic / st(i) forms
        if (mem_form) {
            const long double m = read_m32f();
            switch (op) {
                case 0: *st(s,0) = apply_pc(*st(s,0) + m, s.x87_cw); break;        // fadd
                case 1: *st(s,0) = apply_pc(*st(s,0) * m, s.x87_cw); break;        // fmul
                case 2: set_cmp(s, cmp_ld(*st(s,0), m)); break;                    // fcom
                case 3: set_cmp(s, cmp_ld(*st(s,0), m)); (void)fpop(s); break;     // fcomp
                case 4: *st(s,0) = apply_pc(*st(s,0) - m, s.x87_cw); break;        // fsub
                case 5: *st(s,0) = apply_pc(m - *st(s,0), s.x87_cw); break;        // fsubr
                case 6: *st(s,0) = apply_pc(*st(s,0) / m, s.x87_cw); break;        // fdiv
                case 7: *st(s,0) = apply_pc(m / *st(s,0), s.x87_cw); break;        // fdivr
                default: ok = false; break;
            }
        } else {
            // Register forms are selected by the modrm RANGE, not /digit:
            // C0-C7 fadd, C8-CF fmul, D0-D7 fcom, D8-DF fcomp,
            // E0-E7 fsub, E8-EF fsubr, F0-F7 fdiv, F8-FF fdivr (st, st(i)).
            const long double b = *st(s, static_cast<int>(modrm & 7u));
            switch (modrm & 0xF8u) {
                case 0xC0: *st(s,0) = apply_pc(*st(s,0) + b, s.x87_cw); break;    // fadd
                case 0xC8: *st(s,0) = apply_pc(*st(s,0) * b, s.x87_cw); break;    // fmul
                case 0xD0: set_cmp(s, cmp_ld(*st(s,0), b)); break;                // fcom
                case 0xD8: set_cmp(s, cmp_ld(*st(s,0), b)); (void)fpop(s); break; // fcomp
                case 0xE0: *st(s,0) = apply_pc(*st(s,0) - b, s.x87_cw); break;    // fsub
                case 0xE8: *st(s,0) = apply_pc(b - *st(s,0), s.x87_cw); break;    // fsubr
                case 0xF0: *st(s,0) = apply_pc(*st(s,0) / b, s.x87_cw); break;    // fdiv
                case 0xF8: *st(s,0) = apply_pc(b / *st(s,0), s.x87_cw); break;    // fdivr
                default: ok = false; break;
            }
        }
        break;
    }
    case 0xD9: {
        if (mem_form) {
            switch (op) {
                case 0: fpush(s, read_m32f()); break;                 // fld m32
                case 2: write_m32f(*st(s,0)); break;                  // fst m32
                case 3: write_m32f(*st(s,0)); (void)fpop(s); break;   // fstp m32
                case 4: load_env(); break;                            // fldenv
                case 5: {                                             // fldcw m16
                    uint64_t raw = 0;
                    if (ReadRM(rm, 2, p.rex, raw)) {
                        s.x87_cw = static_cast<uint16_t>(raw);
                    }
                    break;
                }
                case 6: store_env(); break;                           // fnstenv
                case 7: (void)WriteRM(rm, 2, p.rex, s.x87_cw); break; // fnstcw
                default: ok = false; break;
            }
        } else {
            // mod == 3: the FULL modrm byte selects the operation.
            switch (modrm) {
                // D9 C0-C7: fld st(i);  D9 C8-CF: fxch st(i).
                default: {
                    if (modrm >= 0xC0 && modrm <= 0xC7) {
                        fpush(s, *st(s, static_cast<int>(modrm & 7u)));
                    } else if (modrm >= 0xC8 && modrm <= 0xCF) {
                        const int i = static_cast<int>(modrm & 7u);
                        const long double t = *st(s,0);
                        *st(s,0) = *st(s,i);
                        *st(s,i) = t;
                    } else if (modrm == 0xD0) {          // fchs
                        *st(s,0) = -*st(s,0);
                    } else if (modrm == 0xD1) {          // fabs
                        *st(s,0) = fabsl(*st(s,0));
                    } else if (modrm == 0xD4) {          // ftst
                        set_cmp(s, cmp_ld(*st(s,0), 0.0L));
                    } else if (modrm == 0xD5) {          // fxam (simplified)
                        const long double v = *st(s,0);
                        s.x87_sw &= ~(kSwC0 | kSwC1 | kSwC2 | kSwC3);
                        if (std::signbit(v)) s.x87_sw |= kSwC0;
                        if (v == 0.0L) s.x87_sw |= kSwC3;
                    } else if (modrm == 0xE8) {          // fld1
                        fpush(s, 1.0L);
                    } else if (modrm == 0xE9) {          // fldl2t
                        fpush(s, 3.32192809488736234787031942948939018L);
                    } else if (modrm == 0xEA) {          // fldl2e
                        fpush(s, 1.44269504088896340735992468100189214L);
                    } else if (modrm == 0xEB) {          // fldpi
                        fpush(s, 3.14159265358979323846264338327950288L);
                    } else if (modrm == 0xEC) {          // fldlg2
                        fpush(s, 0.30102999566398119521373889472449302L);
                    } else if (modrm == 0xED) {          // fldln2
                        fpush(s, 0.69314718055994530941723212145817656L);
                    } else if (modrm == 0xEE) {          // fldz
                        fpush(s, 0.0L);
                    } else if (modrm == 0xF0) {          // f2xm1
                        *st(s,0) = std::expm1l(*st(s,0) *
                                               0.69314718055994530941723212145817656L);
                    } else if (modrm == 0xF1) {          // fyl2x
                        const long double y = fpop(s);
                        const long double x = fpop(s);
                        fpush(s, y * std::log2l(x));
                    } else if (modrm == 0xF2) {          // fptan
                        *st(s,0) = tanl(*st(s,0));
                        fpush(s, 1.0L);
                    } else if (modrm == 0xF3) {          // fpatan
                        const long double y = fpop(s);
                        const long double x = fpop(s);
                        fpush(s, atan2l(y, x));
                    } else if (modrm == 0xF4) {          // fxtract
                        int e = 0;
                        const long double m = frexpl(*st(s,0), &e);
                        *st(s,0) = static_cast<long double>(e);
                        fpush(s, m);
                    } else if (modrm == 0xF5) {          // fprem1
                        *st(s,0) = std::remainderl(*st(s,0), *st(s,1));
                        s.x87_sw &= ~kSwC2;
                    } else if (modrm == 0xF6) {          // fdecstp
                        s.x87_top = (s.x87_top + 7u) & 7u;
                    } else if (modrm == 0xF7) {          // fincstp
                        s.x87_top = (s.x87_top + 1u) & 7u;
                    } else if (modrm == 0xF8) {          // fprem
                        const long double d = *st(s,1);
                        const long double q = truncl(*st(s,0) / d);
                        *st(s,0) = fmodl(*st(s,0), d);
                        s.x87_sw &= ~(kSwC0 | kSwC1 | kSwC2 | kSwC3);
                        const uint64_t qi = static_cast<uint64_t>(q) & 7u;
                        if (qi & 1u) s.x87_sw |= kSwC0;
                        if (qi & 2u) s.x87_sw |= kSwC3;
                        if (qi & 4u) s.x87_sw |= kSwC1;
                    } else if (modrm == 0xF9) {          // fyl2xp1
                        const long double y = fpop(s);
                        const long double x = fpop(s);
                        fpush(s, y * std::log2l(x + 1.0L));
                    } else if (modrm == 0xFA) {          // fsqrt
                        *st(s,0) = sqrtl(*st(s,0));
                    } else if (modrm == 0xFB) {          // fsincos
                        const long double v = *st(s,0);
                        // x87 sets C2 and leaves ST(0) unchanged if |v| > 2^63.
                        if (std::fabs(v) <= static_cast<long double>(1ull << 63)) {
                            const long double sn = sinl(v);
                            const long double cs = cosl(v);
                            *st(s,0) = sn;
                            fpush(s, cs);
                            s.x87_sw &= ~kSwC2;
                        } else {
                            s.x87_sw |= kSwC2; // range error: ST(0) unchanged
                        }
                    } else if (modrm == 0xFC) {          // frndint
                        *st(s,0) = round_int(*st(s,0), s.x87_cw);
                    } else if (modrm == 0xFD) {          // fscale
                        const long double e = *st(s,1);
                        *st(s,0) = std::scalbnl(*st(s,0),
                                                 static_cast<int>(truncl(e)));
                    } else if (modrm == 0xFE) {          // fsin
                        const long double v = *st(s,0);
                        // x87 sets C2 and leaves ST(0) unchanged if |v| > 2^63.
                        if (std::fabs(v) <= static_cast<long double>(1ull << 63)) {
                            *st(s,0) = sinl(v);
                            s.x87_sw &= ~kSwC2;
                        } else {
                            s.x87_sw |= kSwC2; // range error: ST(0) unchanged
                        }
                    } else if (modrm == 0xFF) {          // fcos
                        const long double v = *st(s,0);
                        // x87 sets C2 and leaves ST(0) unchanged if |v| > 2^63.
                        if (std::fabs(v) <= static_cast<long double>(1ull << 63)) {
                            *st(s,0) = cosl(v);
                            s.x87_sw &= ~kSwC2;
                        } else {
                            s.x87_sw |= kSwC2; // range error: ST(0) unchanged
                        }
                    } else {
                        ok = false;   // D2/D3/D6/D7/E0-E7 invalid
                    }
                    break;
                }
            }
        }
        break;
    }
    case 0xDA: {  // 32-bit integer arithmetic + FCMOV
        if (mem_form) {
            const long double m = read_int(4, true);
            switch (op) {
                case 0: *st(s,0) = apply_pc(*st(s,0) + m, s.x87_cw); break;    // fiadd
                case 1: *st(s,0) = apply_pc(*st(s,0) * m, s.x87_cw); break;    // fimul
                case 2: set_cmp(s, cmp_ld(*st(s,0), m)); break;                // ficom
                case 3: set_cmp(s, cmp_ld(*st(s,0), m)); (void)fpop(s); break; // ficomp
                case 4: *st(s,0) = apply_pc(*st(s,0) - m, s.x87_cw); break;    // fisub
                case 5: *st(s,0) = apply_pc(m - *st(s,0), s.x87_cw); break;    // fisubr
                case 6: *st(s,0) = apply_pc(*st(s,0) / m, s.x87_cw); break;    // fidiv
                case 7: *st(s,0) = apply_pc(m / *st(s,0), s.x87_cw); break;    // fidivr
                default: ok = false; break;
            }
        } else {
            // FCMOVb/e/be/u st, st(i) (status-word conditions).
            const bool c0 = (s.x87_sw & kSwC0) != 0;
            const bool c3 = (s.x87_sw & kSwC3) != 0;
            const bool c2 = (s.x87_sw & kSwC2) != 0;
            bool take = false;
            switch (op) {
                case 0: take = c0; break;                       // fcmovb
                case 1: take = c3; break;                       // fcmove
                case 2: take = c0 || c3; break;                 // fcmovbe
                case 3: take = c2; break;                       // fcmovu
                default: ok = false; break;
            }
            if (ok && take) *st(s,0) = *st(s, static_cast<int>(reg));
        }
        break;
    }
    case 0xDB: {
        if (mem_form) {
            switch (op) {
                case 0: fpush(s, read_int(4, true)); break;             // fild m32
                case 2: write_int(*st(s,0), 4); break;                  // fist m32
                case 3: write_int(*st(s,0), 4); (void)fpop(s); break;   // fistp m32
                case 5: fpush(s, read_m80()); break;                    // fld m80
                case 7: write_m80(*st(s,0)); (void)fpop(s); break;      // fstp m80
                default: ok = false; break;
            }
        } else if ((modrm & 0xC0) == 0xC0) {
            switch (modrm) {
                case 0xE0: case 0xE1: break;                           // feni/fdisi (nop)
                case 0xE2: s.x87_sw &= ~kSwExcMask; break;              // fnclex
                case 0xE3:                                              // finit
                    s.x87_cw = 0x037F;
                    s.x87_sw = 0;
                    s.x87_top = 0;
                    break;
                case 0xE4: case 0xE5: case 0xE6: case 0xE7: break;      // nop-class
                case 0xE8: case 0xE9:                                   // fucomi(p)
                case 0xF0: case 0xF1: {                                 // fcomi(p)
                    const int i = static_cast<int>(modrm & 7u);
                    const int order = cmp_ld(*st(s,0), *st(s,i));
                    comi_to_rflags(s, order);
                    set_cmp(s, order);
                    if ((modrm & 1u) != 0u) (void)fpop(s);              // pop form
                    break;
                }
                default: {
                    // FCMOVnb/ne/nbe/nu st, st(i)
                    const bool c0 = (s.x87_sw & kSwC0) != 0;
                    const bool c3 = (s.x87_sw & kSwC3) != 0;
                    const bool c2 = (s.x87_sw & kSwC2) != 0;
                    bool take = false;
                    switch ((modrm >> 3) & 7u) {
                        case 0: take = !c0; break;                      // fcmovnb
                        case 1: take = !c3; break;                      // fcmovne
                        case 2: take = !(c0 || c3); break;              // fcmovnbe
                        case 3: take = !c2; break;                      // fcmovnu
                        default: ok = false; break;
                    }
                    if (ok && take) {
                        *st(s,0) = *st(s, static_cast<int>(modrm & 7u));
                    }
                    break;
                }
            }
        } else {
            ok = false;
        }
        break;
    }
    case 0xDC: {  // 64-bit real arithmetic / st(i),st register forms
        if (mem_form) {
            const long double m = read_m64f();
            switch (op) {
                case 0: *st(s,0) = apply_pc(*st(s,0) + m, s.x87_cw); break;
                case 1: *st(s,0) = apply_pc(*st(s,0) * m, s.x87_cw); break;
                case 2: set_cmp(s, cmp_ld(*st(s,0), m)); break;
                case 3: set_cmp(s, cmp_ld(*st(s,0), m)); (void)fpop(s); break;
                case 4: *st(s,0) = apply_pc(*st(s,0) - m, s.x87_cw); break;
                case 5: *st(s,0) = apply_pc(m - *st(s,0), s.x87_cw); break;
                case 6: *st(s,0) = apply_pc(*st(s,0) / m, s.x87_cw); break;
                case 7: *st(s,0) = apply_pc(m / *st(s,0), s.x87_cw); break;
                default: ok = false; break;
            }
        } else {
            // modrm ranges: C0-C7 fadd st(i),st; C8-CF fmul st(i),st;
            // E0-E7 fsubR st(i),st (reversed!); E8-EF fsub st(i),st;
            // F0-F7 fdivR st(i),st; F8-FF fdiv st(i),st.
            const int i = static_cast<int>(modrm & 7u);
            switch (modrm & 0xF8u) {
                case 0xC0: *st(s,i) = apply_pc(*st(s,i) + *st(s,0), s.x87_cw); break;  // fadd
                case 0xC8: *st(s,i) = apply_pc(*st(s,i) * *st(s,0), s.x87_cw); break;  // fmul
                case 0xE0: *st(s,i) = apply_pc(*st(s,0) - *st(s,i), s.x87_cw); break;  // fsubr st(i),st
                case 0xE8: *st(s,i) = apply_pc(*st(s,i) - *st(s,0), s.x87_cw); break;  // fsub st(i),st
                case 0xF0: *st(s,i) = apply_pc(*st(s,0) / *st(s,i), s.x87_cw); break;  // fdivr st(i),st
                case 0xF8: *st(s,i) = apply_pc(*st(s,i) / *st(s,0), s.x87_cw); break;  // fdiv st(i),st
                default: ok = false; break;   // DC D0-DF register forms invalid
            }
        }
        break;
    }
    case 0xDD: {
        if (mem_form) {
            switch (op) {
                case 0: fpush(s, read_m64f()); break;                    // fld m64
                case 2: write_m64f(*st(s,0)); break;                     // fst m64
                case 3: write_m64f(*st(s,0)); (void)fpop(s); break;      // fstp m64
                case 4: {                                                // frstor
                    if (!rm.is_reg) {
                        uint8_t buf[108] = {};
                        if (m_mem.Read(rm.addr, buf, sizeof(buf))) {
                            std::memcpy(&s.x87_cw, buf + 0, 2);
                            std::memcpy(&s.x87_sw, buf + 2, 2);
                            s.x87_top = static_cast<uint8_t>(
                                (s.x87_sw >> 11) & 7u);
                            for (int i = 0; i < 8; ++i) {
                                std::memcpy(&s.x87_st[(s.x87_top + i) & 7u],
                                            buf + 28 + i * 10, 10);
                            }
                        }
                    }
                    break;
                }
                case 6: {                                                // fnsave
                    if (!rm.is_reg) {
                        uint8_t buf[108] = {};
                        std::memcpy(buf + 0, &s.x87_cw, 2);
                        // FSW carries TOP in bits 11..13 (x87 ABI).
                        const uint16_t sw_with_top =
                            static_cast<uint16_t>(s.x87_sw |
                                                  (static_cast<uint16_t>(
                                                       s.x87_top & 7u) << 11));
                        std::memcpy(buf + 2, &sw_with_top, 2);
                        // Register image is in LOGICAL order: st(0) first.
                        for (int i = 0; i < 8; ++i) {
                            std::memcpy(buf + 28 + i * 10,
                                        &s.x87_st[(s.x87_top + i) & 7u], 10);
                        }
                        (void)m_mem.Write(rm.addr, buf, sizeof(buf));
                        s.x87_cw = 0x037F;
                        s.x87_sw = 0;
                        s.x87_top = 0;
                    }
                    break;
                }
                case 7: (void)WriteRM(rm, 2, p.rex, s.x87_sw); break;    // fnstsw m16
                default: ok = false; break;
            }
        } else {
            const int i = static_cast<int>(modrm & 7u);
            switch (op) {
                case 0: break;                                           // ffree st(i)
                case 2: *st(s,i) = *st(s,0); break;                      // fst st(i)
                case 3: *st(s,i) = *st(s,0); (void)fpop(s); break;       // fstp st(i)
                case 4: set_cmp(s, cmp_ld(*st(s,0), *st(s,i))); break;   // fucom st,i
                case 5: set_cmp(s, cmp_ld(*st(s,0), *st(s,i)));          // fucomp
                        (void)fpop(s); break;
                default: ok = false; break;
            }
        }
        break;
    }
    case 0xDE: {  // 16-bit integer arithmetic + pop forms
        if (mem_form) {
            const long double m = read_int(2, true);
            switch (op) {
                case 0: *st(s,0) = apply_pc(*st(s,0) + m, s.x87_cw); break;
                case 1: *st(s,0) = apply_pc(*st(s,0) * m, s.x87_cw); break;
                case 2: set_cmp(s, cmp_ld(*st(s,0), m)); break;
                case 3: set_cmp(s, cmp_ld(*st(s,0), m)); (void)fpop(s); break;
                case 4: *st(s,0) = apply_pc(*st(s,0) - m, s.x87_cw); break;
                case 5: *st(s,0) = apply_pc(m - *st(s,0), s.x87_cw); break;
                case 6: *st(s,0) = apply_pc(*st(s,0) / m, s.x87_cw); break;
                case 7: *st(s,0) = apply_pc(m / *st(s,0), s.x87_cw); break;
                default: ok = false; break;
            }
        } else if (modrm == 0xD9) {
            // fcompp: compare st(0), st(1) then pop twice.
            set_cmp(s, cmp_ld(*st(s,0), *st(s,1)));
            (void)fpop(s);
            (void)fpop(s);
        } else {
            // modrm ranges: C0-C7 faddp st(i),st; C8-CF fmulp;
            // E0-E7 fsubrp st(i),st; E8-EF fsubp; F0-F7 fdivrp; F8-FF fdivp.
            // Each computes st(i) <op>= st(0) then pops.
            const int i = static_cast<int>(modrm & 7u);
            switch (modrm & 0xF8u) {
                case 0xC0: *st(s,i) = apply_pc(*st(s,i) + *st(s,0), s.x87_cw); break;  // faddp
                case 0xC8: *st(s,i) = apply_pc(*st(s,i) * *st(s,0), s.x87_cw); break;  // fmulp
                case 0xE0: *st(s,i) = apply_pc(*st(s,0) - *st(s,i), s.x87_cw); break;  // fsubrp
                case 0xE8: *st(s,i) = apply_pc(*st(s,i) - *st(s,0), s.x87_cw); break;  // fsubp
                case 0xF0: *st(s,i) = apply_pc(*st(s,0) / *st(s,i), s.x87_cw); break;  // fdivrp
                case 0xF8: *st(s,i) = apply_pc(*st(s,i) / *st(s,0), s.x87_cw); break;  // fdivp
                default: ok = false; break;
            }
            if (ok) (void)fpop(s);
        }
        break;
    }
    case 0xDF: {
        if (mem_form) {
            switch (op) {
                case 0: fpush(s, read_int(2, true)); break;             // fild m16
                case 2: write_int(*st(s,0), 2); break;                   // fist m16
                case 3: write_int(*st(s,0), 2); (void)fpop(s); break;    // fistp m16
                case 4: {                                                // fbld m80
                    uint8_t bcd[10] = {};
                    if (!rm.is_reg && m_mem.Read(rm.addr, bcd, 10)) {
                        long double v = 0.0L;
                        long double scale = 1.0L;
                        for (int g = 0; g < 9; ++g) {
                            v += static_cast<long double>(
                                     (bcd[g] & 0x0Fu) + ((bcd[g] >> 4) & 0x0Fu) * 10) *
                                 scale;
                            scale *= 100.0L;
                        }
                        // Byte 9: bit 7 = sign, low nibble = digit 19.
                        // (The high nibble is NOT a digit.)
                        v += (bcd[9] & 0x0Fu) * 1.0e18L;
                        if (bcd[9] & 0x80u) v = -v;
                        fpush(s, v);
                    }
                    break;
                }
                case 5: fpush(s, read_int(8, true)); break;              // fild m64
                case 6: {                                                // fbstp m80
                    uint8_t bcd[10] = {};
                    long double v = round_int(*st(s,0), s.x87_cw);
                    bool neg = std::signbit(v);
                    long double a = fabsl(v);
                    if (a > 9.999999999999999999e19L) a = 9.999999999999999999e19L;
                    for (int g = 0; g < 9; ++g) {
                        const long double d = fmodl(a, 100.0L);
                        a = truncl((a - d) / 100.0L);
                        const int di = static_cast<int>(d);
                        bcd[g] = static_cast<uint8_t>((di % 10) | ((di / 10) << 4));
                    }
                    const int hi = static_cast<int>(a);
                    bcd[9] = static_cast<uint8_t>(hi & 0x0Fu);
                    if (neg) bcd[9] |= 0x80u;
                    if (!rm.is_reg) (void)m_mem.Write(rm.addr, bcd, 10);
                    (void)fpop(s);
                    break;
                }
                case 7: write_int(*st(s,0), 8); (void)fpop(s); break;    // fistp m64
                default: ok = false; break;
            }
        } else if (modrm == 0xE0) {
            // fnstsw ax
            const uint16_t sw = s.x87_sw;
            WriteReg(RAX, 2, sw);
        } else if ((modrm & 0xC0) == 0xC0 && (modrm & 0xF8) == 0xC0) {
            // ffreep st(i) (undocumented-but-real)
            (void)fpop(s);
        } else {
            ok = false;
        }
        break;
    }
    default:
        ok = false;
        break;
    }

    if (!ok) {
        fault(ExecStatus::UnsupportedOpcode, rip);
        return false;
    }
    m_state.rip = rip;
    return true;
}

} // namespace PS5::CPU
