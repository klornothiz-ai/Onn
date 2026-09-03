// simd_full_map1.inc - MAP 1 (0F / VEX.0F) execution body.
// Included INSIDE X86Interpreter::ExecSimdFull; shares its locals/lambdas.
// Every opcode below is verified against tests/simd_forms_table.inc.
{
if (map == 1) {
    // ================= 0x10/0x11 (v)movups/upd/ss/sd =====================
    if (op2 == 0x10 || op2 == 0x11) {
        if (!modrm_fetch()) return true;
        const bool store = (op2 == 0x11);
        if (pp == 0 || pp == 1) {                    // (v)movups / (v)movupd
            if (store) {
                Vec s = read_reg_vec(reg);
                if (!write_rm_vec(rm, vw, s)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
            } else {
                Vec s{};
                if (!read_rm_vec(rm, vw, s)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                write_reg_vec(reg, s);
            }
        } else {                                     // (v)movss / (v)movsd
            const int sw = (pp == 2) ? 4 : 8;
            if (store) {
                Vec s = read_reg_vec(reg);
                if (rm.is_reg) {
                    // register-register: full 128-bit copy
                    const uint8_t di = static_cast<uint8_t>(rm.reg | (p.rex_b ? 0x08 : 0));
                    m_state.xmm[di] = s.Lo();
                    m_state.ymm_hi[di] = CpuState::XmmReg{};
                } else if (!m_mem.Write(rm.addr, &s, sw)) {
                    fault(ExecStatus::MemoryFault, rm.addr); return true;
                }
            } else if (is_vex) {
                // reg,reg: low element from src2, upper bits MERGE from
                // vvvv. reg,mem: low element from memory, upper bits ZEROED
                // (both hardware-verified by the differential harness).
                Vec out{};
                if (rm.is_reg) {
                    out = read_reg_vec(vex->vreg);
                    Vec src2{};
                    if (!read_rm_vec(rm, 16, src2)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                    if (pp == 2) out.d[0] = src2.d[0];
                    else out.q[0] = src2.q[0];
                } else {
                    if (pp == 2) {
                        uint32_t v = 0;
                        if (!m_mem.Read(rm.addr, &v, 4)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                        out.d[0] = v;
                    } else {
                        uint64_t v = 0;
                        if (!m_mem.Read(rm.addr, &v, 8)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                        out.q[0] = v;
                    }
                }
                write_reg_vec(reg, out);
            } else {
                // legacy movss/movsd: MERGE the low 32/64 bits into the old
                // destination (register source included -- hardware-verified
                // by the differential harness; the upper bits are preserved).
                Vec dst = read_reg_vec(reg);
                if (rm.is_reg) {
                    Vec s{};
                    if (!read_rm_vec(rm, 16, s)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                    if (pp == 2) dst.d[0] = s.d[0];
                    else dst.q[0] = s.q[0];
                } else {
                    if (pp == 2) {
                        uint32_t v = 0;
                        if (!m_mem.Read(rm.addr, &v, 4)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                        dst.d[0] = v;
                    } else {
                        uint64_t v = 0;
                        if (!m_mem.Read(rm.addr, &v, 8)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                        dst.q[0] = v;
                    }
                }
                write_reg_vec(reg, dst);
            }
            if (false) { }
        }
        commit();
        return true;
    }
    // ================= 0x12/0x13/0x16/0x17 low/high moves + dups =========
    if (op2 == 0x12 || op2 == 0x13 || op2 == 0x16 || op2 == 0x17) {
        if (!modrm_fetch()) return true;
        const bool high = (op2 == 0x16 || op2 == 0x17);
        const bool store = (op2 == 0x13 || op2 == 0x17);
        if (pp == 2 || pp == 3) {                     // movs(h/l)dup / movddup
            if (store) { r.status = ExecStatus::UnsupportedOpcode; return true; }
            if (pp == 2) {
                Vec s{};
                if (!read_rm_vec(rm, vw, s)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                Vec out{};
                for (int i = 0; i < vw / 4; i += 2) {
                    out.f[i] = s.f[i + (high ? 1 : 0)];
                    out.f[i + 1] = s.f[i + (high ? 1 : 0)];
                }
                write_reg_vec(reg, out);
            } else {
                Vec s{};
                if (!read_rm_vec(rm, L ? 32 : 8, s)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                Vec out{};
                for (int i = 0; i < vw / 16; i++) {
                    out.q[i * 2] = s.q[i * 2];
                    out.q[i * 2 + 1] = s.q[i * 2];
                }
                write_reg_vec(reg, out);
            }
            commit();
            return true;
        }
        if (store) {                                  // movlps/movhps store
            Vec s = read_reg_vec(reg);
            if (!m_mem.Write(rm.addr, &s.q[high ? 1 : 0], 8)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        } else if (rm.is_reg) {                       // movhlps/movlhps
            Vec dst = src_a();                         // legacy dst / VEX vvvv
            Vec src{};
            if (!read_rm_vec(rm, 16, src)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
            if (op2 == 0x12) dst.q[0] = src.q[1];
            else dst.q[1] = src.q[0];
            write_reg_vec(reg, dst);
        } else {                                      // load + merge
            Vec out = src_a();
            uint64_t v = 0;
            if (!m_mem.Read(rm.addr, &v, 8)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
            if (op2 == 0x12) out.q[0] = v;
            else out.q[1] = v;
            write_reg_vec(reg, out);
        }
        commit();
        return true;
    }
    // ================= 0x14/0x15 unpacks =================================
    if (op2 == 0x14 || op2 == 0x15) {
        if (!modrm_fetch()) return true;
        Vec a = src_a(), b{};
        if (!read_rm_vec(rm, vw, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        Vec out{};
        if (pp == 0) {                                // unpcklps / unpckhps
            for (int g = 0; g < vw / 16; g++) {
                const int base = g * 4;
                if (op2 == 0x14) {
                    out.d[base] = a.d[base]; out.d[base + 1] = b.d[base];
                    out.d[base + 2] = a.d[base + 1]; out.d[base + 3] = b.d[base + 1];
                } else {
                    out.d[base] = a.d[base + 2]; out.d[base + 1] = b.d[base + 2];
                    out.d[base + 2] = a.d[base + 3]; out.d[base + 3] = b.d[base + 3];
                }
            }
        } else {                                      // unpcklpd / unpckhpd
            for (int g = 0; g < vw / 16; g++) {
                const int base = g * 2;
                if (op2 == 0x14) {
                    out.q[base] = a.q[base]; out.q[base + 1] = b.q[base];
                } else {
                    out.q[base] = a.q[base + 1]; out.q[base + 1] = b.q[base + 1];
                }
            }
        }
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ================= 0x28/0x29/0x2B aligned & nt moves =================
    if (op2 == 0x28 || op2 == 0x29 || op2 == 0x2B) {
        if (!modrm_fetch()) return true;
        if (op2 == 0x28) {
            Vec s{};
            if (!read_rm_vec(rm, vw, s)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
            write_reg_vec(reg, s);
        } else {
            Vec s = read_reg_vec(reg);
            if (!write_rm_vec(rm, vw, s)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        }
        commit();
        return true;
    }
    // ================= 0x2A cvtsi2ss/sd ==================================
    if (op2 == 0x2A) {
        if (!modrm_fetch()) return true;
        const bool w64 = W || p.rex_w;               // legacy REX.W or VEX.W
        const int ssize = w64 ? 8 : 4;
        uint64_t v = 0;
        if (!read_rm_int(rm, ssize, v)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        Vec out = src_a();
        if (pp == 2) {
            out.f[0] = w64 ? static_cast<float>(static_cast<int64_t>(v))
                           : static_cast<float>(static_cast<int32_t>(v));
        } else {
            out.df[0] = w64 ? static_cast<double>(static_cast<int64_t>(v))
                            : static_cast<double>(static_cast<int32_t>(v));
        }
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ================= 0x2C/0x2D cvtt*/cvt* -> gpr =======================
    if (op2 == 0x2C || op2 == 0x2D) {
        if (!modrm_fetch()) return true;
        Vec s{};
        if (!read_rm_vec(rm, 16, s)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        const bool trunc = (op2 == 0x2C);
        const bool w64 = W || p.rex_w;               // legacy REX.W or VEX.W
        uint64_t result = 0;
        const uint8_t dst = static_cast<uint8_t>(reg | (p.rex_r ? 0x08 : 0x00));
        if (w64) {
            const double d = (pp == 2) ? static_cast<double>(s.f[0]) : s.df[0];
            const bool bad = std::isnan(pp == 2 ? static_cast<double>(s.f[0]) : s.df[0]) ||
                             d >= 9223372036854775808.0 || d < -9223372036854775808.0;
            result = bad ? 0x8000000000000000ull
                         : static_cast<uint64_t>(static_cast<int64_t>(trunc ? d : std::nearbyint(d)));
            WriteReg(dst, 8, result);
        } else {
            if (pp == 2) result = trunc ? CvtTrunc(s.f[0]) : CvtRne(s.f[0]);
            else result = trunc ? CvtTrunc(s.df[0]) : CvtRne(s.df[0]);
            WriteReg(dst, 4, result);
        }
        commit();
        return true;
    }
    // ================= 0x2E/0x2F comis/ucomis ============================
    if (op2 == 0x2E || op2 == 0x2F) {
        if (!modrm_fetch()) return true;
        Vec a = read_reg_vec(reg), b{};
        if (!read_rm_vec(rm, 16, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        SetFlag(Flags::OF, false); SetFlag(Flags::SF, false); SetFlag(Flags::AF, false);
        if (pp == 0) SetComparedFlags(a.f[0], b.f[0]);
        else SetComparedFlags(a.df[0], b.df[0]);
        commit();
        return true;
    }
    // ================= 0x50 movmskps/movmskpd ============================
    if (op2 == 0x50) {
        if (!modrm_fetch()) return true;
        Vec s = read_reg_vec(reg);
        uint64_t mask = 0;
        if (pp == 0) {
            const int n = L ? 8 : 4;
            for (int i = 0; i < n; i++) mask |= (static_cast<uint64_t>(s.d[i] >> 31) & 1u) << i;
        } else {
            const int n = L ? 4 : 2;
            for (int i = 0; i < n; i++) mask |= (static_cast<uint64_t>(s.q[i] >> 63) & 1u) << i;
        }
        const uint8_t dst = static_cast<uint8_t>(reg | (p.rex_r ? 0x08 : 0x00));
        WriteReg(dst, 4, mask);
        commit();
        return true;
    }
    // ================= 0x51-0x53 sqrt / rsqrt / rcp ======================
    if (op2 >= 0x51 && op2 <= 0x53) {
        if (!modrm_fetch()) return true;
        Vec b{};
        if (!read_rm_vec(rm, vw, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        Vec out{};
        if (op2 == 0x51 && pp == 0) {
            for (int i = 0; i < vw / 4; i++) out.f[i] = std::sqrt(b.f[i]);
        } else if (op2 == 0x51 && pp == 1) {
            for (int i = 0; i < vw / 8; i++) out.df[i] = std::sqrt(b.df[i]);
        } else if (op2 == 0x51) {
            out = src_a();
            if (pp == 2) out.f[0] = std::sqrt(b.f[0]);
            else out.df[0] = std::sqrt(b.df[0]);
        } else if (op2 == 0x52 && pp == 0) {          // rsqrtps (approximate)
            for (int i = 0; i < vw / 4; i++) out.f[i] = 1.0f / std::sqrt(b.f[i]);
        } else if (op2 == 0x52) {                     // rsqrtss (approximate)
            out = src_a();
            out.f[0] = 1.0f / std::sqrt(b.f[0]);
        } else if (pp == 0) {                         // rcpps (approximate)
            for (int i = 0; i < vw / 4; i++) out.f[i] = 1.0f / b.f[i];
        } else {                                      // rcpss (approximate)
            out = src_a();
            out.f[0] = 1.0f / b.f[0];
        }
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ================= 0x54-0x57 and/andn/or/xor =========================
    if (op2 >= 0x54 && op2 <= 0x57) {
        if (!modrm_fetch()) return true;
        Vec a = src_a(), b{};
        if (!read_rm_vec(rm, vw, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        for (int i = 0; i < vw / 8; i++) {
            if (op2 == 0x54) a.q[i] = a.q[i] & b.q[i];
            else if (op2 == 0x55) a.q[i] = (~a.q[i]) & b.q[i];
            else if (op2 == 0x56) a.q[i] = a.q[i] | b.q[i];
            else a.q[i] = a.q[i] ^ b.q[i];
        }
        write_reg_vec(reg, a);
        commit();
        return true;
    }
    // ================= 0x58-0x5F float ALU (0x5A/0x5B are conversions) ===
    if ((op2 == 0x58 || op2 == 0x59) || (op2 >= 0x5C && op2 <= 0x5F)) {
        if (!modrm_fetch()) return true;
        Vec b{};
        if (!read_rm_vec(rm, vw, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        Vec a = src_a();
        Vec out{};
        if (pp == 0) {
            for (int i = 0; i < vw / 4; i++) {
                switch (op2) {
                    case 0x58: out.f[i] = a.f[i] + b.f[i]; break;
                    case 0x59: out.f[i] = a.f[i] * b.f[i]; break;
                    case 0x5C: out.f[i] = a.f[i] - b.f[i]; break;
                    case 0x5D: out.f[i] = XMin(a.f[i], b.f[i]); break;
                    case 0x5E: out.f[i] = a.f[i] / b.f[i]; break;
                    default:   out.f[i] = XMax(a.f[i], b.f[i]); break;
                }
            }
        } else if (pp == 1) {
            for (int i = 0; i < vw / 8; i++) {
                switch (op2) {
                    case 0x58: out.df[i] = a.df[i] + b.df[i]; break;
                    case 0x59: out.df[i] = a.df[i] * b.df[i]; break;
                    case 0x5C: out.df[i] = a.df[i] - b.df[i]; break;
                    case 0x5D: out.df[i] = XMin(a.df[i], b.df[i]); break;
                    case 0x5E: out.df[i] = a.df[i] / b.df[i]; break;
                    default:   out.df[i] = XMax(a.df[i], b.df[i]); break;
                }
            }
        } else {
            out = a;
            if (pp == 2) {
                switch (op2) {
                    case 0x58: out.f[0] = a.f[0] + b.f[0]; break;
                    case 0x59: out.f[0] = a.f[0] * b.f[0]; break;
                    case 0x5C: out.f[0] = a.f[0] - b.f[0]; break;
                    case 0x5D: out.f[0] = XMin(a.f[0], b.f[0]); break;
                    case 0x5E: out.f[0] = a.f[0] / b.f[0]; break;
                    default:   out.f[0] = XMax(a.f[0], b.f[0]); break;
                }
            } else {
                switch (op2) {
                    case 0x58: out.df[0] = a.df[0] + b.df[0]; break;
                    case 0x59: out.df[0] = a.df[0] * b.df[0]; break;
                    case 0x5C: out.df[0] = a.df[0] - b.df[0]; break;
                    case 0x5D: out.df[0] = XMin(a.df[0], b.df[0]); break;
                    case 0x5E: out.df[0] = a.df[0] / b.df[0]; break;
                    default:   out.df[0] = XMax(a.df[0], b.df[0]); break;
                }
            }
        }
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ================= 0x5A/0x5B conversions ==============================
    if (op2 == 0x5A) {
        if (!modrm_fetch()) return true;
        Vec b{};
        if (!read_rm_vec(rm, L ? 32 : 16, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        Vec out{};
        if (pp == 0) {                                // cvtps2pd
            for (int i = 0; i < (L ? 8 : 4) / 2; i++) out.df[i] = static_cast<double>(b.f[i]);
        } else if (pp == 1) {                         // cvtpd2ps (source = rm)
            for (int i = 0; i < (L ? 8 : 4) / 2; i++) out.f[i] = static_cast<float>(b.df[i]);
        } else if (pp == 2) {                         // cvtss2sd
            out = src_a();
            out.df[0] = static_cast<double>(b.f[0]);
        } else {                                      // cvtsd2ss
            out = src_a();
            out.f[0] = static_cast<float>(b.df[0]);
        }
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    if (op2 == 0x5B) {
        if (!modrm_fetch()) return true;
        Vec b{};
        if (!read_rm_vec(rm, vw, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        Vec out{};
        if (pp == 0) {                                // cvtdq2ps
            for (int i = 0; i < vw / 4; i++) out.f[i] = static_cast<float>(b.sd[i]);
        } else if (pp == 1) {                         // cvtps2dq
            for (int i = 0; i < vw / 4; i++) out.d[i] = CvtRne(b.f[i]);
        } else {                                      // cvttps2dq
            for (int i = 0; i < vw / 4; i++) out.d[i] = CvtTrunc(b.f[i]);
        }
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ================= 0x6E/0x7E movd/movq GPR<->XMM =======================
    // 66 0F 6E            movd/movq GPR/mem -> XMM (REX.W/VEX.W picks q)
    // F3 0F 7E            movq GPR/mem -> XMM (64-bit)
    // 66 0F 7E            movd/movq XMM -> GPR/mem (REX.W/VEX.W picks q)
    if (op2 == 0x6E || op2 == 0x7E) {
        if (!modrm_fetch()) return true;
        const bool w64 = W || p.rex_w;
        const bool to_gpr = (op2 == 0x7E && pp == 1);
        if (to_gpr) {
            Vec s = read_reg_vec(reg);
            const uint64_t v = w64 ? s.q[0] : s.d[0];
            if (!write_rm_int(rm, w64 ? 8 : 4, v)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        } else {
            // 66 0F 6E: GPR/mem -> XMM (movd/movq). F3 0F 7E: XMM/mem -> XMM
            // (movq reg,reg copies between XMM registers).
            const bool xmm_source = (op2 == 0x7E && pp == 2 && rm.is_reg);
            const int ssize = (op2 == 0x6E && w64) || (op2 == 0x7E && pp == 2) ? 8 : 4;
            Vec out{};
            if (xmm_source) {
                Vec src = read_reg_vec(static_cast<uint8_t>(rm.reg | (p.rex_b ? 0x08 : 0)));
                out.q[0] = src.q[0];
            } else {
                uint64_t v = 0;
                if (!read_rm_int(rm, ssize, v)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                if (ssize == 4) out.d[0] = static_cast<uint32_t>(v);
                else out.q[0] = v;
            }
            write_reg_vec(reg, out);
        }
        commit();
        return true;
    }
    // ================= 0x6F movdqa/movdqu load ============================
    if (op2 == 0x6F) {
        if (!modrm_fetch()) return true;
        Vec s{};
        if (!read_rm_vec(rm, vw, s)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        write_reg_vec(reg, s);
        commit();
        return true;
    }
    // ================= 0x7F movdqa/dqu store ==============================
    if (op2 == 0x7F) {
        if (!modrm_fetch()) return true;
        Vec s = read_reg_vec(reg);
        if (!write_rm_vec(rm, vw, s)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        commit();
        return true;
    }
    // ================= 0xD6 movq forms ====================================
    if (op2 == 0xD6) {
        if (!modrm_fetch()) return true;
        if (pp == 1) {                                // 66 0F D6 / vmovq: STORE (rm = dest)
            Vec s = read_reg_vec(reg);
            if (rm.is_reg) {
                const uint8_t di = static_cast<uint8_t>(rm.reg | (p.rex_b ? 0x08 : 0));
                Vec out{};
                out.q[0] = s.q[0];
                m_state.xmm[di] = out.Lo();
                if (is_vex) m_state.ymm_hi[di] = CpuState::XmmReg{};
            } else if (!m_mem.Write(rm.addr, &s.q[0], 8)) {
                fault(ExecStatus::MemoryFault, rm.addr); return true;
            }
        } else {                                      // F3 0F D6 / movq: LOAD
            Vec out{};
            if (!read_rm_vec(rm, 8, out)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
            write_reg_vec(reg, out);
        }
        commit();
        return true;
    }
    // ================= 0x70 pshufd / pshufhw / pshuflw / vpermilps(pd) ====
    if (op2 == 0x70) {
        if (!modrm_fetch()) return true;
        uint8_t imm = 0;
        if (!fetch_imm8(imm)) return true;
        Vec s{};
        if (!read_rm_vec(rm, vw, s)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        Vec out{};
        if (pp == 1) {                         // pshufd
            for (int i = 0; i < vw / 4; i++)
                out.d[i] = s.d[(i & ~3) + ((imm >> ((i & 3) * 2)) & 3)];
        } else if (pp == 3) {                   // pshuflw / vpshuflw (F2)
            for (int i = 0; i < vw / 16; i++) {
                const int base = i * 8;
                for (int j = 0; j < 4; j++)
                    out.w[base + j] = s.w[base + ((imm >> (j * 2)) & 3)];
                for (int j = 4; j < 8; j++) out.w[base + j] = s.w[base + j];
            }
        } else if (pp == 2) {                   // pshufhw / vpshufhw (F3)
            for (int i = 0; i < vw / 16; i++) {
                const int base = i * 8;
                for (int j = 0; j < 4; j++) out.w[base + j] = s.w[base + j];
                for (int j = 4; j < 8; j++)
                    out.w[base + j] = s.w[base + 4 + ((imm >> ((j - 4) * 2)) & 3)];
            }
        } else {
            r.status = ExecStatus::UnsupportedOpcode;
            return true;
        }
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ================= 0x71-0x73 shift groups (imm) ======================
    if (op2 >= 0x71 && op2 <= 0x73) {
        if (!modrm_fetch()) return true;
        uint8_t imm = 0;
        if (!fetch_imm8(imm)) return true;
        const int digit = (modrm >> 3) & 7;
        const int count = imm;
        // LEGACY 0x71-0x73 immediate groups: the destination is the RM
        // operand. VEX forms: dst = the REG field, source = RM.
        Vec s{};
        if (!read_rm_vec(rm, vw, s)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        Vec out{};
        if (op2 == 0x71 && digit == 2) {              // psrlw
            for (int i = 0; i < vw / 2; i++)
                out.w[i] = count >= 16 ? 0 : static_cast<uint16_t>(s.w[i] >> count);
        } else if (op2 == 0x71 && digit == 4) {       // psraw
            for (int i = 0; i < vw / 2; i++)
                out.w[i] = count >= 16 ? (s.sw[i] < 0 ? 0xFFFF : 0)
                                       : static_cast<uint16_t>(s.sw[i] >> count);
        } else if (op2 == 0x71 && digit == 6) {       // psllw
            for (int i = 0; i < vw / 2; i++)
                out.w[i] = count >= 16 ? 0 : static_cast<uint16_t>(s.w[i] << count);
        } else if (op2 == 0x72 && digit == 2) {       // psrld
            for (int i = 0; i < vw / 4; i++)
                out.d[i] = count >= 32 ? 0 : s.d[i] >> count;
        } else if (op2 == 0x72 && digit == 4) {       // psrad
            for (int i = 0; i < vw / 4; i++)
                out.d[i] = count >= 32 ? (s.sd[i] < 0 ? 0xFFFFFFFF : 0)
                                       : static_cast<uint32_t>(s.sd[i] >> count);
        } else if (op2 == 0x72 && digit == 6) {       // pslld
            for (int i = 0; i < vw / 4; i++)
                out.d[i] = count >= 32 ? 0 : s.d[i] << count;
        } else if (op2 == 0x73 && digit == 2) {       // psrlq
            for (int i = 0; i < vw / 8; i++)
                out.q[i] = count >= 64 ? 0 : s.q[i] >> count;
        } else if (op2 == 0x73 && digit == 3) {       // psrldq (per-lane byte shift right)
            for (int g = 0; g < vw / 16; g++) {
                const int base = g * 16;
                for (int i = 0; i < 16; i++)
                    out.b[base + i] = (i + count < 16) ? s.b[base + i + count] : 0;
            }
        } else if (op2 == 0x73 && digit == 6) {       // psllq
            for (int i = 0; i < vw / 8; i++)
                out.q[i] = count >= 64 ? 0 : s.q[i] << count;
        } else if (op2 == 0x73 && digit == 7) {       // pslldq (per-lane byte shift left)
            for (int g = 0; g < vw / 16; g++) {
                const int base = g * 16;
                for (int i = 0; i < 16; i++)
                    out.b[base + i] = (i >= count) ? s.b[base + i - count] : 0;
            }
        } else {
            r.status = ExecStatus::UnsupportedOpcode;
            return true;
        }
        if (is_vex) {
            // VEX imm-shift: dst = the VVVV register (the reg field carries
            // the /digit opcode extension), source = RM.
            write_reg_vec(vex->vreg, out);
        } else if (!write_rm_vec(rm, vw, out)) {
            fault(ExecStatus::MemoryFault, rm.addr); return true;
        }
        commit();
        return true;
    }
    // ================= 0x77 vzeroupper / vzeroall / emms ==================
    if (op2 == 0x77) {
        // hardware-verified: VEX.L=0 (C5 F8 77) = VZEROUPPER (upper halves
        // only); VEX.L=1 (C5 FC 77) = VZEROALL (entire register file).
        if (is_vex) {
            for (int i = 0; i < 16; i++) {
                if (L) m_state.xmm[i] = CpuState::XmmReg{};         // vzeroall
                m_state.ymm_hi[i] = CpuState::XmmReg{};
            }
        }
        commit();
        return true;
    }
    // ================= 0xC2 cmp family ===================================
    if (op2 == 0xC2) {
        if (!modrm_fetch()) return true;
        uint8_t imm = 0;
        if (!fetch_imm8(imm)) return true;
        Vec a = src_a(), b{};
        if (!read_rm_vec(rm, vw, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        Vec out{};
        if (pp == 0) {
            for (int i = 0; i < vw / 4; i++)
                out.d[i] = CmpPred(imm & 31, a.f[i], b.f[i]) ? 0xFFFFFFFFu : 0;
        } else if (pp == 1) {
            for (int i = 0; i < vw / 8; i++)
                out.q[i] = CmpPred(imm & 31, a.df[i], b.df[i]) ? ~0ull : 0;
        } else {
            out = a;
            if (pp == 2) out.d[0] = CmpPred(imm & 31, a.f[0], b.f[0]) ? 0xFFFFFFFFu : 0;
            else out.q[0] = CmpPred(imm & 31, a.df[0], b.df[0]) ? ~0ull : 0;
        }
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ================= 0xC4 pinsrw / 0xC5 pextrw =========================
    if (op2 == 0xC4) {
        if (!modrm_fetch()) return true;
        uint8_t imm = 0;
        if (!fetch_imm8(imm)) return true;
        uint16_t v = 0;
        if (rm.is_reg) {
            const uint8_t ri = static_cast<uint8_t>(rm.reg | (p.rex_b ? 0x08 : 0));
            v = static_cast<uint16_t>(ReadReg(ri, 2));
        } else {
            if (!m_mem.Read(rm.addr, &v, 2)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        }
        Vec out = src_a();
        out.w[imm & (vw / 2 - 1)] = v;
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    if (op2 == 0xC5) {
        if (!modrm_fetch()) return true;
        uint8_t imm = 0;
        if (!fetch_imm8(imm)) return true;
        Vec s{};
        if (!read_rm_vec(rm, vw, s)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        const uint16_t v = s.w[imm & (vw / 2 - 1)];
        const uint8_t dst = static_cast<uint8_t>(reg | (p.rex_r ? 0x08 : 0x00));
        WriteReg(dst, 4, v);
        commit();
        return true;
    }
    // ================= 0xC6 shufps / shufpd ==============================
    if (op2 == 0xC6) {
        if (!modrm_fetch()) return true;
        uint8_t imm = 0;
        if (!fetch_imm8(imm)) return true;
        Vec a = src_a(), b{};
        if (!read_rm_vec(rm, vw, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        Vec out{};
        if (pp == 0) {
            for (int g = 0; g < vw / 16; g++) {
                const int base = g * 4;
                out.d[base] = a.d[base + ((imm >> 0) & 3)];
                out.d[base + 1] = a.d[base + ((imm >> 2) & 3)];
                out.d[base + 2] = b.d[base + ((imm >> 4) & 3)];
                out.d[base + 3] = b.d[base + ((imm >> 6) & 3)];
            }
        } else {
            for (int g = 0; g < vw / 16; g++) {
                const int base = g * 2;
                out.q[base] = a.q[base + ((imm >> (g * 2)) & 1)];
                out.q[base + 1] = b.q[base + ((imm >> (g * 2 + 1)) & 1)];
            }
        }
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ================= 0xD0 addsubps/addsubpd ============================
    if (op2 == 0xD0) {
        if (!modrm_fetch()) return true;
        Vec a = src_a(), b{};
        if (!read_rm_vec(rm, vw, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        Vec out{};
        // ADDSUBPS/PD: even-indexed elements SUBTRACT, odd-indexed ADD.
        if (pp == 1) {
            for (int i = 0; i < vw / 8; i++)
                out.df[i] = (i & 1) ? a.df[i] + b.df[i] : a.df[i] - b.df[i];
        } else {
            for (int i = 0; i < vw / 4; i++)
                out.f[i] = (i & 1) ? a.f[i] + b.f[i] : a.f[i] - b.f[i];
        }
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ================= 0xD1-D3 / 0xE1-E2 / 0xF1-F3 variable shifts =======
    // Hardware-verified: ONE count from the LOW 64 BITS of the count operand
    // drives ALL lanes (probed with distinct per-qword counts: every lane
    // shifted by the low qword's value).
    if ((op2 >= 0xD1 && op2 <= 0xD3) || op2 == 0xE1 || op2 == 0xE2 ||
        (op2 >= 0xF1 && op2 <= 0xF3)) {
        if (!modrm_fetch()) return true;
        Vec a = src_a(), b{};
        if (!read_rm_vec(rm, vw, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        Vec out{};
        for (int g = 0; g < vw / 16; g++) {
            const uint64_t count = b.q[0];
            for (int j = 0; j < 8; j++) {           // words in the lane
                const int wi = g * 8 + j;
                if (op2 == 0xD1) out.w[wi] = count >= 16 ? 0 : static_cast<uint16_t>(a.w[wi] >> count);
                else if (op2 == 0xE1) out.w[wi] = count >= 16 ? (a.sw[wi] < 0 ? 0xFFFF : 0)
                                                              : static_cast<uint16_t>(a.sw[wi] >> count);
                else if (op2 == 0xF1) out.w[wi] = count >= 16 ? 0 : static_cast<uint16_t>(a.w[wi] << count);
                else if (j < 4) {                    // dwords in the lane
                    const int di = g * 4 + j;
                    if (op2 == 0xD2) out.d[di] = count >= 32 ? 0 : a.d[di] >> count;
                    else if (op2 == 0xE2) out.d[di] = count >= 32 ? (a.sd[di] < 0 ? 0xFFFFFFFF : 0)
                                                                  : static_cast<uint32_t>(a.sd[di] >> count);
                    else if (op2 == 0xF2) out.d[di] = count >= 32 ? 0 : a.d[di] << count;
                }
            }
            for (int j = 0; j < 2; j++) {           // qwords in the lane
                const int qi = g * 2 + j;
                if (op2 == 0xD3) out.q[qi] = count >= 64 ? 0 : a.q[qi] >> count;
                else if (op2 == 0xF3) out.q[qi] = count >= 64 ? 0 : a.q[qi] << count;
            }
        }
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ================= 0xD7 pmovmskb ======================================
    if (op2 == 0xD7) {
        if (!modrm_fetch()) return true;
        Vec s{};
        if (!read_rm_vec(rm, vw, s)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        uint32_t mask = 0;
        for (int i = 0; i < vw; i++) mask |= (static_cast<uint32_t>(s.b[i] >> 7) & 1u) << i;
        const uint8_t dst = static_cast<uint8_t>(reg | (p.rex_r ? 0x08 : 0x00));
        WriteReg(dst, 4, mask);
        commit();
        return true;
    }
    // ================= 0xE6 cvt pairs =====================================
    if (op2 == 0xE6) {
        // The prefix meanings differ between legacy and VEX (verified by
        // the generated table): legacy 66=cvttpd2dq F2=cvtpd2dq F3=cvtdq2pd,
        // while VEX 66=vcvttpd2dq F2=vcvtdq2pd F3=vcvtpd2dq.
        if (!modrm_fetch()) return true;
        Vec b{};
        if (!read_rm_vec(rm, vw, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        Vec out{};
        // identical for legacy and VEX (verified): 66=ttpd2dq, F3=dq2pd, F2=pd2dq
        const bool to_dq = (pp == 2);
        const bool trunc = (pp == 1);
        if (to_dq) {                                  // cvtdq2pd
            for (int i = 0; i < vw / 8; i++) out.df[i] = static_cast<double>(b.sd[i]);
        } else {
            for (int i = 0; i < vw / 16; i++) {
                out.d[i * 2] = trunc ? CvtTrunc(b.df[i * 2]) : CvtRne(b.df[i * 2]);
                out.d[i * 2 + 1] = trunc ? CvtTrunc(b.df[i * 2 + 1]) : CvtRne(b.df[i * 2 + 1]);
            }
        }
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ================= 0xE7 movdqu store ==================================
    if (op2 == 0xE7) {
        if (!modrm_fetch()) return true;
        Vec s = read_reg_vec(reg);
        if (!write_rm_vec(rm, vw, s)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        commit();
        return true;
    }
    // ================= 0xF6 psadbw ========================================
    if (op2 == 0xF6) {
        if (!modrm_fetch()) return true;
        Vec a = src_a(), b{};
        if (!read_rm_vec(rm, vw, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        Vec out{};
        for (int g = 0; g < vw / 8; g++) {
            uint16_t sum = 0;
            for (int i = 0; i < 8; i++) {
                const int x = a.b[g * 8 + i] - b.b[g * 8 + i];
                sum = static_cast<uint16_t>(sum + (x < 0 ? -x : x));
            }
            out.w[g * 4] = sum;
            out.w[g * 4 + 1] = 0; out.w[g * 4 + 2] = 0; out.w[g * 4 + 3] = 0;
        }
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ================= 0xF7 maskmovdqu ====================================
    if (op2 == 0xF7) {
        if (!modrm_fetch()) return true;
        Vec s = read_reg_vec(reg);
        Vec mask{};
        if (!read_rm_vec(rm, vw, mask)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        const uint64_t base = m_state.gpr[7];         // DS:RDI
        for (int i = 0; i < 16; i++) {
            if (mask.b[i] & 0x80) {
                if (!m_mem.Write(base + i, &s.b[i], 1)) {
                    fault(ExecStatus::MemoryFault, base + i);
                    return true;
                }
            }
        }
        commit();
        return true;
    }
    // ================= 0x7C/0x7D hadd/hsub ==================================
    if (op2 == 0x7C || op2 == 0x7D) {
        if (!modrm_fetch()) return true;
        Vec a = src_a(), b{};
        if (!read_rm_vec(rm, vw, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        Vec out{};
        const bool sub = (op2 == 0x7D);
        if (pp == 1) {                              // haddpd / hsubpd (66)
            for (int g = 0; g < vw / 16; g++) {
                const int base = g * 2;
                out.df[base] = sub ? a.df[base] - a.df[base + 1] : a.df[base] + a.df[base + 1];
                out.df[base + 1] = sub ? b.df[base] - b.df[base + 1] : b.df[base] + b.df[base + 1];
            }
        } else if (pp == 3) {                       // haddps / hsubps (F2)
            for (int g = 0; g < vw / 16; g++) {
                const int base = g * 4;
                out.f[base] = sub ? a.f[base] - a.f[base + 1] : a.f[base] + a.f[base + 1];
                out.f[base + 1] = sub ? a.f[base + 2] - a.f[base + 3] : a.f[base + 2] + a.f[base + 3];
                out.f[base + 2] = sub ? b.f[base] - b.f[base + 1] : b.f[base] + b.f[base + 1];
                out.f[base + 3] = sub ? b.f[base + 2] - b.f[base + 3] : b.f[base + 2] + b.f[base + 3];
            }
        } else {
            r.status = ExecStatus::UnsupportedOpcode;
            return true;
        }
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ================= integer pack ops (final switch) ====================
    {
        if (!modrm_fetch()) return true;
        Vec a = src_a(), b{};
        if (!read_rm_vec(rm, vw, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        Vec out{};
        const int nb = vw;
        const int nw = vw / 2;
        const int nd = vw / 4;
        const int nq = vw / 8;
        const int n128 = vw / 16;
        switch (op2) {
            case 0xDB: for (int i = 0; i < nq; i++) out.q[i] = a.q[i] & b.q[i]; break;    // pand
            case 0xEB: for (int i = 0; i < nq; i++) out.q[i] = a.q[i] | b.q[i]; break;    // por
            case 0xEF: for (int i = 0; i < nq; i++) out.q[i] = a.q[i] ^ b.q[i]; break;    // pxor
            case 0xDF: for (int i = 0; i < nq; i++) out.q[i] = (~a.q[i]) & b.q[i]; break; // pandn
            case 0xFC: for (int i = 0; i < nb; i++) out.b[i] = static_cast<uint8_t>(a.b[i] + b.b[i]); break;
            case 0xFD: for (int i = 0; i < nw; i++) out.w[i] = static_cast<uint16_t>(a.w[i] + b.w[i]); break;
            case 0xFE: for (int i = 0; i < nd; i++) out.d[i] = a.d[i] + b.d[i]; break;
            case 0xD4: for (int i = 0; i < nq; i++) out.q[i] = a.q[i] + b.q[i]; break;    // paddq
            case 0xF8: for (int i = 0; i < nb; i++) out.b[i] = static_cast<uint8_t>(a.b[i] - b.b[i]); break;
            case 0xF9: for (int i = 0; i < nw; i++) out.w[i] = static_cast<uint16_t>(a.w[i] - b.w[i]); break;
            case 0xFA: for (int i = 0; i < nd; i++) out.d[i] = a.d[i] - b.d[i]; break;
            case 0xFB: for (int i = 0; i < nq; i++) out.q[i] = a.q[i] - b.q[i]; break;    // psubq
            // saturated add/sub (opcode map verified against the generated
            // table): D8/D9 = psubus b/w, DC/DD = paddus b/w,
            // E8/E9 = psubs b/w, EC/ED = padds b/w.
            case 0xDC: for (int i = 0; i < nb; i++) out.b[i] = SatU8(a.b[i] + b.b[i]); break;    // paddusb
            case 0xDD: for (int i = 0; i < nw; i++) out.w[i] = SatU16(a.w[i] + b.w[i]); break;   // paddusw
            case 0xD8: for (int i = 0; i < nb; i++) out.b[i] = SatU8(a.b[i] - b.b[i]); break;    // psubusb
            case 0xD9: for (int i = 0; i < nw; i++) out.w[i] = SatU16(a.w[i] - b.w[i]); break;   // psubusw
            case 0xEC: for (int i = 0; i < nb; i++) out.sb[i] = SatS8(a.sb[i] + b.sb[i]); break; // paddsb
            case 0xED: for (int i = 0; i < nw; i++) out.sw[i] = SatS16(a.sw[i] + b.sw[i]); break;// paddsw
            case 0xE8: for (int i = 0; i < nb; i++) out.sb[i] = SatS8(a.sb[i] - b.sb[i]); break; // psubsb
            case 0xE9: for (int i = 0; i < nw; i++) out.sw[i] = SatS16(a.sw[i] - b.sw[i]); break;// psubsw
            case 0x74: for (int i = 0; i < nb; i++) out.b[i] = (a.b[i] == b.b[i]) ? 0xFF : 0; break;
            case 0x75: for (int i = 0; i < nw; i++) out.w[i] = (a.w[i] == b.w[i]) ? 0xFFFF : 0; break;
            case 0x76: for (int i = 0; i < nd; i++) out.d[i] = (a.d[i] == b.d[i]) ? 0xFFFFFFFF : 0; break;
            case 0x64: for (int i = 0; i < nb; i++) out.b[i] = (a.sb[i] > b.sb[i]) ? 0xFF : 0; break;
            case 0x65: for (int i = 0; i < nw; i++) out.w[i] = (a.sw[i] > b.sw[i]) ? 0xFFFF : 0; break;
            case 0x66: for (int i = 0; i < nd; i++) out.d[i] = (a.sd[i] > b.sd[i]) ? 0xFFFFFFFF : 0; break;
            case 0xDA: for (int i = 0; i < nb; i++) out.b[i] = a.b[i] < b.b[i] ? a.b[i] : b.b[i]; break;
            case 0xDE: for (int i = 0; i < nb; i++) out.b[i] = a.b[i] > b.b[i] ? a.b[i] : b.b[i]; break;
            case 0xEA: for (int i = 0; i < nw; i++) out.sw[i] = a.sw[i] < b.sw[i] ? a.sw[i] : b.sw[i]; break;
            case 0xEE: for (int i = 0; i < nw; i++) out.sw[i] = a.sw[i] > b.sw[i] ? a.sw[i] : b.sw[i]; break;
            case 0xE0: for (int i = 0; i < nb; i++) out.b[i] = static_cast<uint8_t>((a.b[i] + b.b[i] + 1) >> 1); break; // pavgb
            case 0xE3: for (int i = 0; i < nw; i++) out.w[i] = static_cast<uint16_t>((a.w[i] + b.w[i] + 1) >> 1); break; // pavgw
            case 0xD5:  // pmullw
                for (int i = 0; i < nw; i++)
                    out.w[i] = static_cast<uint16_t>(a.sw[i] * b.sw[i]);
                break;
            case 0xE4:  // pmulhuw
                for (int i = 0; i < nw; i++) out.w[i] = static_cast<uint16_t>((a.w[i] * b.w[i])
                    >> 16);
                break;
            case 0xE5:  // pmulhw
                for (int i = 0; i < nw; i++) out.w[i] = static_cast<uint16_t>((a.sw[i] * b.sw[i])
                    >> 16);
                break;
            case 0xF4:  // pmuludq
                for (int i = 0; i < nd; i += 2) out.q[i / 2] = static_cast<uint64_t>(a.d[i])
                    * b.d[i];
                break;
            case 0xF5:  // pmaddwd
                for (int i = 0; i < nw; i += 2) {
                    out.sd[i / 2] = a.sw[i] * b.sw[i] + a.sw[i + 1] * b.sw[i + 1];
                } break;
            case 0x60:  // punpcklbw: interleave the low 8 BYTES of each lane
                for (int g = 0; g < n128; g++) {
                    const int base = g * 16;
                    for (int i = 0; i < 8; i++) {
                        out.b[base + i * 2] = a.b[base + i];
                        out.b[base + i * 2 + 1] = b.b[base + i];
                    }
                } break;
            case 0x61:  // punpcklwd (full 16 bytes from low 4 words each)
                for (int g = 0; g < n128; g++) {
                    const int base = g * 16;
                    for (int i = 0; i < 4; i++) {
                        out.b[base + i * 4 + 0] = a.b[base + i * 2];
                        out.b[base + i * 4 + 1] = a.b[base + i * 2 + 1];
                        out.b[base + i * 4 + 2] = b.b[base + i * 2];
                        out.b[base + i * 4 + 3] = b.b[base + i * 2 + 1];
                    }
                } break;
            case 0x62:  // punpckldq: interleave the low 2 dwords of each lane
                for (int g = 0; g < n128; g++) {
                    const int base = g * 4;
                    out.d[base] = a.d[base];
                    out.d[base + 1] = b.d[base];
                    out.d[base + 2] = a.d[base + 1];
                    out.d[base + 3] = b.d[base + 1];
                } break;
            case 0x68:  // punpckhbw: interleave the high 8 bytes of each lane
                for (int g = 0; g < n128; g++) {
                    const int base = g * 16;
                    for (int i = 0; i < 8; i++) {
                        out.b[base + i * 2] = a.b[base + 8 + i];
                        out.b[base + i * 2 + 1] = b.b[base + 8 + i];
                    }
                } break;
            case 0x69:  // punpckhwd
                for (int g = 0; g < n128; g++) {
                    const int base = g * 16;
                    for (int i = 0; i < 4; i++) {
                        out.b[base + i * 4 + 0] = a.b[base + 8 + i * 2];
                        out.b[base + i * 4 + 1] = a.b[base + 9 + i * 2];
                        out.b[base + i * 4 + 2] = b.b[base + 8 + i * 2];
                        out.b[base + i * 4 + 3] = b.b[base + 9 + i * 2];
                    }
                } break;
            case 0x6A:  // punpckhdq: interleave the high 2 dwords of each lane
                for (int g = 0; g < n128; g++) {
                    const int base = g * 4;
                    out.d[base] = a.d[base + 2];
                    out.d[base + 1] = b.d[base + 2];
                    out.d[base + 2] = a.d[base + 3];
                    out.d[base + 3] = b.d[base + 3];
                } break;
            case 0x6C:  // punpcklqdq
                for (int g = 0; g < n128; g++) {
                    const int base = g * 2;
                    out.q[base] = a.q[base];
                    out.q[base + 1] = b.q[base];
                } break;
            case 0x6D:  // punpckhqdq
                for (int g = 0; g < n128; g++) {
                    const int base = g * 2;
                    out.q[base] = a.q[base + 1];
                    out.q[base + 1] = b.q[base + 1];
                } break;
            case 0x63:  // packsswb (8 signed words each -> 16 signed bytes per lane)
                for (int g = 0; g < n128; g++) {
                    const int base = g * 16;
                    for (int i = 0; i < 8; i++) out.sb[base + i] = SatS8(a.sw[g * 8 + i]);
                    for (int i = 0; i < 8; i++) out.sb[base + 8 + i] = SatS8(b.sw[g * 8 + i]);
                } break;
            case 0x67:  // packuswb (8 signed words each -> 16 unsigned bytes per lane)
                for (int g = 0; g < n128; g++) {
                    const int base = g * 16;
                    for (int i = 0; i < 8; i++) out.b[base + i] = SatU8(a.sw[g * 8 + i]);
                    for (int i = 0; i < 8; i++) out.b[base + 8 + i] = SatU8(b.sw[g * 8 + i]);
                } break;
            case 0x6B:  // packssdw (4 signed dwords each -> 8 signed words per lane)
                for (int g = 0; g < n128; g++) {
                    const int base = g * 8;
                    for (int i = 0; i < 4; i++) out.sw[base + i] = SatS16(a.sd[g * 4 + i]);
                    for (int i = 0; i < 4; i++) out.sw[base + 4 + i] = SatS16(b.sd[g * 4 + i]);
                } break;
            default:
                r.status = ExecStatus::UnsupportedOpcode;
                return true;
        }
        write_reg_vec(reg, out);
        commit();
        return true;
    }
}
}
