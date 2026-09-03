// ============================================================================
// ProsperoLayer RDNA2 Core - PM4 render-target binding test (round 18)
// ----------------------------------------------------------------------------
// Proves the round-18 "deliberately next" step end-to-end: the draw path's
// raster target + viewport + depth state come from the REAL CB/DB/PA context
// registers (CB_COLOR0_BASE/SITCH/PITCH/INFO, CB_COLOR0_BASE_EXT,
// CB_TARGET_MASK, DB_Z_WRITE_BASE(+HI), DB_Z_INFO, DB_DEPTH_CONTROL,
// DB_DEPTH_SIZE_XY, PA_CL_VPORT_*), NOT from the host API:
//
//   A. a CB-programmed draw writes pixels at the CB_COLOR0_BASE plane while
//      a previously-bound HOST-API target stays untouched,
//   B. without CB programming the host-API target keeps working (back-compat),
//   C. PM4 overrides the host API when both are programmed,
//   D. depth from DB_* registers: binding fields + LESS rejection of a
//      farther triangle against a nearer one already in the plane,
//   E. unsupported CB_COLOR0_INFO formats fail the RASTER stage closed while
//      the vertex stage still executes (hardware behaviour),
//   F. CB_TARGET_MASK=0 disables colour writes (depth-only pass),
//   G. the 256-byte alignment convention (low 8 bits of BASE are reserved),
//   H. malformed PITCH/SLICE tile geometry fails closed.
// ============================================================================
#include "gpu/software_rasterizer.hpp"
#include "gpu/pm4_decoder.hpp"
#include "gpu/pm4_translator.hpp"
#include "gpu/vulkan_backend.hpp"
#include "gpu/vulkan_compute_executor.hpp"
#include "graphics/guest_gpu/pm4.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

using namespace PS5::GPU;
using namespace Pm4;

int g_failures = 0;
int g_checks = 0;
bool Check(bool v, const char* e, int line) {
    ++g_checks;
    if (!v) { ++g_failures; std::cerr << "  [FAIL] line " << line << ": " << e << '\n'; }
    return v;
}
#define CHECK(e) Check((e), #e, __LINE__)

uint32_t FBits(float f) { uint32_t v; std::memcpy(&v, &f, 4); return v; }

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
    void PutDwords(uint64_t gva, const std::vector<uint32_t>& words) {
        WriteDwords(gva, words.data(), words.size());
    }
    uint8_t PixelR(uint64_t gva, uint32_t x, uint32_t y, uint32_t w) {
        uint32_t v = 0;
        ReadDwords(gva + (static_cast<uint64_t>(y) * w + x) * 4, &v, 1);
        return v & 0xFF;
    }
    uint8_t PixelG(uint64_t gva, uint32_t x, uint32_t y, uint32_t w) {
        uint32_t v = 0;
        ReadDwords(gva + (static_cast<uint64_t>(y) * w + x) * 4, &v, 1);
        return (v >> 8) & 0xFF;
    }
    float DepthAt(uint64_t gva, uint32_t x, uint32_t y, uint32_t w) {
        uint32_t v = 0;
        ReadDwords(gva + (static_cast<uint64_t>(y) * w + x) * 4, &v, 1);
        float f; std::memcpy(&f, &v, 4);
        return f;
    }
    bool PlaneIsBlack(uint64_t gva, size_t bytes) {
        uint32_t v = 0;
        for (size_t off = 0; off < bytes / 4; ++off) {
            if (!ReadDwords(gva + off * 4, &v, 1) || v != 0) return false;
        }
        return true;
    }
private:
    uint64_t m_base;
    std::vector<uint32_t> m_storage;
};

constexpr uint32_t W = 32, H = 32;

// Builds one fullscreen 32x32 triangle draw ring (GCN vertex program =
// s_endpgm passthrough + fetch table with 8-dword clip+RGBA vertices),
// optionally preceded by the round-18 CB/DB/PA register programming.
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
        // Linear-mode tile math (see pm4.h): pitch = (TILE_MAX+1)*8 px,
        // pixels = (SLICE_TILE_MAX+1)*64.
        CtxReg(Pm4::CB_COLOR0_BASE,
               static_cast<uint32_t>(color_gva & 0xFFFFFFFFu));
        CtxReg(Pm4::CB_COLOR0_BASE_EXT,
               static_cast<uint32_t>(color_gva >> 32));
        CtxReg(Pm4::CB_COLOR0_PITCH, w / Pm4::CB_LINEAR_TILE_PIXELS - 1u);
        CtxReg(Pm4::CB_COLOR0_SLICE,
               (w * h) / Pm4::CB_LINEAR_TILE_AREA - 1u);
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
              const float (*tri)[8], uint32_t z_dword_index = 2) {
        (void)z_dword_index;
        gmem.PutDwords(shader_gva, { 0xBF810000u });   // s_endpgm
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
        // INDEX_BASE + INDEX_TYPE u32 + DRAW_INDEX_2.
        ring.push_back((3u << 30) | (1u << 16) | (0x2Du << 8));
        ring.push_back(static_cast<uint32_t>(idx_gva & 0xffffffff));
        ring.push_back(static_cast<uint32_t>(idx_gva >> 32));
        ring.push_back((3u << 30) | (0u << 16) | (0x2Eu << 8));
        ring.push_back(Pm4::INDEX_TYPE_U32);
        ring.push_back((3u << 30) | (2u << 16) | (0x22u << 8));
        ring.push_back(3); ring.push_back(0); ring.push_back(0);
    }
};

const float kTriNear[3][8] = {
    {-1.0f, -1.0f, 0.2f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f},
    { 1.0f, -1.0f, 0.2f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f},
    {-1.0f,  1.0f, 0.2f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f},
};
const float kTriFar[3][8] = {
    {-1.0f, -1.0f, 0.9f, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f},
    { 1.0f, -1.0f, 0.9f, 1.0f, 1.1f, 1.1f, 0.0f, 1.0f},
    {-1.0f,  1.0f, 0.9f, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f},
};

} // namespace

int main() {
    std::cout << "[cb-bind] round 18: render-target binding from PM4 registers\n";
    VulkanComputeExecutor exec;
    (void)exec.Initialize();
    VulkanRendererBackend backend;

    const uint64_t BASE        = 0x1400000000ull;
    const uint64_t SHADER_GVA  = BASE + 0x0000;
    const uint64_t FETCH_GVA   = BASE + 0x0800;
    const uint64_t VERT_GVA    = BASE + 0x1000;
    const uint64_t VERT2_GVA   = BASE + 0x1200;
    const uint64_t IDX_GVA     = BASE + 0x1400;
    const uint64_t OUT_GVA     = BASE + 0x2000;
    const uint64_t CB_COLOR    = BASE + 0x3000;   // PM4-bound colour plane
    const uint64_t DB_DEPTH    = BASE + 0x5000;   // PM4-bound depth plane
    const uint64_t HOST_COLOR  = BASE + 0x7000;   // host-API colour plane
    FlatGuestMemory gmem(BASE, 0x10000 / 4);

    // Prime the depth plane to 1.0 (what a guest clear does).
    std::vector<uint32_t> clear_depth(W * H, FBits(1.0f));
    gmem.WriteDwords(DB_DEPTH, clear_depth.data(), clear_depth.size());

    const uint32_t kDbLess =
        Pm4::DB_CONTROL_Z_ENABLE_MASK | Pm4::DB_CONTROL_Z_WRITE_MASK |
        (static_cast<uint32_t>(ZFunc::Less) << Pm4::DB_CONTROL_ZFUNC_SHIFT);

    // =====================================================================
    // A + C: CB-programmed draw lands at CB_COLOR0_BASE; the host-API target
    // stays black (the register binding REPLACES the host API).
    // =====================================================================
    std::cout << "[cb-bind] A/C: CB binding overrides the host API\n";
    {
        PM4VulkanTranslator translator(backend);
        // Host-API target at a DIFFERENT plane, programmed FIRST.
        RasterTarget host_target{HOST_COLOR, 0, W, H};
        translator.SetRasterTarget(host_target);

        RingBuilder rb;
        rb.ProgramRenderTarget(CB_COLOR, W, H, DB_DEPTH, kDbLess);
        rb.ProgramViewport(W, H);
        rb.Draw(gmem, SHADER_GVA, FETCH_GVA, VERT_GVA, IDX_GVA, OUT_GVA,
                kTriNear);
        translator.BindComputeExecutor(&exec, &gmem);
        const auto result = translator.TranslateAndExecuteCommandRingChecked(
            rb.ring.data(), rb.ring.size());
        CHECK(result.ok());

        const auto& bind = translator.GetLastRenderTargetBinding();
        CHECK(bind.programmed);
        CHECK(bind.bound);
        CHECK(bind.color_gva == CB_COLOR);
        CHECK(bind.depth_gva == DB_DEPTH);
        CHECK(bind.depth_bound);
        CHECK(bind.depth_write);
        CHECK(bind.width == W);
        CHECK(bind.height == H);
        CHECK(bind.viewport_from_registers);
        CHECK(bind.zfunc == ZFunc::Less);

        // The ACTIVE target is the PM4 one (not the host-API one).
        CHECK(translator.GetRasterTarget().color_gva == CB_COLOR);
        CHECK(translator.GetRasterTarget().depth_gva == DB_DEPTH);
        CHECK(translator.GetRasterTarget().width == W);
        CHECK(translator.GetRasterViewport().scale_x == 16.0f);
        CHECK(translator.GetRasterViewport().scale_y == -16.0f);

        const auto& rstats = translator.GetLastRasterStats();
        CHECK(rstats.ok);
        CHECK(rstats.triangles_in == 1);
        CHECK(rstats.triangles_drawn >= 1);
        CHECK(rstats.pixels_written > 200);

        // Pixels landed at the CB plane... (smooth-shaded triangle: the
        // barycentric weights near the red/green corners are ~0.78/~0.83)
        CHECK(gmem.PixelR(CB_COLOR, 3, 28, W) > 150);
        CHECK(gmem.PixelG(CB_COLOR, 3, 28, W) < 80);
        CHECK(gmem.PixelG(CB_COLOR, 26, 28, W) > 150);
        CHECK(gmem.PixelR(CB_COLOR, 26, 28, W) < 80);
        CHECK(gmem.PixelR(CB_COLOR, 28, 4, W) == 0);   // outside: stays black
        // ...depth 0.2 was written (LESS passed vs the cleared 1.0)...
        CHECK(std::fabs(gmem.DepthAt(DB_DEPTH, 16, 16, W) - 0.2f) < 1e-4f);
        // ...and the HOST-API plane stays completely black.
        CHECK(gmem.PlaneIsBlack(HOST_COLOR, W * H * 4));
    }

    // =====================================================================
    // B: without CB programming the host-API target still works.
    // =====================================================================
    std::cout << "[cb-bind] B: host-API back-compat\n";
    {
        PM4VulkanTranslator translator(backend);
        RasterTarget host_target{HOST_COLOR, 0, W, H};
        translator.SetRasterTarget(host_target);

        RingBuilder rb;
        rb.Draw(gmem, SHADER_GVA, FETCH_GVA, VERT_GVA, IDX_GVA, OUT_GVA,
                kTriNear);
        translator.BindComputeExecutor(&exec, &gmem);
        const auto result = translator.TranslateAndExecuteCommandRingChecked(
            rb.ring.data(), rb.ring.size());
        CHECK(result.ok());

        const auto& bind = translator.GetLastRenderTargetBinding();
        CHECK(!bind.programmed);
        CHECK(!bind.bound);
        CHECK(bind.error.empty());
        CHECK(translator.GetRasterTarget().color_gva == HOST_COLOR);

        const auto& rstats = translator.GetLastRasterStats();
        CHECK(rstats.ok && rstats.pixels_written > 200);
        CHECK(gmem.PixelR(HOST_COLOR, 3, 28, W) > 150);
        CHECK(gmem.PixelR(HOST_COLOR, 28, 4, W) == 0);
        // The unused CB plane from part A keeps its pixels (no clobbering).
        CHECK(gmem.PixelR(CB_COLOR, 3, 28, W) > 150);
    }

    // =====================================================================
    // D: ZFUNC=LESS from DB_DEPTH_CONTROL -- the farther triangle is fully
    // rejected against the 0.2 already in the depth plane.
    // =====================================================================
    std::cout << "[cb-bind] D: DB depth state drives the test\n";
    {
        PM4VulkanTranslator translator(backend);
        RingBuilder rb;
        rb.ProgramRenderTarget(CB_COLOR, W, H, DB_DEPTH, kDbLess);
        rb.ProgramViewport(W, H);
        rb.Draw(gmem, SHADER_GVA, FETCH_GVA, VERT2_GVA, IDX_GVA, OUT_GVA,
                kTriFar);
        translator.BindComputeExecutor(&exec, &gmem);
        const auto result = translator.TranslateAndExecuteCommandRingChecked(
            rb.ring.data(), rb.ring.size());
        CHECK(result.ok());
        const auto& bind = translator.GetLastRenderTargetBinding();
        CHECK(bind.bound && bind.depth_bound);
        const auto& rstats = translator.GetLastRasterStats();
        CHECK(rstats.ok);
        CHECK(rstats.pixels_written == 0);            // fully occluded
        CHECK(rstats.pixels_depth_rejected > 200);
        // Depth plane unchanged (still 0.2).
        CHECK(std::fabs(gmem.DepthAt(DB_DEPTH, 16, 16, W) - 0.2f) < 1e-4f);
    }

    // =====================================================================
    // E: unsupported CB_COLOR0_INFO format -> raster fails closed, the
    // vertex stage still executed.
    // =====================================================================
    std::cout << "[cb-bind] E: fail-closed on a bad format\n";
    {
        PM4VulkanTranslator translator(backend);
        RingBuilder rb;
        rb.ProgramRenderTarget(CB_COLOR, W, H, 0, 0);
        rb.ProgramViewport(W, H);
        // Overwrite INFO with FORMAT=5 (not 8_8_8_8).
        rb.CtxReg(Pm4::CB_COLOR0_INFO,
                  (5u << Pm4::CB_INFO_FORMAT_SHIFT) |
                  (Pm4::CB_NUMBER_UNORM << Pm4::CB_INFO_NUMBER_SHIFT));
        rb.Draw(gmem, SHADER_GVA, FETCH_GVA, VERT_GVA, IDX_GVA, OUT_GVA,
                kTriNear);
        translator.BindComputeExecutor(&exec, &gmem);
        const auto result = translator.TranslateAndExecuteCommandRingChecked(
            rb.ring.data(), rb.ring.size());
        CHECK(result.ok());

        const auto& bind = translator.GetLastRenderTargetBinding();
        CHECK(bind.programmed);
        CHECK(!bind.bound);
        CHECK(!bind.error.empty());
        // The VERTEX stage still ran (hardware does not stop the draw).
        const auto& draw = translator.GetLastDrawDispatch();
        CHECK(draw.attempted);
        CHECK(draw.out_dwords_per_lane == 8);
        // ...and the raster stage failed closed with the binding error.
        CHECK(!translator.GetLastRasterStats().ok);
        CHECK(!translator.GetLastRasterStats().error.empty());
    }

    // =====================================================================
    // F: CB_TARGET_MASK = 0 -> colour writes off; depth still updates.
    // =====================================================================
    std::cout << "[cb-bind] F: CB_TARGET_MASK colour gate\n";
    {
        // Re-clear the depth plane so the near triangle passes again.
        gmem.WriteDwords(DB_DEPTH, clear_depth.data(), clear_depth.size());
        // And blank a colour witness plane.
        std::vector<uint32_t> black(W * H, 0);
        gmem.WriteDwords(HOST_COLOR, black.data(), black.size());

        PM4VulkanTranslator translator(backend);
        RingBuilder rb;
        rb.ProgramRenderTarget(HOST_COLOR, W, H, DB_DEPTH, kDbLess);
        rb.ProgramViewport(W, H);
        rb.CtxReg(Pm4::CB_TARGET_MASK, 0x0u);   // TARGET0_ENABLE = 0
        rb.Draw(gmem, SHADER_GVA, FETCH_GVA, VERT_GVA, IDX_GVA, OUT_GVA,
                kTriNear);
        translator.BindComputeExecutor(&exec, &gmem);
        const auto result = translator.TranslateAndExecuteCommandRingChecked(
            rb.ring.data(), rb.ring.size());
        CHECK(result.ok());

        const auto& bind = translator.GetLastRenderTargetBinding();
        CHECK(bind.bound);
        CHECK(!bind.color_write);
        const auto& rstats = translator.GetLastRasterStats();
        CHECK(rstats.ok);
        CHECK(rstats.pixels_written > 200);          // fragments still ran
        CHECK(gmem.PlaneIsBlack(HOST_COLOR, W * H * 4));  // colour untouched
        CHECK(std::fabs(gmem.DepthAt(DB_DEPTH, 16, 16, W) - 0.2f) < 1e-4f);
    }

    // =====================================================================
    // G: 256-byte alignment convention (low 8 bits of CB_COLOR0_BASE are
    // reserved and masked off).
    // =====================================================================
    std::cout << "[cb-bind] G: base alignment convention\n";
    {
        PM4VulkanTranslator translator(backend);
        RingBuilder rb;
        rb.ProgramRenderTarget(CB_COLOR | 0x37u, W, H, 0, 0);   // junk low bits
        rb.ProgramViewport(W, H);
        rb.Draw(gmem, SHADER_GVA, FETCH_GVA, VERT_GVA, IDX_GVA, OUT_GVA,
                kTriNear);
        translator.BindComputeExecutor(&exec, &gmem);
        const auto result = translator.TranslateAndExecuteCommandRingChecked(
            rb.ring.data(), rb.ring.size());
        CHECK(result.ok());
        const auto& bind = translator.GetLastRenderTargetBinding();
        CHECK(bind.bound);
        CHECK(bind.color_gva == CB_COLOR);          // ~0xFF applied
        CHECK(translator.GetLastRasterStats().ok);
        CHECK(translator.GetLastRasterStats().pixels_written > 200);
        CHECK(gmem.PixelR(CB_COLOR, 3, 28, W) > 150);
    }

    // =====================================================================
    // H: malformed PITCH/SLICE geometry fails closed.
    // =====================================================================
    std::cout << "[cb-bind] H: malformed geometry rejection\n";
    {
        PM4VulkanTranslator translator(backend);
        RingBuilder rb;
        rb.ProgramRenderTarget(CB_COLOR, W, H, 0, 0);
        rb.ProgramViewport(W, H);
        // 33 px of pitch cannot divide a whole-tile slice count.
        rb.CtxReg(Pm4::CB_COLOR0_PITCH, 33u / Pm4::CB_LINEAR_TILE_PIXELS);
        rb.Draw(gmem, SHADER_GVA, FETCH_GVA, VERT_GVA, IDX_GVA, OUT_GVA,
                kTriNear);
        translator.BindComputeExecutor(&exec, &gmem);
        const auto result = translator.TranslateAndExecuteCommandRingChecked(
            rb.ring.data(), rb.ring.size());
        CHECK(result.ok());
        const auto& bind = translator.GetLastRenderTargetBinding();
        CHECK(bind.programmed);
        CHECK(!bind.bound);
        CHECK(!bind.error.empty());
        CHECK(!translator.GetLastRasterStats().ok);
    }

    std::cout << "[cb-bind] " << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
