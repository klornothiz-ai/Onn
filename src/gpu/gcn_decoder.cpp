// ============================================================================
// ProsperoLayer RDNA2 Core - Real GCN/GFX10 instruction decoder (round 12)
// ============================================================================
#include "gpu/gcn_decoder.hpp"
#include "gpu/gpu_guest_memory.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>

namespace PS5::GPU {

const char* ToString(GcnFormat f) {
    switch (f) {
        case GcnFormat::SOP1: return "SOP1";
        case GcnFormat::SOP2: return "SOP2";
        case GcnFormat::SOPK: return "SOPK";
        case GcnFormat::SOPC: return "SOPC";
        case GcnFormat::SOPP: return "SOPP";
        case GcnFormat::VOP1: return "VOP1";
        case GcnFormat::VOP2: return "VOP2";
        case GcnFormat::VOPC: return "VOPC";
        case GcnFormat::VOP3: return "VOP3";
        case GcnFormat::SMEM: return "SMEM";
        case GcnFormat::MUBUF: return "MUBUF";
        case GcnFormat::MTBUF: return "MTBUF";
        case GcnFormat::DS: return "DS";
        default: return "UNKNOWN";
    }
}

bool GcnDecodeInlineConstant32(uint16_t source, uint32_t& bits) {
    // Standard GCN inline-constant encodings (LLVM AMDGPUInlineConstant /
    // ISA manual "Source Operand encodings for inline constants").
    if (source >= 128 && source <= 192) { bits = source - 128; return true; }   // 0..64
    if (source >= 193 && source <= 208) { bits = 0u - (source - 192); return true; } // -1..-16
    switch (source) {
        case 240: bits = 0x3F000000u; return true; //  0.5f
        case 241: bits = 0xBF000000u; return true; // -0.5f
        case 242: bits = 0x3F800000u; return true; //  1.0f
        case 243: bits = 0xBF800000u; return true; // -1.0f
        case 244: bits = 0x40000000u; return true; //  2.0f
        case 245: bits = 0xC0000000u; return true; // -2.0f
        case 246: bits = 0x40800000u; return true; //  4.0f
        case 247: bits = 0xC0800000u; return true; // -4.0f
        case 248: bits = 0x3E22F983u; return true; //  1/(2*pi)
        case 249: bits = 0xBE22F983u; return true; // -1/(2*pi)
        case 250: bits = 0x3F000000u; return true; //  0.5 (64-bit lo) -- same bits
        case 251: bits = 0xBF000000u; return true; // -0.5 (64-bit lo)
        case 252: bits = 0x3F800000u; return true; //  1.0 (64-bit lo)
        case 253: bits = 0xBF800000u; return true; // -1.0 (64-bit lo)
        case 254: bits = 0x40000000u; return true; //  2.0 (64-bit lo)
        case 255: bits = 0xC0000000u; return true; // -2.0 (64-bit lo) [not literal in SOP]
        default: return false;
    }
}

namespace {

int64_t SignExtend16(uint16_t v) {
    return static_cast<int16_t>(v);
}

} // namespace

bool GcnDecoder::Decode(const uint32_t* code, size_t dwords, size_t pc,
                        GcnInstruction& out) const {
    out = GcnInstruction{};
    if (code == nullptr || pc >= dwords) {
        return false;
    }
    const uint32_t w0 = code[pc];
    out.raw0 = w0;

    // --- specific 9-bit-prefix scalar formats first (they overlap VOP2's
    // "bit31 == 0" space, so they must be matched before the generic VOP2).
    if ((w0 & 0xFF800000u) == 0xBF800000u) {          // SOPP
        out.format = GcnFormat::SOPP;
        out.opcode = (w0 >> 16) & 0x7Fu;
        out.simm16 = w0 & 0xFFFFu;
        out.is_terminator = out.opcode == GcnOp::S_ENDPGM;
        out.is_branch = out.opcode == GcnOp::S_BRANCH;
        out.is_cond_branch = (out.opcode == GcnOp::S_CBRANCH_SCC0 ||
                              out.opcode == GcnOp::S_CBRANCH_SCC1 ||
                              out.opcode == GcnOp::S_CBRANCH_VCCZ ||
                              out.opcode == GcnOp::S_CBRANCH_VCCNZ ||
                              out.opcode == GcnOp::S_CBRANCH_EXECZ ||
                              out.opcode == GcnOp::S_CBRANCH_EXECNZ);
        if (out.is_branch || out.is_cond_branch) {
            out.branch_target = static_cast<int64_t>(pc) + 1 + SignExtend16(out.simm16);
        }
        return true;
    }
    if ((w0 & 0xFF800000u) == 0x7D800000u) {          // SOP1
        out.format = GcnFormat::SOP1;
        out.opcode = (w0 >> 16) & 0x7Fu;
        out.dst = (w0 >> 9) & 0x7Fu;
        out.src0 = w0 & 0x1FFu;
        // SOP1 ssrc0 == 0xFF consumes a literal dword from the stream.
        if (out.src0 == 0xFFu && pc + 1 < dwords) {
            out.literal = code[pc + 1];
            out.has_literal = true;
            out.dwords_consumed = 2;
        }
        return true;
    }
    if ((w0 & 0xFF800000u) == 0x7E800000u) {          // SOPC
        out.format = GcnFormat::SOPC;
        out.opcode = (w0 >> 16) & 0x7Fu;
        out.src1 = (w0 >> 9) & 0x7Fu;
        out.src0 = w0 & 0x1FFu;
        if (out.src0 == 0xFFu && pc + 1 < dwords) {
            out.literal = code[pc + 1];
            out.has_literal = true;
            out.dwords_consumed = 2;
        }
        return true;
    }
    if ((w0 & 0xF8000000u) == 0xB0000000u) {          // SOPK
        out.format = GcnFormat::SOPK;
        out.opcode = (w0 >> 20) & 0x7Fu;
        out.dst = (w0 >> 16) & 0xFu;
        out.simm16 = w0 & 0xFFFFu;
        return true;
    }
    if ((w0 & 0xE0000000u) == 0x80000000u) {          // SOP2
        out.format = GcnFormat::SOP2;
        out.opcode = (w0 >> 23) & 0x3Fu;
        out.dst = (w0 >> 16) & 0x7Fu;
        out.src1 = (w0 >> 9) & 0x7Fu;
        out.src0 = w0 & 0x1FFu;
        if (out.src0 == 0xFFu && pc + 1 < dwords) {
            out.literal = code[pc + 1];
            out.has_literal = true;
            out.dwords_consumed = 2;
        }
        return true;
    }
    if ((w0 & 0xFE000000u) == 0x7E000000u) {          // VOP1
        out.format = GcnFormat::VOP1;
        out.opcode = (w0 >> 17) & 0xFFu;
        out.dst = (w0 >> 9) & 0xFFu;
        out.src0 = w0 & 0x1FFu;
        if (out.src0 == 0xFFu && pc + 1 < dwords) {   // 32-bit literal
            out.literal = code[pc + 1];
            out.has_literal = true;
            out.dwords_consumed = 2;
        }
        return true;
    }
    if ((w0 & 0xFE000000u) == 0x7C000000u) {          // VOPC
        out.format = GcnFormat::VOPC;
        out.opcode = (w0 >> 17) & 0xFFu;
        out.src1 = (w0 >> 9) & 0xFFu;
        out.src0 = w0 & 0x1FFu;
        if (out.src0 == 0xFFu && pc + 1 < dwords) {
            out.literal = code[pc + 1];
            out.has_literal = true;
            out.dwords_consumed = 2;
        }
        return true;
    }
    if ((w0 & 0xFC000000u) == 0xD0000000u) {          // VOP3P (GFX10: 0x34 << 26)
        if (pc + 1 >= dwords) {
            return false;    // truncated dual-dword instruction (fail-closed)
        }
        const uint32_t w1 = code[pc + 1];
        out.format = GcnFormat::VOP3P;
        out.opcode = (w0 >> 16) & 0x3FFu;
        out.dst = w0 & 0xFFu;
        // Research layout (documented): w0[10:8] = per-source half selector
        // (bit i selects the HIGH f16 of source i), w0[15:11] kept raw for
        // the neg_hi/opsel_hi family. dword1 is the standard VOP3 source
        // triple with omod/neg modifiers.
        out.opsel = static_cast<uint8_t>((w0 >> 8) & 0x07u);
        out.src0 = w1 & 0x1FFu;
        out.src1 = (w1 >> 9) & 0x1FFu;
        out.src2 = (w1 >> 18) & 0x1FFu;
        out.omod = (w1 >> 27) & 0x3u;
        out.neg0 = ((w1 >> 29) & 1u) != 0;
        out.neg1 = ((w1 >> 30) & 1u) != 0;
        out.neg2 = ((w1 >> 31) & 1u) != 0;
        out.raw1 = w1;
        out.dwords_consumed = 2;
        return true;
    }
    if ((w0 & 0xFC000000u) == 0xD4000000u) {          // VOP3 (GFX10: 0x35 << 26)
        if (pc + 1 >= dwords) {
            return false;    // truncated dual-dword instruction (fail-closed)
        }
        const uint32_t w1 = code[pc + 1];
        out.format = GcnFormat::VOP3;
        out.opcode = (w0 >> 16) & 0x3FFu;
        out.dst = w0 & 0xFFu;
        // dword1 (bits 32..63 of the 64-bit instruction): src0[8:0],
        // src1[17:9], src2[26:18], omod[28:27], neg0[29], neg1[30], neg2[31].
        out.src0 = w1 & 0x1FFu;
        out.src1 = (w1 >> 9) & 0x1FFu;
        out.src2 = (w1 >> 18) & 0x1FFu;
        out.omod = (w1 >> 27) & 0x3u;
        out.neg0 = ((w1 >> 29) & 1u) != 0;
        out.neg1 = ((w1 >> 30) & 1u) != 0;
        out.neg2 = ((w1 >> 31) & 1u) != 0;
        out.abs0 = ((w0 >> 8) & 1u) != 0;
        out.abs1 = ((w0 >> 9) & 1u) != 0;
        out.abs2 = ((w0 >> 10) & 1u) != 0;
        out.clamp = ((w0 >> 15) & 1u) != 0;
        out.raw1 = w1;
        out.dwords_consumed = 2;
        return true;
    }
    if ((w0 & 0xFC000000u) == 0xC0000000u) {          // SMEM (GFX10)
        if (pc + 1 >= dwords) {
            return false;    // SMEM is always 64-bit
        }
        const uint64_t w01 = (static_cast<uint64_t>(code[pc + 1]) << 32) | w0;
        out.format = GcnFormat::SMEM;
        out.opcode = (w0 >> 18) & 0xFFu;
        out.sbase = (w0 & 0x3Fu) * 2;          // sbase{6:1} -> sgpr pair index
        out.dst = (w0 >> 6) & 0x7Fu;
        out.smem_imm = ((w0 >> 17) & 1u) != 0;
        out.glc = ((w0 >> 16) & 1u) != 0;
        out.smem_offset = static_cast<uint32_t>((w01 >> 32) & 0x1FFFFFu); // offset[52:32]
        out.raw1 = code[pc + 1];
        out.dwords_consumed = 2;
        return true;
    }
    if ((w0 & 0xFC000000u) == 0xE0000000u) {          // MUBUF (GFX10)
        if (pc + 1 >= dwords) {
            return false;
        }
        const uint32_t w1 = code[pc + 1];
        out.format = GcnFormat::MUBUF;
        out.opcode = (w0 >> 18) & 0xFFu;
        out.buf_offset = w0 & 0xFFFu;
        out.slc = ((w0 >> 12) & 1u) != 0;
        out.glc = ((w0 >> 14) & 1u) != 0;
        // dword1 (bits 32..63): vaddr[7:0], vdata[15:8], srsrc[20:16],
        // tfe[21], offen[22], idxen[23], soffset[31:24].
        out.vaddr = w1 & 0xFFu;
        out.vdata = (w1 >> 8) & 0xFFu;
        out.srsrc = ((w1 >> 16) & 0x1Fu) * 4;   // srsrc{6:2} -> sgpr quad
        out.offen = ((w1 >> 22) & 1u) != 0;
        out.idxen = ((w1 >> 23) & 1u) != 0;
        // soffset[31:24]: a scalar register index exposed via src0.
        out.src0 = (w1 >> 24) & 0xFFu;
        out.raw1 = w1;
        out.dwords_consumed = 2;
        return true;
    }
    if ((w0 & 0xFC000000u) == 0xE8000000u) {          // MTBUF
        if (pc + 1 >= dwords) {
            return false;
        }
        const uint32_t w1 = code[pc + 1];
        out.format = GcnFormat::MTBUF;
        out.opcode = (w0 >> 16) & 0x3Fu;
        out.buf_offset = w0 & 0xFFFu;
        out.vaddr = w1 & 0xFFu;
        out.vdata = (w1 >> 8) & 0xFFu;
        out.srsrc = ((w1 >> 16) & 0x1Fu) * 4;
        out.offen = ((w1 >> 22) & 1u) != 0;
        out.idxen = ((w1 >> 23) & 1u) != 0;
        out.src0 = (w1 >> 24) & 0xFFu;
        out.raw1 = w1;
        out.dwords_consumed = 2;
        return true;
    }
    if ((w0 & 0xFC000000u) == 0xD8000000u) {          // DS
        if (pc + 1 >= dwords) {
            return false;
        }
        const uint32_t w1 = code[pc + 1];
        out.format = GcnFormat::DS;
        // Dual opcode decode (round 29): legacy encodings place the 8-bit
        // opcode at [25:18] (low 2 bits of the 10-bit view are zero); real
        // GFX10 uses the full 10-bit [25:16] space (the wavefront ops live
        // at 0x36c/0x36d). Ambiguity is resolved by the low bits: legacy
        // always reads 0 there.
        {
            const uint32_t op10 = (w0 >> 16) & 0x3FFu;
            out.opcode = ((op10 & 3u) == 0u) ? (op10 >> 2) : op10;
        }
        out.ds_offset0 = w0 & 0xFFu;
        out.ds_offset1 = (w0 >> 8) & 0xFFu;
        out.dst = (w1 >> 24) & 0xFFu;               // vdst[63:56]
        out.src0 = (w1 >> 0) & 0xFFu;               // addr[39:32]
        out.src1 = (w1 >> 8) & 0xFFu;               // data0[47:40]
        out.src2 = (w1 >> 16) & 0xFFu;              // data1[55:48]
        out.raw1 = w1;
        out.dwords_consumed = 2;
        return true;
    }
    // ---- image / export block: prefix 111110 at bits[31:26] (0xF8..0xFB).
    // EXP is the degenerate encoding of the same prefix with ALL of bits
    // [25:15] zero (its target/en/done fields live below bit 15); anything
    // with a nonzero opcode field is MIMG.
    if ((w0 & 0xFC000000u) == 0xF8000000u) {
        if (pc + 1 >= dwords) {
            return false;    // image/export instructions are always 64-bit
        }
        const uint32_t w1 = code[pc + 1];
        if (((w0 >> 15) & 0x7FFu) == 0u) {
            out.format = GcnFormat::EXP;
            out.exp_target = w0 & 0x3Fu;
            out.exp_en = (w0 >> 8) & 0xFu;
            out.exp_done = ((w0 >> 14) & 1u) != 0;
            out.vsrc0 = w1 & 0x1Fu;
            out.vsrc1 = (w1 >> 5) & 0x1Fu;
            out.vsrc2 = (w1 >> 10) & 0x1Fu;
            out.vsrc3 = (w1 >> 15) & 0x1Fu;
            out.raw1 = w1;
            out.dwords_consumed = 2;
            out.is_terminator = out.exp_done;         // done=1 ends the epilogue
            return true;
        }
        out.format = GcnFormat::MIMG;
        out.opcode = (w0 >> 18) & 0xFFu;              // 8-bit opcode at [25:18]
        out.vaddr = w0 & 0xFFu;                       // first address vgpr
        out.dmask = (w0 >> 12) & 0xFu;                // write mask at [15:12]
        out.unorm = ((w0 >> 16) & 1u) != 0;
        out.glc = ((w0 >> 17) & 1u) != 0;
        out.vdata = w1 & 0xFFu;
        out.srsrc = ((w1 >> 8) & 0x7Fu) * 4;          // sgpr quad
        out.ssamp = ((w1 >> 15) & 0xFu) * 4;          // sampler pair
        out.raw1 = w1;
        out.dwords_consumed = 2;
        out.is_terminator = false;
        return true;
    }
    if ((w0 & 0xFF800000u) == 0xDC800000u) {          // FLAT (research layout)
        if (pc + 1 >= dwords) {
            return false;    // FLAT is always 64-bit
        }
        const uint32_t w1 = code[pc + 1];
        out.format = GcnFormat::FLAT;
        out.opcode = (w0 >> 16) & 0x7Fu;              // 7-bit opcode at [22:16]
        out.flat_offset = w0 & 0xFFFFu;               // unsigned byte offset
        out.vaddr = w1 & 0xFFu;
        out.vdata = (w1 >> 8) & 0xFFu;
        out.flat_saddr = ((w1 >> 19) & 1u) != 0;      // SGA-based addressing
        out.raw1 = w1;
        out.dwords_consumed = 2;
        return true;
    }
    if ((w0 & 0x80000000u) == 0u) {                  // VOP2
        out.format = GcnFormat::VOP2;
        out.opcode = (w0 >> 25) & 0x3Fu;
        out.dst = (w0 >> 17) & 0xFFu;
        out.src1 = (w0 >> 9) & 0xFFu;
        out.src0 = w0 & 0x1FFu;
        if (out.src0 == 0xFFu && pc + 1 < dwords) { // 32-bit literal
            out.literal = code[pc + 1];
            out.has_literal = true;
            out.dwords_consumed = 2;
        }
        return true;
    }
    return false;   // does not start any GFX10 instruction (fail-closed)
}

const char* GcnDecoder::Mnemonic(const GcnInstruction& i) {
    switch (i.format) {
        case GcnFormat::SOPP:
            switch (i.opcode) {
                case GcnOp::S_NOP: return "s_nop";
                case GcnOp::S_ENDPGM: return "s_endpgm";
                case GcnOp::S_BRANCH: return "s_branch";
                case GcnOp::S_CBRANCH_SCC0: return "s_cbranch_scc0";
                case GcnOp::S_CBRANCH_SCC1: return "s_cbranch_scc1";
                case GcnOp::S_CBRANCH_VCCZ: return "s_cbranch_vccz";
                case GcnOp::S_CBRANCH_VCCNZ: return "s_cbranch_vccnz";
                case GcnOp::S_CBRANCH_EXECZ: return "s_cbranch_execz";
                case GcnOp::S_CBRANCH_EXECNZ: return "s_cbranch_execnz";
                case GcnOp::S_BARRIER: return "s_barrier";
                case GcnOp::S_WAITCNT: return "s_waitcnt";
                default: return "s_*";
            }
        case GcnFormat::SOP1:
            switch (i.opcode) {
                case GcnOp::S_MOV_B32: return "s_mov_b32";
                case GcnOp::S_MOV_B64: return "s_mov_b64";
                case GcnOp::S_CMOV_B32: return "s_cmov_b32";
                case GcnOp::S_NOT_B32: return "s_not_b32";
                case GcnOp::S_BREV_B32: return "s_brev_b32";
                case GcnOp::S_SEXT_I32_I8: return "s_sext_i32_i8";
                case GcnOp::S_ABS_I32: return "s_abs_i32";
                default: return "s_*";
            }
        case GcnFormat::SOP2:
            switch (i.opcode) {
                case GcnOp::S_ADD_U32: return "s_add_u32";
                case GcnOp::S_SUB_U32: return "s_sub_u32";
                case GcnOp::S_ADD_I32: return "s_add_i32";
                case GcnOp::S_MIN_U32: return "s_min_u32";
                case GcnOp::S_MAX_U32: return "s_max_u32";
                case GcnOp::S_CSELECT_B32: return "s_cselect_b32";
                case GcnOp::S_AND_B32: return "s_and_b32";
                case GcnOp::S_OR_B32: return "s_or_b32";
                case GcnOp::S_XOR_B32: return "s_xor_b32";
                case GcnOp::S_LSHL_B32: return "s_lshl_b32";
                case GcnOp::S_LSHR_B32: return "s_lshr_b32";
                case GcnOp::S_ASHR_I32: return "s_ashr_i32";
                case GcnOp::S_MUL_I32: return "s_mul_i32";
                default: return "s_*";
            }
        case GcnFormat::SOPK:
            switch (i.opcode) {
                case GcnOp::S_MOVK_I32: return "s_movk_i32";
                case GcnOp::S_ADDK_I32: return "s_addk_i32";
                case GcnOp::S_MULK_I32: return "s_mulk_i32";
                default: return "s_*k_*";
            }
        case GcnFormat::SOPC:
            switch (i.opcode) {
                case GcnOp::S_CMP_EQ_U32: return "s_cmp_eq_u32";
                case GcnOp::S_CMP_LG_U32: return "s_cmp_lg_u32";
                case GcnOp::S_CMP_GT_U32: return "s_cmp_gt_u32";
                case GcnOp::S_CMP_LT_U32: return "s_cmp_lt_u32";
                case GcnOp::S_CMP_EQ_I32: return "s_cmp_eq_i32";
                case GcnOp::S_CMP_GT_I32: return "s_cmp_gt_i32";
                default: return "s_cmp_*";
            }
        case GcnFormat::VOP1:
            switch (i.opcode) {
                case GcnOp::V_NOP: return "v_nop";
                case GcnOp::V_MOV_B32: return "v_mov_b32";
                case GcnOp::V_CVT_F32_I32: return "v_cvt_f32_i32";
                case GcnOp::V_CVT_I32_F32: return "v_cvt_i32_f32";
                case GcnOp::V_FLOOR_F32: return "v_floor_f32";
                case GcnOp::V_CEIL_F32: return "v_ceil_f32";
                case GcnOp::V_TRUNC_F32: return "v_trunc_f32";
                case GcnOp::V_RNDNE_F32: return "v_rndne_f32";
                case GcnOp::V_FRACT_F32: return "v_fract_f32";
                case GcnOp::V_SQRT_F32: return "v_sqrt_f32";
                case GcnOp::V_RCP_F32: return "v_rcp_f32";
                case GcnOp::V_RSQ_F32: return "v_rsq_f32";
                case GcnOp::V_LOG_F32: return "v_log_f32";
                case GcnOp::V_EXP_F32: return "v_exp_f32";
                case GcnOp::V_NOT_B32: return "v_not_b32";
                default: return "v_*";
            }
        case GcnFormat::VOP2:
            switch (i.opcode) {
                case GcnOp::V_CNDMASK_B32: return "v_cndmask_b32";
                case GcnOp::V_ADD_F32: return "v_add_f32";
                case GcnOp::V_SUB_F32: return "v_sub_f32";
                case GcnOp::V_SUBREV_F32: return "v_subrev_f32";
                case GcnOp::V_MUL_F32: return "v_mul_f32";
                case GcnOp::V_MUL_U32_U24: return "v_mul_u32_u24";
                case GcnOp::V_MIN_F32: return "v_min_f32";
                case GcnOp::V_MAX_F32: return "v_max_f32";
                case GcnOp::V_MIN_I32: return "v_min_i32";
                case GcnOp::V_MAX_I32: return "v_max_i32";
                case GcnOp::V_MIN_U32: return "v_min_u32";
                case GcnOp::V_MAX_U32: return "v_max_u32";
                case GcnOp::V_AND_B32: return "v_and_b32";
                case GcnOp::V_OR_B32: return "v_or_b32";
                case GcnOp::V_XOR_B32: return "v_xor_b32";
                case GcnOp::V_LSHLREV_B32: return "v_lshlrev_b32";
                case GcnOp::V_LSHRREV_B32: return "v_lshrrev_b32";
                case GcnOp::V_ASHRREV_I32: return "v_ashrrev_i32";
                case GcnOp::V_MAC_F32: return "v_mac_f32";
                default: return "v_*";
            }
        case GcnFormat::VOP3:
            switch (i.opcode) {
                case GcnOp::V_MAD_F32: return "v_mad_f32";
                case GcnOp::V_MAD_U32_U24: return "v_mad_u32_u24";
                case GcnOp::V_FMA_F32: return "v_fma_f32";
                case GcnOp::V_MIN3_F32: return "v_min3_f32";
                case GcnOp::V_MAX3_F32: return "v_max3_f32";
                case GcnOp::V_MED3_I32: return "v_med3_i32";
                case GcnOp::V_MUL_LO_U32: return "v_mul_lo_u32";
                case GcnOp::V_MUL_HI_U32: return "v_mul_hi_u32";
                case GcnOp::V_BFE_U32: return "v_bfe_u32";
                case GcnOp::V_BCNT_U32_B32: return "v_bcnt_u32_b32";
                case GcnOp::V_BFM_B32: return "v_bfm_b32";
                case GcnOp::V_LDEXP_F32: return "v_ldexp_f32";
                case GcnOp::V_INTERP_P1_F32: return "v_interp_p1_f32";
                case GcnOp::V_INTERP_P2_F32: return "v_interp_p2_f32";
                case GcnOp::V_INTERP_MOV_F32: return "v_interp_mov_f32";
                default: return "v_*_e64";
            }
        case GcnFormat::VOPC:
            switch (i.opcode) {
                case GcnOp::V_CMP_LT_F32: return "v_cmp_lt_f32";
                case GcnOp::V_CMP_EQ_F32: return "v_cmp_eq_f32";
                case GcnOp::V_CMP_LE_F32: return "v_cmp_le_f32";
                case GcnOp::V_CMP_GT_F32: return "v_cmp_gt_f32";
                case GcnOp::V_CMP_LG_F32: return "v_cmp_lg_f32";
                case GcnOp::V_CMP_GE_F32: return "v_cmp_ge_f32";
                case GcnOp::V_CMP_LT_I32: return "v_cmp_lt_i32";
                case GcnOp::V_CMP_EQ_I32: return "v_cmp_eq_i32";
                case GcnOp::V_CMP_GT_I32: return "v_cmp_gt_i32";
                case GcnOp::V_CMP_LT_U32: return "v_cmp_lt_u32";
                case GcnOp::V_CMP_EQ_U32: return "v_cmp_eq_u32";
                case GcnOp::V_CMP_GT_U32: return "v_cmp_gt_u32";
                default: return "v_cmp_*";
            }
        case GcnFormat::SMEM:
            switch (i.opcode) {
                case GcnOp::S_LOAD_DWORD: return "s_load_dword";
                case GcnOp::S_LOAD_DWORDX2: return "s_load_dwordx2";
                case GcnOp::S_LOAD_DWORDX4: return "s_load_dwordx4";
                case GcnOp::S_BUFFER_LOAD_DWORD: return "s_buffer_load_dword";
                case GcnOp::S_BUFFER_LOAD_DWORDX4: return "s_buffer_load_dwordx4";
                default: return "s_load_*";
            }
        case GcnFormat::MUBUF:
            switch (i.opcode) {
                case GcnOp::BUFFER_LOAD_DWORD: return "buffer_load_dword";
                case GcnOp::BUFFER_LOAD_DWORDX2: return "buffer_load_dwordx2";
                case GcnOp::BUFFER_LOAD_DWORDX4: return "buffer_load_dwordx4";
                case GcnOp::BUFFER_STORE_DWORD: return "buffer_store_dword";
                case GcnOp::BUFFER_LOAD_FORMAT_XYZW: return "buffer_load_format_xyzw";
                default: return "buffer_*";
            }
        case GcnFormat::MTBUF: return "tbuffer_*";
        case GcnFormat::DS:
            switch (i.opcode) {
                case GcnOp::DS_WRITE_B32: return "ds_write_b32";
                case GcnOp::DS_READ_B32: return "ds_read_b32";
                case GcnOp::DS_ADD_U32: return "ds_add_u32";
                default: return "ds_*";
            }
        case GcnFormat::MIMG:
            switch (i.opcode) {
                case GcnOp::IMAGE_LOAD: return "image_load";
                case GcnOp::IMAGE_LOAD_MIP: return "image_load_mip";
                case GcnOp::IMAGE_STORE: return "image_store";
                case GcnOp::IMAGE_STORE_MIP: return "image_store_mip";
                case GcnOp::IMAGE_GET_RESINFO: return "image_get_resinfo";
                case GcnOp::IMAGE_GET_LOD: return "image_get_lod";
                case GcnOp::IMAGE_SAMPLE: return "image_sample";
                case GcnOp::IMAGE_SAMPLE_L: return "image_sample_l";
                case GcnOp::IMAGE_SAMPLE_LZ: return "image_sample_lz";
                case GcnOp::IMAGE_SAMPLE_B: return "image_sample_b";
                case GcnOp::IMAGE_SAMPLE_C: return "image_sample_c";
                case GcnOp::IMAGE_SAMPLE_D: return "image_sample_d";
                case GcnOp::IMAGE_SAMPLE_DZ: return "image_sample_dz";
                case GcnOp::IMAGE_SAMPLE_CL: return "image_sample_cl";
                case GcnOp::IMAGE_SAMPLE_O: return "image_sample_o";
                case GcnOp::IMAGE_SAMPLE_CD: return "image_sample_cd";
                case GcnOp::IMAGE_SAMPLE_PCK: return "image_sample_pck";
                case GcnOp::IMAGE_GATHER4: return "image_gather4";
                case GcnOp::IMAGE_GATHER4_LZ: return "image_gather4_lz";
                case GcnOp::IMAGE_GATHER4_B: return "image_gather4_b";
                case GcnOp::IMAGE_GATHER4_C_LZ: return "image_gather4_c_lz";
                case GcnOp::IMAGE_GATHER4_PO: return "image_gather4_po";
                case GcnOp::IMAGE_GATHER4H: return "image_gather4h";
                case GcnOp::IMAGE_ATOMIC_SWAP: return "image_atomic_swap";
                case GcnOp::IMAGE_ATOMIC_CMPSWAP: return "image_atomic_cmpswap";
                case GcnOp::IMAGE_ATOMIC_ADD: return "image_atomic_add";
                case GcnOp::IMAGE_ATOMIC_SUB: return "image_atomic_sub";
                case GcnOp::IMAGE_ATOMIC_SMIN: return "image_atomic_smin";
                case GcnOp::IMAGE_ATOMIC_UMIN: return "image_atomic_umin";
                case GcnOp::IMAGE_ATOMIC_SMAX: return "image_atomic_smax";
                case GcnOp::IMAGE_ATOMIC_UMAX: return "image_atomic_umax";
                case GcnOp::IMAGE_ATOMIC_AND: return "image_atomic_and";
                case GcnOp::IMAGE_ATOMIC_OR: return "image_atomic_or";
                case GcnOp::IMAGE_ATOMIC_XOR: return "image_atomic_xor";
                case GcnOp::IMAGE_ATOMIC_INC: return "image_atomic_inc";
                case GcnOp::IMAGE_ATOMIC_DEC: return "image_atomic_dec";
                default: return "image_*";
            }
        case GcnFormat::VOP3P:
            switch (i.opcode) {
                case GcnOp::V_MAD_MIX_F32: return "v_mad_mix_f32";
                default: return "vop3p_*";
            }
        case GcnFormat::EXP:
            return (i.exp_target >= 32) ? "exp_param" : "exp_mrt";
        case GcnFormat::FLAT:
            switch (i.opcode) {
                case GcnOp::FLAT_LOAD_UBYTE: return "flat_load_ubyte";
                case GcnOp::FLAT_LOAD_SBYTE: return "flat_load_sbyte";
                case GcnOp::FLAT_LOAD_USHORT: return "flat_load_ushort";
                case GcnOp::FLAT_LOAD_SSHORT: return "flat_load_sshort";
                case GcnOp::FLAT_LOAD_DWORD: return "flat_load_dword";
                case GcnOp::FLAT_LOAD_DWORDX2: return "flat_load_dwordx2";
                case GcnOp::FLAT_LOAD_DWORDX3: return "flat_load_dwordx3";
                case GcnOp::FLAT_LOAD_DWORDX4: return "flat_load_dwordx4";
                case GcnOp::FLAT_STORE_BYTE: return "flat_store_byte";
                case GcnOp::FLAT_STORE_SHORT: return "flat_store_short";
                case GcnOp::FLAT_STORE_DWORD: return "flat_store_dword";
                case GcnOp::FLAT_STORE_DWORDX2: return "flat_store_dwordx2";
                case GcnOp::FLAT_STORE_DWORDX3: return "flat_store_dwordx3";
                case GcnOp::FLAT_STORE_DWORDX4: return "flat_store_dwordx4";
                case GcnOp::FLAT_ATOMIC_SWAP: return "flat_atomic_swap";
                case GcnOp::FLAT_ATOMIC_ADD: return "flat_atomic_add";
                case GcnOp::FLAT_ATOMIC_CMPSWAP: return "flat_atomic_cmpswap";
                default: return "flat_*";
            }
        default: return "unknown";
    }
}

// ---------------------------------------------------------------------------
// Software executor
// ---------------------------------------------------------------------------
namespace {

float BitsToF(uint32_t v) { float f; std::memcpy(&f, &v, 4); return f; }
uint32_t FToBits(float f) { uint32_t v; std::memcpy(&v, &f, 4); return v; }

} // namespace

bool GcnSwExecutor::SwImage::SetTexel(uint32_t x, uint32_t y, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    if (x >= width || y >= height) return false;
    const size_t idx = (static_cast<size_t>(y) * width + x) * 4;
    if (idx + 3 >= rgba.size()) return false;
    rgba[idx] = r; rgba[idx + 1] = g; rgba[idx + 2] = b; rgba[idx + 3] = a;
    return true;
}

bool GcnSwExecutor::SwImage::GetTexel(uint32_t x, uint32_t y, uint32_t& r, uint32_t& g, uint32_t& b, uint32_t& a) const {
    if (x >= width || y >= height) return false;
    const size_t idx = (static_cast<size_t>(y) * width + x) * 4;
    if (idx + 3 >= rgba.size()) return false;
    r = rgba[idx]; g = rgba[idx + 1]; b = rgba[idx + 2]; a = rgba[idx + 3];
    return true;
}

GcnSwExecResult GcnSwExecutor::Run(const uint32_t* code, size_t dwords, size_t lanes,
                                   const std::vector<uint32_t>& input, uint32_t k_in,
                                   uint32_t m_out, std::vector<uint32_t>& output,
                                   GpuGuestMemory* mem,
                                   const std::vector<GcnBufferResource>* buffers,
                                   size_t instruction_limit) {
    GcnSwExecResult res;
    if (code == nullptr || dwords == 0) {
        res.error = "empty program";
        return res;
    }
    if (lanes > kMaxLanes) {
        res.error = "lane count exceeds software-executor budget";
        return res;
    }
    if (m_out == 0) {
        res.error = "m_out must be >= 1";
        return res;
    }
    output.assign(static_cast<size_t>(lanes) * m_out, 0);
    m_vgprs.assign(static_cast<size_t>(kMaxLanes) * kVgprCount, 0);
    m_vcc = 0;
    m_exec = ~0ull;
    m_scc = 0;

    const GcnDecoder decoder;

    auto sgpr_read = [&](uint16_t enc, uint32_t& out_val) -> bool {
        if (enc <= GCN_SSRC_SGPR_MAX) {
            out_val = m_sgprs[enc];
            return true;
        }
        // Special scalar registers.
        switch (enc) {
            case GCN_SSRC_VCC_LO: out_val = m_vcc; return true;
            case GCN_SSRC_VCC_HI: out_val = 0; return true;
            case GCN_SSRC_M0: out_val = 0; return true;
            case GCN_SSRC_EXEC_LO: out_val = static_cast<uint32_t>(m_exec); return true;
            case GCN_SSRC_EXEC_HI: out_val = static_cast<uint32_t>(m_exec >> 32); return true;
            default: return GcnDecodeInlineConstant32(enc, out_val);
        }
    };

    // 9-bit source-field resolution (VOP1/VOP2/VOPC src0, VOP3 src0/1/2):
    // 0..101 = SGPR, 102..111 = special (vcc/m0/exec), 128..192 = inline
    // ints, 193..208 = negative ints, 240..247 = floats, 255 = literal
    // (consumed from the stream), 256..511 = VGPR v0..v255.
    auto read_src9 = [&](size_t lane, uint16_t enc, uint32_t& out_val,
                         const GcnInstruction& ins) -> bool {
        if (enc >= 256) {
            out_val = m_vgprs[static_cast<size_t>(lane) * kVgprCount + (enc - 256)];
            return true;
        }
        if (ins.has_literal && enc == 0xFFu) {
            out_val = ins.literal;
            return true;
        }
        return sgpr_read(enc, out_val);
    };

    // 8-bit src1 of VOP2/VOPC: a direct VGPR index.
    auto read_src8_vgpr = [&](size_t lane, uint8_t idx, uint32_t& out_val) -> bool {
        out_val = m_vgprs[static_cast<size_t>(lane) * kVgprCount + idx];
        return true;
    };

    auto scalar_src = [&](const GcnInstruction& ins, uint16_t enc) -> uint32_t {
        if (ins.has_literal && enc == 0xFFu) {
            return ins.literal;
        }
        uint32_t v = 0;
        sgpr_read(enc, v);
        return v;
    };

    // Apply VOP3 modifiers (abs/neg/omod) to a float source.
    auto apply_f32_mods = [&](float f, const GcnInstruction& ins, int which) {
        bool neg = which == 0 ? ins.neg0 : (which == 1 ? ins.neg1 : ins.neg2);
        bool abs = which == 0 ? ins.abs0 : (which == 1 ? ins.abs1 : ins.abs2);
        if (abs) f = std::fabs(f);
        if (neg) f = -f;
        return f;
    };

    // Apply integer negate (VOP3 neg on integer sources is a bitwise NOT).
    auto apply_int_mods = [&](uint32_t v, const GcnInstruction& ins, int which) -> uint32_t {
        bool neg = which == 0 ? ins.neg0 : (which == 1 ? ins.neg1 : ins.neg2);
        return neg ? ~v : v;
    };

    auto apply_omod = [&](float f, uint8_t omod) {
        if (omod == 1) f *= 2.0f;
        else if (omod == 2) f *= 4.0f;
        else if (omod == 3) f *= 0.5f;
        return f;
    };

    size_t total_exec = 0;
    bool terminated = false;
    size_t barrier_count = 0;
    const size_t lanes_total = lanes;
    (void)barrier_count;
    // Shared local data store (LDS). 64 KiB matches the practical GCN/RDNA
    // workgroup LDS budget used by this research core. It is shared across
    // lanes in one Run() call and reset per dispatch.
    constexpr size_t kLdsDwords = 16u * 1024u;
    std::vector<uint32_t> lds(kLdsDwords, 0);
    m_wave_columns.clear();

    // Round 29: seed EVERY lane's inputs BEFORE any lane executes. The
    // wavefront ops (ds_bpermute_b32 and friends) read other lanes' VGPRs;
    // under the previous lazy per-lane seeding a lane that shuffled from a
    // not-yet-started lane would read zeros instead of that lane's inputs
    // (inputs must be visible wavefront-wide, like hardware).
    for (size_t lane = 0; lane < lanes; ++lane) {
        uint32_t* const vg_all = m_vgprs.data() + static_cast<size_t>(lane) * kVgprCount;
        std::memset(vg_all, 0, kVgprCount * sizeof(uint32_t));
        for (uint32_t j = 0; j < k_in && j < kVgprCount; ++j) {
            const size_t idx = static_cast<size_t>(lane) * k_in + j;
            if (idx < input.size()) {
                vg_all[j] = input[idx];
            }
        }
    }

    for (size_t lane = 0; lane < lanes; ++lane) {
        uint32_t* vg = m_vgprs.data() + static_cast<size_t>(lane) * kVgprCount;

        size_t pc = 0;
        size_t executed = 0;
        bool lane_done = false;

        while (pc < dwords && !lane_done) {
            if (executed >= instruction_limit) {
                res.error = "instruction limit exceeded";
                return res;
            }
            GcnInstruction ins;
            if (!decoder.Decode(code, dwords, pc, ins)) {
                res.error = "undecodable dword at " + std::to_string(pc);
                return res;
            }
            ++executed;

            switch (ins.format) {
                case GcnFormat::SOPP: {
                    if (ins.opcode == GcnOp::S_ENDPGM) {
                        lane_done = true;
                        terminated = true;
                    } else if (ins.opcode == GcnOp::S_BRANCH) {
                        pc = static_cast<size_t>(ins.branch_target);
                        continue;   // pc advanced below would double-step
                    } else if (ins.opcode == GcnOp::S_CBRANCH_SCC1 ||
                               ins.opcode == GcnOp::S_CBRANCH_SCC0) {
                        const bool want = ins.opcode == GcnOp::S_CBRANCH_SCC1;
                        if ((m_scc != 0) == want) {
                            pc = static_cast<size_t>(ins.branch_target);
                            continue;
                        }
                    } else if (ins.opcode == GcnOp::S_CBRANCH_VCCZ ||
                               ins.opcode == GcnOp::S_CBRANCH_VCCNZ) {
                        const bool want_nz = ins.opcode == GcnOp::S_CBRANCH_VCCNZ;
                        if ((m_vcc != 0) == want_nz) {
                            pc = static_cast<size_t>(ins.branch_target);
                            continue;
                        }
                    } else if (ins.opcode == GcnOp::S_CBRANCH_EXECZ ||
                               ins.opcode == GcnOp::S_CBRANCH_EXECNZ) {
                        const bool want_nz = ins.opcode == GcnOp::S_CBRANCH_EXECNZ;
                        if ((m_exec != 0) == want_nz) {
                            pc = static_cast<size_t>(ins.branch_target);
                            continue;
                        }
                    }
                    if (ins.opcode == GcnOp::S_BARRIER) {
                        // Wavefront convergence point. The software executor
                        // runs lanes sequentially, so a barrier is a no-op
                        // here -- but it is RECOGNISED (the SPIR-V path emits
                        // a real OpControlBarrier for Vulkan execution).
                        barrier_count++;
                    }
                    // s_nop / s_waitcnt / others: no architectural effect here.
                    break;
                }
                case GcnFormat::SOP1: {
                    // Round 18: sdst >= 104 addresses special registers
                    // (106 = SCC); the software executor models plain SGPR
                    // destinations only -- fail closed instead of writing
                    // out of bounds (latent round-12 OOB).
                    if (static_cast<size_t>(ins.dst) >= GcnSwExecutor::kSgprCount) {
                        res.error = "SOP1 sdst special register unsupported";
                        return res;
                    }
                    const uint32_t a = scalar_src(ins, ins.src0);
                    switch (ins.opcode) {
                        case GcnOp::S_MOV_B32: m_sgprs[ins.dst] = a; break;
                        case GcnOp::S_MOV_B64:
                            // 64-bit move from a single 32-bit source: lo = src,
                            // hi = 0 (documented simplification).
                            // Guard against OOB: dst+1 must be within SGPR bounds.
                            assert(static_cast<size_t>(ins.dst) + 1 < GcnSwExecutor::kSgprCount);
                            if (static_cast<size_t>(ins.dst) + 1 >= GcnSwExecutor::kSgprCount) {
                                res.error = "S_MOV_B64 sdst+1 out of bounds";
                                return res;
                            }
                            m_sgprs[ins.dst] = a;
                            m_sgprs[ins.dst + 1] = 0;
                            break;
                        case GcnOp::S_NOT_B32: m_sgprs[ins.dst] = ~a; m_scc = (a != 0); break;
                        case GcnOp::S_BREV_B32: {
                            uint32_t r = 0;
                            for (int bit = 0; bit < 32; ++bit) {
                                if (a & (1u << bit)) r |= 1u << (31 - bit);
                            }
                            m_sgprs[ins.dst] = r;
                            break;
                        }
                        case GcnOp::S_SEXT_I32_I8:
                            m_sgprs[ins.dst] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(a))); break;
                        case GcnOp::S_SEXT_I32_I16:
                            m_sgprs[ins.dst] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(a))); break;
                        case GcnOp::S_ABS_I32:
                            m_sgprs[ins.dst] = static_cast<uint32_t>(
                                std::abs(static_cast<int32_t>(a))); break;
                        case GcnOp::S_CMOV_B32:
                            if (m_scc) m_sgprs[ins.dst] = a;
                            break;
                        default: res.error = "unsupported SOP1 opcode"; return res;
                    }
                    break;
                }
                case GcnFormat::SOP2: {
                    if (static_cast<size_t>(ins.dst) >= GcnSwExecutor::kSgprCount) {
                        res.error = "SOP2 sdst special register unsupported";
                        return res;
                    }
                    const uint32_t a = scalar_src(ins, ins.src0);
                    const uint32_t b = scalar_src(ins, ins.src1);
                    switch (ins.opcode) {
                        case GcnOp::S_ADD_U32:
                        case GcnOp::S_ADD_I32: {
                            const uint64_t r = static_cast<uint64_t>(a) + b;
                            m_sgprs[ins.dst] = static_cast<uint32_t>(r);
                            m_scc = (r >> 32) != 0;
                            break;
                        }
                        case GcnOp::S_SUB_U32:
                        case GcnOp::S_SUB_I32: {
                            m_sgprs[ins.dst] = a - b;
                            m_scc = a < b;   // borrow
                            break;
                        }
                        case GcnOp::S_MIN_U32: m_sgprs[ins.dst] = std::min(a, b); m_scc = a < b; break;
                        case GcnOp::S_MAX_U32: m_sgprs[ins.dst] = std::max(a, b); m_scc = a > b; break;
                        case GcnOp::S_MIN_I32:
                            m_sgprs[ins.dst] = static_cast<uint32_t>(
                                std::min(static_cast<int32_t>(a), static_cast<int32_t>(b)));
                            m_scc = static_cast<int32_t>(a) < static_cast<int32_t>(b);
                            break;
                        case GcnOp::S_MAX_I32:
                            m_sgprs[ins.dst] = static_cast<uint32_t>(
                                std::max(static_cast<int32_t>(a), static_cast<int32_t>(b)));
                            m_scc = static_cast<int32_t>(a) > static_cast<int32_t>(b);
                            break;
                        case GcnOp::S_CSELECT_B32:
                            m_sgprs[ins.dst] = m_scc ? a : b; break;
                        case GcnOp::S_AND_B32: m_sgprs[ins.dst] = a & b; m_scc = (a & b) != 0; break;
                        case GcnOp::S_OR_B32:  m_sgprs[ins.dst] = a | b; m_scc = (a | b) != 0; break;
                        case GcnOp::S_XOR_B32: m_sgprs[ins.dst] = a ^ b; m_scc = (a ^ b) != 0; break;
                        case GcnOp::S_ANDN2_B32: m_sgprs[ins.dst] = a & ~b; m_scc = (a & ~b) != 0; break;
                        case GcnOp::S_ORN2_B32:  m_sgprs[ins.dst] = a | ~b; m_scc = (a | ~b) != 0; break;
                        case GcnOp::S_NAND_B32: m_sgprs[ins.dst] = ~(a & b); m_scc = ~(a & b) != 0; break;
                        case GcnOp::S_NOR_B32:  m_sgprs[ins.dst] = ~(a | b); m_scc = ~(a | b) != 0; break;
                        case GcnOp::S_XNOR_B32: m_sgprs[ins.dst] = ~(a ^ b); m_scc = ~(a ^ b) != 0; break;
                        case GcnOp::S_LSHL_B32: m_sgprs[ins.dst] = a << (b & 31); m_scc = false; break;
                        case GcnOp::S_LSHR_B32: m_sgprs[ins.dst] = a >> (b & 31); m_scc = false; break;
                        case GcnOp::S_ASHR_I32:
                            m_sgprs[ins.dst] = static_cast<uint32_t>(
                                static_cast<int32_t>(a) >> (b & 31));
                            m_scc = false;
                            break;
                        case GcnOp::S_MUL_I32:
                            m_sgprs[ins.dst] = static_cast<uint32_t>(
                                static_cast<int64_t>(static_cast<int32_t>(a)) *
                                static_cast<int64_t>(static_cast<int32_t>(b)));
                            break;
                        case GcnOp::S_BFM_B32: {
                            const uint32_t width = a & 31;
                            const uint32_t off = b & 31;
                            m_sgprs[ins.dst] = (width == 32) ? 0xFFFFFFFFu : ((1u << width) - 1u) << off;
                            break;
                        }
                        default: res.error = "unsupported SOP2 opcode"; return res;
                    }
                    break;
                }
                case GcnFormat::SOPK: {
                    if (static_cast<size_t>(ins.dst) >= GcnSwExecutor::kSgprCount) {
                        res.error = "SOPK sdst special register unsupported";
                        return res;
                    }
                    const int32_t simm = static_cast<int32_t>(SignExtend16(ins.simm16));
                    switch (ins.opcode) {
                        case GcnOp::S_MOVK_I32: m_sgprs[ins.dst] = static_cast<uint32_t>(simm); break;
                        case GcnOp::S_ADDK_I32:
                            m_sgprs[ins.dst] = m_sgprs[ins.dst] + static_cast<uint32_t>(simm); break;
                        case GcnOp::S_MULK_I32:
                            m_sgprs[ins.dst] = m_sgprs[ins.dst] * static_cast<uint32_t>(simm); break;
                        // SOPK comparisons write SCC (the sdst field is ignored).
                        case GcnOp::S_CMPK_EQ_I32:
                            m_scc = static_cast<int32_t>(m_sgprs[ins.dst]) == simm ? 1 : 0; break;
                        case GcnOp::S_CMPK_LG_I32:
                            m_scc = static_cast<int32_t>(m_sgprs[ins.dst]) != simm ? 1 : 0; break;
                        case GcnOp::S_CMPK_GT_I32:
                            m_scc = static_cast<int32_t>(m_sgprs[ins.dst]) > simm ? 1 : 0; break;
                        case GcnOp::S_CMPK_LT_I32:
                            m_scc = static_cast<int32_t>(m_sgprs[ins.dst]) < simm ? 1 : 0; break;
                        case GcnOp::S_CMPK_EQ_U32:
                            m_scc = m_sgprs[ins.dst] == static_cast<uint32_t>(simm) ? 1 : 0; break;
                        case GcnOp::S_CMPK_LG_U32:
                            m_scc = m_sgprs[ins.dst] != static_cast<uint32_t>(simm) ? 1 : 0; break;
                        case GcnOp::S_CMPK_GT_U32:
                            m_scc = m_sgprs[ins.dst] > static_cast<uint32_t>(simm) ? 1 : 0; break;
                        case GcnOp::S_CMPK_LT_U32:
                            m_scc = m_sgprs[ins.dst] < static_cast<uint32_t>(simm) ? 1 : 0; break;
                        default: res.error = "unsupported SOPK opcode"; return res;
                    }
                    break;
                }
                case GcnFormat::SOPC: {
                    const uint32_t a = scalar_src(ins, ins.src0);
                    const uint32_t b = scalar_src(ins, ins.src1);
                    bool scc = false;
                    switch (ins.opcode) {
                        case GcnOp::S_CMP_EQ_U32: scc = a == b; break;
                        case GcnOp::S_CMP_LG_U32: scc = a != b; break;
                        case GcnOp::S_CMP_GT_U32: scc = a > b; break;
                        case GcnOp::S_CMP_GE_U32: scc = a >= b; break;
                        case GcnOp::S_CMP_LT_U32: scc = a < b; break;
                        case GcnOp::S_CMP_LE_U32: scc = a <= b; break;
                        case GcnOp::S_CMP_EQ_I32: scc = static_cast<int32_t>(a) == static_cast<int32_t>(b); break;
                        case GcnOp::S_CMP_LG_I32: scc = static_cast<int32_t>(a) != static_cast<int32_t>(b); break;
                        case GcnOp::S_CMP_GT_I32: scc = static_cast<int32_t>(a) > static_cast<int32_t>(b); break;
                        case GcnOp::S_CMP_LT_I32: scc = static_cast<int32_t>(a) < static_cast<int32_t>(b); break;
                        default: res.error = "unsupported SOPC opcode"; return res;
                    }
                    m_scc = scc ? 1 : 0;
                    break;
                }
                case GcnFormat::VOP1: {
                    // Defensive bounds check: VOP1 dst is 8-bit (0..255) which
                    // matches kVgprCount=256 exactly, but corrupted bytecode
                    // could still encode an out-of-range index.
                    assert(static_cast<size_t>(ins.dst) < GcnSwExecutor::kVgprCount);
                    if (static_cast<size_t>(ins.dst) >= GcnSwExecutor::kVgprCount) {
                        res.error = "VOP1 dst out of VGPR bounds";
                        return res;
                    }
                    uint32_t s = 0;
                    if (!read_src9(lane, ins.src0, s, ins)) {
                        res.error = "bad VOP1 src0";
                        return res;
                    }
                    switch (ins.opcode) {
                        case GcnOp::V_NOP: break;
                        case GcnOp::V_MOV_B32: vg[ins.dst] = s; break;
                        case GcnOp::V_CVT_F32_I32: vg[ins.dst] = FToBits(static_cast<float>(static_cast<int32_t>(s))); break;
                        case GcnOp::V_CVT_F32_U32: vg[ins.dst] = FToBits(static_cast<float>(s)); break;
                        case GcnOp::V_CVT_I32_F32: vg[ins.dst] = static_cast<uint32_t>(BitsToF(s)); break;
                        case GcnOp::V_CVT_U32_F32: vg[ins.dst] = static_cast<uint32_t>(BitsToF(s)); break;
                        case GcnOp::V_CVT_RPI_I32_F32: vg[ins.dst] = static_cast<uint32_t>(std::nearbyint(BitsToF(s))); break;
                        case GcnOp::V_CVT_FLR_I32_F32: vg[ins.dst] = static_cast<uint32_t>(std::floor(BitsToF(s))); break;
                        case GcnOp::V_FLOOR_F32: vg[ins.dst] = FToBits(std::floor(BitsToF(s))); break;
                        case GcnOp::V_CEIL_F32: vg[ins.dst] = FToBits(std::ceil(BitsToF(s))); break;
                        case GcnOp::V_TRUNC_F32: vg[ins.dst] = FToBits(std::trunc(BitsToF(s))); break;
                        case GcnOp::V_RNDNE_F32: vg[ins.dst] = FToBits(std::nearbyint(BitsToF(s))); break;
                        case GcnOp::V_FRACT_F32: vg[ins.dst] = FToBits(BitsToF(s) - std::floor(BitsToF(s))); break;
                        case GcnOp::V_SQRT_F32: vg[ins.dst] = FToBits(std::sqrt(BitsToF(s))); break;
                        case GcnOp::V_RCP_F32: vg[ins.dst] = FToBits(1.0f / BitsToF(s)); break;
                        case GcnOp::V_RSQ_F32: vg[ins.dst] = FToBits(1.0f / std::sqrt(BitsToF(s))); break;
                        case GcnOp::V_LOG_F32: vg[ins.dst] = FToBits(std::log2(BitsToF(s))); break;
                        case GcnOp::V_EXP_F32: vg[ins.dst] = FToBits(std::exp2(BitsToF(s))); break;
                        case GcnOp::V_SIN_F32: vg[ins.dst] = FToBits(std::sin(BitsToF(s))); break;
                        case GcnOp::V_COS_F32: vg[ins.dst] = FToBits(std::cos(BitsToF(s))); break;
                        case GcnOp::V_NOT_B32: vg[ins.dst] = ~s; break;
                        case GcnOp::V_BFREV_B32: {
                            uint32_t r = 0;
                            for (int bit = 0; bit < 32; ++bit) if (s & (1u << bit)) r |= 1u << (31 - bit);
                            vg[ins.dst] = r;
                            break;
                        }
                        default: res.error = "unsupported VOP1 opcode"; return res;
                    }
                    break;
                }
                case GcnFormat::VOP2: {
                    assert(static_cast<size_t>(ins.dst) < GcnSwExecutor::kVgprCount);
                    if (static_cast<size_t>(ins.dst) >= GcnSwExecutor::kVgprCount) {
                        res.error = "VOP2 dst out of VGPR bounds";
                        return res;
                    }
                    uint32_t s0 = 0, s1 = 0;
                    if (!read_src9(lane, ins.src0, s0, ins) ||
                        !read_src8_vgpr(lane, static_cast<uint8_t>(ins.src1), s1)) {
                        res.error = "bad VOP2 source";
                        return res;
                    }
                    // src1 of VOP2 may also be an SGPR encoding? No: the 8-bit
                    // src1 field is a VGPR index; scalar sources use VOP3.
                    switch (ins.opcode) {
                        case GcnOp::V_CNDMASK_B32:
                            vg[ins.dst] = (m_vcc & 1) ? s1 : s0; break;
                        case GcnOp::V_ADD_F32: vg[ins.dst] = FToBits(BitsToF(s0) + BitsToF(s1)); break;
                        case GcnOp::V_SUB_F32: vg[ins.dst] = FToBits(BitsToF(s0) - BitsToF(s1)); break;
                        case GcnOp::V_SUBREV_F32: vg[ins.dst] = FToBits(BitsToF(s1) - BitsToF(s0)); break;
                        case GcnOp::V_MUL_F32: vg[ins.dst] = FToBits(BitsToF(s0) * BitsToF(s1)); break;
                        case GcnOp::V_MUL_U32_U24: {
                            const uint64_t r = static_cast<uint64_t>(s0 & 0xFFFFFFu) * (s1 & 0xFFFFFFu);
                            vg[ins.dst] = static_cast<uint32_t>(r);
                            break;
                        }
                        case GcnOp::V_MIN_F32: vg[ins.dst] = FToBits(std::min(BitsToF(s0), BitsToF(s1))); break;
                        case GcnOp::V_MAX_F32: vg[ins.dst] = FToBits(std::max(BitsToF(s0), BitsToF(s1))); break;
                        case GcnOp::V_MIN_I32:
                            vg[ins.dst] = static_cast<uint32_t>(std::min(static_cast<int32_t>(s0), static_cast<int32_t>(s1))); break;
                        case GcnOp::V_MAX_I32:
                            vg[ins.dst] = static_cast<uint32_t>(std::max(static_cast<int32_t>(s0), static_cast<int32_t>(s1))); break;
                        case GcnOp::V_MIN_U32: vg[ins.dst] = std::min(s0, s1); break;
                        case GcnOp::V_MAX_U32: vg[ins.dst] = std::max(s0, s1); break;
                        case GcnOp::V_AND_B32: vg[ins.dst] = s0 & s1; break;
                        case GcnOp::V_OR_B32: vg[ins.dst] = s0 | s1; break;
                        case GcnOp::V_XOR_B32: vg[ins.dst] = s0 ^ s1; break;
                        case GcnOp::V_LSHLREV_B32: vg[ins.dst] = s1 << (s0 & 31); break;
                        case GcnOp::V_LSHRREV_B32: vg[ins.dst] = s1 >> (s0 & 31); break;
                        case GcnOp::V_ASHRREV_I32:
                            vg[ins.dst] = static_cast<uint32_t>(static_cast<int32_t>(s1) >> (s0 & 31)); break;
                        case GcnOp::V_MAC_F32:
                            vg[ins.dst] = FToBits(BitsToF(vg[ins.dst]) + BitsToF(s0) * BitsToF(s1)); break;
                        default: res.error = "unsupported VOP2 opcode"; return res;
                    }
                    break;
                }
                case GcnFormat::VOP3: {
                    assert(static_cast<size_t>(ins.dst) < GcnSwExecutor::kVgprCount);
                    if (static_cast<size_t>(ins.dst) >= GcnSwExecutor::kVgprCount) {
                        res.error = "VOP3 dst out of VGPR bounds";
                        return res;
                    }
                    uint32_t s0 = 0, s1 = 0, s2 = 0;
                    if (!read_src9(lane, ins.src0, s0, ins) ||
                        !read_src9(lane, ins.src1, s1, ins) ||
                        !read_src9(lane, ins.src2, s2, ins)) {
                        res.error = "bad VOP3 source";
                        return res;
                    }
                    switch (ins.opcode) {
                        case GcnOp::V_MAD_F32: {
                            const float a = apply_f32_mods(BitsToF(s0), ins, 0);
                            const float b = apply_f32_mods(BitsToF(s1), ins, 1);
                            const float c = apply_f32_mods(BitsToF(s2), ins, 2);
                            float r = a * b + c;
                            r = apply_omod(r, ins.omod);
                            vg[ins.dst] = FToBits(r);
                            break;
                        }
                        case GcnOp::V_FMA_F32: {
                            const float a = apply_f32_mods(BitsToF(s0), ins, 0);
                            const float b = apply_f32_mods(BitsToF(s1), ins, 1);
                            const float c = apply_f32_mods(BitsToF(s2), ins, 2);
                            vg[ins.dst] = FToBits(std::fma(a, b, c));
                            break;
                        }
                        case GcnOp::V_MAD_U32_U24: {
                            const uint32_t a = apply_int_mods(s0 & 0xFFFFFFu, ins, 0);
                            const uint32_t b = apply_int_mods(s1 & 0xFFFFFFu, ins, 1);
                            const uint32_t c = apply_int_mods(s2, ins, 2);
                            vg[ins.dst] = a * b + c;
                            break;
                        }
                        case GcnOp::V_MIN3_F32: {
                            float r = std::min(apply_f32_mods(BitsToF(s0), ins, 0),
                                               apply_f32_mods(BitsToF(s1), ins, 1));
                            r = std::min(r, apply_f32_mods(BitsToF(s2), ins, 2));
                            vg[ins.dst] = FToBits(r);
                            break;
                        }
                        case GcnOp::V_MAX3_F32: {
                            float r = std::max(apply_f32_mods(BitsToF(s0), ins, 0),
                                               apply_f32_mods(BitsToF(s1), ins, 1));
                            r = std::max(r, apply_f32_mods(BitsToF(s2), ins, 2));
                            vg[ins.dst] = FToBits(r);
                            break;
                        }
                        case GcnOp::V_MED3_I32: {
                            const int32_t a = static_cast<int32_t>(apply_int_mods(s0, ins, 0));
                            const int32_t b = static_cast<int32_t>(apply_int_mods(s1, ins, 1));
                            const int32_t c = static_cast<int32_t>(apply_int_mods(s2, ins, 2));
                            const int32_t lo = std::min(a, std::min(b, c));
                            const int32_t hi = std::max(a, std::max(b, c));
                            vg[ins.dst] = static_cast<uint32_t>(a + b + c - lo - hi);
                            break;
                        }
                        case GcnOp::V_MUL_LO_U32: vg[ins.dst] = s0 * s1; break;
                        case GcnOp::V_MUL_HI_U32: {
                            const uint64_t r = static_cast<uint64_t>(s0) * s1;
                            vg[ins.dst] = static_cast<uint32_t>(r >> 32);
                            break;
                        }
                        case GcnOp::V_BFE_U32: {
                            const uint32_t shift = s1 & 31;
                            const uint32_t width = s2 & 31;
                            const uint32_t mask = (width == 32) ? 0xFFFFFFFFu : ((1u << width) - 1u);
                            vg[ins.dst] = (s0 >> shift) & mask;
                            break;
                        }
                        case GcnOp::V_BCNT_U32_B32: vg[ins.dst] = static_cast<uint32_t>(__builtin_popcount(s0)); break;
                        case GcnOp::V_BFM_B32: {
                            const uint32_t width = s0 & 31;
                            const uint32_t off = s1 & 31;
                            vg[ins.dst] = (width == 32) ? 0xFFFFFFFFu : ((1u << width) - 1u) << off;
                            break;
                        }
                        case GcnOp::V_LDEXP_F32: {
                            const float a = apply_f32_mods(BitsToF(s0), ins, 0);
                            vg[ins.dst] = FToBits(std::ldexp(a, static_cast<int>(s1)));
                            break;
                        }
                        case GcnOp::V_LSHL_OR_B32:
                            vg[ins.dst] = (s0 << (s1 & 31)) | s2; break;
                        case GcnOp::V_AND_OR_B32:
                            vg[ins.dst] = (s0 & s1) | s2; break;
                        case GcnOp::V_ADD_CO_U32: {
                            const uint64_t r = static_cast<uint64_t>(s0) + s1;
                            vg[ins.dst] = static_cast<uint32_t>(r);
                            break;
                        }
                        case GcnOp::V_SUB_CO_U32: {
                            vg[ins.dst] = s0 - s1;
                            break;
                        }
                        // ---- Round 29: VINTRP (fragment interpolation) ----
                        // The three GFX10 VINTRP opcodes live in the VOP3
                        // opcode space (0xC0..0xC2). Research model (documented):
                        // parameter values come from the EXP param exports
                        // (attr*4+chan, lane-indexed; 0 when nothing was
                        // exported -- matching the "undefined but stable"
                        // hardware rule for unexported params).
                        case GcnOp::V_INTERP_MOV_F32: {
                            const uint32_t attr = s1 & 0x1Fu;
                            const uint32_t chan = s2 & 0x3u;
                            const auto& p = m_params[attr & 31u];
                            const size_t pi = lane * 4 + chan;
                            vg[ins.dst] = (pi < p.size()) ? p[pi] : 0u;
                            break;
                        }
                        case GcnOp::V_INTERP_P1_F32: {
                            // stage 1: dst = param[attr][chan] * i-coordinate (s0)
                            const uint32_t attr = s1 & 0x1Fu;
                            const uint32_t chan = s2 & 0x3u;
                            const auto& p = m_params[attr & 31u];
                            const size_t pi = lane * 4 + chan;
                            const uint32_t pv = (pi < p.size()) ? p[pi] : 0u;
                            float pf, vf, r;
                            std::memcpy(&pf, &pv, 4);
                            std::memcpy(&vf, &s0, 4);
                            r = pf * vf;
                            std::memcpy(&vg[ins.dst], &r, 4);
                            break;
                        }
                        case GcnOp::V_INTERP_P2_F32: {
                            // stage 2 (READ-MODIFY-WRITE per the ISA):
                            // dst = dst + param[attr][chan] * j-coordinate (s0)
                            const uint32_t attr = s1 & 0x1Fu;
                            const uint32_t chan = s2 & 0x3u;
                            const auto& p = m_params[attr & 31u];
                            const size_t pi = lane * 4 + chan;
                            const uint32_t pv = (pi < p.size()) ? p[pi] : 0u;
                            float pf, jf, acc, r;
                            std::memcpy(&pf, &pv, 4);
                            std::memcpy(&jf, &s0, 4);
                            std::memcpy(&acc, &vg[ins.dst], 4);
                            r = acc + pf * jf;
                            std::memcpy(&vg[ins.dst], &r, 4);
                            break;
                        }
                        default: res.error = "unsupported VOP3 opcode"; return res;
                    }
                    break;
                }
                case GcnFormat::VOPC: {
                    // VOPC writes to VCC, not to a VGPR dst, but defensive
                    // check on any VGPR source indices is done in read_src9.
                    uint32_t s0 = 0, s1 = 0;
                    if (!read_src9(lane, ins.src0, s0, ins) ||
                        !read_src8_vgpr(lane, static_cast<uint8_t>(ins.src1), s1)) {
                        res.error = "bad VOPC source";
                        return res;
                    }
                    bool r = false;
                    switch (ins.opcode) {
                        case GcnOp::V_CMP_LT_F32: r = BitsToF(s0) < BitsToF(s1); break;
                        case GcnOp::V_CMP_EQ_F32: r = BitsToF(s0) == BitsToF(s1); break;
                        case GcnOp::V_CMP_LE_F32: r = BitsToF(s0) <= BitsToF(s1); break;
                        case GcnOp::V_CMP_GT_F32: r = BitsToF(s0) > BitsToF(s1); break;
                        case GcnOp::V_CMP_LG_F32: r = BitsToF(s0) != BitsToF(s1); break;
                        case GcnOp::V_CMP_GE_F32: r = BitsToF(s0) >= BitsToF(s1); break;
                        case GcnOp::V_CMP_LT_I32:
                            r = static_cast<int32_t>(s0) < static_cast<int32_t>(s1); break;
                        case GcnOp::V_CMP_EQ_I32:
                            r = static_cast<int32_t>(s0) == static_cast<int32_t>(s1); break;
                        case GcnOp::V_CMP_LE_I32:
                            r = static_cast<int32_t>(s0) <= static_cast<int32_t>(s1); break;
                        case GcnOp::V_CMP_GT_I32:
                            r = static_cast<int32_t>(s0) > static_cast<int32_t>(s1); break;
                        case GcnOp::V_CMP_GE_I32:
                            r = static_cast<int32_t>(s0) >= static_cast<int32_t>(s1); break;
                        case GcnOp::V_CMP_LT_U32: r = s0 < s1; break;
                        case GcnOp::V_CMP_EQ_U32: r = s0 == s1; break;
                        case GcnOp::V_CMP_LE_U32: r = s0 <= s1; break;
                        case GcnOp::V_CMP_GT_U32: r = s0 > s1; break;
                        case GcnOp::V_CMP_GE_U32: r = s0 >= s1; break;
                        default:
                            res.error = "unsupported VOPC opcode";
                            return res;
                    }
                    m_vcc = r ? 1u : 0u;
                    break;
                }
                case GcnFormat::SMEM: {
                    if (mem == nullptr) {
                        res.error = "SMEM needs a memory bridge";
                        return res;
                    }
                    const int dwords_loaded =
                        ins.opcode == GcnOp::S_LOAD_DWORD ? 1 :
                        ins.opcode == GcnOp::S_LOAD_DWORDX2 ? 2 :
                        ins.opcode == GcnOp::S_LOAD_DWORDX4 ? 4 :
                        ins.opcode == GcnOp::S_LOAD_DWORDX8 ? 8 :
                        ins.opcode == GcnOp::S_BUFFER_LOAD_DWORD ? 1 :
                        ins.opcode == GcnOp::S_BUFFER_LOAD_DWORDX2 ? 2 :
                        ins.opcode == GcnOp::S_BUFFER_LOAD_DWORDX4 ? 4 : 0;
                    if (dwords_loaded == 0) {
                        res.error = "unsupported SMEM opcode";
                        return res;
                    }
                    if (ins.dst + static_cast<uint8_t>(dwords_loaded) >
                        GcnSwExecutor::kSgprCount) {
                        res.error = "SMEM sdst register pair out of range";
                        return res;
                    }
                    uint64_t addr = 0;
                    if (ins.opcode == GcnOp::S_BUFFER_LOAD_DWORD ||
                        ins.opcode == GcnOp::S_BUFFER_LOAD_DWORDX2 ||
                        ins.opcode == GcnOp::S_BUFFER_LOAD_DWORDX4) {
                        if (buffers == nullptr || ins.sbase < 4 ||
                            (ins.sbase / 4u) > buffers->size()) {
                            res.error = "SMEM buffer descriptor out of range";
                            return res;
                        }
                        const size_t desc_index = ins.sbase / 4u - 1u;
                        const auto& buf = (*buffers)[desc_index];
                        const uint64_t bytes = static_cast<uint64_t>(dwords_loaded) * 4u;
                        if (ins.smem_offset > buf.size_dwords * 4ull ||
                            bytes > static_cast<uint64_t>(buf.size_dwords) * 4ull - ins.smem_offset) {
                            res.error = "SMEM buffer load out of range";
                            return res;
                        }
                        addr = buf.base_gva + ins.smem_offset;
                    } else {
                        const uint64_t base_addr =
                            static_cast<uint64_t>(m_sgprs[ins.sbase]) |
                            (static_cast<uint64_t>(m_sgprs[ins.sbase + 1]) << 32);
                        addr = base_addr + ins.smem_offset;
                    }
                    uint32_t tmp[8] = {};
                    if (!mem->ReadDwords(addr, tmp, static_cast<size_t>(dwords_loaded))) {
                        res.error = "SMEM read out of range";
                        return res;
                    }
                    for (int i = 0; i < dwords_loaded; ++i) {
                        m_sgprs[ins.dst + static_cast<uint8_t>(i)] = tmp[i];
                    }
                    break;
                }
                case GcnFormat::DS: {
                    const uint32_t byte_addr = vg[ins.src0] + ins.ds_offset0;
                    const size_t idx = static_cast<size_t>(byte_addr >> 2);
                    auto check = [&](size_t words) { return idx < lds.size() && words <= lds.size() - idx; };
                    switch (ins.opcode) {
                        case GcnOp::DS_WRITE_B32:
                            if (!check(1)) { res.error = "DS write outside LDS"; return res; }
                            lds[idx] = vg[ins.src1];
                            break;
                        case GcnOp::DS_WRITE_B64:
                            if (!check(2)) { res.error = "DS write64 outside LDS"; return res; }
                            lds[idx] = vg[ins.src1]; lds[idx + 1] = vg[ins.src2];
                            break;
                        case GcnOp::DS_WRITE2_B32: {
                            const size_t i0 = idx;
                            const size_t i1 = static_cast<size_t>((vg[ins.src0] + ins.ds_offset1) >> 2);
                            if (i0 >= lds.size() || i1 >= lds.size()) { res.error = "DS write2 outside LDS"; return res; }
                            lds[i0] = vg[ins.src1]; lds[i1] = vg[ins.src2];
                            break;
                        }
                        case GcnOp::DS_READ_B32:
                            if (!check(1) || static_cast<size_t>(ins.dst) >= GcnSwExecutor::kVgprCount) { res.error = "DS read outside LDS"; return res; }
                            vg[ins.dst] = lds[idx];
                            break;
                        case GcnOp::DS_READ2_B32:
                            if (!check(1) || static_cast<size_t>(idx + (ins.ds_offset1 >> 2)) >= lds.size() ||
                                static_cast<size_t>(ins.dst) + 1u >= GcnSwExecutor::kVgprCount) { res.error = "DS read2 outside LDS"; return res; }
                            vg[ins.dst] = lds[idx];
                            vg[ins.dst + 1] = lds[idx + (ins.ds_offset1 >> 2)];
                            break;
                        case GcnOp::DS_READ_B64:
                            if (!check(2) || static_cast<size_t>(ins.dst) + 1u >= GcnSwExecutor::kVgprCount) { res.error = "DS read64 outside LDS"; return res; }
                            vg[ins.dst] = lds[idx]; vg[ins.dst + 1] = lds[idx + 1];
                            break;
                        case GcnOp::DS_ADD_U32:
                            if (!check(1)) { res.error = "DS add outside LDS"; return res; }
                            lds[idx] += vg[ins.src1];
                            break;
                        case GcnOp::DS_SUB_U32:
                            if (!check(1)) { res.error = "DS sub outside LDS"; return res; }
                            lds[idx] -= vg[ins.src1];
                            break;
                        case GcnOp::DS_INC_U32:
                            if (!check(1)) { res.error = "DS inc outside LDS"; return res; }
                            ++lds[idx];
                            break;
                        case GcnOp::DS_DEC_U32:
                            if (!check(1)) { res.error = "DS dec outside LDS"; return res; }
                            --lds[idx];
                            break;
                        case GcnOp::DS_MIN_U32:
                            if (!check(1)) { res.error = "DS min outside LDS"; return res; }
                            lds[idx] = std::min(lds[idx], vg[ins.src1]);
                            break;
                        case GcnOp::DS_MAX_U32:
                            if (!check(1)) { res.error = "DS max outside LDS"; return res; }
                            lds[idx] = std::max(lds[idx], vg[ins.src1]);
                            break;
                        case GcnOp::DS_AND_B32:
                            if (!check(1)) { res.error = "DS and outside LDS"; return res; }
                            lds[idx] &= vg[ins.src1];
                            break;
                        case GcnOp::DS_OR_B32:
                            if (!check(1)) { res.error = "DS or outside LDS"; return res; }
                            lds[idx] |= vg[ins.src1];
                            break;
                        case GcnOp::DS_XOR_B32:
                            if (!check(1)) { res.error = "DS xor outside LDS"; return res; }
                            lds[idx] ^= vg[ins.src1];
                            break;
                        // ---- Round 29: integer/float reductions + wavefront ops ----
                        case GcnOp::DS_MIN_I32: {
                            if (!check(1)) { res.error = "DS min_i outside LDS"; return res; }
                            lds[idx] = static_cast<uint32_t>(std::min(
                                static_cast<int32_t>(lds[idx]),
                                static_cast<int32_t>(vg[ins.src1])));
                            break;
                        }
                        case GcnOp::DS_MAX_I32: {
                            if (!check(1)) { res.error = "DS max_i outside LDS"; return res; }
                            lds[idx] = static_cast<uint32_t>(std::max(
                                static_cast<int32_t>(lds[idx]),
                                static_cast<int32_t>(vg[ins.src1])));
                            break;
                        }
                        case GcnOp::DS_MIN_F32: case GcnOp::DS_MAX_F32:
                        case GcnOp::DS_ADD_F32: {
                            if (!check(1)) { res.error = "DS fp outside LDS"; return res; }
                            float a, b, r = 0.0f;
                            std::memcpy(&a, &lds[idx], 4);
                            std::memcpy(&b, &vg[ins.src1], 4);
                            switch (ins.opcode) {
                                case GcnOp::DS_MIN_F32: r = std::min(a, b); break;
                                case GcnOp::DS_MAX_F32: r = std::max(a, b); break;
                                default: r = a + b; break;   // DS_ADD_F32
                            }
                            std::memcpy(&lds[idx], &r, 4);
                            break;
                        }
                        case GcnOp::DS_SWIZZLE_B32: {
                            // Lane-local swizzle: each lane indexes the LDS
                            // word at (own_addr + (pattern ^ lane_id)*4).
                            const uint32_t pattern = ins.ds_offset1;
                            const uint32_t delta = (pattern ^ static_cast<uint32_t>(lane)) & 0x1Fu;
                            const size_t sidx = idx + delta;
                            if (sidx >= lds.size() || static_cast<size_t>(ins.dst) >= GcnSwExecutor::kVgprCount) {
                                res.error = "DS swizzle outside LDS";
                                return res;
                            }
                            vg[ins.dst] = lds[sidx];
                            break;
                        }
                        case GcnOp::DS_BPERMUTE_B32: {
                            // WAVEFRONT op: lane i reads vgpr[src0] of the
                            // lane its selector points at. Hardware shuffles
                            // SIMULTANEOUSLY, so the source column is
                            // captured ONCE per instruction (keyed by pc,
                            // when the first lane arrives) and reused by
                            // every lane -- later lanes' reads must not see
                            // earlier lanes' dst writes (including the
                            // dst == src0 case, which hardware handles).
                            if (static_cast<size_t>(ins.dst) >= GcnSwExecutor::kVgprCount ||
                                lanes_total == 0) {
                                res.error = "DS bpermute outside wave";
                                return res;
                            }
                            auto& col = m_wave_columns[pc];
                            if (col.empty()) {
                                col.resize(lanes_total, 0);
                                for (size_t l = 0; l < lanes_total; ++l) {
                                    col[l] = m_vgprs[l * kVgprCount + ins.src0];
                                }
                            }
                            const uint32_t sel = (vg[ins.src0] + ins.ds_offset0) &
                                                 static_cast<uint32_t>(lanes_total - 1u);
                            vg[ins.dst] = col[sel];
                            break;
                        }
                        default:
                            res.error = std::string("unsupported DS opcode ") + GcnDecoder::Mnemonic(ins);
                            return res;
                    }
                    break;
                }
                case GcnFormat::MUBUF: {
                    if (mem == nullptr || buffers == nullptr) {
                        res.error = "MUBUF needs a memory bridge + buffer table";
                        return res;
                    }
                    const int dwords_loaded =
                        ins.opcode == GcnOp::BUFFER_LOAD_DWORD ? 1 :
                        ins.opcode == GcnOp::BUFFER_LOAD_DWORDX2 ? 2 :
                        ins.opcode == GcnOp::BUFFER_LOAD_DWORDX3 ? 3 :
                        ins.opcode == GcnOp::BUFFER_LOAD_DWORDX4 ? 4 : 0;
                    const bool is_store =
                        ins.opcode == GcnOp::BUFFER_STORE_DWORD ||
                        ins.opcode == GcnOp::BUFFER_STORE_DWORDX2 ||
                        ins.opcode == GcnOp::BUFFER_STORE_DWORDX4;
                    if (dwords_loaded == 0 && !is_store) {
                        res.error = "unsupported MUBUF opcode";
                        return res;
                    }
                    // Resource descriptor: in real hardware srsrc selects the
                    // SGPR quad (s[4..7], s[8..11], ...) holding the 4-dword
                    // buffer descriptor. The host-side table uses the same
                    // convention: descriptor i lives at s[4+4i..7+4i], so the
                    // table index is srsrc/4 - 1 (srsrc 4 -> buffer 0).
                    if (ins.srsrc < 4 || ins.srsrc / 4 > buffers->size()) {
                        res.error = "MUBUF descriptor out of range";
                        return res;
                    }
                    const size_t desc_index = ins.srsrc / 4 - 1;
                    const GcnBufferResource& buf = (*buffers)[desc_index];
                    uint64_t addr = buf.base_gva + ins.buf_offset;
                    if (ins.offen) {
                        addr += static_cast<uint64_t>(vg[ins.vaddr]) * 4ull;
                    } else if (ins.idxen) {
                        addr += static_cast<uint64_t>(vg[ins.vaddr]) *
                                static_cast<uint64_t>(buf.stride ? buf.stride : 1u) * 4ull;
                    }
                    const uint64_t byte_end = buf.base_gva + static_cast<uint64_t>(buf.size_dwords) * 4ull;
                    const uint64_t access_bytes = static_cast<uint64_t>(
                        is_store ? (ins.opcode == GcnOp::BUFFER_STORE_DWORD ? 4 :
                                    ins.opcode == GcnOp::BUFFER_STORE_DWORDX2 ? 8 : 16)
                                 : dwords_loaded * 4);
                    if (addr < buf.base_gva || addr > byte_end || access_bytes > byte_end - addr) {
                        res.error = "MUBUF access outside descriptor bounds";
                        return res;
                    }
                    if (is_store) {
                        const int store_count =
                            ins.opcode == GcnOp::BUFFER_STORE_DWORD ? 1 :
                            ins.opcode == GcnOp::BUFFER_STORE_DWORDX2 ? 2 : 4;
                        if (!mem->WriteDwords(addr, &vg[ins.vdata], static_cast<size_t>(store_count))) {
                            res.error = "MUBUF store out of range";
                            return res;
                        }
                    } else {
                        uint32_t tmp[4] = {};
                        if (!mem->ReadDwords(addr, tmp, static_cast<size_t>(dwords_loaded))) {
                            res.error = "MUBUF load out of range";
                            return res;
                        }
                        for (int i = 0; i < dwords_loaded; ++i) {
                            vg[ins.vdata + static_cast<uint8_t>(i)] = tmp[i];
                        }
                    }
                    break;
                }
                case GcnFormat::MIMG: {
                    // Resource descriptor quad: [idx, width, height, format].
                    // Round 28: the descriptor quad ALSO follows the compiler
                    // convention (image i lives at sgpr quad srsrc = 4*(i+1),
                    // quad[0] = i), so hardware and software agree on every
                    // program. Out-of-bounds LOAD/FETCH returns zero (hardware
                    // semantics; the pre-round-28 wrap is gone), SAMPLE is
                    // NEAREST + CLAMP_TO_EDGE (the exact sampler the Vulkan
                    // path binds), and single-mip LOD/bias/grad variants
                    // collapse onto the base sample -- matching a 1-mip
                    // VkImage where every LOD clamps to 0.
                    if (static_cast<size_t>(ins.srsrc) + 3 >= GcnSwExecutor::kSgprCount) {
                        res.error = "MIMG descriptor out of SGPR range";
                        return res;
                    }
                    const uint32_t img_idx = m_sgprs[ins.srsrc];
                    if (img_idx >= GcnSwExecutor::kMaxImages) {
                        res.error = "MIMG image index out of range";
                        return res;
                    }
                    const GcnSwExecutor::SwImage& img = m_images[img_idx];
                    const uint32_t w = img.width ? img.width : 1;
                    const uint32_t h = img.height ? img.height : 1;
                    // auto-address helper: read VGPR vaddr+k or 0.
                    const auto vgaddr = [&](uint32_t k) -> uint32_t {
                        const size_t idx = static_cast<size_t>(ins.vaddr) + k;
                        return idx < GcnSwExecutor::kVgprCount ? vg[idx] : 0;
                    };
                    // nearest sample from normalized (u, v) -- CLAMP_TO_EDGE.
                    const auto sample_nearest = [&](float u, float v,
                                                    uint32_t out[4]) -> void {
                        int sx = static_cast<int>(u * static_cast<float>(w));
                        int sy = static_cast<int>(v * static_cast<float>(h));
                        if (sx < 0) sx = 0;
                        if (sy < 0) sy = 0;
                        if (sx >= static_cast<int>(w)) sx = static_cast<int>(w) - 1;
                        if (sy >= static_cast<int>(h)) sy = static_cast<int>(h) - 1;
                        uint32_t r = 0, g = 0, b = 0, a = 0;
                        img.GetTexel(static_cast<uint32_t>(sx),
                                     static_cast<uint32_t>(sy), r, g, b, a);
                        out[0] = r; out[1] = g; out[2] = b; out[3] = a;
                    };
                    // scatter helper for dmask-selected components.
                    const auto scatter = [&](const uint32_t comps[4]) -> bool {
                        for (int c = 0; c < 4; ++c) {
                            if ((ins.dmask >> c) & 1u) {
                                if (static_cast<size_t>(ins.vdata) + c >=
                                    GcnSwExecutor::kVgprCount) {
                                    return false;
                                }
                                vg[ins.vdata + static_cast<uint8_t>(c)] = comps[c];
                            }
                        }
                        return true;
                    };
                    switch (ins.opcode) {
                        case GcnOp::IMAGE_LOAD:
                        case GcnOp::IMAGE_LOAD_MIP: {
                            const uint32_t x = vgaddr(0);
                            const uint32_t y = vgaddr(1);
                            uint32_t comps[4] = {0, 0, 0, 0};
                            if (x < w && y < h) {
                                img.GetTexel(x, y, comps[0], comps[1],
                                             comps[2], comps[3]);
                            }
                            if (!scatter(comps)) {
                                res.error = "IMAGE_LOAD vdata out of range";
                                return res;
                            }
                            break;
                        }
                        case GcnOp::IMAGE_STORE:
                        case GcnOp::IMAGE_STORE_MIP: {
                            const uint32_t x = vgaddr(0);
                            const uint32_t y = vgaddr(1);
                            if (x >= w || y >= h) {
                                res.error = "IMAGE_STORE texel out of range";
                                return res;
                            }
                            uint32_t comps[4] = {0, 0, 0, 0xFFFFFFFFu};
                            for (int c = 0; c < 4; ++c) {
                                if ((ins.dmask >> c) & 1u) {
                                    if (static_cast<size_t>(ins.vdata) + c >=
                                        GcnSwExecutor::kVgprCount) {
                                        res.error = "IMAGE_STORE vdata out of range";
                                        return res;
                                    }
                                    comps[c] = vg[ins.vdata + static_cast<uint8_t>(c)];
                                }
                            }
                            if (!m_images[img_idx].SetTexel(x, y, comps[0],
                                                            comps[1], comps[2],
                                                            comps[3])) {
                                res.error = "IMAGE_STORE failed";
                                return res;
                            }
                            break;
                        }
                        case GcnOp::IMAGE_SAMPLE:
                        case GcnOp::IMAGE_SAMPLE_L:
                        case GcnOp::IMAGE_SAMPLE_LZ:
                        case GcnOp::IMAGE_SAMPLE_B:
                        case GcnOp::IMAGE_SAMPLE_D:
                        case GcnOp::IMAGE_SAMPLE_CL:
                        case GcnOp::IMAGE_SAMPLE_DZ: {
                            // Single-mip nearest: LOD/bias/grad/LOD-clamp all
                            // collapse to the mip-0 texel (documented model,
                            // identical to a 1-mip VkImage + NEAREST sampler).
                            const float u = BitsToF(vgaddr(0));
                            const float v = BitsToF(vgaddr(1));
                            uint32_t comps[4] = {0, 0, 0, 0};
                            sample_nearest(u, v, comps);
                            if (!scatter(comps)) {
                                res.error = "IMAGE_SAMPLE vdata out of range";
                                return res;
                            }
                            break;
                        }
                        case GcnOp::IMAGE_SAMPLE_C:
                        case GcnOp::IMAGE_SAMPLE_C_LZ: {
                            // Depth-compare sample: dref from vaddr[2]; the
                            // compare passes when dref <= texel.r (LESS_EQUAL
                            // depth semantics), producing 1.0/0.0 broadcast to
                            // the dmask components (software model).
                            const float u = BitsToF(vgaddr(0));
                            const float v = BitsToF(vgaddr(1));
                            const float dref = BitsToF(vgaddr(2));
                            uint32_t comps[4] = {0, 0, 0, 0};
                            sample_nearest(u, v, comps);
                            const float ref = BitsToF(comps[0]);
                            const uint32_t result =
                                (dref <= ref) ? 0x3F800000u : 0u;   // 1.0f / 0.0f
                            uint32_t outc[4] = {result, result, result, result};
                            if (!scatter(outc)) {
                                res.error = "IMAGE_SAMPLE_C vdata out of range";
                                return res;
                            }
                            break;
                        }
                        case GcnOp::IMAGE_SAMPLE_O:
                        case GcnOp::IMAGE_SAMPLE_CD: {
                            // Offset/compare-with-derivative variants: the
                            // offset (integer texel shift) applies to the
                            // sampled coordinate before the clamp.
                            const float u = BitsToF(vgaddr(0));
                            const float v = BitsToF(vgaddr(1));
                            const int offx = static_cast<int>(vgaddr(2));
                            const int offy = static_cast<int>(vgaddr(3));
                            int sx = static_cast<int>(u * static_cast<float>(w)) + offx;
                            int sy = static_cast<int>(v * static_cast<float>(h)) + offy;
                            if (sx < 0) sx = 0;
                            if (sy < 0) sy = 0;
                            if (sx >= static_cast<int>(w)) sx = static_cast<int>(w) - 1;
                            if (sy >= static_cast<int>(h)) sy = static_cast<int>(h) - 1;
                            uint32_t comps[4] = {0, 0, 0, 0};
                            img.GetTexel(static_cast<uint32_t>(sx),
                                         static_cast<uint32_t>(sy), comps[0],
                                         comps[1], comps[2], comps[3]);
                            if (!scatter(comps)) {
                                res.error = "IMAGE_SAMPLE_O vdata out of range";
                                return res;
                            }
                            break;
                        }
                        case GcnOp::IMAGE_GATHER4:
                        case GcnOp::IMAGE_GATHER4_LZ: {
                            // Gather the component chosen by the lowest set
                            // dmask bit; the 2x2 footprint around the floored
                            // coordinate lands in vdata[0..3] (clamped edge,
                            // matching the Vulkan gather with CLAMP_TO_EDGE).
                            const float u = BitsToF(vgaddr(0));
                            const float v = BitsToF(vgaddr(1));
                            uint32_t component = 0;
                            for (uint32_t c = 0; c < 4; ++c) {
                                if ((ins.dmask >> c) & 1u) {
                                    component = c;
                                    break;
                                }
                            }
                            // The bilinear footprint: i0 = floor(u*w - 0.5)
                            // (the quad OpImageGather gathers), clamped.
                            const float fx = u * static_cast<float>(w) - 0.5f;
                            const float fy = v * static_cast<float>(h) - 0.5f;
                            int i0 = static_cast<int>(std::floor(fx));
                            int j0 = static_cast<int>(std::floor(fy));
                            if (i0 < 0) i0 = 0;
                            if (j0 < 0) j0 = 0;
                            if (i0 > static_cast<int>(w) - 1) i0 = static_cast<int>(w) - 1;
                            if (j0 > static_cast<int>(h) - 1) j0 = static_cast<int>(h) - 1;
                            int i1 = (i0 + 1 < static_cast<int>(w) ? i0 + 1 : static_cast<int>(w) - 1);
                            int j1 = (j0 + 1 < static_cast<int>(h) ? j0 + 1 : static_cast<int>(h) - 1);
                            // GLSL textureGather component order: the j1 row
                            // lands in x/y, the j0 row in z/w.
                            const int xs[4] = {i0, i1, i0, i1};
                            const int ys[4] = {j1, j1, j0, j0};
                            uint32_t comps[4] = {0, 0, 0, 0};
                            for (int i = 0; i < 4; ++i) {
                                uint32_t r = 0, g = 0, b = 0, a = 0;
                                img.GetTexel(static_cast<uint32_t>(xs[i]),
                                             static_cast<uint32_t>(ys[i]), r, g,
                                             b, a);
                                const uint32_t all[4] = {r, g, b, a};
                                comps[i] = all[component];
                            }
                            // gather4 writes ALL FOUR results (dmask only picks
                            // the component).
                            for (int c = 0; c < 4; ++c) {
                                if (static_cast<size_t>(ins.vdata) + c >=
                                    GcnSwExecutor::kVgprCount) {
                                    res.error = "IMAGE_GATHER4 vdata out of range";
                                    return res;
                                }
                                vg[ins.vdata + static_cast<uint8_t>(c)] = comps[c];
                            }
                            break;
                        }
                        case GcnOp::IMAGE_GET_RESINFO: {
                            if (static_cast<size_t>(ins.vdata) + 3 >= GcnSwExecutor::kVgprCount) {
                                res.error = "IMAGE_GET_RESINFO vdata out of range";
                                return res;
                            }
                            vg[ins.vdata] = img.width;
                            vg[ins.vdata + 1] = img.height;
                            vg[ins.vdata + 2] = 1;              // depth
                            vg[ins.vdata + 3] = img.mips;        // mip levels
                            break;
                        }
                        case GcnOp::IMAGE_GET_LOD: {
                            // Compute-shader GET_LOD: single-mip model -> both
                            // LODs report 0. (The hardware path rejects this
                            // opcode as fragment-only.)
                            if (static_cast<size_t>(ins.vdata) + 1 >= GcnSwExecutor::kVgprCount) {
                                res.error = "IMAGE_GET_LOD vdata out of range";
                                return res;
                            }
                            vg[ins.vdata] = 0;
                            vg[ins.vdata + 1] = 0;
                            break;
                        }
                        case GcnOp::IMAGE_ATOMIC_SWAP:
                        case GcnOp::IMAGE_ATOMIC_CMPSWAP:
                        case GcnOp::IMAGE_ATOMIC_ADD:
                        case GcnOp::IMAGE_ATOMIC_SUB:
                        case GcnOp::IMAGE_ATOMIC_SMIN:
                        case GcnOp::IMAGE_ATOMIC_UMIN:
                        case GcnOp::IMAGE_ATOMIC_SMAX:
                        case GcnOp::IMAGE_ATOMIC_UMAX:
                        case GcnOp::IMAGE_ATOMIC_AND:
                        case GcnOp::IMAGE_ATOMIC_OR:
                        case GcnOp::IMAGE_ATOMIC_XOR:
                        case GcnOp::IMAGE_ATOMIC_INC:
                        case GcnOp::IMAGE_ATOMIC_DEC: {
                            const uint32_t x = vgaddr(0);
                            const uint32_t y = vgaddr(1);
                            if (x >= w || y >= h) {
                                res.error = "IMAGE_ATOMIC texel out of range";
                                return res;
                            }
                            if (static_cast<size_t>(ins.vdata) + 1 >=
                                GcnSwExecutor::kVgprCount) {
                                res.error = "IMAGE_ATOMIC vdata out of range";
                                return res;
                            }
                            GcnSwExecutor::SwImage& dst = m_images[img_idx];
                            uint32_t r = 0, g = 0, b = 0, a = 0;
                            dst.GetTexel(x, y, r, g, b, a);
                            const uint32_t src = vg[ins.vdata];
                            uint32_t old = r;
                            uint32_t result = r;
                            switch (ins.opcode) {
                                case GcnOp::IMAGE_ATOMIC_SWAP:    result = src; break;
                                case GcnOp::IMAGE_ATOMIC_ADD:     result = old + src; break;
                                case GcnOp::IMAGE_ATOMIC_SUB:     result = old - src; break;
                                case GcnOp::IMAGE_ATOMIC_SMIN:
                                    result = static_cast<uint32_t>(
                                        std::min(static_cast<int32_t>(old),
                                                 static_cast<int32_t>(src)));
                                    break;
                                case GcnOp::IMAGE_ATOMIC_UMIN:
                                    result = std::min(old, src);
                                    break;
                                case GcnOp::IMAGE_ATOMIC_SMAX:
                                    result = static_cast<uint32_t>(
                                        std::max(static_cast<int32_t>(old),
                                                 static_cast<int32_t>(src)));
                                    break;
                                case GcnOp::IMAGE_ATOMIC_UMAX:
                                    result = std::max(old, src);
                                    break;
                                case GcnOp::IMAGE_ATOMIC_AND:     result = old & src; break;
                                case GcnOp::IMAGE_ATOMIC_OR:      result = old | src; break;
                                case GcnOp::IMAGE_ATOMIC_XOR:     result = old ^ src; break;
                                case GcnOp::IMAGE_ATOMIC_INC:     result = old + 1u; break;
                                case GcnOp::IMAGE_ATOMIC_DEC:     result = old - 1u; break;
                                case GcnOp::IMAGE_ATOMIC_CMPSWAP: {
                                    const uint32_t cmp = vg[ins.vdata + 1];
                                    result = (old == cmp) ? src : old;
                                    break;
                                }
                                default: result = old; break;
                            }
                            if (!dst.SetTexel(x, y, result, g, b, a)) {
                                res.error = "IMAGE_ATOMIC store failed";
                                return res;
                            }
                            vg[ins.vdata] = old;   // the OLD value is returned
                            break;
                        }
                        default:
                            res.error = std::string("unsupported MIMG opcode ") + GcnDecoder::Mnemonic(ins);
                            return res;
                    }
                    break;
                }
                case GcnFormat::VOP3P: {
                    // Round 29: v_mad_mix_f32 -- mixed-precision FMA. Each
                    // 32-bit source packs two f16 values; the per-source
                    // opsel bit selects which half feeds the f32 math:
                    // dst = f32(a_half * b_half) + c_half.
                    if (ins.opcode != GcnOp::V_MAD_MIX_F32) {
                        res.error = "unsupported VOP3P opcode";
                        return res;
                    }
                    if (static_cast<size_t>(ins.dst) >= GcnSwExecutor::kVgprCount) {
                        res.error = "VOP3P dst out of range";
                        return res;
                    }
                    uint32_t sa = 0, sb = 0, sc = 0;
                    if (!read_src9(lane, ins.src0, sa, ins) ||
                        !read_src9(lane, ins.src1, sb, ins) ||
                        !read_src9(lane, ins.src2, sc, ins)) {
                        res.error = "VOP3P src";
                        return res;
                    }
                    auto pick_half = [&](uint32_t v, bool hi) -> float {
                        const uint32_t bits = hi ? (v >> 16) : (v & 0xFFFFu);
                        const uint32_t sign = (bits >> 15) & 1u;
                        const uint32_t exp = (bits >> 10) & 0x1Fu;
                        const uint32_t frac = bits & 0x3FFu;
                        uint32_t out = 0;
                        if (exp == 0) {
                            if (frac == 0) {
                                out = sign << 31;
                            } else {
                                // f16 subnormal: value = frac * 2^-24.
                                // Normalize to 1.x: the biased f32 exponent
                                // is 113 - shifts (verified: 0x0001 -> 2^-24).
                                int shifts = 0;
                                uint32_t m = frac;
                                while ((m & 0x400u) == 0) { m <<= 1; ++shifts; }
                                m &= 0x3FFu;
                                out = (sign << 31) |
                                      (static_cast<uint32_t>(113 - shifts) << 23) |
                                      (m << 13);
                            }
                        } else if (exp == 0x1F) {
                            out = (sign << 31) | 0x7F800000u | (frac << 13);
                        } else {
                            out = (sign << 31) | ((exp - 15 + 127) << 23) | (frac << 13);
                        }
                        float f;
                        std::memcpy(&f, &out, 4);
                        return f;
                    };
                    const float a = pick_half(sa, (ins.opsel & 1u) != 0u);
                    const float b = pick_half(sb, (ins.opsel & 2u) != 0u);
                    const float c = pick_half(sc, (ins.opsel & 4u) != 0u);
                    float r = a * b + c;
                    if (ins.omod == 1) r *= 2.0f;
                    else if (ins.omod == 2) r *= 4.0f;
                    else if (ins.omod == 3) r *= 0.5f;
                    if (ins.clamp) r = std::min(1.0f, std::max(0.0f, r));
                    std::memcpy(&vg[ins.dst], &r, 4);
                    break;
                }
                case GcnFormat::EXP: {
                    // Per-lane export: keep the last value per target.
                    const uint32_t comps[4] = {vg[ins.vsrc0], vg[ins.vsrc1], vg[ins.vsrc2], vg[ins.vsrc3]};
                    if (ins.exp_target < 8) {
                        auto& mrt = m_mrt[ins.exp_target];
                        mrt.resize(lanes_total * 4, 0);
                        for (int c = 0; c < 4; ++c) {
                            if ((ins.exp_en >> c) & 1u) {
                                mrt[lane * 4 + static_cast<size_t>(c)] = comps[c];
                            }
                        }
                    } else if (ins.exp_target >= 32) {
                        const uint32_t slot = ins.exp_target - 32;
                        auto& prm = m_params[slot & 31];
                        prm.resize(lanes_total * 4, 0);
                        for (int c = 0; c < 4; ++c) {
                            if ((ins.exp_en >> c) & 1u) {
                                prm[lane * 4 + static_cast<size_t>(c)] = comps[c];
                            }
                        }
                    }
                    if (ins.exp_done) {
                        // the final export of the shader epilogue
                        lane_done = true;
                        terminated = true;
                    }
                    break;
                }
                case GcnFormat::FLAT: {
                    if (mem == nullptr) {
                        res.error = "FLAT needs a memory bridge";
                        return res;
                    }
                    // flat address = vgpr[vaddr] (+ immediate offset)
                    const uint64_t addr =
                        static_cast<uint64_t>(vg[ins.vaddr]) + ins.flat_offset;
                    const int loads =
                        ins.opcode == GcnOp::FLAT_LOAD_DWORD ? 1 :
                        ins.opcode == GcnOp::FLAT_LOAD_DWORDX2 ? 2 :
                        ins.opcode == GcnOp::FLAT_LOAD_DWORDX3 ? 3 :
                        ins.opcode == GcnOp::FLAT_LOAD_DWORDX4 ? 4 : 0;
                    const bool is_store =
                        ins.opcode == GcnOp::FLAT_STORE_DWORD ||
                        ins.opcode == GcnOp::FLAT_STORE_DWORDX2 ||
                        ins.opcode == GcnOp::FLAT_STORE_DWORDX3 ||
                        ins.opcode == GcnOp::FLAT_STORE_DWORDX4;
                    if (loads > 0) {
                        uint32_t tmp[4] = {};
                        if (!mem->ReadDwords(addr, tmp, static_cast<size_t>(loads))) {
                            res.error = "FLAT load out of range";
                            return res;
                        }
                        for (int i = 0; i < loads; ++i) {
                            if (static_cast<size_t>(ins.vdata) + static_cast<size_t>(i) >= GcnSwExecutor::kVgprCount) {
                                res.error = "FLAT vdata out of range";
                                return res;
                            }
                            vg[ins.vdata + static_cast<uint8_t>(i)] = tmp[i];
                        }
                    } else if (is_store) {
                        const int stores =
                            ins.opcode == GcnOp::FLAT_STORE_DWORD ? 1 :
                            ins.opcode == GcnOp::FLAT_STORE_DWORDX2 ? 2 :
                            ins.opcode == GcnOp::FLAT_STORE_DWORDX3 ? 3 : 4;
                        if (!mem->WriteDwords(addr, &vg[ins.vdata], static_cast<size_t>(stores))) {
                            res.error = "FLAT store out of range";
                            return res;
                        }
                    } else if (ins.opcode == GcnOp::FLAT_ATOMIC_ADD) {
                        uint32_t old = 0;
                        if (!mem->ReadDwords(addr, &old, 1)) {
                            res.error = "FLAT atomic out of range";
                            return res;
                        }
                        const uint32_t nv = old + vg[ins.vdata];
                        if (!mem->WriteDwords(addr, &nv, 1)) {
                            res.error = "FLAT atomic store out of range";
                            return res;
                        }
                        if (ins.dst != 0x7F) {          // RTN form writes the old value
                            if (static_cast<size_t>(ins.dst) >= GcnSwExecutor::kVgprCount) {
                                res.error = "FLAT atomic dst out of range";
                                return res;
                            }
                            vg[ins.dst] = old;
                        }
                    } else if (ins.opcode == GcnOp::FLAT_ATOMIC_SWAP) {
                        uint32_t nv = vg[ins.vdata];
                        if (!mem->WriteDwords(addr, &nv, 1)) {
                            res.error = "FLAT atomic swap out of range";
                            return res;
                        }
                    } else {
                        res.error = std::string("unsupported FLAT opcode ") + GcnDecoder::Mnemonic(ins);
                        return res;
                    }
                    break;
                }
                default:
                    res.error = std::string("unhandled format ") + ToString(ins.format);
                    return res;
            }
            pc += ins.dwords_consumed;
        }
        total_exec += executed;
        ++res.lanes_run;
    }

    // Harvest v0..v{m-1} per lane.
    for (size_t lane = 0; lane < lanes; ++lane) {
        for (uint32_t j = 0; j < m_out && j < kVgprCount; ++j) {
            output[static_cast<size_t>(lane) * m_out + j] =
                m_vgprs[static_cast<size_t>(lane) * kVgprCount + j];
        }
    }

    res.ok = true;
    res.instructions_executed = total_exec;
    res.terminated = terminated;
    return res;
}

} // namespace PS5::GPU
