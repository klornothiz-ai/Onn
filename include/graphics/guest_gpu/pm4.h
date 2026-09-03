#pragma once
// ProsperoLayer PS5 emulator - PM4 packet constants (RDNA2 / GCN3-style, Kyty-compatible)
#include "common/common.h"
#include <cstdint>
#include <cstdio>

namespace Pm4 {

// PM4 opcodes (subset used by the guest graphics driver)
constexpr uint32_t IT_NOP                        = 0x00;
constexpr uint32_t IT_SET_BASE                   = 0x01;
constexpr uint32_t IT_INDEX_BUFFER_SIZE          = 0x03;
constexpr uint32_t IT_DISPATCH_DIRECT            = 0x04;
constexpr uint32_t IT_DISPATCH_INDIRECT          = 0x05;
constexpr uint32_t IT_INDIRECT_BUFFER            = 0x3F;
constexpr uint32_t IT_WAIT_REG_MEM               = 0x3C;
constexpr uint32_t IT_COND_EXEC                  = 0x01;
constexpr uint32_t IT_COPY_DATA                  = 0x40;
constexpr uint32_t IT_WRITE_DATA                 = 0x41;
constexpr uint32_t IT_EVENT_WRITE                = 0x46;
constexpr uint32_t IT_EVENT_WRITE_EOP            = 0x47;
constexpr uint32_t IT_EVENT_WRITE_EOS            = 0x48;
constexpr uint32_t IT_RELEASE_MEM                = 0x49;
constexpr uint32_t IT_PFP_SYNC_ME                = 0x51;
constexpr uint32_t IT_DRAW_INDEX_2               = 0x22;
constexpr uint32_t IT_DRAW_INDEX_AUTO            = 0x23;
constexpr uint32_t IT_DRAW_INDEX_INDIRECT        = 0x24;
constexpr uint32_t IT_DRAW_INDEX_INDIRECT_MULTI  = 0x25;
constexpr uint32_t IT_DRAW_INDEX_OFFSET_2        = 0x26;
constexpr uint32_t IT_DRAW_INDIRECT              = 0x27;
constexpr uint32_t IT_DRAW_INDEX_INDIRECT_MULTI2 = 0x2B;
constexpr uint32_t IT_DISPATCH_DRAW_PREAMBLE     = 0x2C;
constexpr uint32_t IT_INDEX_BASE                 = 0x2D;
constexpr uint32_t IT_INDEX_TYPE                 = 0x2E;
constexpr uint32_t IT_NUM_INSTANCES              = 0x2F;
constexpr uint32_t IT_SET_PREDICATION            = 0x30;
constexpr uint32_t IT_DISPATCH_DRAW              = 0x31;
constexpr uint32_t IT_SET_CONTEXT_REG            = 0x69;
constexpr uint32_t IT_SET_CONTEXT_REG_INDIRECT   = 0x73;
constexpr uint32_t IT_SET_SH_REG                 = 0x76;
constexpr uint32_t IT_SET_SH_REG_INDIRECT        = 0x77;
constexpr uint32_t IT_SET_UCONFIG_REG            = 0x79;
constexpr uint32_t IT_SET_UCONFIG_REG_INDEX      = 0x7A;
constexpr uint32_t IT_SET_UCONFIG_REG_INDIRECT   = 0x7B;
constexpr uint32_t IT_CLEAR_STATE                = 0x03;
constexpr uint32_t IT_REWIND                     = 0x66;
constexpr uint32_t IT_GET_LOD_STATS              = 0x45;
constexpr uint32_t IT_DMA_DATA                   = 0x50;

// R-type packet identifiers (second dword)
constexpr uint32_t R_ZERO           = 0x00000000u;
constexpr uint32_t R_CONTEXT_STATE  = 0x00000001u;
constexpr uint32_t R_FLIP           = 0x00000002u;
constexpr uint32_t R_WAIT_FLIP_DONE = 0x00000003u;
constexpr uint32_t R_PUSH_MARKER    = 0x00000004u;
constexpr uint32_t R_POP_MARKER     = 0x00000005u;
constexpr uint32_t R_WAIT_MEM_32    = 0x00000006u;
constexpr uint32_t R_WAIT_MEM_64    = 0x00000007u;
constexpr uint32_t R_RELEASE_MEM    = 0x00000008u;
constexpr uint32_t R_ACQUIRE_MEM    = 0x00000009u;
constexpr uint32_t R_DISPATCH_RESET = 0x0000000Au;

// Shader register offsets (RDNA2-ish)
constexpr uint32_t SPI_SHADER_PGM_LO_PS = 0x00;
constexpr uint32_t SPI_SHADER_PGM_LO_ES = 0x10;
constexpr uint32_t SPI_SHADER_PGM_LO_LS = 0x20;
constexpr uint32_t SPI_SHADER_PGM_LO_GS = 0x30;
constexpr uint32_t SPI_SHADER_PGM_LO_HS = 0x40;
constexpr uint32_t SPI_SHADER_PGM_RSRC1_GS = 0x31;
constexpr uint32_t SPI_SHADER_PGM_RSRC1_HS = 0x41;
constexpr uint32_t SPI_SHADER_PGM_RSRC2_GS = 0x32;
constexpr uint32_t SPI_SHADER_PGM_RSRC2_HS = 0x42;
constexpr uint32_t SPI_SHADER_PGM_CHKSUM_GS = 0x33;
constexpr uint32_t SPI_SHADER_PGM_CHKSUM_HS = 0x43;
constexpr uint32_t COMPUTE_PGM_LO          = 0x60;
constexpr uint32_t COMPUTE_PGM_RSRC1       = 0x61;
constexpr uint32_t COMPUTE_PGM_RSRC2       = 0x62;
constexpr uint32_t COMPUTE_PGM_HI          = 0x64;
// COMPUTE_USER_DATA_0..15 hold the shader's user SGPR words. ProsperoLayer's
// compute ABI (see docs/GPU_COMPUTE_PIPELINE.md) uses:
//   user_data[0..1] = input SSBO GVA  (lo, hi)
//   user_data[2..3] = output SSBO GVA (lo, hi)
//   user_data[4]    = element count (uint32)
constexpr uint32_t COMPUTE_USER_DATA_0     = 0x240;
constexpr uint32_t COMPUTE_NUM_THREAD_X    = 0x07;  // logical, dwords in DISPATCH payload

constexpr uint32_t VGT_SHADER_STAGES_EN = 0x24;
constexpr uint32_t VGT_GS_OUT_PRIM_TYPE = 0x25;
constexpr uint32_t VGT_PRIMITIVE_TYPE   = 0x26;
constexpr uint32_t VGT_INDEX_TYPE       = 0x27;
constexpr uint32_t GE_CNTL              = 0x28;
constexpr uint32_t GE_USER_VGPR_EN      = 0x29;
constexpr uint32_t CX_PS_SHADER_USAGE_BASE = 0x80;

// ----------------------------------------------------------------------------
// ProsperoLayer draw ABI (round 9; see docs/GPU_COMPUTE_PIPELINE.md "draw
// path"). The vertex-stage program is programmed exactly like the compute
// program: the SH-register pair SPI_SHADER_PGM_LO_VS / HI_VS carries the
// program GVA with the same 256-byte alignment convention (LO stores
// gva >> 8), and the VS user SGPRs carry the buffers:
//   SPI_SHADER_USER_DATA_VS_0 + 0..1 = draw input buffer GVA (lo, hi)
//       -- for indexed draws the index stream at INDEX_BASE is decoded
//          (u16 zero-extended or u32, per PKT3_INDEX_TYPE) and fed to the
//          vertex-stage kernel lane-per-element; for non-indexed draws the
//          raw buffer contents are passed through.
//   SPI_SHADER_USER_DATA_VS_0 + 2..3 = transformed-vertex output buffer GVA
//   SPI_SHADER_USER_DATA_VS_0 + 4    = element count (uint32)
// ----------------------------------------------------------------------------
constexpr uint32_t SPI_SHADER_PGM_LO_VS      = 0x2A;
constexpr uint32_t SPI_SHADER_PGM_HI_VS      = 0x2B;
constexpr uint32_t SPI_SHADER_USER_DATA_VS_0 = 0x250;

// ----------------------------------------------------------------------------
// ProsperoLayer VGT attribute-fetch ABI (round 10). On real RDNA2 hardware
// the VGT hands each vertex's attributes to the shader by running the guest's
// fetch shader (TF buffer loads). ProsperoLayer models the same gather
// declaratively: the guest programs a small attribute-fetch descriptor table
// through the VS user SGPRs and the translator performs the fixed-function
// gather (index -> per-attribute address -> concatenated attribute dwords per
// lane) before executing the vertex-stage kernel:
//   SPI_SHADER_USER_DATA_VS_0 + 5..6 = attribute-fetch descriptor table GVA
//       (lo, hi). Zero (the default) keeps the round-9 single-stream model
//       (one dword per lane) -- full back-compatibility.
//   SPI_SHADER_USER_DATA_VS_0 + 7    = transformed-vertex output size in
//       dwords per vertex (1..16; the recorded default is 1).
// The descriptor table in guest memory is self-describing:
//   dword 0    = attribute count N (1..VGT_MAX_ATTRIBUTES)
//   then N entries of 4 dwords each:
//     +0..1 = attribute buffer GVA (lo, hi)
//     +2    = stride in dwords between consecutive vertices (>= 1)
//     +3    = attribute size in dwords (1..VGT_MAX_ATTRIBUTE_DWORDS)
// Per lane i: vertex id = index[i] (indexed draws, after u16/u32 decode) or i
// (non-indexed); attribute a contributes its size dwords read at
// attr_gva[a] + vertex_id * stride[a]; the concatenated dwords (v0..v{k-1},
// k = sum of sizes) are the kernel's lane input, and the kernel's first m
// output dwords go to output_gva + i*m (lane-major interleaved layout).
// ----------------------------------------------------------------------------
constexpr uint32_t SPI_SHADER_USER_DATA_VS_FETCH_LO = 0x255;  // +5
constexpr uint32_t SPI_SHADER_USER_DATA_VS_FETCH_HI = 0x256;  // +6
constexpr uint32_t SPI_SHADER_USER_DATA_VS_OUT_DWORDS = 0x257; // +7
constexpr uint32_t VGT_MAX_ATTRIBUTES       = 8;
constexpr uint32_t VGT_MAX_ATTRIBUTE_DWORDS = 8;

// ----------------------------------------------------------------------------
// Round 19 -- buffer-resource table ABI (the MUBUF/SMEM -> real-Vulkan step).
// The compiler + executor have understood MUBUF (per-descriptor SSBOs) and
// SMEM (scalar-segment mirror) since round 18; the PM4 side now PLUMBS the
// guest's resource tables into the dispatch. Same self-describing-table style
// as the round-10 fetch descriptors:
//   Compute: COMPUTE_USER_DATA_0 + 5..6 = resource-table GVA (lo, hi)
//   Draw VS: SPI_SHADER_USER_DATA_VS_0 + 8..9 = resource-table GVA (lo, hi)
// Zero (the default) keeps the round-18 behaviour exactly (MUBUF/SMEM fail
// closed on the hardware path -> honest software fallback).
// The table in guest memory:
//   dword 0      = buffer count N (1..GCN_MAX_BUFFER_RESOURCES)
//   dword 1..2   = scalar-mirror base GVA (lo, hi; 0 = no mirror)
//   dword 3      = scalar-mirror size in dwords (0 when no mirror; 1..
//                  GCN_MAX_MIRROR_DWORDS otherwise)
//   then N entries of 4 dwords each:
//     +0..1 = buffer base GVA (lo, hi; != 0)
//     +2    = buffer size in dwords (1..GCN_MAX_BUFFER_DWORDS)
//     +3    = idxen stride in dwords (>= 1; stride 0 is rejected so the
//             SPIR-V lowering (stride ? stride*4 : 4) never diverges from
//             the software executor's vaddr*stride*4)
// Descriptor i maps to the srsrc convention both executors already use
// (srsrc quad s[4+4i..7+4i], table index srsrc/4 - 1).
// ----------------------------------------------------------------------------
constexpr uint32_t COMPUTE_USER_DATA_RESOURCE_LO     = 0x245;  // +5
constexpr uint32_t COMPUTE_USER_DATA_RESOURCE_HI     = 0x246;  // +6
constexpr uint32_t SPI_SHADER_USER_DATA_VS_RESOURCE_LO = 0x258;  // +8
constexpr uint32_t SPI_SHADER_USER_DATA_VS_RESOURCE_HI = 0x259;  // +9
constexpr uint32_t GCN_MAX_BUFFER_RESOURCES = 8;
constexpr uint32_t GCN_MAX_MIRROR_DWORDS    = 65536;  // 256 KB scalar window
constexpr uint32_t GCN_MAX_BUFFER_DWORDS    = 262144; // 1 MB per descriptor

// ----------------------------------------------------------------------------
// Round 28 -- image-resource table extension (the MIMG -> real-Vulkan step).
// The round-19 table above grows an optional second header half + image
// entries; the extension is marked by a magic so OLD tables (which simply
// stop after the buffer entries) still parse byte-identically:
//   dword 4      = image count M (1..GCN_MAX_IMAGE_RESOURCES when extended)
//   dword 5      = extension magic 0x494D4745 ("IMGE")
//   dword 6..7   = reserved (0)
//   then M entries of 6 dwords each, directly after the buffer entries:
//     +0..1 = texel-array base GVA (lo, hi; != 0)
//     +2    = width  in texels (1..GCN_MAX_IMAGE_DIM)
//     +3    = height in texels (1..GCN_MAX_IMAGE_DIM)
//     +4    = mip levels (1..GCN_MAX_IMAGE_MIPS)
//     +5    = reserved (0)
// Image i maps to the srsrc convention both executors use (descriptor quad
// s[4*(1+bufN)+4i ...], table index srsrc/4 - 1 - bufN).
// ----------------------------------------------------------------------------
constexpr uint32_t GCN_RESOURCE_TABLE_IMAGE_MAGIC = 0x494D4745u;  // "IMGE"
constexpr uint32_t GCN_MAX_IMAGE_RESOURCES = 8;
constexpr uint32_t GCN_MAX_IMAGE_DIM       = 4096;
constexpr uint32_t GCN_MAX_IMAGE_MIPS      = 8;

// ----------------------------------------------------------------------------
// Round 18: REAL RDNA (GFX10) context-register offsets for render-target
// binding. Numbers verified against the Linux kernel AMD register headers
// (drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_offset.h and
// gc_10_1_0_sh_mask.h in torvalds/linux), i.e. the same register map the
// PS5's Oberon (RDNA2) PM4 engine decodes. The SET_CONTEXT_REG payload
// register index is (offset - 0xA000); the constants below are the full
// offsets the translator keys on.
//   CB_COLOR0_BASE  0xA318  bits[31:8] = colour-plane GVA[39:8] (256B aligned,
//                              low 8 bits reserved, the hardware convention)
//   CB_COLOR0_PITCH 0xA319  TILE_MAX[10:0]: linear-mode pitch = (TILE_MAX+1)*8
//                              pixels (AMD's linear-allocation convention)
//   CB_COLOR0_SLICE 0xA31A  TILE_MAX[21:0]: linear-mode pixel count =
//                              (SLICE_TILE_MAX+1)*64, so height = count/pitch
//   CB_COLOR0_INFO  0xA31C  FORMAT[6:2] (AMD colour-buffer format enum;
//                              8_8_8_8 = 3) + NUMBER_TYPE[10:8] (UNORM = 0)
//   CB_COLOR0_BASE_EXT 0xA390  colour-plane GVA[47:32]
//   CB_TARGET_MASK  0xA08E  TARGET0_ENABLE[3:0]: 0xF = colour writes on
//   DB_DEPTH_SIZE_XY 0xA007  X_MAX[13:0] | Y_MAX[29:16] (inclusive last pixel)
//   DB_Z_INFO       0xA010  FORMAT[1:0]: 3 = 32_FLOAT (the model's depth plane)
//   DB_Z_WRITE_BASE 0xA014  depth-plane GVA[31:0] (256B aligned)
//   DB_Z_WRITE_BASE_HI 0xA01C  depth-plane GVA[47:32]
//   DB_DEPTH_CONTROL 0xA200  Z_ENABLE[1], Z_WRITE_ENABLE[2], ZFUNC[6:4]
//                              (0=NEVER 1=LESS 2=EQUAL 3=LEQUAL 4=GREATER
//                               5=NOTEQUAL 6=GEQUAL 7=ALWAYS)
//   PA_CL_VPORT_XSCALE 0xA10F, XOFFSET 0xA110, YSCALE 0xA111, YOFFSET 0xA112
//                              (float bits; screen = ndc * scale + offset)
// ProsperoLayer interpretation notes (documented deviations from hardware):
//   * the model rasterizer is linear/untiled, so PITCH/SLICE use AMD's
//     linear-mode arithmetic instead of reconstructing a tile mode;
//   * DB_DEPTH_SIZE_XY X_MAX/Y_MAX are read as inclusive last-pixel
//     coordinates (width = X_MAX+1, height = Y_MAX+1);
//   * the pixel stage clamps to the target extent; a separate scissor
//     (PA_SC_SCREEN_SCISSOR_TL/TR 0xA00C/0xA00D) is not enforced yet.
// ----------------------------------------------------------------------------
constexpr uint32_t CB_COLOR0_BASE          = 0xA318;
constexpr uint32_t CB_COLOR0_PITCH         = 0xA319;
constexpr uint32_t CB_COLOR0_SLICE         = 0xA31A;
constexpr uint32_t CB_COLOR0_INFO          = 0xA31C;
constexpr uint32_t CB_COLOR0_BASE_EXT      = 0xA390;
constexpr uint32_t CB_TARGET_MASK          = 0xA08E;
constexpr uint32_t DB_DEPTH_SIZE_XY        = 0xA007;
constexpr uint32_t DB_Z_INFO               = 0xA010;
constexpr uint32_t DB_Z_WRITE_BASE         = 0xA014;
constexpr uint32_t DB_Z_WRITE_BASE_HI      = 0xA01C;
constexpr uint32_t DB_DEPTH_CONTROL        = 0xA200;  // real DB_DEPTH_CONTROL
constexpr uint32_t PA_CL_VPORT_XSCALE      = 0xA10F;
constexpr uint32_t PA_CL_VPORT_XOFFSET     = 0xA110;
constexpr uint32_t PA_CL_VPORT_YSCALE      = 0xA111;
constexpr uint32_t PA_CL_VPORT_YOFFSET     = 0xA112;
constexpr uint32_t PA_SC_SCREEN_SCISSOR_TL = 0xA00C;
constexpr uint32_t PA_SC_SCREEN_SCISSOR_BR = 0xA00D;

// CB_COLOR0_INFO field decode (shifts/masks are the hardware ones).
constexpr uint32_t CB_INFO_FORMAT_SHIFT    = 2;
constexpr uint32_t CB_INFO_FORMAT_MASK     = 0x7Cu;
constexpr uint32_t CB_INFO_NUMBER_SHIFT    = 8;
constexpr uint32_t CB_INFO_NUMBER_MASK     = 0x700u;
// Round 19 CORRECTION (latent round-18 defect): the CB_COLOR0_INFO FORMAT
// field on Liverpool is programmed with the SQIMG *DataFormat* enum -- the
// same numbering Gnm::DataFormat uses (RenderTarget::init takes a DataFormat)
// -- NOT the PC-GCN COLOR_* colour-buffer enum. Verified against shadPS4
// (the project's reference model): regs_color.h decodes the field as
// `DataFormat(info.format)` and pixel_format.h's Sea-Islands table puts
// Format8_8_8_8 = 10 (8=1, 16=2, 8_8=3, 32=4, 16_16=5, 10_11_11=6,
// 11_11_10=7, 10_10_10_2=8, 2_10_10_10=9, 8_8_8_8=10). The round-18 value
// (3, the PC COLOR_8_8_8_8) would reject every real guest RGBA8 target.
constexpr uint32_t CB_FORMAT_8_8_8_8       = 10u;   // SQIMG DataFormat value
constexpr uint32_t CB_FORMAT_16_16_16_16   = 12u;   // 64 bpp
constexpr uint32_t CB_FORMAT_11_11_10      = 7u;    // 32 bpp packed float (VK B10G11R11)
constexpr uint32_t CB_FORMAT_2_10_10_10    = 9u;    // 32 bpp packed unorm (VK A2B10G10R10)
// NUMBER_TYPE: 0 = UNORM on both enum families (shadPS4 NumberFormat::Unorm).
constexpr uint32_t CB_NUMBER_UNORM         = 0u;
constexpr uint32_t CB_NUMBER_SNORM         = 1u;
constexpr uint32_t CB_NUMBER_FLOAT         = 7u;
// CB number_type 6 is NumberFormat::SnormNz in the TEXTURE enum, but for
// COLOR BUFFERS it means SRGB (shadPS4 GetNumberFormat: "there is a small
// difference between T# and CB number types" -- SnormNz -> Srgb).
constexpr uint32_t CB_NUMBER_SRGB          = 6u;
// CB_COLOR0_INFO.COMP_SWAP bits [12:11] (shadPS4 SwapMode: Standard=0,
// Alternate=1, StandardReverse=2, AlternateReverse=3). Alternate swaps the
// R/B channels on RGBA layouts -- how a guest programs a BGRA8 target.
constexpr uint32_t CB_INFO_COMP_SWAP_SHIFT = 11;
constexpr uint32_t CB_INFO_COMP_SWAP_MASK  = 0x1800u;
constexpr uint32_t CB_COMP_SWAP_STANDARD   = 0u;
constexpr uint32_t CB_COMP_SWAP_ALTERNATE  = 1u;
// DB_Z_INFO FORMAT values (hardware enum).
constexpr uint32_t DB_Z_FORMAT_32_FLOAT    = 3u;
// DB_DEPTH_CONTROL fields (hardware shifts/masks).
constexpr uint32_t DB_CONTROL_Z_ENABLE_MASK     = 0x2u;
constexpr uint32_t DB_CONTROL_Z_WRITE_MASK      = 0x4u;
constexpr uint32_t DB_CONTROL_ZFUNC_SHIFT       = 4;
constexpr uint32_t DB_CONTROL_ZFUNC_MASK        = 0x70u;
// CB_TARGET_MASK fields.
constexpr uint32_t CB_TARGET0_ENABLE_MASK       = 0xFu;
// DB_DEPTH_SIZE_XY fields.
constexpr uint32_t DB_SIZE_X_MAX_MASK           = 0x3FFFu;
constexpr uint32_t DB_SIZE_Y_MAX_SHIFT          = 16;
constexpr uint32_t DB_SIZE_Y_MAX_MASK           = 0x3FFF0000u;
// CB_COLOR0_PITCH/SLICE fields.
constexpr uint32_t CB_PITCH_TILE_MAX_MASK       = 0x7FFu;
constexpr uint32_t CB_SLICE_TILE_MAX_MASK       = 0x3FFFFFu;
// Linear-mode geometry: pitch = (PITCH_TILE_MAX+1)*8 px; total pixels =
// (SLICE_TILE_MAX+1)*64 (each "tile" is the 8x8 quantum the registers count).
constexpr uint32_t CB_LINEAR_TILE_PIXELS        = 8;
constexpr uint32_t CB_LINEAR_TILE_AREA          = 64;

// ZFUNC comparison-function ids (AMD CompareFunc, identical ordering to
// Vulkan/OpenGL). The software rasterizer's depth test selects on these.
enum class ZFunc : uint32_t {
    Never = 0, Less = 1, Equal = 2, LEqual = 3,
    Greater = 4, NotEqual = 5, GEqual = 6, Always = 7,
};

// ----------------------------------------------------------------------------
// Round 19 (phase 2) -- CB_COLOR0_INFO -> concrete raster formats. The enum
// carries the REAL VkFormat numeric values (stable Vulkan spec constants,
// verified against KhronosGroup/Vulkan-Headers vulkan_core.h) so the
// Vulkan-guarded executor code can static_cast< VkFormat > directly and the
// conversion stays unit-testable on a headless host. Only formats the whole
// model actually implements appear; everything else fails closed (returns
// Invalid) exactly like the round-18 reject gate.
// ----------------------------------------------------------------------------
enum class GuestColorFormat : uint32_t {
    Invalid = 0,
    R8G8B8A8Unorm = 37,   // VK_FORMAT_R8G8B8A8_UNORM (CB 8_8_8_8 / UNORM)
    // Round 20 -- the formats games actually program beyond RGBA8 UNORM.
    // Every numeric value verified against KhronosGroup/Vulkan-Headers
    // vulkan_core.h (the vendored copy in refs/):
    R8G8B8A8Snorm = 38,      // VK_FORMAT_R8G8B8A8_SNORM  (8_8_8_8 / SNORM)
    R8G8B8A8Srgb = 43,       // VK_FORMAT_R8G8B8A8_SRGB   (8_8_8_8 / CB number 6)
    B8G8R8A8Unorm = 44,      // VK_FORMAT_B8G8R8A8_UNORM  (8_8_8_8 / UNORM / COMP_SWAP=Alternate)
    A2B10G10R10UnormPack32 = 64,  // VK_FORMAT_A2B10G10R10_UNORM_PACK32 (2_10_10_10 / UNORM)
    R16G16B16A16Unorm = 91,  // VK_FORMAT_R16G16B16A16_UNORM (16_16_16_16 / UNORM)
    R16G16B16A16Snorm = 92,  // VK_FORMAT_R16G16B16A16_SNORM (16_16_16_16 / SNORM)
    R16G16B16A16Sfloat = 97, // VK_FORMAT_R16G16B16A16_SFLOAT (16_16_16_16 / FLOAT)
    R32Sfloat = 100,         // VK_FORMAT_R32_SFLOAT      (32 / FLOAT -- single channel)
    B10G11R11UfloatPack32 = 122, // VK_FORMAT_B10G11R11_UFLOAT_PACK32 (11_11_10 / FLOAT)
};

// Bytes per guest colour pixel for each implemented format (the plane
// stride the software rasterizer and the Vulkan readback both use).
inline uint32_t GuestColorFormatBytesPerPixel(GuestColorFormat fmt) {
    switch (fmt) {
        case GuestColorFormat::R8G8B8A8Unorm:
        case GuestColorFormat::R8G8B8A8Snorm:
        case GuestColorFormat::R8G8B8A8Srgb:
        case GuestColorFormat::B8G8R8A8Unorm:
        case GuestColorFormat::A2B10G10R10UnormPack32:
        case GuestColorFormat::B10G11R11UfloatPack32:
            return 4;
        case GuestColorFormat::R16G16B16A16Unorm:
        case GuestColorFormat::R16G16B16A16Snorm:
        case GuestColorFormat::R16G16B16A16Sfloat:
            return 8;
        case GuestColorFormat::R32Sfloat:
            return 4;
        default:
            return 0;   // Invalid
    }
}

// Converts the decoded CB_COLOR0_INFO FORMAT[6:2] + NUMBER_TYPE[10:8] pair
// (+ COMP_SWAP[12:11], Standard when omitted). Round 20 grows the table from
// the single round-19 RGBA8 UNORM pair to the ten formats games program:
//   8_8_8_8  x {UNORM, SNORM, CB-6(=SRGB)} x {Standard, Alternate}
//   16_16_16_16 x {UNORM, SNORM, FLOAT}
//   11_11_10 / FLOAT,  2_10_10_10 / UNORM
// Every pair is verified against shadPS4's model (refs/pixel_format.h:
// DataFormat/NumberFormat enums + RemapDataFormat + the CB SnormNz->Srgb
// note; refs/regs_color.h: SwapMode + Color0Info bitfield).
inline bool CbInfoToGuestColorFormat(uint32_t format, uint32_t number,
                                     GuestColorFormat& out,
                                     uint32_t comp_swap = CB_COMP_SWAP_STANDARD) {
    if (comp_swap == CB_COMP_SWAP_STANDARD) {
        if (format == CB_FORMAT_8_8_8_8) {
            if (number == CB_NUMBER_UNORM) { out = GuestColorFormat::R8G8B8A8Unorm; return true; }
            if (number == CB_NUMBER_SNORM) { out = GuestColorFormat::R8G8B8A8Snorm; return true; }
            if (number == CB_NUMBER_SRGB)  { out = GuestColorFormat::R8G8B8A8Srgb;  return true; }
            return false;
        }
        if (format == CB_FORMAT_16_16_16_16) {
            if (number == CB_NUMBER_UNORM)  { out = GuestColorFormat::R16G16B16A16Unorm;  return true; }
            if (number == CB_NUMBER_SNORM)  { out = GuestColorFormat::R16G16B16A16Snorm;  return true; }
            if (number == CB_NUMBER_FLOAT)  { out = GuestColorFormat::R16G16B16A16Sfloat; return true; }
            return false;
        }
        if (format == CB_FORMAT_11_11_10 && number == CB_NUMBER_FLOAT) {
            out = GuestColorFormat::B10G11R11UfloatPack32; return true;
        }
        if (format == CB_FORMAT_2_10_10_10 && number == CB_NUMBER_UNORM) {
            out = GuestColorFormat::A2B10G10R10UnormPack32; return true;
        }
        return false;
    }
    if (comp_swap == CB_COMP_SWAP_ALTERNATE) {
        // Alternate swaps R<->B on the RGBA layouts; on the packed formats the
        // swap degenerates to the same layout (the pack order is fixed).
        if (format == CB_FORMAT_8_8_8_8) {
            if (number == CB_NUMBER_UNORM) { out = GuestColorFormat::B8G8R8A8Unorm; return true; }
            return false;
        }
        return false;
    }
    return false;   // reverse swaps: fail closed
}

// DB_DEPTH_CONTROL.ZFUNC -> VkCompareOp (verified: NEVER=0 ... ALWAYS=7 in
// both enums, a numeric identity). Exposed for the graphics pipeline's
// depth-stencil state and for headless tests.
inline uint32_t ZFuncToVkCompareOp(ZFunc zfunc) {
    return static_cast<uint32_t>(zfunc);   // identical ordering
}

// PKT3_INDEX_TYPE payload values (VGT_INDEX_TYPE semantics: 16- or 32-bit
// indices in the buffer programmed by PKT3_INDEX_BASE).
constexpr uint32_t INDEX_TYPE_U16 = 0;
constexpr uint32_t INDEX_TYPE_U32 = 1;

// PM4 packet header builder: (count-1)<<16 | opcode<<8 | type
constexpr uint32_t KYTY_PM4_HEADER(uint32_t count_dw, uint32_t opcode, uint32_t type) {
        return ((count_dw - 1u) << 16u) | ((opcode & 0xFFu) << 8u) | (type & 0xFFu);
}
#define KYTY_PM4(count_dw, opcode, type) Pm4::KYTY_PM4_HEADER((count_dw), (opcode), (type))
constexpr uint32_t KYTY_PM4_LEN(uint32_t header) { return ((header >> 16u) & 0x3FFFu) + 1u; }

// Packet header helpers (used by the PM4 translator).
constexpr uint32_t KYTY_PM4_R(uint32_t header) { return header & 0x3FFFFFFFu; }
constexpr uint32_t KYTY_PM4_IT(uint32_t header) { return (header >> 24u) & 0xFFu; }

// Packet type helpers, visible at Pm4 scope (Kyty-compatible).
constexpr uint32_t KYTY_PMN(uint32_t op) { return ((op & 0xFFu) << 24u) | 0x80000000u; }
constexpr uint32_t KYTY_PMN_LEN(uint32_t header) { return ((header >> 16u) & 0x3FFFu) + 1u; }

// R-type packet helpers (used by the PM4 translator).
namespace PmN {
constexpr uint32_t KYTY_PMN_R(uint32_t op) { return KYTY_PMN(op) | 0x00000000u; }
} // namespace PmN

// Context-state operations used by GraphicsDcbContextStateOp.
enum class ContextStateOperation : uint32_t {
        Clear      = 0,
        Push       = 1,
        Pop        = 2,
        PushClear  = 3,
};

void DumpPm4PacketStream(FILE* f, const uint32_t* cmd_buffer, uint32_t offset, uint32_t num_dw);

} // namespace Pm4
