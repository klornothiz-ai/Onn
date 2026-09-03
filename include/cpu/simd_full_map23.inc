// simd_full_map23.inc - MAP 2 (0F38/VEX.0F38) + MAP 3 (0F3A/VEX.0F3A)
// execution body. Included INSIDE X86Interpreter::ExecSimdFull; shares its
// locals/lambdas. (The map-1 hadd/hsub pair lives in simd_full_map1.inc.)
{

if (map == 2) {
    // ---- binary integer families (SSSE3 / SSE4.1 / AVX2) -----------------
    {
        const bool is_binary =
            op2 == 0x00 || op2 == 0x01 || op2 == 0x02 || op2 == 0x03 || op2 == 0x04 ||
            op2 == 0x05 || op2 == 0x06 || op2 == 0x07 || op2 == 0x08 || op2 == 0x09 ||
            op2 == 0x0A || op2 == 0x0B ||
            op2 == 0x28 || op2 == 0x29 || op2 == 0x2B ||
            op2 == 0x37 || op2 == 0x38 || op2 == 0x39 || op2 == 0x3A || op2 == 0x3B ||
            op2 == 0x3C || op2 == 0x3D || op2 == 0x3E || op2 == 0x3F || op2 == 0x40;
        if (is_binary) {
            if (!modrm_fetch()) return true;
            Vec a = src_a(), b{};
            if (!read_rm_vec(rm, vw, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
            Vec out{};
            const int nb = vw, nw = vw / 2, nd = vw / 4;
            switch (op2) {
                case 0x00:  // pshufb
                    for (int g = 0; g < vw / 16; g++) {
                        const int base = g * 16;
                        for (int i = 0; i < 16; i++) {
                            const uint8_t sel = b.b[base + i];
                            out.b[base + i] = (sel & 0x80) ? 0 : a.b[base + (sel & 0x0F)];
                        }
                    } break;
                case 0x01:  // phaddw
                    for (int g = 0; g < vw / 16; g++) {
                        const int wb = g * 8;
                        for (int i = 0; i < 4; i++) out.w[wb + i] = static_cast<uint16_t>(a.sw[wb + 2 * i] + a.sw[wb + 2 * i + 1]);
                        for (int i = 0; i < 4; i++) out.w[wb + 4 + i] = static_cast<uint16_t>(b.sw[wb + 2 * i] + b.sw[wb + 2 * i + 1]);
                    } break;
                case 0x02:  // phaddd
                    for (int g = 0; g < vw / 16; g++) {
                        const int db = g * 4;
                        out.d[db] = a.d[db] + a.d[db + 1];
                        out.d[db + 1] = a.d[db + 2] + a.d[db + 3];
                        out.d[db + 2] = b.d[db] + b.d[db + 1];
                        out.d[db + 3] = b.d[db + 2] + b.d[db + 3];
                    } break;
                case 0x03:  // phaddsw
                    for (int g = 0; g < vw / 16; g++) {
                        const int wb = g * 8;
                        for (int i = 0; i < 4; i++) out.sw[wb + i] = SatS16(a.sw[wb + 2 * i] + a.sw[wb + 2 * i + 1]);
                        for (int i = 0; i < 4; i++) out.sw[wb + 4 + i] = SatS16(b.sw[wb + 2 * i] + b.sw[wb + 2 * i + 1]);
                    } break;
                case 0x04:  // pmaddubsw
                    for (int i = 0; i < nw; i++)
                        out.sw[i] = SatS16(a.b[2 * i] * b.sb[2 * i] + a.b[2 * i + 1] * b.sb[2 * i + 1]);
                    break;
                case 0x05:  // phsubw
                    for (int g = 0; g < vw / 16; g++) {
                        const int wb = g * 8;
                        for (int i = 0; i < 4; i++) out.w[wb + i] = static_cast<uint16_t>(a.sw[wb + 2 * i] - a.sw[wb + 2 * i + 1]);
                        for (int i = 0; i < 4; i++) out.w[wb + 4 + i] = static_cast<uint16_t>(b.sw[wb + 2 * i] - b.sw[wb + 2 * i + 1]);
                    } break;
                case 0x06:  // phsubd
                    for (int g = 0; g < vw / 16; g++) {
                        const int db = g * 4;
                        out.d[db] = a.d[db] - a.d[db + 1];
                        out.d[db + 1] = a.d[db + 2] - a.d[db + 3];
                        out.d[db + 2] = b.d[db] - b.d[db + 1];
                        out.d[db + 3] = b.d[db + 2] - b.d[db + 3];
                    } break;
                case 0x07:  // phsubsw
                    for (int g = 0; g < vw / 16; g++) {
                        const int wb = g * 8;
                        for (int i = 0; i < 4; i++) out.sw[wb + i] = SatS16(a.sw[wb + 2 * i] - a.sw[wb + 2 * i + 1]);
                        for (int i = 0; i < 4; i++) out.sw[wb + 4 + i] = SatS16(b.sw[wb + 2 * i] - b.sw[wb + 2 * i + 1]);
                    } break;
                case 0x08:  // psignb
                    for (int i = 0; i < nb; i++)
                        out.b[i] = b.sb[i] < 0 ? static_cast<uint8_t>(-a.b[i]) : (b.sb[i] == 0 ? 0 : a.b[i]);
                    break;
                case 0x09:  // psignw
                    for (int i = 0; i < nw; i++)
                        out.w[i] = b.sw[i] < 0 ? static_cast<uint16_t>(-a.w[i]) : (b.sw[i] == 0 ? 0 : a.w[i]);
                    break;
                case 0x0A:  // psignd
                    for (int i = 0; i < nd; i++)
                        out.d[i] = b.sd[i] < 0 ? static_cast<uint32_t>(-a.d[i]) : (b.sd[i] == 0 ? 0 : a.d[i]);
                    break;
                case 0x0B:  // pmulhrsw
                    for (int i = 0; i < nw; i++)
                        out.sw[i] = SatS16((a.sw[i] * b.sw[i] + 0x4000) >> 15);
                    break;
                case 0x28:  // pmuldq
                    for (int i = 0; i < nd; i += 2)
                        out.q[i / 2] = static_cast<uint64_t>(static_cast<int64_t>(a.sd[i]) * b.sd[i]);
                    break;
                case 0x29:  // pcmpeqq
                    for (int i = 0; i < vw / 8; i++) out.q[i] = (a.q[i] == b.q[i]) ? ~0ull : 0;
                    break;
                case 0x2B:  // packusdw (4 dwords each -> 8 words per lane)
                    for (int g = 0; g < vw / 16; g++) {
                        const int base = g * 8;
                        for (int i = 0; i < 4; i++) out.w[base + i] = SatU16(a.sd[g * 4 + i]);
                        for (int i = 0; i < 4; i++) out.w[base + 4 + i] = SatU16(b.sd[g * 4 + i]);
                    } break;
                case 0x37:  // pcmpgtq
                    for (int i = 0; i < vw / 8; i++) out.q[i] = (a.sq[i] > b.sq[i]) ? ~0ull : 0;
                    break;
                case 0x38:  // pminsb
                    for (int i = 0; i < nb; i++)
                        out.sb[i] = a.sb[i] < b.sb[i] ? a.sb[i] : b.sb[i];
                    break;
                case 0x39:  // pminsd
                    for (int i = 0; i < nd; i++)
                        out.sd[i] = a.sd[i] < b.sd[i] ? a.sd[i] : b.sd[i];
                    break;
                case 0x3A:  // pminuw
                    for (int i = 0; i < nw; i++)
                        out.w[i] = a.w[i] < b.w[i] ? a.w[i] : b.w[i];
                    break;
                case 0x3B:  // pminud
                    for (int i = 0; i < nd; i++)
                        out.d[i] = a.d[i] < b.d[i] ? a.d[i] : b.d[i];
                    break;
                case 0x3C:  // pmaxsb
                    for (int i = 0; i < nb; i++)
                        out.sb[i] = a.sb[i] > b.sb[i] ? a.sb[i] : b.sb[i];
                    break;
                case 0x3D:  // pmaxsd
                    for (int i = 0; i < nd; i++)
                        out.sd[i] = a.sd[i] > b.sd[i] ? a.sd[i] : b.sd[i];
                    break;
                case 0x3E:  // pmaxuw
                    for (int i = 0; i < nw; i++)
                        out.w[i] = a.w[i] > b.w[i] ? a.w[i] : b.w[i];
                    break;
                case 0x3F:  // pmaxud
                    for (int i = 0; i < nd; i++)
                        out.d[i] = a.d[i] > b.d[i] ? a.d[i] : b.d[i];
                    break;
                case 0x40:  // pmulld
                    for (int i = 0; i < nd; i++) {
                        const int64_t v = static_cast<int64_t>(a.sd[i]) * b.sd[i];
                        out.d[i] = static_cast<uint32_t>(v);
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
    // ---- unary families -----------------------------------------------------
    {
        const bool is_unary =
            op2 == 0x1C || op2 == 0x1D || op2 == 0x1E ||      // pabs b/w/d
            op2 == 0x41 ||                                     // phminposuw
            (op2 >= 0x20 && op2 <= 0x25) || (op2 >= 0x30 && op2 <= 0x35) ||  // pmovsx/zx
            op2 == 0x2A ||                                     // movntdqa
            op2 == 0x13 ||                                     // vcvtph2ps
            op2 == 0x18 || op2 == 0x19 || op2 == 0x1A ||       // vbroadcastss/sd/f128
            op2 == 0x58 || op2 == 0x59 || op2 == 0x78 || op2 == 0x79;  // vpbroadcast
        if (is_unary) {
            if (!modrm_fetch()) return true;
            int readw = vw;
            if (op2 == 0x20 || op2 == 0x30) readw = vw / 2;    // bw: N bytes -> N words
            else if (op2 == 0x21 || op2 == 0x31) readw = vw / 4; // bd: N bytes -> N dwords
            else if (op2 == 0x22 || op2 == 0x32) readw = vw / 8; // bq: N bytes -> N qwords
            else if (op2 == 0x23 || op2 == 0x33) readw = vw / 2; // wd: N words -> N dwords
            else if (op2 == 0x24 || op2 == 0x34) readw = vw / 4; // wq: N words -> N qwords
            else if (op2 == 0x25 || op2 == 0x35) readw = vw / 2; // dq: N dwords -> N qwords
            else if (op2 == 0x13) readw = L ? 16 : 8;          // vcvtph2ps
            else if (op2 == 0x18) readw = 4;
            else if (op2 == 0x19) readw = 8;
            else if (op2 == 0x1A) readw = 16;
            else if (op2 == 0x58) readw = 4;
            else if (op2 == 0x59) readw = 8;
            else if (op2 == 0x78) readw = 1;
            else if (op2 == 0x79) readw = 2;
            Vec b{};
            if (!read_rm_vec(rm, readw, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
            Vec out{};
            const int nb = vw, nw = vw / 2, nd = vw / 4;
            switch (op2) {
                case 0x1C: for (int i = 0; i < nb; i++) out.b[i] = static_cast<uint8_t>(b.sb[i] < 0 ? -b.sb[i] : b.sb[i]); break;
                case 0x1D: for (int i = 0; i < nw; i++) out.w[i] = static_cast<uint16_t>(b.sw[i] < 0 ? -b.sw[i] : b.sw[i]); break;
                case 0x1E: for (int i = 0; i < nd; i++) out.d[i] = static_cast<uint32_t>(b.sd[i] < 0 ? -b.sd[i] : b.sd[i]); break;
                case 0x41: {                          // phminposuw (128-bit only)
                    uint16_t minv = b.w[0]; int mini = 0;
                    for (int i = 1; i < 8; i++) {
                        if (b.w[i] < minv) { minv = b.w[i]; mini = i; }
                    }
                    out.w[0] = minv;
                    out.w[1] = static_cast<uint16_t>(mini);
                } break;
                case 0x20: for (int i = 0; i < vw / 2; i++) out.sw[i] = b.sb[i]; break;
                case 0x21: for (int i = 0; i < nb / 4; i++) out.sd[i] = b.sb[i]; break;
                case 0x22: for (int i = 0; i < nb / 8; i++) out.sq[i] = b.sb[i]; break;
                case 0x23: for (int i = 0; i < vw / 4; i++) out.sd[i] = b.sw[i]; break;
                case 0x24: for (int i = 0; i < vw / 8; i++) out.sq[i] = b.sw[i]; break;
                case 0x25: for (int i = 0; i < vw / 8; i++) out.sq[i] = b.sd[i]; break;
                case 0x30: for (int i = 0; i < vw / 2; i++) out.w[i] = b.b[i]; break;
                case 0x31: for (int i = 0; i < nb / 4; i++) out.d[i] = b.b[i]; break;
                case 0x32: for (int i = 0; i < nb / 8; i++) out.q[i] = b.b[i]; break;
                case 0x33: for (int i = 0; i < vw / 4; i++) out.d[i] = b.w[i]; break;
                case 0x34: for (int i = 0; i < vw / 8; i++) out.q[i] = b.w[i]; break;
                case 0x35: for (int i = 0; i < vw / 8; i++) out.q[i] = static_cast<uint64_t>(b.d[i]); break;
                case 0x2A: out = b; break;            // movntdqa
                case 0x13:
                    for (int i = 0; i < vw / 4; i++) out.f[i] = F32FromF16(b.w[i]);
                    break;
                case 0x18: case 0x58:
                    for (int i = 0; i < vw / 4; i++) out.d[i] = b.d[0];
                    break;
                case 0x19: case 0x59:                // vbroadcastsd / vpbroadcastq
                    for (int i = 0; i < vw / 8; i++) out.q[i] = b.q[0];
                    break;
                case 0x1A:
                    for (int i = 0; i < vw / 16; i++) {
                        out.q[i * 2] = b.q[0];
                        out.q[i * 2 + 1] = b.q[1];
                    }
                    break;
                case 0x78: for (int i = 0; i < vw; i++) out.b[i] = b.b[0]; break;
                case 0x79: for (int i = 0; i < vw / 2; i++) out.w[i] = b.w[0]; break;
                default:
                    r.status = ExecStatus::UnsupportedOpcode;
                    return true;
            }
            write_reg_vec(reg, out);
            commit();
            return true;
        }
    }
    // ---- ptest / vtestps / vtestpd -------------------------------------------
    if (op2 == 0x17 || op2 == 0x0E || op2 == 0x0F) {
        if (!modrm_fetch()) return true;
        Vec a = read_reg_vec(reg), b{};
        if (!read_rm_vec(rm, vw, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        bool zf = true, cf = true;
        if (op2 == 0x17) {
            for (int i = 0; i < vw / 8; i++) {
                if (a.q[i] & b.q[i]) zf = false;
                if ((~a.q[i]) & b.q[i]) cf = false;
            }
        } else if (op2 == 0x0E) {
            for (int i = 0; i < vw / 4; i++) {
                if ((a.d[i] & b.d[i]) & 0x80000000u) zf = false;
                if ((~a.d[i] & b.d[i]) & 0x80000000u) cf = false;
            }
        } else {
            for (int i = 0; i < vw / 8; i++) {
                if ((a.q[i] & b.q[i]) & 0x8000000000000000ull) zf = false;
                if ((~a.q[i] & b.q[i]) & 0x8000000000000000ull) cf = false;
            }
        }
        SetFlag(Flags::ZF, zf); SetFlag(Flags::CF, cf);
        SetFlag(Flags::OF, false); SetFlag(Flags::SF, false); SetFlag(Flags::AF, false); SetFlag(Flags::PF, false);
        commit();
        return true;
    }
    // ---- legacy blendvb family (mask = implicit XMM0) ------------------------
    if (op2 == 0x10 || op2 == 0x14 || op2 == 0x15) {
        if (is_vex) { r.status = ExecStatus::UnsupportedOpcode; return true; }
        if (!modrm_fetch()) return true;
        Vec a = read_reg_vec(reg), b{};
        if (!read_rm_vec(rm, vw, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        Vec mask = read_reg_vec(0);
        Vec out{};
        if (op2 == 0x10) {
            for (int i = 0; i < vw; i++) out.b[i] = (mask.b[i] & 0x80) ? b.b[i] : a.b[i];
        } else if (op2 == 0x14) {
            for (int i = 0; i < vw / 4; i++) out.d[i] = (mask.d[i] & 0x80000000u) ? b.d[i] : a.d[i];
        } else {
            for (int i = 0; i < vw / 8; i++) out.q[i] = (mask.q[i] >> 63) ? b.q[i] : a.q[i];
        }
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ---- vpermilps/pd 2-register + vpermps/vpermd -----------------------------
    if (op2 == 0x0C || op2 == 0x0D || op2 == 0x16 || op2 == 0x36) {
        if (!modrm_fetch()) return true;
        Vec a = read_reg_vec(vex->vreg), b{};
        if (!read_rm_vec(rm, vw, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        Vec out{};
        if (op2 == 0x0C) {                            // vpermilps: a=values, b=selectors
            for (int i = 0; i < vw / 4; i++)
                out.d[i] = a.d[(i & ~3) + (b.d[i] & 3)];
        } else if (op2 == 0x0D) {                     // vpermilpd (selector = bit 1)
            for (int i = 0; i < vw / 8; i++)
                out.q[i] = a.q[(i & ~1) + ((b.q[i] >> 1) & 1)];
        } else if (op2 == 0x16) {                     // vpermps: a=indices(vvvv), b=values(rm)
            for (int i = 0; i < 8; i++) out.f[i] = b.f[a.d[i] & 7];
        } else {                                      // vpermd
            for (int i = 0; i < 8; i++) out.d[i] = b.d[a.d[i] & 7];
        }
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ---- variable shifts ------------------------------------------------------
    if (op2 == 0x45 || op2 == 0x46 || op2 == 0x47) {
        if (!modrm_fetch()) return true;
        Vec a = read_reg_vec(vex->vreg), b{};
        if (!read_rm_vec(rm, vw, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        Vec out{};
        // the count is NOT masked: counts >= element width produce zero
        // (hardware-verified against the differential harness).
        if (W) {
            for (int i = 0; i < vw / 8; i++) {
                const uint64_t c = b.q[i];
                if (op2 == 0x47) out.q[i] = c >= 64 ? 0 : (a.q[i] << c);
                else out.q[i] = c >= 64 ? 0 : (a.q[i] >> c);
            }
        } else {
            for (int i = 0; i < vw / 4; i++) {
                const uint32_t c = b.d[i];
                if (op2 == 0x47) out.d[i] = c >= 32 ? 0 : (a.d[i] << c);
                else if (op2 == 0x46) out.d[i] = c >= 32 ? (a.sd[i] < 0 ? 0xFFFFFFFFu : 0)
                                                         : static_cast<uint32_t>(a.sd[i] >> c);
                else out.d[i] = c >= 32 ? 0 : (a.d[i] >> c);
            }
        }
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ---- vmaskmov / vpmaskmov STORES (element-wise conditional) ----------------
    if (op2 == 0x2E || op2 == 0x2F || op2 == 0x8E) {
        if (!modrm_fetch()) return true;
        if (rm.is_reg) { r.status = ExecStatus::UnsupportedOpcode; return true; }
        Vec mask = read_reg_vec(vex->vreg);
        Vec src = read_reg_vec(reg);
        if (W || op2 == 0x2F) {
            for (int i = 0; i < vw / 8; i++) {
                if (mask.q[i] >> 63) {
                    if (!m_mem.Write(rm.addr + 8 * i, &src.q[i], 8)) {
                        fault(ExecStatus::MemoryFault, rm.addr); return true;
                    }
                }
            }
        } else {
            for (int i = 0; i < vw / 4; i++) {
                if (mask.d[i] & 0x80000000u) {
                    if (!m_mem.Write(rm.addr + 4 * i, &src.d[i], 4)) {
                        fault(ExecStatus::MemoryFault, rm.addr); return true;
                    }
                }
            }
        }
        commit();
        return true;
    }
    // ---- vmaskmov / vpmaskmov LOADS (element-wise conditional) -----------------
    if (op2 == 0x2C || op2 == 0x2D || op2 == 0x8C) {
        if (!modrm_fetch()) return true;
        if (rm.is_reg) { r.status = ExecStatus::UnsupportedOpcode; return true; }
        Vec mask = read_reg_vec(vex->vreg);
        Vec out{};                                    // unloaded lanes are ZERO
        if (W || op2 == 0x2D) {                       // 64-bit elements
            for (int i = 0; i < vw / 8; i++) {
                if (mask.q[i] >> 63) {
                    uint64_t v = 0;
                    if (!m_mem.Read(rm.addr + 8 * i, &v, 8)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                    out.q[i] = v;
                }
            }
        } else {
            for (int i = 0; i < vw / 4; i++) {
                if (mask.d[i] & 0x80000000u) {
                    uint32_t v = 0;
                    if (!m_mem.Read(rm.addr + 4 * i, &v, 4)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
                    out.d[i] = v;
                }
            }
        }
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ---- gathers (VSIB) ---------------------------------------------------------
    if (op2 == 0x90 || op2 == 0x92) {
        uint8_t m = 0;
        if (!Fetch8(rip, m)) { fault(ExecStatus::DecodeFault, rip); return true; }
        uint64_t base_addr = 0;
        uint8_t vindex = 0;
        int scale = 1;
        const uint8_t mod = m >> 6;
        const uint8_t rmb = m & 7;
        if (mod == 3) { r.status = ExecStatus::UnsupportedOpcode; return true; }
        if (rmb == 4) {
            uint8_t sib = 0;
            if (!Fetch8(rip, sib)) { fault(ExecStatus::DecodeFault, rip); return true; }
            scale = 1 << (sib >> 6);
            vindex = (sib >> 3) & 7;
            const uint8_t sb = sib & 7;
            if (sb == 5 && mod == 0) {
                uint32_t disp = 0;
                if (!Fetch32(rip, disp)) { fault(ExecStatus::DecodeFault, rip); return true; }
                base_addr = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(disp)));
            } else {
                base_addr = ReadReg(static_cast<uint8_t>(sb | (p.rex_b ? 0x08 : 0)), 8);
            }
        } else {
            vindex = rmb;
        }
        if (p.rex_x) vindex |= 8;
        int64_t disp = 0;
        if (mod == 1) {
            uint8_t d8 = 0;
            if (!Fetch8(rip, d8)) { fault(ExecStatus::DecodeFault, rip); return true; }
            disp = static_cast<int8_t>(d8);
        } else if (mod == 2) {
            uint32_t d32 = 0;
            if (!Fetch32(rip, d32)) { fault(ExecStatus::DecodeFault, rip); return true; }
            disp = static_cast<int32_t>(d32);
        }
        base_addr += static_cast<uint64_t>(disp);
        // 0x90 W0: VPGATHERDD (idx32/val32)  0x90 W1: VPGATHERQQ (64/64)
        // 0x92 W0: VPGATHERDQ (idx32/val64)  0x92 W1: VPGATHERQD (idx64/val32)
        const bool idx_is64 = (op2 == 0x90) ? W : !W;
        const bool val_is64 = (op2 == 0x90) ? W : !W;
        const bool val_is64_fix = (op2 == 0x90) ? W : (W ? false : true);
        const bool v64 = (op2 == 0x92) ? !W : W;
        (void)val_is64;
        Vec mask = read_reg_vec(vex->vreg);
        Vec out = mask;
        Vec idxv{};
        if (!read_rm_vec(ModRMOperand{true, vindex}, 16, idxv)) { fault(ExecStatus::MemoryFault, rip); return true; }
        if (idx_is64) {
            for (int i = 0; i < vw / 8; i++) {
                if (mask.q[i] >> 63) {
                    uint64_t v = 0;
                    const uint64_t addr = base_addr + idxv.q[i] * static_cast<uint64_t>(scale);
                    if (!m_mem.Read(addr, &v, v64 ? 8 : 4)) { fault(ExecStatus::MemoryFault, addr); return true; }
                    if (v64) out.q[i] = v;
                    else out.d[i] = static_cast<uint32_t>(v);
                    out.q[i] &= ~0x8000000000000000ull;
                }
            }
        } else {
            for (int i = 0; i < vw / 4; i++) {
                if (mask.d[i] & 0x80000000u) {
                    uint64_t v = 0;
                    const uint64_t addr = base_addr + idxv.d[i] * static_cast<uint64_t>(scale);
                    if (!m_mem.Read(addr, &v, v64 ? 8 : 4)) { fault(ExecStatus::MemoryFault, addr); return true; }
                    if (v64) out.q[i] = v;
                    else out.d[i] = static_cast<uint32_t>(v);
                    out.d[i] &= 0x7FFFFFFFu;
                }
            }
        }
        (void)val_is64_fix;
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ---- AES ----------------------------------------------------------------------
    if (op2 >= 0xDB && op2 <= 0xDF) {
        if (!modrm_fetch()) return true;
        if (op2 == 0xDB) {                            // aesimc = InvMixColumns only
            Vec b{};
            if (!read_rm_vec(rm, 16, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
            uint8_t t[16];
            std::memcpy(t, &b, 16);
            AesInvMixColumns(t);
            Vec out{};
            std::memcpy(&out, t, 16);
            write_reg_vec(reg, out);
            commit();
            return true;
        }
        // state: legacy = the destination register; VEX = the VVVV operand
        Vec a = is_vex ? read_reg_vec(vex->vreg) : read_reg_vec(reg), b{};
        if (!read_rm_vec(rm, 16, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        uint8_t x[16], rk[16], o[16];
        std::memcpy(x, &a, 16);
        std::memcpy(rk, &b, 16);
        switch (op2) {
            case 0xDC: AesRound(x, rk, o); break;         // aesenc
            case 0xDD: AesRoundLast(x, rk, o); break;     // aesenclast
            case 0xDE: AesInvRound(x, rk, o); break;      // aesdec
            default:   AesInvRoundLast(x, rk, o); break;  // 0xDF: aesdeclast
        }
        Vec out{};
        std::memcpy(&out, o, 16);
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ---- crc32 -----------------------------------------------------------------------
    if (op2 == 0xF0 || op2 == 0xF1) {
        if (pp != 3) { return false; }                // crc32 requires F2
        if (!modrm_fetch()) return true;
        const bool w64 = W || p.rex_w;
        const int size = op2 == 0xF0 ? 1 : (w64 ? 8 : (p.opsize ? 2 : 4));
        uint64_t v = 0;
        if (!read_rm_int(rm, size, v)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        const uint8_t dst = static_cast<uint8_t>(reg | (p.rex_r ? 0x08 : 0));
        const uint32_t init = static_cast<uint32_t>(ReadReg(dst, 4));
        const uint32_t res = Crc32c(init, &v, size);
        WriteReg(dst, w64 ? 8 : 4, res);
        commit();
        return true;
    }
    // ---- FMA ---------------------------------------------------------------------------
    if ((op2 >= 0x96 && op2 <= 0x9F) || (op2 >= 0xA6 && op2 <= 0xAF) ||
        (op2 >= 0xB6 && op2 <= 0xBF)) {
        if (!modrm_fetch()) return true;
        const Vec d = read_reg_vec(reg);              // operand 1 (dst)
        const Vec a = read_reg_vec(vex->vreg);        // operand 2 (vvvv)
        Vec b{};
        if (!read_rm_vec(rm, vw, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        // rows: 0x90 = "132", 0xA0 = "213", 0xB0 = "231"
        //   132: dst = dst * rm  + vvvv
        //   213: dst = vvvv * dst + rm
        //   231: dst = vvvv * rm  + dst
        const int order = (op2 & 0xF0) == 0x90 ? 132 : ((op2 & 0xF0) == 0xA0 ? 213 : 231);
        const int col = op2 & 0x0F;
        // col: 6=addsub 7=subadd 8/9=fmadd A/B=fmsub C/D=fnmadd E/F=fnmsub
        // (odd columns 9/B/D/F are the scalar ss/sd forms; W picks sd)
        int kind;                                     // 0 add,1 sub,2 nmadd,3 nmsub,4 addsub,5 subadd
        if (col == 6) kind = 4;
        else if (col == 7) kind = 5;
        else if (col == 8 || col == 9) kind = 0;
        else if (col == 0xA || col == 0xB) kind = 1;
        else if (col == 0xC || col == 0xD) kind = 2;
        else kind = 3;
        const bool scalar = (col == 9 || col == 0xB || col == 0xD || col == 0xF);
        // pick multiplicands / addend per order
        Vec m1{}, m2{}, addv{};
        if (order == 132) { m1 = d; m2 = b; addv = a; }
        else if (order == 213) { m1 = a; m2 = d; addv = b; }
        else { m1 = a; m2 = b; addv = d; }
        Vec out{};
        // Hardware FMA rounds ONCE; mul+add would double-round and diverge
        // by 1 ULP (differential-verified). Use std::fma for exact parity.
        if (scalar) {
            out = d;                    // upper bits merge from DST (verified)
            if (W) {
                const double x = m1.df[0], y = m2.df[0], z = addv.df[0];
                switch (kind) {
                    case 0: out.df[0] = std::fma(x, y, z); break;
                    case 1: out.df[0] = std::fma(x, y, NegN(z)); break;
                    case 2: out.df[0] = std::fma(NegN(x), y, z); break;
                    default: out.df[0] = std::fma(NegN(x), y, NegN(z)); break;
                }
            } else {
                const float x = m1.f[0], y = m2.f[0], z = addv.f[0];
                switch (kind) {
                    case 0: out.f[0] = std::fma(x, y, z); break;
                    case 1: out.f[0] = std::fma(x, y, NegN(z)); break;
                    case 2: out.f[0] = std::fma(NegN(x), y, z); break;
                    default: out.f[0] = std::fma(NegN(x), y, NegN(z)); break;
                }
            }
        } else if (W) {                               // pd
            for (int i = 0; i < vw / 8; i++) {
                const double x = m1.df[i], y = m2.df[i], z = addv.df[i];
                switch (kind) {
                    case 0: out.df[i] = std::fma(x, y, z); break;
                    case 1: out.df[i] = std::fma(x, y, NegN(z)); break;
                    case 2: out.df[i] = std::fma(NegN(x), y, z); break;
                    case 3: out.df[i] = std::fma(NegN(x), y, NegN(z)); break;
                    case 4: out.df[i] = (i & 1) ? std::fma(x, y, z) : std::fma(x, y, NegN(z)); break;
                    default: out.df[i] = (i & 1) ? std::fma(x, y, NegN(z)) : std::fma(x, y, z); break;
                }
            }
        } else {                                       // ps
            for (int i = 0; i < vw / 4; i++) {
                const float x = m1.f[i], y = m2.f[i], z = addv.f[i];
                switch (kind) {
                    case 0: out.f[i] = std::fma(x, y, z); break;
                    case 1: out.f[i] = std::fma(x, y, NegN(z)); break;
                    case 2: out.f[i] = std::fma(NegN(x), y, z); break;
                    case 3: out.f[i] = std::fma(NegN(x), y, NegN(z)); break;
                    case 4: out.f[i] = (i & 1) ? std::fma(x, y, z) : std::fma(x, y, NegN(z)); break;
                    default: out.f[i] = (i & 1) ? std::fma(x, y, NegN(z)) : std::fma(x, y, z); break;
                }
            }
        }
        write_reg_vec(reg, out);
        commit();
        return true;
    }
}

#include "cpu/simd_full_map3.inc"
}
