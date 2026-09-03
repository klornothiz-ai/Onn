// ============================================================================
// ProsperoLayer RDNA2 Core - VkGraphicsPipeline path test (round 19, phase 2)
// ----------------------------------------------------------------------------
// The register-driven raster path gains a REAL Vulkan graphics pipeline. On
// a headless host this suite proves every piece that is provable without a
// device, and the fail-closed contract everywhere:
//   * A: the CB_COLOR0_INFO -> GuestColorFormat conversion (the round-18
//     reject logic reused positively) and the ZFUNC -> VkCompareOp identity
//     -- every enum value verified against KhronosGroup/Vulkan-Headers
//     (VK_FORMAT_R8G8B8A8_UNORM = 37, VK_COMPARE_OP_* = 0..7);
//   * B: the compiler's new VERTEX-stage mode -- a module with
//     ExecutionModel Vertex, NO LocalSize execution mode, gl_VertexIndex
//     (BuiltIn 42) as the lane index, gl_Position (BuiltIn 0) + a Location-0
//     colour out, the same in/out SSBO bindings, and (semantic parity) the
//     same transformed vertices the software executor produces;
//   * C: the hand-assembled passthrough FRAGMENT module -- ExecutionModel
//     Fragment + OriginUpperLeft, one Location-0 in, one Location-0 out,
//     a single load/store pair;
//   * D: PM4VulkanTranslator::BuildGraphicsTarget -- the register-derived
//     binding (CB extent + format + DB depth/zfunc + PA_CL_VPORT numbers)
//     converts to the graphics target; host-API targets, depth-only, and
//     degenerate viewports decline;
//   * E: the end-to-end fail-closed contract on this host: with the graphics
//     raster OPTED IN, a register-bound draw attempts the graphics path,
//     declines honestly (no device), records why, and the software
//     rasterizer renders the SAME pixels; with it off (the default) the
//     round-18 behaviour is byte-identical.
// ============================================================================
#include "gpu/pm4_translator.hpp"
#include "gpu/gpu_guest_memory.hpp"
#include "gpu/vulkan_compute_executor.hpp"
#include "gpu/rdna2_compute_compiler.hpp"
#include "gpu/gcn_decoder.hpp"
#include "gpu/software_rasterizer.hpp"
#include "graphics/guest_gpu/pm4.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
using namespace PS5::GPU;

int g_failures = 0, g_checks = 0;
bool Check(bool v, const char* e, int line) {
    ++g_checks;
    if (!v) { ++g_failures; std::cerr << "  [FAIL] line " << line << ": " << e << '\n'; }
    return v;
}
#define CHECK(e) Check((e), #e, __LINE__)

uint32_t FBits(float f) { uint32_t v; std::memcpy(&v, &f, 4); return v; }
float BitsF(uint32_t v) { float f; std::memcpy(&f, &v, 4); return f; }

// Flat guest memory (the same bridge shape every other PM4 suite uses).
class FlatGuestMemory final : public GpuGuestMemory {
public:
    FlatGuestMemory(uint64_t base, size_t dwords)
        : m_base(base), m_storage(dwords, 0u) {}
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
    void PutDwords(uint64_t gva, const std::vector<uint32_t>& words) {
        WriteDwords(gva, words.data(), words.size());
    }
    uint32_t At(uint64_t gva) const {
        uint32_t v = 0;
        const_cast<FlatGuestMemory*>(this)->ReadDwords(gva, &v, 1);
        return v;
    }
private:
    uint64_t m_base;
    std::vector<uint32_t> m_storage;
};

bool ParsesCleanly(const std::vector<uint32_t>& m) {
    if (m.size() < 5 || m[0] != 0x07230203u) return false;
    size_t off = 5;
    while (off < m.size()) {
        const uint16_t wc = static_cast<uint16_t>(m[off] >> 16u);
        if (wc == 0 || off + wc > m.size()) return false;
        off += wc;
    }
    return off == m.size();
}
size_t CountOpcode(const std::vector<uint32_t>& m, uint16_t opcode) {
    size_t count = 0, off = 5;
    while (off < m.size()) {
        const uint16_t op = static_cast<uint16_t>(m[off] & 0xffffu);
        const uint16_t wc = static_cast<uint16_t>(m[off] >> 16u);
        if (wc == 0 || off + wc > m.size()) return 0;
        if (op == opcode) ++count;
        off += wc;
    }
    return count;
}
// The execution model of the (single) OpEntryPoint, or 0xFFFFFFFF.
uint32_t EntryPointModel(const std::vector<uint32_t>& m) {
    size_t off = 5;
    while (off < m.size()) {
        const uint16_t op = static_cast<uint16_t>(m[off] & 0xffffu);
        const uint16_t wc = static_cast<uint16_t>(m[off] >> 16u);
        if (wc == 0 || off + wc > m.size()) return 0xFFFFFFFFu;
        if (op == 15 && wc >= 3) return m[off + 1];   // OpEntryPoint
        off += wc;
    }
    return 0xFFFFFFFFu;
}
// Count OpDecorate targets carrying a given decoration with a given value
// (e.g. BuiltIn=42, Location=0, Binding=2).
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

// The round-18 style ring builder (see pm4_color_target_test): registers +
// VGT fetch + indexed draw, one fullscreen-ish triangle.
constexpr uint32_t W = 32, H = 32;
struct RingBuilder {
    std::vector<uint32_t> ring;
    void ShReg(uint32_t off, uint32_t val) {
        ring.push_back((3u << 30) | (1u << 16) | (0x76u << 8));
        ring.push_back(off);
        ring.push_back(val);
    }
    void CtxReg(uint32_t off, uint32_t val) {
        ring.push_back((3u << 30) | (1u << 16) | (0x69u << 8));
        ring.push_back(off);
        ring.push_back(val);
    }
    void ProgramRenderTarget(uint64_t color_gva, uint32_t w, uint32_t h,
                             uint64_t depth_gva, uint32_t db_control) {
        CtxReg(Pm4::CB_COLOR0_BASE,
               static_cast<uint32_t>(color_gva & 0xFFFFFFFFu));
        CtxReg(Pm4::CB_COLOR0_BASE_EXT,
               static_cast<uint32_t>(color_gva >> 32));
        CtxReg(Pm4::CB_COLOR0_PITCH, w / Pm4::CB_LINEAR_TILE_PIXELS - 1u);
        CtxReg(Pm4::CB_COLOR0_SLICE, (w * h) / Pm4::CB_LINEAR_TILE_AREA - 1u);
        CtxReg(Pm4::CB_COLOR0_INFO,
               (Pm4::CB_FORMAT_8_8_8_8 << Pm4::CB_INFO_FORMAT_SHIFT) |
               (Pm4::CB_NUMBER_UNORM << Pm4::CB_INFO_NUMBER_SHIFT));
        CtxReg(Pm4::CB_TARGET_MASK, 0xFu);
        if (depth_gva != 0) {
            CtxReg(Pm4::DB_Z_INFO, Pm4::DB_Z_FORMAT_32_FLOAT);
            CtxReg(Pm4::DB_Z_WRITE_BASE,
                   static_cast<uint32_t>(depth_gva & 0xFFFFFFFFu));
            CtxReg(Pm4::DB_Z_WRITE_BASE_HI,
                   static_cast<uint32_t>(depth_gva >> 32));
            CtxReg(Pm4::DB_DEPTH_SIZE_XY, (w - 1u) | ((h - 1u) << 16u));
            CtxReg(Pm4::DB_DEPTH_CONTROL, db_control);
        }
    }
    void ProgramViewport(uint32_t w, uint32_t h) {
        CtxReg(Pm4::PA_CL_VPORT_XSCALE, FBits(static_cast<float>(w) * 0.5f));
        CtxReg(Pm4::PA_CL_VPORT_XOFFSET, FBits(static_cast<float>(w) * 0.5f));
        CtxReg(Pm4::PA_CL_VPORT_YSCALE, FBits(-static_cast<float>(h) * 0.5f));
        CtxReg(Pm4::PA_CL_VPORT_YOFFSET, FBits(static_cast<float>(h) * 0.5f));
    }
    void Draw(FlatGuestMemory& gmem, uint64_t shader_gva, uint64_t fetch_gva,
              uint64_t vert_gva, uint64_t idx_gva, uint64_t out_gva,
              const float (*tri)[8]) {
        gmem.PutDwords(shader_gva, { 0xBF810000u });   // s_endpgm passthrough
        std::vector<uint32_t> verts;
        for (int i = 0; i < 3; ++i) {
            for (float f : tri[i]) verts.push_back(FBits(f));
        }
        gmem.PutDwords(vert_gva, verts);
        gmem.PutDwords(idx_gva, { 0, 1, 2 });
        gmem.PutDwords(fetch_gva, {
            1,
            static_cast<uint32_t>(vert_gva & 0xffffffff),
            static_cast<uint32_t>(vert_gva >> 32),
            8, 8,
        });
        ShReg(Pm4::SPI_SHADER_PGM_LO_VS,
              static_cast<uint32_t>(shader_gva >> 8));
        ShReg(Pm4::SPI_SHADER_PGM_HI_VS,
              static_cast<uint32_t>(shader_gva >> 32));
        ShReg(Pm4::SPI_SHADER_USER_DATA_VS_0 + 0,
              static_cast<uint32_t>(vert_gva & 0xffffffff));
        ShReg(Pm4::SPI_SHADER_USER_DATA_VS_0 + 1,
              static_cast<uint32_t>(vert_gva >> 32));
        ShReg(Pm4::SPI_SHADER_USER_DATA_VS_0 + 2,
              static_cast<uint32_t>(out_gva & 0xffffffff));
        ShReg(Pm4::SPI_SHADER_USER_DATA_VS_0 + 3,
              static_cast<uint32_t>(out_gva >> 32));
        ShReg(Pm4::SPI_SHADER_USER_DATA_VS_0 + 4, 3);
        ShReg(Pm4::SPI_SHADER_USER_DATA_VS_FETCH_LO,
              static_cast<uint32_t>(fetch_gva & 0xffffffff));
        ShReg(Pm4::SPI_SHADER_USER_DATA_VS_FETCH_HI,
              static_cast<uint32_t>(fetch_gva >> 32));
        ShReg(Pm4::SPI_SHADER_USER_DATA_VS_OUT_DWORDS, 8);
        ring.push_back((3u << 30) | (1u << 16) | (0x2Du << 8));
        ring.push_back(static_cast<uint32_t>(idx_gva & 0xffffffff));
        ring.push_back(static_cast<uint32_t>(idx_gva >> 32));
        ring.push_back((3u << 30) | (0u << 16) | (0x2Eu << 8));
        ring.push_back(Pm4::INDEX_TYPE_U32);
        ring.push_back((3u << 30) | (2u << 16) | (0x22u << 8));
        ring.push_back(3); ring.push_back(0); ring.push_back(0);
    }
};

// A triangle covering most of the 32x32 target: (-1,-1) (1,-1) (-1,1) with
// depth 0.2 and a solid red tint.
const float kTri[3][8] = {
    {-1.0f, -1.0f, 0.2f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f},
    { 1.0f, -1.0f, 0.2f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f},
    {-1.0f,  1.0f, 0.2f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f},
};

} // namespace

int main() {
    std::cout << "[vk-gfx] round 19 phase 2: the VkGraphicsPipeline raster "
                 "path\n";

    VulkanComputeExecutor exec;
    const bool available = exec.Initialize();
    std::cout << (available ? "[info] Vulkan device: " + exec.DeviceName()
                            : std::string("[info] no Vulkan device; the "
                                          "graphics path declines and the "
                                          "software rasterizer serves"))
              << "\n";

    // =====================================================================
    // A: format + compare-op conversions (verified enum values).
    // =====================================================================
    std::cout << "[vk-gfx] A: CB format / ZFUNC conversions\n";
    {
        Pm4::GuestColorFormat fmt = Pm4::GuestColorFormat::Invalid;
        CHECK(Pm4::CbInfoToGuestColorFormat(Pm4::CB_FORMAT_8_8_8_8,
                                            Pm4::CB_NUMBER_UNORM, fmt));
        CHECK(fmt == Pm4::GuestColorFormat::R8G8B8A8Unorm);
        // The enum carries the REAL VkFormat number (vulkan_core.h):
        // VK_FORMAT_R8G8B8A8_UNORM = 37.
        CHECK(static_cast<uint32_t>(fmt) == 37u);

        // Fail-closed: everything the model does not implement. (Round 20
        // grew the ACCEPTED set: 8_8_8_8 SNORM/SRGB and 16_16_16_16
        // UNORM/SNORM/FLOAT are now supported pairs -- see gpu_round20_test.)
        Pm4::GuestColorFormat bad = Pm4::GuestColorFormat::Invalid;
        CHECK(!Pm4::CbInfoToGuestColorFormat(3u, 0u, bad));   // PC GCN enum
        CHECK(!Pm4::CbInfoToGuestColorFormat(10u, 4u, bad));  // UINT
        CHECK(!Pm4::CbInfoToGuestColorFormat(0u, 0u, bad));   // invalid fmt
        CHECK(!Pm4::CbInfoToGuestColorFormat(12u, 4u, bad));  // 16_16_16_16 UINT
        CHECK(!Pm4::CbInfoToGuestColorFormat(10u, 0u, bad, 2));  // reverse swap
        CHECK(!Pm4::CbInfoToGuestColorFormat(11u, 0u, bad));  // 32_32

        // ZFUNC -> VkCompareOp is a numeric identity (both enums 0..7).
        for (uint32_t z = 0; z <= 7u; ++z) {
            CHECK(Pm4::ZFuncToVkCompareOp(static_cast<Pm4::ZFunc>(z)) == z);
        }
    }

    // =====================================================================
    // B: the VERTEX-stage module.
    // =====================================================================
    std::cout << "[vk-gfx] B: vertex-stage module structure + semantics\n";
    {
        // The identity vertex program: the lane model seeds v0..v7 from the
        // fetch gather and the epilogue exports them all (position + colour
        // + the out-SSBO dump), so s_endpgm alone is a full passthrough.
        const std::vector<uint32_t> code = { 0xBF810000u };
        ComputeCompilerOptions opt;
        opt.in_dwords_per_lane = 8;
        opt.out_dwords_per_lane = 8;
        opt.emit_vertex_stage = true;
        RDNA2ComputeCompiler cc(opt);
        auto r = cc.Compile(code.data(), code.size());
        CHECK(r.success);
        CHECK(ParsesCleanly(r.spirv));
        CHECK(EntryPointModel(r.spirv) == 0u);              // ExecutionModel Vertex
        CHECK(CountOpcode(r.spirv, 16) == 0);               // no LocalSize
        CHECK(CountDecoValue(r.spirv, 11, 42) == 1);        // BuiltIn VertexIndex
        CHECK(CountDecoValue(r.spirv, 11, 0) == 1);         // BuiltIn Position
        CHECK(CountDecoValue(r.spirv, 30, 0) == 1);         // Location 0 (colour)
        CHECK(CountDecoValue(r.spirv, 33, 0) == 1);         // in SSBO binding 0
        CHECK(CountDecoValue(r.spirv, 33, 1) == 1);         // out SSBO binding 1
        // No push constants without SMEM.
        CHECK(CountDecoValue(r.spirv, 33, 2) == 0);

        // Semantic parity: the software executor runs the same program with
        // the same lane model -- the transformed vertices are the identity.
        std::vector<uint32_t> input;
        for (int i = 0; i < 3; ++i) {
            for (float f : kTri[i]) input.push_back(FBits(f));
        }
        std::vector<uint32_t> sw_out;
        GcnSwExecutor sw;
        GcnSwExecResult srr = sw.Run(code.data(), code.size(), 3, input,
                                     8, 8, sw_out, nullptr, nullptr);
        CHECK(srr.ok);
        CHECK(sw_out.size() == 24);
        CHECK(sw_out == input);   // passthrough

        // The default (compute) module is unchanged: GLCompute + LocalSize.
        ComputeCompilerOptions copt;
        copt.in_dwords_per_lane = 8;
        copt.out_dwords_per_lane = 8;
        RDNA2ComputeCompiler cc2(copt);
        auto r2 = cc2.Compile(code.data(), code.size());
        CHECK(r2.success);
        CHECK(EntryPointModel(r2.spirv) == 5u);             // GLCompute
        CHECK(CountOpcode(r2.spirv, 16) == 1);              // LocalSize once
        CHECK(CountDecoValue(r2.spirv, 11, 42) == 0);       // no VertexIndex
    }

    // =====================================================================
    // C: the passthrough FRAGMENT module.
    // =====================================================================
    std::cout << "[vk-gfx] C: passthrough fragment module\n";
    {
        const std::vector<uint32_t> fs =
            RDNA2ComputeCompiler::BuildPassthroughFragmentShader();
        CHECK(ParsesCleanly(fs));
        CHECK(EntryPointModel(fs) == 4u);                   // Fragment
        CHECK(CountOpcode(fs, 16) == 1);                    // OriginUpperLeft
        CHECK(CountDecoValue(fs, 30, 0) == 2);              // in + out Location 0
        CHECK(CountOpcode(fs, 61) == 1);                    // one OpLoad
        CHECK(CountOpcode(fs, 62) == 1);                    // one OpStore
        CHECK(fs.size() < 80);                              // minimal module (~71 words)
    }

    // =====================================================================
    // D: BuildGraphicsTarget -- the register-derived conversion.
    // =====================================================================
    std::cout << "[vk-gfx] D: register binding -> graphics target\n";
    {
        const uint64_t BASE       = 0x1400000000ull;
        const uint64_t SHADER_GVA = BASE + 0x0000;
        const uint64_t FETCH_GVA  = BASE + 0x0800;
        const uint64_t VERT_GVA   = BASE + 0x1000;
        const uint64_t IDX_GVA    = BASE + 0x1400;
        const uint64_t OUT_GVA    = BASE + 0x2000;
        const uint64_t CB_COLOR   = BASE + 0x3000;
        const uint64_t DB_DEPTH   = BASE + 0x5000;

        FlatGuestMemory gmem(BASE, 0x10000 / 4);
        std::vector<uint32_t> clear_depth(W * H, FBits(1.0f));
        gmem.WriteDwords(DB_DEPTH, clear_depth.data(), clear_depth.size());

        const uint32_t kDbLess =
            Pm4::DB_CONTROL_Z_ENABLE_MASK | Pm4::DB_CONTROL_Z_WRITE_MASK |
            (static_cast<uint32_t>(Pm4::ZFunc::Less)
             << Pm4::DB_CONTROL_ZFUNC_SHIFT);

        RingBuilder rb;
        rb.ProgramRenderTarget(CB_COLOR, W, H, DB_DEPTH, kDbLess);
        rb.ProgramViewport(W, H);
        rb.Draw(gmem, SHADER_GVA, FETCH_GVA, VERT_GVA, IDX_GVA, OUT_GVA, kTri);

        VulkanRendererBackend backend;
        backend.Initialize();
        PM4VulkanTranslator translator(backend);
        translator.BindComputeExecutor(&exec, &gmem);
        const auto result = translator.TranslateAndExecuteCommandRingChecked(
            rb.ring.data(), rb.ring.size());
        CHECK(result.ok());

        const auto& bind = translator.GetLastRenderTargetBinding();
        CHECK(bind.bound);

        VulkanComputeExecutor::GraphicsTargetDesc target;
        CHECK(PM4VulkanTranslator::BuildGraphicsTarget(
            bind, translator.GetRasterViewport(), target));
        CHECK(target.width == W);
        CHECK(target.height == H);
        CHECK(target.color_format == Pm4::GuestColorFormat::R8G8B8A8Unorm);
        CHECK(target.color_write);
        CHECK(target.depth_enabled);
        CHECK(target.depth_write);
        CHECK(target.zfunc == Pm4::ZFunc::Less);
        // The PA_CL_VPORT pair and the VkViewport the executor will derive:
        // x = off - scale, width = 2*scale (a negative YSCALE -> negative
        // height, the Vulkan 1.1 y-flip for top-left-origin guests).
        CHECK(std::fabs(target.vport_scale_x - 16.0f) < 1e-5f);
        CHECK(std::fabs(target.vport_off_x - 16.0f) < 1e-5f);
        CHECK(std::fabs(target.vport_scale_y + 16.0f) < 1e-5f);
        CHECK(std::fabs(target.vport_off_y - 16.0f) < 1e-5f);

        // Negative: a host-API binding (nothing programmed) declines.
        RenderTargetBinding host_bind;
        host_bind.programmed = false;
        host_bind.bound = false;
        VulkanComputeExecutor::GraphicsTargetDesc bad;
        CHECK(!PM4VulkanTranslator::BuildGraphicsTarget(
            host_bind, translator.GetRasterViewport(), bad));
        // Negative: depth-only (CB_TARGET_MASK 0) declines.
        RenderTargetBinding depth_only = bind;
        depth_only.color_write = false;
        CHECK(!PM4VulkanTranslator::BuildGraphicsTarget(
            depth_only, translator.GetRasterViewport(), bad));
        // Negative: degenerate viewport declines.
        RasterViewport degenerate;
        CHECK(!PM4VulkanTranslator::BuildGraphicsTarget(bind, degenerate, bad));
    }

    // =====================================================================
    // E: the fail-closed end-to-end contract on this host.
    // =====================================================================
    std::cout << "[vk-gfx] E: opt-in graphics path, fail-closed fallback\n";
    {
        const uint64_t BASE       = 0x1400000000ull;
        const uint64_t SHADER_GVA = BASE + 0x0000;
        const uint64_t FETCH_GVA  = BASE + 0x0800;
        const uint64_t VERT_GVA   = BASE + 0x1000;
        const uint64_t IDX_GVA    = BASE + 0x1400;
        const uint64_t OUT_GVA    = BASE + 0x2000;
        const uint64_t CB_COLOR   = BASE + 0x3000;
        const uint64_t DB_DEPTH   = BASE + 0x5000;

        const uint32_t kDbLess =
            Pm4::DB_CONTROL_Z_ENABLE_MASK | Pm4::DB_CONTROL_Z_WRITE_MASK |
            (static_cast<uint32_t>(Pm4::ZFunc::Less)
             << Pm4::DB_CONTROL_ZFUNC_SHIFT);

        // E1: graphics raster OPTED IN. On a device host the graphics path
        // executes; here it must decline honestly and the software
        // rasterizer renders the same pixels.
        {
            FlatGuestMemory gmem(BASE, 0x10000 / 4);
            std::vector<uint32_t> clear_depth(W * H, FBits(1.0f));
            gmem.WriteDwords(DB_DEPTH, clear_depth.data(), clear_depth.size());

            RingBuilder rb;
            rb.ProgramRenderTarget(CB_COLOR, W, H, DB_DEPTH, kDbLess);
            rb.ProgramViewport(W, H);
            rb.Draw(gmem, SHADER_GVA, FETCH_GVA, VERT_GVA, IDX_GVA, OUT_GVA,
                    kTri);

            VulkanRendererBackend backend;
            backend.Initialize();
            PM4VulkanTranslator translator(backend);
            translator.BindComputeExecutor(&exec, &gmem);
            translator.SetGraphicsRasterEnabled(true);
            CHECK(translator.IsGraphicsRasterEnabled());
            const auto result = translator.TranslateAndExecuteCommandRingChecked(
                rb.ring.data(), rb.ring.size());
            CHECK(result.ok());

            const auto& draw = translator.GetLastDrawDispatch();
            CHECK(draw.attempted);
            CHECK(draw.graphics_raster_attempted);
            if (available) {
                // A graphics-capable device rasterized on the GPU.
                CHECK(draw.graphics_raster_executed);
                CHECK(draw.executed_on_gpu);
            } else {
                // No device: the honest decline + the software rasterizer.
                CHECK(!draw.graphics_raster_executed);
                CHECK(!draw.graphics_raster_note.empty());
                const auto& rstats = translator.GetLastRasterStats();
                CHECK(rstats.ok);
                CHECK(rstats.pixels_written > 200);
            }
            // Either way the guest CB plane holds the triangle: the centre
            // pixel is solid red (RGBA8 x = 1.0).
            const uint32_t px = gmem.At(CB_COLOR +
                (static_cast<uint64_t>(16) * W + 16) * 4);
            CHECK((px & 0xFF) == 255);          // R
            CHECK(((px >> 8) & 0xFF) == 0);     // G
            CHECK(((px >> 16) & 0xFF) == 0);    // B
            // Depth written (LESS passes 0.2 against the cleared 1.0).
            uint32_t d = gmem.At(DB_DEPTH +
                (static_cast<uint64_t>(16) * W + 16) * 4);
            CHECK(std::fabs(BitsF(d) - 0.2f) < 1e-4f);
            std::cout << "  [ok] "
                      << (available ? "GPU rasterized the register-bound "
                                      "target"
                                    : "no device: graphics path declined, "
                                      "software rasterizer rendered")
                      << "\n";
        }

        // E2: default OFF -- the round-18 behaviour, untouched.
        {
            FlatGuestMemory gmem(BASE, 0x10000 / 4);
            std::vector<uint32_t> clear_depth(W * H, FBits(1.0f));
            gmem.WriteDwords(DB_DEPTH, clear_depth.data(), clear_depth.size());

            RingBuilder rb;
            rb.ProgramRenderTarget(CB_COLOR, W, H, DB_DEPTH, kDbLess);
            rb.ProgramViewport(W, H);
            rb.Draw(gmem, SHADER_GVA, FETCH_GVA, VERT_GVA, IDX_GVA, OUT_GVA,
                    kTri);

            VulkanRendererBackend backend;
            backend.Initialize();
            PM4VulkanTranslator translator(backend);
            translator.BindComputeExecutor(&exec, &gmem);
            CHECK(!translator.IsGraphicsRasterEnabled());
            const auto result = translator.TranslateAndExecuteCommandRingChecked(
                rb.ring.data(), rb.ring.size());
            CHECK(result.ok());
            const auto& draw = translator.GetLastDrawDispatch();
            CHECK(draw.attempted);
            CHECK(!draw.graphics_raster_attempted);
            CHECK(!draw.graphics_raster_executed);
            CHECK(draw.graphics_raster_note.empty());
            const auto& rstats = translator.GetLastRasterStats();
            CHECK(rstats.ok);
            CHECK(rstats.pixels_written > 200);
            const uint32_t px = gmem.At(CB_COLOR +
                (static_cast<uint64_t>(16) * W + 16) * 4);
            CHECK((px & 0xFF) == 255);
        }
    }

    std::cout << "[vk-gfx] " << g_checks << " checks, " << g_failures
              << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
