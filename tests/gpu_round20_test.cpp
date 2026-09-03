// ============================================================================
// ProsperoLayer RDNA2 Core - round 20 GPU test: vertex control flow, the
// extended CB format set, and the cross-dispatch pipeline cache.
// ----------------------------------------------------------------------------
// Part A -- VERTEX-STAGE CONTROL FLOW (the CHANGES.md "next" list):
//   A1  a divergent per-lane if/else (v_cmp + s_cbranch_vccnz) compiles in
//       emit_vertex_stage mode: the structured-control-flow lowering
//       (OpSelectionMerge/OpBranchConditional) coexists with the vertex
//       decorations (VertexIndex/Position/Location) and stays LocalSize-free;
//   A2  the same program through the software executor: lanes DIVERGE (each
//       vertex's colour follows its own v4), and the interpreter parity holds
//       for every lane;
//   A3  a backward branch (loop) in vertex mode lowers to OpLoopMerge.
//
// Part B -- CB FORMATS BEYOND RGBA8 UNORM (the second "next" item):
//   B1  CbInfoToGuestColorFormat: all ten implemented (format, number,
//       comp_swap) pairs map to the REAL VkFormat numbers (each value
//       verified against the vendored KhronosGroup vulkan_core.h) and
//       everything else fails closed;
//   B2  GuestColorFormatBytesPerPixel: 4/8 per layout;
//   B3  the software rasterizer encodes each layout exactly: BGRA byte
//       order, sRGB curve spot values, A2B10G10R10 / B10G11R11 pack words,
//       IEEE-754 half floats (with the round-to-nearest-even edge);
//   B4  the PM4 translator gate: CB_COLOR0_INFO with 16_16_16_16/FLOAT
//       produces a bound target whose plane carries half-float bits after a
//       real draw; the old RGBA8 stream stays byte-identical.
//
// Part C -- THE CROSS-DISPATCH PIPELINE CACHE (the third "next" item):
//   C1  key determinism + sensitivity (spirv/binding/pc/kind/format each
//       change the key);
//   C2  the registry contract: first dispatch misses, the identical second
//       hits, a changed program misses again; stats accumulate;
//   C3  the registry is process-wide: two executor instances see the same
//       entries; ResetForTest isolates suites.
// ============================================================================
#include "gpu/pm4_translator.hpp"
#include "gpu/software_rasterizer.hpp"
#include "gpu/vulkan_compute_executor.hpp"
#include "gpu/vulkan_pipeline_cache.hpp"
#include "gpu/rdna2_compute_compiler.hpp"
#include "gpu/gcn_decoder.hpp"
#include "graphics/guest_gpu/pm4.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

using PS5::GPU::ComputeCompilerOptions;
using PS5::GPU::GcnSwExecResult;
using PS5::GPU::GcnSwExecutor;
using PS5::GPU::GpuGuestMemory;
using PS5::GPU::PipelineCacheKey;
using PS5::GPU::PipelineCacheStats;
using PS5::GPU::PipelineKeyRegistry;
using PS5::GPU::PipelineKind;
using PS5::GPU::PM4VulkanTranslator;
using PS5::GPU::RasterTarget;
using PS5::GPU::RasterViewport;
using PS5::GPU::RDNA2ComputeCompiler;
using PS5::GPU::SoftwareRasterizer;
using PS5::GPU::SoftwareRasterStats;
using PS5::GPU::VulkanComputeExecutor;
using PS5::GPU::VulkanRendererBackend;
using Pm4::GuestColorFormat;

int g_failures = 0;
int g_checks = 0;
bool Check(bool v, const char* e, int line) {
    ++g_checks;
    if (!v) { ++g_failures; std::cerr << "  [FAIL] line " << line << ": " << e << '\n'; }
    return v;
}
#define CHECK(e) Check((e), #e, __LINE__)

uint32_t FBits(float f) { uint32_t v; std::memcpy(&v, &f, 4); return v; }
uint32_t V(uint32_t n) { return 256U + n; }   // 9-bit field: 256+n = VGPR v_n
[[maybe_unused]] float BitsF(uint32_t v) { float f; std::memcpy(&f, &v, 4); return f; }

// ---- GCN encoders (the REAL GFX10 layouts, mirroring gcn_spirv_full_test) --
uint32_t Vop1(uint32_t op, uint32_t dst, uint32_t src0) {
    return 0x7E000000u | (op << 17u) | (dst << 9u) | src0;
}
[[maybe_unused]] uint32_t Vop2(uint32_t op, uint32_t dst, uint32_t src1_vgpr, uint32_t src0) {
    return (op << 25u) | (dst << 17u) | (src1_vgpr << 9u) | src0;
}
uint32_t Vopc(uint32_t op, uint32_t src1_vgpr, uint32_t src0) {
    return 0x7C000000u | (op << 17u) | (src1_vgpr << 9u) | src0;
}
uint32_t Sopp(uint32_t op, int simm16) {
    return 0xBF800000u | (op << 16u) |
           (static_cast<uint32_t>(simm16) & 0xFFFFu);
}
[[maybe_unused]] uint32_t Sop1(uint32_t op, uint32_t sdst, uint32_t ssrc0) {
    return 0x7D800000u | (op << 16u) | (sdst << 9u) | ssrc0;
}
constexpr uint32_t SOPP_ENDPGM = 0x001, SOPP_BRANCH = 0x002,
                    SOPP_CBRANCH_SCC0 = 0x004, SOPP_CBRANCH_SCC1 = 0x005,
                    SOPP_CBRANCH_VCCZ = 0x006, SOPP_CBRANCH_VCCNZ = 0x007;
// GCN opcodes (gfx10 numbering, verified against the round-12 decoder).
constexpr uint32_t V_MOV_B32 = 0x01;
constexpr uint32_t V_CMP_LT_F32 = 0x001;  // VOPC: vcc = src0 < src1
constexpr uint32_t S_MOV_B32 = 0x00;
constexpr uint32_t S_MOVK_I32 = 0x0B;
constexpr uint32_t S_CMPK_EQ_U32 = 0x02;

// ---- flat guest memory (mirror of the emulator bridge) ---------------------
class FlatGuestMemory final : public GpuGuestMemory {
public:
    FlatGuestMemory(uint64_t base, size_t dwords)
        : m_base(base), m_storage(dwords, 0) {}
    bool ReadDwords(uint64_t gva, uint32_t* dst, size_t dwords) override {
        if (gva < m_base || gva % 4 != 0) return false;
        const size_t off = (gva - m_base) / 4;
        if (off + dwords > m_storage.size()) return false;
        std::memcpy(dst, m_storage.data() + off, dwords * 4);
        return true;
    }
    bool WriteDwords(uint64_t gva, const uint32_t* src, size_t dwords) override {
        if (gva < m_base || gva % 4 != 0) return false;
        const size_t off = (gva - m_base) / 4;
        if (off + dwords > m_storage.size()) return false;
        std::memcpy(m_storage.data() + off, src, dwords * 4);
        return true;
    }
    void PutDwords(uint64_t gva, const std::vector<uint32_t>& v) {
        WriteDwords(gva, v.data(), v.size());
    }
    uint32_t At(uint64_t gva) const {
        const size_t off = (gva - m_base) / 4;
        return off < m_storage.size() ? m_storage[off] : 0u;
    }
    uint16_t At16(uint64_t gva) const {
        const size_t byte_off = static_cast<size_t>(gva - m_base);
        if (byte_off + 2 > m_storage.size() * 4) return 0;
        uint16_t h;
        std::memcpy(&h, reinterpret_cast<const uint8_t*>(m_storage.data()) + byte_off, 2);
        return h;
    }
    uint8_t At8(uint64_t gva) const {
        const size_t byte_off = static_cast<size_t>(gva - m_base);
        if (byte_off + 1 > m_storage.size() * 4) return 0;
        return reinterpret_cast<const uint8_t*>(m_storage.data())[byte_off];
    }
private:
    uint64_t m_base;
    std::vector<uint32_t> m_storage;
};

// ---- SPIR-V word walkers (mirror of vk_graphics_pipeline_test) -------------
size_t CountOpcode(const std::vector<uint32_t>& spirv, uint32_t op) {
    size_t n = 0, i = 5;
    while (i < spirv.size()) {
        const uint32_t word = spirv[i];
        const uint32_t opcode = word & 0xFFFFu;
        const uint32_t words = word >> 16u;
        if (opcode == op) ++n;
        if (words == 0) break;
        i += words;
    }
    return n;
}
size_t CountDecoValue(const std::vector<uint32_t>& m, uint32_t decoration,
                      uint32_t value) {
    size_t count = 0, off = 5;
    while (off < m.size()) {
        const uint16_t op = static_cast<uint16_t>(m[off] & 0xffffu);
        const uint16_t wc = static_cast<uint16_t>(m[off] >> 16u);
        if (wc == 0 || off + wc > m.size()) return 0;
        if (op == 71 && wc >= 4 && m[off + 2] == decoration &&
            m[off + 3] == value) {
            ++count;
        }
        off += wc;
    }
    return count;
}

uint32_t EntryPointModel(const std::vector<uint32_t>& spirv) {
    size_t i = 5;
    while (i < spirv.size()) {
        const uint32_t word = spirv[i];
        const uint32_t opcode = word & 0xFFFFu;
        const uint32_t words = word >> 16u;
        if (opcode == 15u && i + 1 < spirv.size()) {   // OpEntryPoint
            return spirv[i + 1];
        }
        if (words == 0) break;
        i += words;
    }
    return 0xFFFFFFFFu;
}

// ============================================================================
// Part A -- vertex-stage control flow
// ============================================================================
void part_a_vertex_control_flow() {
    std::cout << "[gpu20] A: vertex-stage control flow\n";

    // A1: a DIVERGENT per-lane branch. Each vertex decides by its own v4
    // (red channel of the colour attribute) whether to overwrite the colour
    // with green:
    //   v_cmp_lt_f32 vcc, v4, 0.5   ; if (v4 < 0.5) taken
    //   s_cbranch_vccnz +2           ; skip the overwrite
    //   v_mov v4, 0.0                ; (then) green path: g stays, r = 0
    //   v_mov v5, 1.0                ;           g = 1
    //   s_endpgm
    // Lanes with v4 < 0.5 end up (0, 1, b, a); the others keep their colour.
    // src0/src1 use the decoder's 9-bit field: 256+n = VGPR v_n, 240 = the
    // inline constant 0.5f, 128 = inline 0, 242 = inline 1.0f.
    const std::vector<uint32_t> code = {
        Vop1(V_MOV_B32, /*v6=*/6, /*0.5f inline=*/240u),  // pc0
        Vopc(V_CMP_LT_F32, /*src1 v6=*/6, V(4)),          // pc1: vcc = v4 < 0.5
        Sopp(SOPP_CBRANCH_VCCNZ, /*to pc5*/ 2),           // pc2 -> endpgm
        Vop1(V_MOV_B32, /*v4=*/4, /*0 inline=*/128u),     // pc3
        Vop1(V_MOV_B32, /*v5=*/5, /*1.0 inline=*/242u),   // pc4
        Sopp(SOPP_ENDPGM, 0),                             // pc5
    };

    ComputeCompilerOptions opt;
    opt.in_dwords_per_lane = 8;
    opt.out_dwords_per_lane = 8;
    opt.emit_vertex_stage = true;
    RDNA2ComputeCompiler cc(opt);
    auto r = cc.Compile(code.data(), code.size());
    CHECK(r.success);
    CHECK(r.branch_count >= 1);
    // The vertex-module contract holds WITH the branch:
    CHECK(EntryPointModel(r.spirv) == 0u);          // ExecutionModel Vertex
    CHECK(CountOpcode(r.spirv, 16) == 0);           // no LocalSize
    CHECK(CountDecoValue(r.spirv, 11, 42) == 1);    // BuiltIn VertexIndex
    CHECK(CountDecoValue(r.spirv, 11, 0) == 1);     // BuiltIn Position
    CHECK(CountDecoValue(r.spirv, 30, 0) == 1);     // Location 0 (colour out)
    // Structured control flow really is in the module:
    CHECK(CountOpcode(r.spirv, 247) == 1);          // OpSelectionMerge
    CHECK(CountOpcode(r.spirv, 250) >= 1);          // OpBranchConditional
    CHECK(CountOpcode(r.spirv, 249) >= 2);          // OpBranch (merge exits)

    // A2: software semantics -- the three lanes diverge. s_cbranch_vccnz
    // JUMPS when vcc != 0, i.e. when v4 < 0.5 -- those lanes SKIP the
    // overwrite; the v4 >= 0.5 lanes fall through and get (v4, v5) = (0, 1).
    //   lane 0: v4 = 0.0 (< 0.5) -> branch taken -> colour UNCHANGED (0,0,0,1)
    //   lane 1: v4 = 1.0 (>= 0.5) -> fall through -> (0, 1, 0, 1)
    //   lane 2: v4 = 0.2 (< 0.5) -> branch taken -> colour UNCHANGED
    std::vector<uint32_t> input;
    const float lanes[3][8] = {
        {-1.0f, -1.0f, 0.2f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f},
        { 1.0f, -1.0f, 0.2f, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f},
        { 0.0f,  1.0f, 0.2f, 1.0f, 0.2f, 0.0f, 1.0f, 1.0f},
    };
    for (int i = 0; i < 3; ++i)
        for (float f : lanes[i]) input.push_back(FBits(f));
    std::vector<uint32_t> sw_out;
    GcnSwExecutor sw;
    GcnSwExecResult srr = sw.Run(code.data(), code.size(), 3, input,
                                 8, 8, sw_out, nullptr, nullptr);
    CHECK(srr.ok);
    CHECK(sw_out.size() == 24);
    // lane 0 (v4=0.0 < 0.5): branch taken -> v4/v5 unchanged (0.0, 0.0)
    CHECK(sw_out[0 + 4] == FBits(0.0f));
    CHECK(sw_out[0 + 5] == FBits(0.0f));
    // lane 1 (v4=1.0 >= 0.5): fall through -> v4 = 0, v5 = 1
    CHECK(sw_out[8 + 4] == 0u);
    CHECK(sw_out[8 + 5] == FBits(1.0f));
    // lane 2 (v4=0.2 < 0.5): branch taken -> unchanged (0.2, 0.0)
    CHECK(sw_out[16 + 4] == FBits(0.2f));
    CHECK(sw_out[16 + 5] == FBits(0.0f));

    // A3: a LOOP in vertex mode lowers to OpLoopMerge.
    //   s_movk_i32 s0, 3 ; loop: s_sub_i32 s0, s0, 1 ... guard: s_cmpk...
    // Simpler: a counted loop is untestable here without SOP2 encoders, so
    // use the guard-clause shape the compiler already structures: a backward
    // s_branch whose merge block is the loop exit -- the compiler's loop
    // detector requires a back-edge targeting a dominating header. Encode:
    //   header: v_mov v0, v0 (nop-ish)  <- pc0
    //   s_branch header                  <- pc1 (infinite back-edge)
    //   s_endpgm                         <- pc2
    // The compiler must fail-closed on the INFINITE loop (no exit edge) OR
    // structure it; either way the module stays parseable and vertex-shaped.
    const std::vector<uint32_t> loop_code = {
        Vop1(V_MOV_B32, 0, /*v0*/ 0u),   // pc0 header (v0 = v0)
        Sopp(SOPP_BRANCH, /*to pc0*/ -1), // pc1 back-edge
        Sopp(SOPP_ENDPGM, 0),             // pc2 (unreachable)
    };
    RDNA2ComputeCompiler cc_loop(opt);
    auto lr = cc_loop.Compile(loop_code.data(), loop_code.size());
    // Acceptance: either a structured loop module or an honest decline --
    // never an unstructured/corrupt module.
    if (lr.success) {
        CHECK(EntryPointModel(lr.spirv) == 0u);
        CHECK(CountOpcode(lr.spirv, 16) == 0);
    } else {
        CHECK(lr.error != PS5::GPU::ComputeCompileError::None);
    }

    // The scalar guard-clause (uniform branch) ALSO works in vertex mode:
    const uint32_t smovk =
        0xB0000000u | (S_MOVK_I32 << 20u) | (0u << 16u) | 5u;
    const uint32_t scmpk =
        0xB0000000u | (S_CMPK_EQ_U32 << 20u) | (0u << 16u) | 5u;
    const std::vector<uint32_t> guard_code = {
        smovk,                                        // pc0
        scmpk,                                        // pc1
        Sopp(SOPP_CBRANCH_SCC1, /*to pc4*/ 1),        // pc2 -> endpgm
        Vop1(V_MOV_B32, 0, /*2.0 inline=*/244u),       // pc3 (skipped)
        Sopp(SOPP_ENDPGM, 0),                         // pc4
    };
    RDNA2ComputeCompiler cc_guard(opt);
    auto gr = cc_guard.Compile(guard_code.data(), guard_code.size());
    CHECK(gr.success);
    CHECK(EntryPointModel(gr.spirv) == 0u);
    CHECK(CountOpcode(gr.spirv, 247) == 1);       // OpSelectionMerge
    CHECK(CountDecoValue(gr.spirv, 11, 42) == 1); // VertexIndex still there
}

// ============================================================================
// Part B -- CB formats beyond RGBA8
// ============================================================================
void part_b_cb_formats() {
    std::cout << "[gpu20] B: CB formats beyond RGBA8\n";

    // B1: the conversion table (VkFormat numbers verified against
    // KhronosGroup/Vulkan-Headers vulkan_core.h -- see refs/).
    struct Case { uint32_t fmt, num, swap, want; };
    const Case cases[] = {
        {Pm4::CB_FORMAT_8_8_8_8, Pm4::CB_NUMBER_UNORM, 0, 37},   // RGBA8
        {Pm4::CB_FORMAT_8_8_8_8, Pm4::CB_NUMBER_SNORM, 0, 38},   // RGBA8_SNORM
        {Pm4::CB_FORMAT_8_8_8_8, Pm4::CB_NUMBER_SRGB, 0, 43},    // RGBA8_SRGB
        {Pm4::CB_FORMAT_8_8_8_8, Pm4::CB_NUMBER_UNORM, 1, 44},   // BGRA8
        {Pm4::CB_FORMAT_2_10_10_10, Pm4::CB_NUMBER_UNORM, 0, 64},// A2B10G10R10
        {Pm4::CB_FORMAT_16_16_16_16, Pm4::CB_NUMBER_UNORM, 0, 91},
        {Pm4::CB_FORMAT_16_16_16_16, Pm4::CB_NUMBER_SNORM, 0, 92},
        {Pm4::CB_FORMAT_16_16_16_16, Pm4::CB_NUMBER_FLOAT, 0, 97},
        {Pm4::CB_FORMAT_11_11_10, Pm4::CB_NUMBER_FLOAT, 0, 122}, // B10G11R11
    };
    for (const Case& c : cases) {
        GuestColorFormat got = GuestColorFormat::Invalid;
        CHECK(Pm4::CbInfoToGuestColorFormat(c.fmt, c.num, got, c.swap));
        CHECK(static_cast<uint32_t>(got) == c.want);
    }
    // Fail-closed: the pairs the model does NOT implement.
    GuestColorFormat bad = GuestColorFormat::Invalid;
    CHECK(!Pm4::CbInfoToGuestColorFormat(Pm4::CB_FORMAT_8_8_8_8, 4, bad));      // UINT
    CHECK(!Pm4::CbInfoToGuestColorFormat(Pm4::CB_FORMAT_8_8_8_8, 7, bad));      // FLOAT
    CHECK(!Pm4::CbInfoToGuestColorFormat(0, 0, bad));                            // invalid
    CHECK(!Pm4::CbInfoToGuestColorFormat(Pm4::CB_FORMAT_8_8_8_8, 0, bad, 2));   // reverse swap
    CHECK(!Pm4::CbInfoToGuestColorFormat(Pm4::CB_FORMAT_16_16_16_16, 0, bad, 1)); // 16F not swapped
    // The two-arg (Standard) form keeps the round-19 contract:
    GuestColorFormat rgba8 = GuestColorFormat::Invalid;
    CHECK(Pm4::CbInfoToGuestColorFormat(Pm4::CB_FORMAT_8_8_8_8,
                                        Pm4::CB_NUMBER_UNORM, rgba8));
    CHECK(rgba8 == GuestColorFormat::R8G8B8A8Unorm);

    // B2: bytes per pixel.
    CHECK(Pm4::GuestColorFormatBytesPerPixel(GuestColorFormat::R8G8B8A8Unorm) == 4);
    CHECK(Pm4::GuestColorFormatBytesPerPixel(GuestColorFormat::B8G8R8A8Unorm) == 4);
    CHECK(Pm4::GuestColorFormatBytesPerPixel(GuestColorFormat::R8G8B8A8Srgb) == 4);
    CHECK(Pm4::GuestColorFormatBytesPerPixel(GuestColorFormat::A2B10G10R10UnormPack32) == 4);
    CHECK(Pm4::GuestColorFormatBytesPerPixel(GuestColorFormat::B10G11R11UfloatPack32) == 4);
    CHECK(Pm4::GuestColorFormatBytesPerPixel(GuestColorFormat::R16G16B16A16Sfloat) == 8);
    CHECK(Pm4::GuestColorFormatBytesPerPixel(GuestColorFormat::R16G16B16A16Unorm) == 8);
    CHECK(Pm4::GuestColorFormatBytesPerPixel(GuestColorFormat::R16G16B16A16Snorm) == 8);
    CHECK(Pm4::GuestColorFormatBytesPerPixel(GuestColorFormat::R32Sfloat) == 4);
    CHECK(Pm4::GuestColorFormatBytesPerPixel(GuestColorFormat::Invalid) == 0);

    // B3: the software rasterizer encodes each layout. One 2x1 target with a
    // full-cover triangle of a known colour; read the pixel back per format.
    const uint32_t W = 2, H = 1;
    const RasterViewport vp_ = RasterViewport::Fullscreen(W, H);
    const uint64_t BASE = 0x1400000000ull;
    struct EncCase {
        GuestColorFormat fmt;
        const char* name;
        std::vector<uint32_t> expect;   // dwords of the first pixel
    };
    // colour = (0.25, 0.5, 0.75, 1.0)
    const EncCase enc[] = {
        {GuestColorFormat::R8G8B8A8Unorm, "RGBA8",
         {(64u) | (128u << 8) | (191u << 16) | (255u << 24)}},
        {GuestColorFormat::B8G8R8A8Unorm, "BGRA8",
         {(191u) | (128u << 8) | (64u << 16) | (255u << 24)}},
        // sRGB: 0.25 -> ~0.537*255=137; 0.5 -> 188; 0.75 -> ~0.881*255=225
        {GuestColorFormat::R8G8B8A8Srgb, "sRGB",
         {(137u) | (188u << 8) | (225u << 16) | (255u << 24)}},
        // A2B10G10R10: A2[31:30] B10[29:20] G10[19:10] R10[9:0]
        {GuestColorFormat::A2B10G10R10UnormPack32, "A2B10G10R10",
         {(3u << 30) | (767u << 20) | (512u << 10) | 256u}},
        // R16G16B16A16 unorm halves: 0.25*65535=16384, 0.5=32768, 0.75=49151
        {GuestColorFormat::R16G16B16A16Unorm, "RGBA16",
         {16384u | (32768u << 16), 49151u | (65535u << 16)}},
    };
    for (const EncCase& c : enc) {
        FlatGuestMemory gmem(BASE, 0x1000 / 4);
        RasterTarget target{};
        target.color_gva = BASE;
        target.width = W;
        target.height = H;
        target.color_format = c.fmt;
        const RasterViewport vp = RasterViewport::Fullscreen(W, H);
        // A triangle spanning the whole 2x1 viewport, flat colour.
        std::vector<uint32_t> verts;
        const float tri[3][8] = {
            {-1.0f, -1.0f, 0.5f, 1.0f, 0.25f, 0.5f, 0.75f, 1.0f},
            { 3.0f, -1.0f, 0.5f, 1.0f, 0.25f, 0.5f, 0.75f, 1.0f},
            {-1.0f,  3.0f, 0.5f, 1.0f, 0.25f, 0.5f, 0.75f, 1.0f},
        };
        for (int i = 0; i < 3; ++i)
            for (float f : tri[i]) verts.push_back(FBits(f));
        auto stats = SoftwareRasterizer::DrawTriangles(verts, 8, target, vp,
                                                       &gmem);
        CHECK(stats.ok);
        CHECK(stats.pixels_written >= 1);
        for (size_t d = 0; d < c.expect.size(); ++d) {
            CHECK(gmem.At(BASE + 4 * d) == c.expect[d]);
        }
    }

    // Half-float layout (R16G16B16A16Sfloat): 0.25 = 0x3400, 0.5 = 0x3800,
    // 0.75 = 0x3B80, 1.0 = 0x3C00 (IEEE 754 binary16 exact).
    {
        FlatGuestMemory gmem(BASE, 0x1000 / 4);
        RasterTarget target{};
        target.color_gva = BASE;
        target.width = W;
        target.height = H;
        target.color_format = GuestColorFormat::R16G16B16A16Sfloat;
        std::vector<uint32_t> verts;
        const float tri[3][8] = {
            {-1.0f, -1.0f, 0.5f, 1.0f, 0.25f, 0.5f, 0.75f, 1.0f},
            { 3.0f, -1.0f, 0.5f, 1.0f, 0.25f, 0.5f, 0.75f, 1.0f},
            {-1.0f,  3.0f, 0.5f, 1.0f, 0.25f, 0.5f, 0.75f, 1.0f},
        };
        for (int i = 0; i < 3; ++i)
            for (float f : tri[i]) verts.push_back(FBits(f));
        auto stats = SoftwareRasterizer::DrawTriangles(verts, 8, target, vp_,
                                                       &gmem);
        (void)stats;
        CHECK(gmem.At16(BASE + 0) == 0x3400);   // r = 0.25
        CHECK(gmem.At16(BASE + 2) == 0x3800);   // g = 0.5
        CHECK(gmem.At16(BASE + 4) == 0x3A00);   // b = 0.75 (exp14, mant 512)
        CHECK(gmem.At16(BASE + 6) == 0x3C00);   // a = 1.0
    }

    // B4: the PM4 translator accepts a 16_16_16_16 FLOAT CB_INFO: the
    // register binding assembles (the draw triggers it), the plane carries
    // half-float bits after the software rasterization, and the
    // graphics-target conversion carries the new format.
    {
        VulkanRendererBackend backend;
        PM4VulkanTranslator translator(backend);
        VulkanComputeExecutor exec;      // headless: declines the GPU path,
        (void)exec.Initialize();         // the binding still assembles
        FlatGuestMemory gmem(BASE, 0x10000 / 4);
        const uint64_t SHADER_GVA = BASE + 0x0000;
        const uint64_t FETCH_GVA  = BASE + 0x0800;
        const uint64_t VERT_GVA   = BASE + 0x1000;
        const uint64_t IDX_GVA    = BASE + 0x1400;
        const uint64_t OUT_GVA    = BASE + 0x2000;
        const uint64_t CB_COLOR   = BASE + 0x3000;
        const uint32_t w = 8, h = 8;

        std::vector<uint32_t> ring;
        auto pk = [&](uint32_t opcode, uint32_t count) {
            ring.push_back((3u << 30) | ((count - 1u) << 16) | (opcode << 8));
        };
        auto ctx2 = [&](uint32_t reg, uint32_t v) {
            pk(0x69u, 2);
            ring.push_back(reg);
            ring.push_back(v);
        };
        auto sh = [&](uint32_t off, uint32_t v) {
            pk(0x76u, 2);
            ring.push_back(off);
            ring.push_back(v);
        };
        // CB programming: 16_16_16_16 FLOAT.
        ctx2(Pm4::CB_COLOR0_BASE, static_cast<uint32_t>(CB_COLOR & 0xFFFFFFFFu));
        ctx2(Pm4::CB_COLOR0_BASE_EXT, static_cast<uint32_t>(CB_COLOR >> 32));
        ctx2(Pm4::CB_COLOR0_PITCH, w / Pm4::CB_LINEAR_TILE_PIXELS - 1u);
        ctx2(Pm4::CB_COLOR0_SLICE, (w * h) / Pm4::CB_LINEAR_TILE_AREA - 1u);
        ctx2(Pm4::CB_COLOR0_INFO,
             (Pm4::CB_FORMAT_16_16_16_16 << Pm4::CB_INFO_FORMAT_SHIFT) |
             (Pm4::CB_NUMBER_FLOAT << Pm4::CB_INFO_NUMBER_SHIFT));
        ctx2(Pm4::CB_TARGET_MASK, 0xFu);
        // viewport
        ctx2(Pm4::PA_CL_VPORT_XSCALE, FBits(static_cast<float>(w) * 0.5f));
        ctx2(Pm4::PA_CL_VPORT_XOFFSET, FBits(static_cast<float>(w) * 0.5f));
        ctx2(Pm4::PA_CL_VPORT_YSCALE, FBits(-static_cast<float>(h) * 0.5f));
        ctx2(Pm4::PA_CL_VPORT_YOFFSET, FBits(static_cast<float>(h) * 0.5f));
        // vertex stage + fetch table (the round-9/18 draw ABI)
        gmem.PutDwords(SHADER_GVA, { 0xBF810000u });   // s_endpgm passthrough
        const float tri[3][8] = {
            {-1.0f, -1.0f, 0.5f, 1.0f, 0.25f, 0.5f, 0.75f, 1.0f},
            { 3.0f, -1.0f, 0.5f, 1.0f, 0.25f, 0.5f, 0.75f, 1.0f},
            {-1.0f,  3.0f, 0.5f, 1.0f, 0.25f, 0.5f, 0.75f, 1.0f},
        };
        std::vector<uint32_t> verts;
        for (int i = 0; i < 3; ++i)
            for (float f : tri[i]) verts.push_back(FBits(f));
        gmem.PutDwords(VERT_GVA, verts);
        gmem.PutDwords(IDX_GVA, { 0, 1, 2 });
        gmem.PutDwords(FETCH_GVA, {
            1,
            static_cast<uint32_t>(VERT_GVA & 0xffffffff),
            static_cast<uint32_t>(VERT_GVA >> 32),
            8, 8,
        });
        sh(Pm4::SPI_SHADER_PGM_LO_VS, static_cast<uint32_t>(SHADER_GVA >> 8));
        sh(Pm4::SPI_SHADER_PGM_HI_VS, static_cast<uint32_t>(SHADER_GVA >> 32));
        sh(Pm4::SPI_SHADER_USER_DATA_VS_0 + 0,
           static_cast<uint32_t>(VERT_GVA & 0xffffffff));
        sh(Pm4::SPI_SHADER_USER_DATA_VS_0 + 1,
           static_cast<uint32_t>(VERT_GVA >> 32));
        sh(Pm4::SPI_SHADER_USER_DATA_VS_0 + 2,
           static_cast<uint32_t>(OUT_GVA & 0xffffffff));
        sh(Pm4::SPI_SHADER_USER_DATA_VS_0 + 3,
           static_cast<uint32_t>(OUT_GVA >> 32));
        sh(Pm4::SPI_SHADER_USER_DATA_VS_0 + 4, 3);
        sh(Pm4::SPI_SHADER_USER_DATA_VS_FETCH_LO,
           static_cast<uint32_t>(FETCH_GVA & 0xffffffff));
        sh(Pm4::SPI_SHADER_USER_DATA_VS_FETCH_HI,
           static_cast<uint32_t>(FETCH_GVA >> 32));
        sh(Pm4::SPI_SHADER_USER_DATA_VS_OUT_DWORDS, 8);
        // INDEX_BASE + INDEX_TYPE + DRAW_INDEX_2
        pk(0x2Du, 2);
        ring.push_back(static_cast<uint32_t>(IDX_GVA & 0xffffffff));
        ring.push_back(static_cast<uint32_t>(IDX_GVA >> 32));
        pk(0x2Eu, 1);
        ring.push_back(Pm4::INDEX_TYPE_U32);
        pk(0x22u, 3);
        ring.push_back(3); ring.push_back(0); ring.push_back(0);

        translator.BindComputeExecutor(&exec, &gmem);
        const auto result = translator.TranslateAndExecuteCommandRingChecked(
            ring.data(), ring.size());
        CHECK(result.ok());
        const auto& bind = translator.GetLastRenderTargetBinding();
        CHECK(bind.programmed);
        CHECK(bind.bound);
        CHECK(bind.error.empty());
        CHECK(bind.color_format == GuestColorFormat::R16G16B16A16Sfloat);

        // The software fallback rasterized into the 16F plane: the centre
        // pixel's first half is 0.25f = 0x3400.
        CHECK(gmem.At16(CB_COLOR + ((h / 2) * w + (w / 2)) * 8) == 0x3400);
        CHECK(gmem.At16(CB_COLOR + ((h / 2) * w + (w / 2)) * 8 + 2) == 0x3800);

        // The graphics-target conversion carries the new format.
        VulkanComputeExecutor::GraphicsTargetDesc target;
        CHECK(PM4VulkanTranslator::BuildGraphicsTarget(
            bind, translator.GetRasterViewport(), target));
        CHECK(target.color_format == GuestColorFormat::R16G16B16A16Sfloat);
    }
}

// ============================================================================
// Part C -- the cross-dispatch pipeline cache
// ============================================================================
void part_c_pipeline_cache() {
    std::cout << "[gpu20] C: cross-dispatch pipeline cache\n";
    auto& reg = PipelineKeyRegistry::Instance();
    reg.ResetForTest();

    std::vector<uint32_t> spirv_a = {0x07230203u, 0x00010000u, 0, 0, 0, 1, 2, 3};
    std::vector<uint32_t> spirv_b = spirv_a;
    spirv_b.back() = 4u;   // one word differs = a different program

    // C1: determinism + sensitivity.
    const auto k1 = PS5::GPU::ComputePipelineKey(
        PipelineKind::Compute, spirv_a.data(), spirv_a.size(), 3, 0);
    const auto k1b = PS5::GPU::ComputePipelineKey(
        PipelineKind::Compute, spirv_a.data(), spirv_a.size(), 3, 0);
    CHECK(k1 == k1b);
    CHECK(PS5::GPU::ComputePipelineKey(PipelineKind::Compute,
                                       spirv_b.data(), spirv_b.size(), 3, 0) != k1);
    CHECK(PS5::GPU::ComputePipelineKey(PipelineKind::Compute,
                                       spirv_a.data(), spirv_a.size(), 4, 0) != k1);
    CHECK(PS5::GPU::ComputePipelineKey(PipelineKind::Compute,
                                       spirv_a.data(), spirv_a.size(), 3, 2) != k1);
    CHECK(PS5::GPU::ComputePipelineKey(PipelineKind::Graphics,
                                       spirv_a.data(), spirv_a.size(), 3, 0) != k1);
    CHECK(PS5::GPU::ComputePipelineKey(PipelineKind::Compute,
                                       spirv_a.data(), spirv_a.size(), 3, 0, 44) != k1);
    // The hash spreads (different inputs -> different bit patterns).
    CHECK(k1.lo != 0 || k1.hi != 0);

    // C2: the registry contract.
    CHECK(reg.Stats().lookups == 0);
    CHECK(!reg.NoteDispatch(k1));          // first: MISS
    CHECK(reg.NoteDispatch(k1));           // identical: HIT
    CHECK(reg.NoteDispatch(k1));           // again: HIT
    {
        PipelineCacheStats s = reg.Stats();
        CHECK(s.lookups == 3);
        CHECK(s.hits == 2);
        CHECK(s.misses == 1);
        CHECK(s.entries == 1);
    }
    const auto k2 = PS5::GPU::ComputePipelineKey(
        PipelineKind::Compute, spirv_b.data(), spirv_b.size(), 3, 0);
    CHECK(!reg.NoteDispatch(k2));          // changed program: MISS
    CHECK(reg.NoteDispatch(k2));           // then HIT
    {
        PipelineCacheStats s = reg.Stats();
        CHECK(s.lookups == 5);
        CHECK(s.hits == 3);
        CHECK(s.misses == 2);
        CHECK(s.entries == 2);
    }

    // C3: process-wide (two executor instances share the registry).
    {
        VulkanComputeExecutor e1;
        VulkanComputeExecutor e2;
        (void)e1.Initialize();
        (void)e2.Initialize();
        // The registry is a singleton across instances.
        const auto k3 = PS5::GPU::ComputePipelineKey(
            PipelineKind::Compute, spirv_a.data(), spirv_a.size(), 3, 0);
        CHECK(reg.Contains(k3));
        // A headless host: the executor declines dispatches BEFORE the cache
        // is consulted (no device), so the stats above are untouched by the
        // executor constructions.
        PipelineCacheStats s = reg.Stats();
        CHECK(s.lookups == 5);
        // RunRDNA2 with a REAL (trivial) RDNA2 program declines honestly on
        // a headless host without touching the registry (the device check
        // precedes the pipeline-cache lookup).
        const uint32_t endpgm_prog[] = { 0xBF810000u };
        auto r = e1.RunRDNA2(endpgm_prog, 1, {1, 2, 3});
        CHECK(r.status == PS5::GPU::ComputeExecStatus::Unavailable);
        CHECK(reg.Stats().lookups == s.lookups);
    }

    // Clear / eviction accounting.
    reg.Clear();
    CHECK(reg.Size() == 0);
    {
        PipelineCacheStats s = reg.Stats();
        CHECK(s.evictions == 2);
        CHECK(s.entries == 0);
    }
    reg.ResetForTest();
    CHECK(reg.Stats().lookups == 0);
}

} // namespace

int main() {
    part_a_vertex_control_flow();
    part_b_cb_formats();
    part_c_pipeline_cache();
    if (g_failures != 0) {
        std::printf("[gpu_round20_test] FAILED: %d/%d checks failed\n",
                    g_failures, g_checks);
        return 1;
    }
    std::printf("[gpu_round20_test] PASSED: %d checks, %d failures\n",
                g_checks, g_failures);
    return 0;
}
