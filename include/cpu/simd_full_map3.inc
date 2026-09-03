// simd_full_map3.inc - MAP 3 (0F3A / VEX.0F3A) execution body.
// Included INSIDE X86Interpreter::ExecSimdFull (via simd_full_map23.inc).
{
if (map == 3) {
    // ---- imm-based permutes -------------------------------------------------
    if (op2 == 0x00 || op2 == 0x01 || op2 == 0x04 || op2 == 0x05) {
        if (!modrm_fetch()) return true;
        uint8_t imm = 0;
        if (!fetch_imm8(imm)) return true;
        Vec s{};
        if (!read_rm_vec(rm, vw, s)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        Vec out{};
        if (op2 == 0x00) {                           // vpermq
            for (int i = 0; i < 4; i++) out.q[i] = s.q[(imm >> (2 * i)) & 3];
        } else if (op2 == 0x01) {                    // vpermpd
            for (int i = 0; i < 4; i++) out.q[i] = s.q[(imm >> (2 * i)) & 3];
        } else if (op2 == 0x04) {                    // vpermilps imm
            for (int i = 0; i < vw / 4; i++)
                out.d[i] = s.d[(i & ~3) + ((imm >> ((i & 3) * 2)) & 3)];
        } else {                                     // vpermilpd imm
            for (int i = 0; i < vw / 8; i++)
                out.q[i] = s.q[(i & ~1) + ((imm >> i) & 1)];
        }
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ---- vperm2f128 / vperm2i128 ----------------------------------------------
    if (op2 == 0x06 || op2 == 0x46) {
        if (!modrm_fetch()) return true;
        uint8_t imm = 0;
        if (!fetch_imm8(imm)) return true;
        Vec a = read_reg_vec(vex->vreg), b{};
        if (!read_rm_vec(rm, vw, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        Vec out{};
        auto pick = [&](int sel, uint64_t& lo, uint64_t& hi) -> void {
            if (sel & 4) { lo = 0; hi = 0; return; }
            switch (sel & 3) {
                case 0: lo = a.q[0]; hi = a.q[1]; break;
                case 1: lo = a.q[2]; hi = a.q[3]; break;
                case 2: lo = b.q[0]; hi = b.q[1]; break;
                default: lo = b.q[2]; hi = b.q[3]; break;
            }
        };
        pick(imm & 0xF, out.q[0], out.q[1]);
        pick((imm >> 4) & 0xF, out.q[2], out.q[3]);
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ---- round family ----------------------------------------------------------
    if (op2 >= 0x08 && op2 <= 0x0B) {
        if (!modrm_fetch()) return true;
        uint8_t imm = 0;
        if (!fetch_imm8(imm)) return true;
        Vec b{};
        if (!read_rm_vec(rm, vw, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        Vec out = src_a();
        const int mode = (imm & 4) ? 0 : (imm & 3);  // bit2: use MXCSR (modelled as nearest-even)
        auto rnd = [&](float v) -> float {
            switch (mode) {
                case 1: return std::floor(v);
                case 2: return std::ceil(v);
                case 3: return std::trunc(v);
                default: return std::nearbyint(v);
            }
        };
        auto rndd = [&](double v) -> double {
            switch (mode) {
                case 1: return std::floor(v);
                case 2: return std::ceil(v);
                case 3: return std::trunc(v);
                default: return std::nearbyint(v);
            }
        };
        if (op2 == 0x08 || op2 == 0x09) {            // roundps / roundpd
            out = Vec{};
            if (op2 == 0x08) {
                for (int i = 0; i < vw / 4; i++) out.f[i] = rnd(b.f[i]);
            } else {
                for (int i = 0; i < vw / 8; i++) out.df[i] = rndd(b.df[i]);
            }
        } else {                                     // roundss / roundsd
            if (op2 == 0x0A) out.f[0] = rnd(b.f[0]);
            else out.df[0] = rndd(b.df[0]);
        }
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ---- blendps/blendpd/pblendw (imm select) -----------------------------------
    if (op2 == 0x0C || op2 == 0x0D || op2 == 0x0E) {
        if (!modrm_fetch()) return true;
        uint8_t imm = 0;
        if (!fetch_imm8(imm)) return true;
        Vec a = src_a(), b{};
        if (!read_rm_vec(rm, vw, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        Vec out{};
        // hardware-verified: blendps/blendpd mask bits span ALL lanes
        // (imm bit i -> lane i); pblendw has only 8 mask bits so the mask
        // RESTARTS per 128-bit lane.
        if (op2 == 0x0C) {                            // blendps
            for (int i = 0; i < vw / 4; i++) out.d[i] = ((imm >> i) & 1) ? b.d[i] : a.d[i];
        } else if (op2 == 0x0D) {                     // blendpd
            for (int i = 0; i < vw / 8; i++) out.q[i] = ((imm >> i) & 1) ? b.q[i] : a.q[i];
        } else {                                      // pblendw
            for (int i = 0; i < vw / 2; i++) out.w[i] = ((imm >> (i % 8)) & 1) ? b.w[i] : a.w[i];
        }
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ---- palignr -------------------------------------------------------------------
    if (op2 == 0x0F) {
        if (!modrm_fetch()) return true;
        uint8_t imm = 0;
        if (!fetch_imm8(imm)) return true;
        Vec a = src_a(), b{};
        if (!read_rm_vec(rm, vw, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        Vec out{};
        for (int lane = 0; lane < vw / 16; lane++) {
            const int base = lane * 16;
            for (int i = 0; i < 16; i++) {
                const int idx = imm + i;
                if (idx < 16) out.b[base + i] = b.b[base + idx];
                else if (idx < 32) out.b[base + i] = a.b[base + (idx - 16)];
                else out.b[base + i] = 0;
            }
        }
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ---- pextrb/w/d/q + extractps (GPR or MEMORY destination) -----------------------
    if (op2 == 0x14 || op2 == 0x15 || op2 == 0x16 || op2 == 0x17) {
        if (!modrm_fetch()) return true;
        uint8_t imm = 0;
        if (!fetch_imm8(imm)) return true;
        Vec s = read_reg_vec(reg);
        uint64_t v = 0;
        int gpr_size = 4;                       // GPR destinations are 32-bit+
        int mem_size = 4;
        const bool w64 = W || p.rex_w;
        switch (op2) {
            case 0x14: v = s.b[imm & 15]; mem_size = 1; break;             // pextrb
            case 0x15: v = s.w[imm & 7]; mem_size = 2; break;              // pextrw
            case 0x16: v = w64 ? s.q[imm & 1] : s.d[imm & 3]; gpr_size = w64 ? 8 : 4; mem_size = gpr_size; break;
            default:   v = s.d[imm & 3]; break;                            // extractps
        }
        if (rm.is_reg) {
            // register destination: zero-extended 32-bit (or 64-bit) write
            WriteReg(static_cast<uint8_t>(rm.reg | (p.rex_b ? 0x08 : 0)), gpr_size, v);
        } else if (!m_mem.Write(rm.addr, &v, mem_size)) {
            fault(ExecStatus::MemoryFault, rm.addr); return true;
        }
        commit();
        return true;
    }
    // ---- vinsertf128 / vextractf128 / vinserti128 / vextracti128 ---------------------
    if (op2 == 0x18 || op2 == 0x19 || op2 == 0x38 || op2 == 0x39) {
        if (!modrm_fetch()) return true;
        uint8_t imm = 0;
        if (!fetch_imm8(imm)) return true;
        const bool insert = (op2 == 0x18 || op2 == 0x38);
        if (insert) {
            Vec dst = read_reg_vec(vex->vreg);
            Vec src{};
            if (!read_rm_vec(rm, 16, src)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
            if (imm & 1) { dst.q[2] = src.q[0]; dst.q[3] = src.q[1]; }
            else { dst.q[0] = src.q[0]; dst.q[1] = src.q[1]; }
            write_reg_vec(reg, dst);
        } else {
            Vec s = read_reg_vec(reg);
            Vec low{};
            if (imm & 1) { low.q[0] = s.q[2]; low.q[1] = s.q[3]; }
            else { low.q[0] = s.q[0]; low.q[1] = s.q[1]; }
            if (!write_rm_vec(rm, 16, low)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        }
        commit();
        return true;
    }
    // ---- vcvtps2ph --------------------------------------------------------------------
    if (op2 == 0x1D) {
        if (!modrm_fetch()) return true;
        uint8_t imm = 0;
        if (!fetch_imm8(imm)) return true;
        Vec s = read_reg_vec(reg);
        Vec out{};
        const int n = vw / 4;
        for (int i = 0; i < n; i++) out.w[i] = F16FromF32(s.f[i]);
        if (!write_rm_vec(rm, n * 2, out)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        commit();
        return true;
    }
    // ---- pinsrb/w/d/q -----------------------------------------------------------------
    if (op2 == 0x20 || op2 == 0x22) {
        if (!modrm_fetch()) return true;
        uint8_t imm = 0;
        if (!fetch_imm8(imm)) return true;
        const bool w64 = W || p.rex_w;
        const int size = op2 == 0x20 ? 1 : (w64 ? 8 : 4);
        uint64_t v = 0;
        if (!read_rm_int(rm, size, v)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        Vec out = src_a();
        if (op2 == 0x20) out.b[imm & 15] = static_cast<uint8_t>(v);
        else if (w64) out.q[imm & 1] = v;
        else out.d[imm & 3] = static_cast<uint32_t>(v);
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ---- insertps ----------------------------------------------------------------------
    if (op2 == 0x21) {
        if (!modrm_fetch()) return true;
        uint8_t imm = 0;
        if (!fetch_imm8(imm)) return true;
        Vec b{};
        if (!read_rm_vec(rm, 16, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        // hardware-verified imm8 layout: bits[3:0] = zmask (bit j zeroes
        // dst dword j, applied AFTER the insert), bits[5:4] = count_d
        // (destination dword), bits[7:6] = count_s (source dword).
        Vec out = src_a();
        out.d[(imm >> 4) & 3] = b.d[(imm >> 6) & 3];
        for (int j = 0; j < 4; j++) {
            if ((imm >> j) & 1) out.d[j] = 0;
        }
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ---- dpps / dppd ----------------------------------------------------------------------
    if (op2 == 0x40 || op2 == 0x41) {
        if (!modrm_fetch()) return true;
        uint8_t imm = 0;
        if (!fetch_imm8(imm)) return true;
        Vec a = src_a(), b{};
        if (!read_rm_vec(rm, vw, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        Vec out{};
        // hardware-verified mask layout: imm[3:0] = destination write mask,
        // imm[7:4] = multiplication mask (probed on real hardware).
        if (op2 == 0x40) {                           // dpps
            // hardware-verified: imm[3:0] = write mask, imm[7:4] = multiply
            // mask, applied PER 128-bit lane group (the 256-bit form
            // computes one dot product per lane group).
            for (int g = 0; g < vw / 16; g++) {
                float acc = 0.0f;
                for (int i = 0; i < 4; i++) {
                    if ((imm >> (4 + i)) & 1) acc += a.f[g * 4 + i] * b.f[g * 4 + i];
                }
                for (int i = 0; i < 4; i++) {
                    out.f[g * 4 + i] = ((imm >> i) & 1) ? acc : 0.0f;
                }
            }
        } else {                                     // dppd
            double acc = 0.0;
            if (imm & 0x10) acc += a.df[0] * b.df[0];
            if (imm & 0x20) acc += a.df[1] * b.df[1];
            out.df[0] = (imm & 1) ? acc : 0.0;
            out.df[1] = (imm & 2) ? acc : 0.0;
        }
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ---- mpsadbw (offsets verified against real hardware) -----------------------------------
    if (op2 == 0x42) {
        if (!modrm_fetch()) return true;
        uint8_t imm = 0;
        if (!fetch_imm8(imm)) return true;
        Vec a = src_a(), b{};
        if (!read_rm_vec(rm, vw, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        Vec out{};
        for (int lane = 0; lane < vw / 16; lane++) {
            // hardware quirk: the upper 128-bit lane behaves as if imm[2:0] == 0
            const int eff = (lane == 0) ? (imm & 7) : 0;
            const int a_off = lane * 16 + 4 * ((eff >> 2) & 1);   // vvvv/legacy dst: slides per word
            const int b_off = lane * 16 + 4 * (eff & 3);          // rm: fixed per lane
            for (int w = 0; w < 8; w++) {
                uint16_t sum = 0;
                for (int k = 0; k < 4; k++) {
                    const int x = a.b[a_off + w + k] - b.b[b_off + k];
                    sum = static_cast<uint16_t>(sum + (x < 0 ? -x : x));
                }
                out.w[lane * 8 + w] = sum;
            }
        }
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ---- pclmulqdq ---------------------------------------------------------------------------
    if (op2 == 0x44) {
        if (!modrm_fetch()) return true;
        uint8_t imm = 0;
        if (!fetch_imm8(imm)) return true;
        Vec a = src_a(), b{};
        if (!read_rm_vec(rm, vw, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        // per 128-bit lane (hardware: the 256-bit form processes each lane)
        Vec out{};
        for (int g = 0; g < vw / 16; g++) {
            const uint64_t x = (imm & 1) ? a.q[g * 2 + 1] : a.q[g * 2];
            const uint64_t y = (imm & 0x10) ? b.q[g * 2 + 1] : b.q[g * 2];
            uint64_t lo = 0, hi = 0;
            ClMul64(x, y, lo, hi);
            out.q[g * 2] = lo;
            out.q[g * 2 + 1] = hi;
        }
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ---- 4-operand variable blends (mask in imm[7:4] for VEX, XMM0 legacy) -------------------
    if (op2 == 0x4A || op2 == 0x4B || op2 == 0x4C) {
        if (!modrm_fetch()) return true;
        uint8_t imm = 0;
        if (!fetch_imm8(imm)) return true;
        Vec a = src_a(), b{};
        if (!read_rm_vec(rm, vw, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        Vec mask{};
        if (is_vex) mask = read_reg_vec(static_cast<uint8_t>(imm >> 4));
        else mask = read_reg_vec(0);
        Vec out{};
        if (op2 == 0x4A) {                           // vblendvps
            for (int i = 0; i < vw / 4; i++) out.d[i] = (mask.d[i] & 0x80000000u) ? b.d[i] : a.d[i];
        } else if (op2 == 0x4B) {                    // vblendvpd
            for (int i = 0; i < vw / 8; i++) out.q[i] = (mask.q[i] >> 63) ? b.q[i] : a.q[i];
        } else {                                     // vpblendvb
            for (int i = 0; i < vw; i++) out.b[i] = (mask.b[i] & 0x80) ? b.b[i] : a.b[i];
        }
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ---- SSE4.2 string compares ------------------------------------------------------------
    if (op2 >= 0x60 && op2 <= 0x63) {
        if (!modrm_fetch()) return true;
        uint8_t imm = 0;
        if (!fetch_imm8(imm)) return true;
        Vec a = read_reg_vec(reg), b{};
        if (!read_rm_vec(rm, 16, b)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        // imm8 control bits:
        //   [1:0] source length format: 0=packed bytes, 1=packed words, 2/3=unused
        //   [2]   aggregation: 0=equal-any, 1=ranges
        //   [3]   polarity: 0=pos, 1=neg (bit0/1 = neg valid/int res)
        //   [5:4] output: 0/1=index (lsi), 2/3=mask (msi)
        const bool explicit_len = (op2 == 0x60 || op2 == 0x61);   // pcmpestri/m
        int len_a = 16, len_b = 16;
        if (explicit_len) {
            const int fmt = imm & 3;
            const uint64_t& la = m_state.gpr[RAX];
            const uint64_t& lb = m_state.gpr[RDX];
            len_a = static_cast<int>(fmt == 1 ? la & 0xFFFF : la & 0xFFFFFFFF);
            len_b = static_cast<int>(fmt == 1 ? lb & 0xFFFF : lb & 0xFFFFFFFF);
        }
        const bool wide = (imm & 3) == 1;
        const int elems = wide ? 8 : 16;
        int la = explicit_len ? len_a : 0;
        int lb = explicit_len ? len_b : 0;
        if (!explicit_len) {                          // pcmpistri/m: implicit NUL termination
            const int n = wide ? 8 : 16;
            la = n; lb = n;
            for (int i = 0; i < n; i++) {
                if (wide) { if (a.w[i] == 0) { la = i; break; } }
                else { if (a.b[i] == 0) { la = i; break; } }
            }
            for (int i = 0; i < n; i++) {
                if (wide) { if (b.w[i] == 0) { lb = i; break; } }
                else { if (b.b[i] == 0) { lb = i; break; } }
            }
        }
        if (la > elems) la = elems;
        if (lb > elems) lb = elems;
        // build masks of valid elements
        uint32_t valid_a = 0, valid_b = 0;
        for (int i = 0; i < elems; i++) {
            if (i < la) valid_a |= 1u << i;
            if (i < lb) valid_b |= 1u << i;
        }
        auto elem_a = [&](int i) -> uint32_t { return wide ? a.w[i] : a.b[i]; };
        auto elem_b = [&](int i) -> uint32_t { return wide ? b.w[i] : b.b[i]; };
        // aggregation: equal-any (result bit i set when SRC1 elem i equals ANY
        // valid SRC2 elem) / ranges (result bit i set when SRC1 elem i falls
        // inside ANY range [SRC2[2j], SRC2[2j+1]] of valid pairs).
        uint32_t result = 0;
        const bool ranges = (imm & 4) != 0;
        if (!ranges) {
            for (int i = 0; i < elems; i++) {
                if (!(valid_a & (1u << i))) continue;
                for (int j = 0; j < elems; j++) {
                    if ((valid_b & (1u << j)) && elem_a(i) == elem_b(j)) {
                        result |= 1u << i;
                        break;
                    }
                }
            }
        } else {
            for (int i = 0; i < elems; i++) {
                if (!(valid_a & (1u << i))) continue;
                for (int j = 0; j + 1 < elems; j += 2) {
                    if ((valid_b & (3u << j)) != (3u << j)) continue;
                    const uint32_t lo = elem_b(j), hi = elem_b(j + 1);
                    const uint32_t v = elem_a(i);
                    if (lo <= hi ? (v >= lo && v <= hi) : (v >= hi && v <= lo)) {
                        result |= 1u << i;
                        break;
                    }
                }
            }
        }
        // polarity: 0=pos, 1=neg, 2=pos+valid, 3=neg+valid
        uint32_t intres = result;
        switch ((imm >> 4) & 3) {
            case 1: intres = ~intres; break;
            case 2: intres = intres | (~valid_a & 0xFFFF); break;
            case 3: intres = (~intres) & (valid_a | valid_b); break;
            default: break;
        }
        intres &= (wide ? 0xFF : 0xFFFF);
        // output select
        const bool index_out = ((imm >> 6) & 1) == 0;   // 0/1 -> index, 2/3 -> mask
        Vec out{};
        if (index_out) {                              // pcmp?stri
            // imm[7]=0: least significant set bit; imm[7]=1: most significant
            int idx = -1;
            const int n = wide ? 8 : 16;
            if (imm & 0x80) {
                for (int i = n - 1; i >= 0; i--) {
                    if (intres & (1u << i)) { idx = i; break; }
                }
            } else {
                for (int i = 0; i < n; i++) {
                    if (intres & (1u << i)) { idx = i; break; }
                }
            }
            const int none = n;
            WriteReg(RCX, 8, static_cast<uint64_t>(idx < 0 ? none : idx));
            out = a;                                   // dst unchanged for index form
            SetFlag(Flags::CF, idx >= 0);
            SetFlag(Flags::ZF, intres == 0);
            SetFlag(Flags::SF, idx == 0);
            SetFlag(Flags::OF, false); SetFlag(Flags::AF, false); SetFlag(Flags::PF, false);
        } else {                                       // pcmp?strm
            const int mode = (imm >> 6) & 3;
            if (mode == 2) {                           // bit mask
                out = Vec{};
                out.d[0] = intres;
            } else {                                   // byte/word mask expand
                out = Vec{};
                for (int i = 0; i < (wide ? 8 : 16); i++) {
                    if (intres & (1u << i)) {
                        if (wide) out.w[i] = 0xFFFF;
                        else out.b[i] = 0xFF;
                    }
                }
            }
            SetFlag(Flags::CF, intres != 0);
            SetFlag(Flags::ZF, intres == 0);
            SetFlag(Flags::SF, (intres & 1) != 0);
            SetFlag(Flags::OF, false); SetFlag(Flags::AF, false); SetFlag(Flags::PF, false);
        }
        write_reg_vec(reg, out);
        commit();
        return true;
    }
    // ---- aeskeygenassist -----------------------------------------------------------------------
    if (op2 == 0xDF) {
        if (!modrm_fetch()) return true;
        uint8_t imm = 0;
        if (!fetch_imm8(imm)) return true;
        Vec s{};
        if (!read_rm_vec(rm, 16, s)) { fault(ExecStatus::MemoryFault, rm.addr); return true; }
        uint8_t x[16], o[16];
        std::memcpy(x, &s, 16);
        // KeyAssist(x3 x2 x1 x0) = [SubWord(x1), RotWord(SubWord(x1)) ^ rcon, SubWord(x3), RotWord(SubWord(x3)) ^ rcon]
        // hardware-verified: rcon = the RAW imm8, XORed into BYTE 0 only of
        // each RotWord(SubWord(...)) result.
        const uint8_t rcon = imm;
        uint8_t sub[4];
        for (int i = 0; i < 4; i++) sub[i] = kAesSbox[x[4 + i]];       // x1 (bytes 4..7)
        o[0] = sub[0]; o[1] = sub[1]; o[2] = sub[2]; o[3] = sub[3];
        o[4] = sub[1] ^ rcon; o[5] = sub[2]; o[6] = sub[3]; o[7] = sub[0];
        for (int i = 0; i < 4; i++) sub[i] = kAesSbox[x[12 + i]];      // x3 (bytes 12..15)
        o[8] = sub[0]; o[9] = sub[1]; o[10] = sub[2]; o[11] = sub[3];
        o[12] = sub[1] ^ rcon; o[13] = sub[2]; o[14] = sub[3]; o[15] = sub[0];
        Vec out{};
        std::memcpy(&out, o, 16);
        write_reg_vec(reg, out);
        commit();
        return true;
    }
}
}
