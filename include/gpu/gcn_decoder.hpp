#pragma once
// ============================================================================
// ProsperoLayer RDNA2 Core - Real GCN/GFX10 instruction decoder (round 12)
// ----------------------------------------------------------------------------
// Decodes the ACTUAL AMD RDNA2 (GFX10.3) instruction encodings -- the same
// bit layouts LLVM's AMDGPU target and the hardware consume -- not a
// simplified model. All opcode numbers and field positions were extracted
// from llvm/lib/Target/AMDGPU/{VOP1,VOP2,VOP3,VOPC,SOP,SM,BUF,DS}Instructions.td
// (GFX10 classes) and are cited per-format below.
//
// Format identification (first dword, top bits):
//   SOPP  (inst & 0xFF800000) == 0xBF800000   op[22:16] simm16[15:0]
//   SOP1  (inst & 0xFF800000) == 0x7D800000   op[22:16] sdst[15:9] ssrc0[8:0]
//   SOPC  (inst & 0xFF800000) == 0x7E800000   op[22:16] ssrc1[15:9] ssrc0[8:0]
//   SOPK  (inst & 0xF8000000) == 0xB0000000   op[26:20] sdst[19:16] simm16[15:0]
//   SOP2  (inst & 0xE0000000) == 0x80000000   op[28:23] sdst[22:16] ssrc1[15:9] ssrc0[8:0]
//   VOP1  (inst & 0xFE000000) == 0x7E000000   op[24:17] vdst[16:9]  src0[8:0]
//   VOPC  (inst & 0xFE000000) == 0x7C000000   op[24:17] src1[16:9]  src0[8:0]
//   VOP2  (inst & 0x80000000) == 0x00000000   op[30:25] vdst[24:17] src1[16:9] src0[8:0]
//   VOP3  (inst & 0xFC000000) == 0xD4000000   op[25:16] vdst[7:0] (2 dwords;
//         dword1: src0[40:32] src1[49:41] src2[58:50] omod[60:59] neg[61..63]
//         abs[8..10] in dword0, clamp[15] in dword0)
//   SMEM  (inst & 0xFC000000) == 0xC0000000   op[25:18] sbase[5:0] sdst[12:6]
//         (64-bit: offset[52:32], soffset[63:57], imm[17], glc[16])
//   MUBUF (inst & 0xFC000000) == 0xE0000000   op[25:18] (2 dwords;
//         dword0: offset[11:0] slc[12] dlc[13] glc[14] offen[54] idxen[55]
//         tfe[53]; dword1: vaddr[39:32] vdata[47:40] srsrc[52:48] soffset[63:56])
//   MTBUF (inst & 0xFC000000) == 0xE8000000   (same shape as MUBUF)
//   DS    (inst & 0xFC000000) == 0xD8000000   op[25:18] (64-bit:
//         offset0[7:0] offset1[15:8] gds[17] addr[39:32] data0[47:40]
//         data1[55:48] vdst[63:56])
//
// Literal constants: a VOP1/VOP2/VOPC src0 == 0xFF means the NEXT dword is a
// 32-bit literal (consumed as part of the instruction). VOP3 has no literal
// form (its sources are 9-bit encodings in dword 1). SOP ssrc encodings below
// 102 address SGPRs / special scalar registers; 128+ are inline constants.
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "gpu/gpu_guest_memory.hpp"  // GpuGuestMemory (software executor)

namespace PS5::GPU {

enum class GcnFormat {
    SOP1, SOP2, SOPK, SOPC, SOPP,
    VOP1, VOP2, VOPC, VOP3,
    SMEM, MUBUF, MTBUF, DS,
    MIMG,   // image load/store/sample (0xF0..0xF7 prefix)
    EXP,    // export to MRT / param (0xF8..0xFB prefix)
    FLAT,   // flat/global memory ops (0xDC prefix)
    VOP3P,  // packed math (0xD3 prefix) -- round 29
    UNKNOWN,
};

const char* ToString(GcnFormat f);

// Scalar source-encoding special registers (the 9-bit ssrc fields).
constexpr uint16_t GCN_SSRC_SGPR_MAX   = 101;  // 0..101 -> s0..s101
constexpr uint16_t GCN_SSRC_VCC_LO     = 106;  // vcc (lo) -- 106 per GFX10
constexpr uint16_t GCN_SSRC_VCC_HI     = 107;
constexpr uint16_t GCN_SSRC_M0         = 108;
constexpr uint16_t GCN_SSRC_EXEC_LO    = 110;
constexpr uint16_t GCN_SSRC_EXEC_HI    = 111;
constexpr uint16_t GCN_SSRC_LITERAL    = 255;  // only valid for ssrc0 of SOP2/SOP1/SOPC
constexpr uint16_t GCN_SSRC_FLOAT_CONST_BASE = 240;  // 240..247: +-.5/1/2/4

// The 32-bit inline-constant decode shared by VOP/SOP source fields.
// Returns true when `source` encodes an inline constant; `bits` receives the
// raw 32-bit value (reinterpreted as float by the consumer when needed).
bool GcnDecodeInlineConstant32(uint16_t source, uint32_t& bits);

// GFX10 opcode numbers (LLVM-verified). Kept as plain constants: the decoder
// stays table-free and the software executor switches on these.
namespace GcnOp {
// ---- SOPP (7-bit) ----
constexpr uint32_t S_NOP = 0x000, S_ENDPGM = 0x001, S_BRANCH = 0x002,
    S_CBRANCH_SCC0 = 0x004, S_CBRANCH_SCC1 = 0x005, S_CBRANCH_VCCZ = 0x006,
    S_CBRANCH_VCCNZ = 0x007, S_CBRANCH_EXECZ = 0x008, S_CBRANCH_EXECNZ = 0x009,
    S_BARRIER = 0x00a, S_WAITCNT = 0x00c;
// ---- SOP1 (7-bit) ----
constexpr uint32_t S_MOV_B32 = 0x003, S_MOV_B64 = 0x004, S_CMOV_B32 = 0x005,
    S_NOT_B32 = 0x007, S_NOT_B64 = 0x008, S_WQM_B32 = 0x009, S_BREV_B32 = 0x00b,
    S_BCNT0_I32_B32 = 0x00d, S_BCNT1_I32_B32 = 0x00f, S_FF0_I32_B32 = 0x011,
    S_FF1_I32_B32 = 0x013, S_FLBIT_I32_B32 = 0x015, S_SEXT_I32_I8 = 0x019,
    S_SEXT_I32_I16 = 0x01a, S_AND_SAVEEXEC_B64 = 0x024, S_ABS_I32 = 0x034;
// ---- SOP2 (6-bit) ----
constexpr uint32_t S_ADD_U32 = 0x000, S_SUB_U32 = 0x001, S_ADD_I32 = 0x002,
    S_SUB_I32 = 0x003, S_ADDC_U32 = 0x004, S_SUBB_U32 = 0x005, S_MIN_I32 = 0x006,
    S_MIN_U32 = 0x007, S_MAX_I32 = 0x008, S_MAX_U32 = 0x009, S_CSELECT_B32 = 0x00a,
    S_AND_B32 = 0x00e, S_AND_B64 = 0x00f, S_OR_B32 = 0x010, S_OR_B64 = 0x011,
    S_XOR_B32 = 0x012, S_XOR_B64 = 0x013, S_ANDN2_B32 = 0x014, S_ORN2_B32 = 0x016,
    S_NAND_B32 = 0x018, S_NOR_B32 = 0x01a, S_XNOR_B32 = 0x01c, S_LSHL_B32 = 0x01e,
    S_LSHR_B32 = 0x020, S_ASHR_I32 = 0x022, S_BFM_B32 = 0x024, S_MUL_I32 = 0x026,
    S_BFE_U32 = 0x027, S_BFE_I32 = 0x028, S_ABSDIFF_I32 = 0x02c,
    S_MUL_HI_U32 = 0x035, S_MUL_HI_I32 = 0x036;
// ---- SOPK (7-bit) ----
constexpr uint32_t S_MOVK_I32 = 0x000, S_CMOVK_I32 = 0x002, S_CMPK_EQ_I32 = 0x003,
    S_CMPK_LG_I32 = 0x004, S_CMPK_GT_I32 = 0x005, S_CMPK_LT_I32 = 0x007,
    S_CMPK_EQ_U32 = 0x009, S_CMPK_LG_U32 = 0x00a, S_CMPK_GT_U32 = 0x00b,
    S_CMPK_LT_U32 = 0x00d, S_ADDK_I32 = 0x00f, S_MULK_I32 = 0x010;
// ---- SOPC (7-bit) ----
constexpr uint32_t S_CMP_EQ_I32 = 0x000, S_CMP_LG_I32 = 0x001, S_CMP_GT_I32 = 0x002,
    S_CMP_GE_I32 = 0x003, S_CMP_LT_I32 = 0x004, S_CMP_LE_I32 = 0x005,
    S_CMP_EQ_U32 = 0x006, S_CMP_LG_U32 = 0x007, S_CMP_GT_U32 = 0x008,
    S_CMP_GE_U32 = 0x009, S_CMP_LT_U32 = 0x00a, S_CMP_LE_U32 = 0x00b;
// ---- VOP1 (8-bit, GFX10) ----
constexpr uint32_t V_NOP = 0x000, V_MOV_B32 = 0x001, V_READFIRSTLANE_B32 = 0x002,
    V_CVT_F32_I32 = 0x005, V_CVT_F32_U32 = 0x006, V_CVT_U32_F32 = 0x007,
    V_CVT_I32_F32 = 0x008, V_CVT_F16_F32 = 0x00a, V_CVT_F32_F16 = 0x00b,
    V_CVT_RPI_I32_F32 = 0x00c, V_CVT_FLR_I32_F32 = 0x00d, V_CVT_F32_F64 = 0x00f,
    V_CVT_F64_F32 = 0x010, V_FRACT_F32 = 0x020, V_TRUNC_F32 = 0x021,
    V_CEIL_F32 = 0x022, V_RNDNE_F32 = 0x023, V_FLOOR_F32 = 0x024,
    V_EXP_F32 = 0x025, V_LOG_F32 = 0x027, V_RCP_F32 = 0x02a,
    V_RCP_IFLAG_F32 = 0x02b, V_RSQ_F32 = 0x02e, V_SQRT_F32 = 0x033,
    V_SIN_F32 = 0x035, V_COS_F32 = 0x036, V_NOT_B32 = 0x037, V_BFREV_B32 = 0x038,
    V_FFBH_U32 = 0x039, V_FFBL_B32 = 0x03a, V_FFBH_I32 = 0x03b,
    V_FREXP_EXP_I32_F32 = 0x03f, V_FREXP_MANT_F32 = 0x040, V_SWAP_B32 = 0x065;
// ---- VOP2 (6-bit, GFX10) ----
constexpr uint32_t V_CNDMASK_B32 = 0x000, V_ADD_F32 = 0x003, V_SUB_F32 = 0x004,
    V_SUBREV_F32 = 0x005, V_MUL_LEGACY_F32 = 0x007, V_MUL_F32 = 0x008,
    V_MUL_I32_I24 = 0x009, V_MUL_HI_I32_I24 = 0x00a, V_MUL_U32_U24 = 0x00b,
    V_MUL_HI_U32_U24 = 0x00c, V_MIN_F32 = 0x00f, V_MAX_F32 = 0x010,
    V_MIN_I32 = 0x011, V_MAX_I32 = 0x012, V_MIN_U32 = 0x013, V_MAX_U32 = 0x014,
    V_LSHRREV_B32 = 0x016, V_ASHRREV_I32 = 0x018, V_LSHLREV_B32 = 0x01a,
    V_AND_B32 = 0x01b, V_OR_B32 = 0x01c, V_XOR_B32 = 0x01d, V_XNOR_B32 = 0x01e,
    V_MAC_F32 = 0x01f, V_MADMK_F32 = 0x020, V_MADAK_F32 = 0x021,
    V_FMAC_F32 = 0x02b, V_FMAMK_F32 = 0x02c, V_FMAAK_F32 = 0x02d,
    V_CVT_PKRTZ_F16_F32 = 0x02f;
// ---- VOP3 (10-bit, GFX10) ----
constexpr uint32_t V_MAD_F32 = 0x141, V_MAD_U32_U24 = 0x143, V_FMA_F32 = 0x14b,
    V_BFE_U32 = 0x148, V_MIN3_F32 = 0x151, V_MAX3_F32 = 0x154, V_MED3_I32 = 0x158,
    V_MUL_LO_U32 = 0x169, V_MUL_HI_U32 = 0x16a, V_LDEXP_F32 = 0x362,
    V_BFM_B32 = 0x363, V_BCNT_U32_B32 = 0x364, V_MBCNT_LO_U32_B32 = 0x365,
    V_MBCNT_HI_U32_B32 = 0x366, V_LSHL_OR_B32 = 0x36f, V_AND_OR_B32 = 0x371,
    V_ADD_CO_U32 = 0x30f, V_SUB_CO_U32 = 0x310, V_SUBREV_CO_U32 = 0x319;
// ---- VOPC (8-bit, GFX10; llvm VOPC_Real_gfx6_gfx7_gfx10) ----
constexpr uint32_t V_CMP_LT_F32 = 0x001, V_CMP_EQ_F32 = 0x002,
    V_CMP_LE_F32 = 0x003, V_CMP_GT_F32 = 0x004, V_CMP_LG_F32 = 0x005,
    V_CMP_GE_F32 = 0x006;
constexpr uint32_t V_CMP_LT_I32 = 0x081, V_CMP_EQ_I32 = 0x082,
    V_CMP_LE_I32 = 0x083, V_CMP_GT_I32 = 0x084, V_CMP_GE_I32 = 0x086;
constexpr uint32_t V_CMP_LT_U32 = 0x0c1, V_CMP_EQ_U32 = 0x0c2,
    V_CMP_LE_U32 = 0x0c3, V_CMP_GT_U32 = 0x0c4, V_CMP_GE_U32 = 0x0c6;
// ---- SMEM (8-bit, GFX10) ----
constexpr uint32_t S_LOAD_DWORD = 0x000, S_LOAD_DWORDX2 = 0x001,
    S_LOAD_DWORDX4 = 0x002, S_LOAD_DWORDX8 = 0x003,
    S_BUFFER_LOAD_DWORD = 0x008, S_BUFFER_LOAD_DWORDX2 = 0x009,
    S_BUFFER_LOAD_DWORDX4 = 0x00a, S_BUFFER_STORE_DWORD = 0x018;
// ---- MUBUF (8-bit, GFX10) ----
constexpr uint32_t BUFFER_LOAD_FORMAT_X = 0x000, BUFFER_LOAD_FORMAT_XYZW = 0x003,
    BUFFER_STORE_FORMAT_XYZW = 0x007, BUFFER_LOAD_UBYTE = 0x008,
    BUFFER_LOAD_SBYTE = 0x009, BUFFER_LOAD_USHORT = 0x00a,
    BUFFER_LOAD_SSHORT = 0x00b, BUFFER_LOAD_DWORD = 0x00c,
    BUFFER_LOAD_DWORDX2 = 0x00d, BUFFER_LOAD_DWORDX3 = 0x00f,
    BUFFER_LOAD_DWORDX4 = 0x00e, BUFFER_STORE_BYTE = 0x018,
    BUFFER_STORE_SHORT = 0x01a, BUFFER_STORE_DWORD = 0x01c,
    BUFFER_STORE_DWORDX2 = 0x01d, BUFFER_STORE_DWORDX4 = 0x01e;
// ---- DS (8-bit, GFX10) ----
constexpr uint32_t DS_ADD_U32 = 0x000, DS_SUB_U32 = 0x001, DS_INC_U32 = 0x003,
    DS_DEC_U32 = 0x004, DS_MIN_I32 = 0x005, DS_MAX_I32 = 0x006,
    DS_MIN_U32 = 0x007, DS_MAX_U32 = 0x008,
    DS_AND_B32 = 0x009, DS_OR_B32 = 0x00a, DS_XOR_B32 = 0x00b,
    DS_WRITE_B32 = 0x00d, DS_WRITE2_B32 = 0x00e, DS_MIN_F32 = 0x012,
    DS_MAX_F32 = 0x013, DS_ADD_F32 = 0x015, DS_SWIZZLE_B32 = 0x035,
    DS_READ_B32 = 0x036, DS_READ2_B32 = 0x037, DS_READ_B64 = 0x076,
    DS_WRITE_B64 = 0x04d,
    // Round 29: wavefront lane-shuffle primitives (GFX8+ numbering,
    // present in GFX10/RDNA2).
    DS_PERMUTE_B32 = 0x36c, DS_BPERMUTE_B32 = 0x36d;

// ---- MIMG (8-bit) ----
constexpr uint32_t IMAGE_LOAD              = 0x000;
constexpr uint32_t IMAGE_LOAD_MIP          = 0x001;
constexpr uint32_t IMAGE_LOAD_1            = 0x002;
constexpr uint32_t IMAGE_LOAD_2            = 0x003;
constexpr uint32_t IMAGE_LOAD_3            = 0x004;
constexpr uint32_t IMAGE_LOAD_SIGNED       = 0x005;
constexpr uint32_t IMAGE_LOAD_MIP_SIGNED   = 0x006;
constexpr uint32_t IMAGE_GET_LOD           = 0x00c;
constexpr uint32_t IMAGE_SAMPLE            = 0x010;
constexpr uint32_t IMAGE_SAMPLE_D          = 0x011;
constexpr uint32_t IMAGE_SAMPLE_L          = 0x014;
constexpr uint32_t IMAGE_SAMPLE_B          = 0x018;
constexpr uint32_t IMAGE_SAMPLE_DZ         = 0x019;
constexpr uint32_t IMAGE_SAMPLE_LZ         = 0x01c;
constexpr uint32_t IMAGE_SAMPLE_CD         = 0x030;
constexpr uint32_t IMAGE_SAMPLE_C          = 0x050;
constexpr uint32_t IMAGE_SAMPLE_C_D        = 0x051;
constexpr uint32_t IMAGE_SAMPLE_C_L        = 0x054;
constexpr uint32_t IMAGE_SAMPLE_C_B        = 0x058;
constexpr uint32_t IMAGE_SAMPLE_C_LZ       = 0x05c;
constexpr uint32_t IMAGE_SAMPLE_O          = 0x070;
constexpr uint32_t IMAGE_SAMPLE_CL         = 0x0d4;
constexpr uint32_t IMAGE_GATHER4           = 0x080;
constexpr uint32_t IMAGE_GATHER4_LZ        = 0x08c;
constexpr uint32_t IMAGE_GATHER4H          = 0x0a2;
constexpr uint32_t IMAGE_GATHER4_B         = 0x088;
constexpr uint32_t IMAGE_GATHER4_C_LZ      = 0x09c;
constexpr uint32_t IMAGE_GATHER4_PO        = 0x0b0;
constexpr uint32_t IMAGE_GET_RESINFO       = 0x0e0;
constexpr uint32_t IMAGE_ATOMIC_SWAP       = 0x0f0;
constexpr uint32_t IMAGE_ATOMIC_CMPSWAP    = 0x0f1;
constexpr uint32_t IMAGE_ATOMIC_ADD        = 0x0f2;
constexpr uint32_t IMAGE_ATOMIC_SUB        = 0x0f3;
constexpr uint32_t IMAGE_ATOMIC_SMIN       = 0x0f4;
constexpr uint32_t IMAGE_ATOMIC_UMIN       = 0x0f5;
constexpr uint32_t IMAGE_ATOMIC_SMAX       = 0x0f6;
constexpr uint32_t IMAGE_ATOMIC_UMAX       = 0x0f7;
constexpr uint32_t IMAGE_ATOMIC_AND        = 0x0f8;
constexpr uint32_t IMAGE_ATOMIC_OR         = 0x0f9;
constexpr uint32_t IMAGE_ATOMIC_XOR        = 0x0fa;
constexpr uint32_t IMAGE_ATOMIC_INC        = 0x0fb;
constexpr uint32_t IMAGE_ATOMIC_DEC        = 0x0fc;
constexpr uint32_t IMAGE_STORE             = 0x020;
constexpr uint32_t IMAGE_STORE_MIP         = 0x021;
constexpr uint32_t IMAGE_STORE_RTN         = 0x022;
constexpr uint32_t IMAGE_SAMPLE_PCK        = 0x0d0;

// ---- EXP ----
constexpr uint32_t EXP_TARGET_MRT0 = 0, EXP_TARGET_MRT7 = 7;
constexpr uint32_t EXP_TARGET_MRTZ = 8;
constexpr uint32_t EXP_TARGET_PARAM0 = 32;   // param exports start at 32

// ---- FLAT (research layout, documented) ----
// Encoding: bits[31:23] = 0b110111001 prefix (w0 & 0xFF800000 == 0xDC800000),
// bits[22:16] = opcode (7 bits), bits[15:0] = unsigned byte offset.
constexpr uint32_t FLAT_LOAD_UBYTE      = 0x01;
constexpr uint32_t FLAT_LOAD_SBYTE      = 0x02;
constexpr uint32_t FLAT_LOAD_USHORT     = 0x03;
constexpr uint32_t FLAT_LOAD_SSHORT     = 0x04;
constexpr uint32_t FLAT_LOAD_DWORD      = 0x05;
constexpr uint32_t FLAT_LOAD_DWORDX2    = 0x06;
constexpr uint32_t FLAT_LOAD_DWORDX3    = 0x07;
constexpr uint32_t FLAT_LOAD_DWORDX4    = 0x08;
constexpr uint32_t FLAT_STORE_BYTE      = 0x09;
constexpr uint32_t FLAT_STORE_SHORT     = 0x0a;
constexpr uint32_t FLAT_STORE_DWORD     = 0x0b;
constexpr uint32_t FLAT_STORE_DWORDX2   = 0x0c;
constexpr uint32_t FLAT_STORE_DWORDX3   = 0x0d;
constexpr uint32_t FLAT_STORE_DWORDX4   = 0x0e;
constexpr uint32_t FLAT_ATOMIC_SWAP     = 0x10;
constexpr uint32_t FLAT_ATOMIC_CMPSWAP  = 0x11;
constexpr uint32_t FLAT_ATOMIC_ADD      = 0x12;
constexpr uint32_t FLAT_ATOMIC_INC      = 0x13;

// ---- VINTRP (GFX10 VOP3 opcode space) ----
constexpr uint32_t V_INTERP_P1_F32 = 0x0c0;
constexpr uint32_t V_INTERP_P2_F32 = 0x0c1;
constexpr uint32_t V_INTERP_MOV_F32 = 0x0c2;

// ---- VOP3P (packed math, 0xD3 prefix; research layout documented) ----
constexpr uint32_t V_MAD_MIX_F32 = 0x020;
} // namespace GcnOp

// One decoded GCN instruction. Field meanings depend on `format`; unused
// fields stay zero. Register encodings are RAW (VGPR indices, SGPR indices,
// or inline-constant encodings -- the executor resolves them).
struct GcnInstruction {
    GcnFormat format{GcnFormat::UNKNOWN};
    uint32_t opcode{0};
    uint8_t  dst{0};        // vdst (VOP*) / sdst index (SOP*)
    uint16_t src0{0};       // 9-bit source encoding
    uint16_t src1{0};
    uint16_t src2{0};       // VOP3 only
    uint16_t simm16{0};     // SOPK/SOPP signed 16-bit immediate
    uint32_t literal{0};    // literal constant consumed from the stream
    bool     has_literal{false};
    // VOP3 modifiers.
    bool neg0{false}, neg1{false}, neg2{false};
    bool abs0{false}, abs1{false}, abs2{false};
    uint8_t omod{0};
    bool clamp{false};
    // SMEM.
    uint8_t  sbase{0};        // sgpr pair base index (already dereferenced from {6:1})
    bool     smem_imm{false};
    uint32_t smem_offset{0};
    // MUBUF/MTBUF.
    uint8_t  vaddr{0};
    uint8_t  vdata{0};
    uint8_t  srsrc{0};        // sgpr quad base index (already shifted)
    uint16_t buf_offset{0};
    bool offen{false}, idxen{false}, glc{false}, slc{false};
    // DS.
    uint8_t ds_offset0{0}, ds_offset1{0};
    // VOP3P per-source half selectors (bit i: source i's HIGH 16 bits).
    uint8_t opsel{0};
    // MIMG.
    uint8_t  dmask{0};      // write mask for image loads
    uint8_t  ssamp{0};      // sampler sgpr pair
    bool     tfe{false};
    bool     a16{false};    // 16-bit address components
    bool     unorm{false};  // normalized coordinates
    // EXP.
    uint8_t  exp_target{0};
    uint8_t  exp_en{0};
    bool     exp_done{false};
    uint8_t  vsrc0{0}, vsrc1{0}, vsrc2{0}, vsrc3{0};
    // FLAT.
    uint16_t flat_offset{0};
    bool     flat_saddr{false};   // SGPR-based addressing
    // Control flow / stream bookkeeping.
    bool     is_terminator{false};
    bool     is_branch{false};
    bool     is_cond_branch{false};
    int64_t  branch_target{0};  // dword offset (pc + sign_extended(simm16))
    uint32_t raw0{0}, raw1{0};
    size_t   dwords_consumed{1};
};

class GcnDecoder {
public:
    // Decode the instruction at dword offset `pc`. Returns false when pc is
    // out of range or the first dword cannot start a GFX10 instruction
    // (fail-closed). Dual-dword formats consume raw1 from pc+1.
    bool Decode(const uint32_t* code, size_t dwords, size_t pc,
                GcnInstruction& out) const;

    // Best-effort mnemonic for diagnostics and tests ("v_add_f32_e32" style
    // suffixes omitted: "v_add_f32", "s_endpgm", ...). Returns "unknown" when
    // the opcode is not in the supported table.
    static const char* Mnemonic(const GcnInstruction& i);
};

// ---------------------------------------------------------------------------
// Software GCN executor: runs a decoded GFX10 program on the CPU with real
// register/memory semantics. Used as the honest fallback when no Vulkan
// device exists (the results are NOT faked -- the instructions execute).
// ---------------------------------------------------------------------------
class GpuGuestMemory;

struct GcnBufferResource {
    uint64_t base_gva{0};     // guest address of the buffer
    uint32_t size_dwords{0};
    uint32_t stride{0};       // for idxen addressing (dwords per index)
};

// Round 28: a guest 2D image resource. The texel data at base_gva is raw
// RGBA (4 dwords per texel) -- the exact bit pattern the software executor's
// SwImage stores, so hardware and software MIMG produce bit-identical
// results (the hardware descriptor is RGBA32UI and MIMG moves raw dwords).
struct GcnImageResource {
    uint64_t base_gva{0};     // guest address of the texel array
    uint32_t width{0};
    uint32_t height{0};
    uint32_t mips{1};
};

struct GcnSwExecResult {
    bool ok{false};
    std::string error;
    size_t instructions_executed{0};
    size_t lanes_run{0};
    bool terminated{false};   // S_ENDPGM seen
};

class GcnSwExecutor {
public:
    // Lane model identical to RDNA2ComputeCompiler/VulkanComputeExecutor:
    // per lane l: VGPRs reset, v0..v{k-1} = input[l*k + j]; after termination
    // v0..v{m-1} -> output[l*m + j]. SGPRs are per-program (shared across
    // lanes, like real hardware): s0..s{k-1} are seeded from the first k input
    // dwords of LANE 0 before lane 0 runs... (documented simplification: the
    // host seeds SGPRs via SetSgpr before Run).
    GcnSwExecResult Run(const uint32_t* code, size_t dwords, size_t lanes,
                        const std::vector<uint32_t>& input, uint32_t k_in,
                        uint32_t m_out, std::vector<uint32_t>& output,
                        GpuGuestMemory* mem = nullptr,
                        const std::vector<GcnBufferResource>* buffers = nullptr,
                        size_t instruction_limit = 100000);

    // Scalar state accessors for host setup/inspection.
    void SetSgpr(size_t index, uint32_t value) {
        if (index < kSgprCount) m_sgprs[index] = value;
    }
    uint32_t GetSgpr(size_t index) const {
        return index < kSgprCount ? m_sgprs[index] : 0;
    }
    uint32_t GetVgpr(size_t lane, size_t index) const {
        const size_t idx = lane * kVgprCount + index;
        return (idx < m_vgprs.size()) ? m_vgprs[idx] : 0;
    }
    uint32_t GetScc() const { return m_scc; }
    uint64_t GetExec() const { return m_exec; }

    // ---- Round 28: software IMAGE model ---------------------------------
    // A tiny 2D RGBA32F image pool the MIMG instructions address. Resource
    // descriptors are (image_index, format) pairs loaded from SGPR quads:
    // sgpr[quad+0] = image index, sgpr[quad+1] = width, sgpr[quad+2] = height,
    // sgpr[quad+3] = format (0 = RGBA32F).
    struct SwImage {
        uint32_t width{0}, height{0};
        uint32_t mips{1};                       // Round 28: GET_RESINFO reports it
        std::vector<uint32_t> rgba;               // width*height*4 dwords
        bool SetTexel(uint32_t x, uint32_t y, uint32_t r, uint32_t g, uint32_t b, uint32_t a);
        bool GetTexel(uint32_t x, uint32_t y, uint32_t& r, uint32_t& g, uint32_t& b, uint32_t& a) const;
    };
    void SetImage(size_t index, const SwImage& image) {
        if (index < kMaxImages) m_images[index] = image;
    }
    const SwImage& GetImage(size_t index) const {
        return (index < kMaxImages) ? m_images[index] : m_images[kMaxImages - 1];
    }

    // Exports: MRT targets (0..7) and parameter slots (32..). The executor
    // keeps the LAST export per lane per target (pixel-shader semantics for a
    // single-pixel-per-lane model).
    std::vector<uint32_t>& MrtExport(uint32_t target) { return m_mrt[target & 7]; }
    const std::vector<uint32_t>& MrtExport(uint32_t target) const { return m_mrt[target & 7]; }
    std::vector<uint32_t>& ParamExport(uint32_t slot) { return m_params[slot & 31]; }
    const std::vector<uint32_t>& ParamExport(uint32_t slot) const { return m_params[slot & 31]; }

    static constexpr size_t kSgprCount = 104;   // s0..s103
    static constexpr size_t kVgprCount = 256;   // v0..v255
    static constexpr size_t kMaxLanes = 1024;   // per-Run lane budget
    static constexpr size_t kMaxImages = 8;

private:
    uint32_t m_sgprs[kSgprCount]{};
    // Lane-major VGPR file [lane * kVgprCount + idx] on the HEAP: a 1 MB
    // stack array (the original layout) overflowed the frame when the
    // executor was constructed inside the software-fallback path.
    std::vector<uint32_t> m_vgprs;
    uint32_t m_scc{0};
    uint32_t m_vcc{0};
    uint64_t m_exec{~0ull};
    SwImage m_images[kMaxImages]{};
    // [lane * 4 + component] exports (RGBA per lane).
    std::vector<uint32_t> m_mrt[8];
    std::vector<uint32_t> m_params[32];
    // Round 29: wavefront shuffle columns, keyed by instruction pc. Captured
    // when the first lane reaches the instruction; every lane reads the same
    // snapshot (simultaneous-execution semantics).
    std::unordered_map<size_t, std::vector<uint32_t>> m_wave_columns;
};

} // namespace PS5::GPU
