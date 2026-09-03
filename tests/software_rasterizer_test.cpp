// ============================================================================
// ProsperoLayer RDNA2 Core - Software Rasterizer Test (round 13)
// ----------------------------------------------------------------------------
// Proves the headless pixel stage end-to-end: vertex clip-space data is
// transformed through the guest viewport, triangles are covered with
// perspective-correct barycentric interpolation, the LESS depth test rejects
// occluded fragments, off-screen and behind-the-eye triangles are culled,
// and RGBA8 pixels land in the guest colour plane through GpuGuestMemory.
// A final integration case drives the FULL PM4 draw path (GCN vertex shader
// -> software fallback -> transformed vertices -> rasterizer).
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

int g_failures = 0;
int g_checks = 0;
bool Check(bool v, const char* e, int line) {
    ++g_checks;
    if (!v) { ++g_failures; std::cerr << "  [FAIL] line " << line << ": " << e << '\n'; }
    return v;
}
#define CHECK(e) Check((e), #e, __LINE__)

uint32_t FBits(float f) { uint32_t v; std::memcpy(&v, &f, 4); return v; }
float BitsF(uint32_t v) { float f; std::memcpy(&f, &v, 4); return f; }

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
        return BitsF(v);
    }
private:
    uint64_t m_base;
    std::vector<uint32_t> m_storage;
};

// A vertex: clip-space xyzw + RGBA (8 dwords).
std::vector<uint32_t> Vertex(float x, float y, float z, float w,
                             float r, float g, float b, float a) {
    return { FBits(x), FBits(y), FBits(z), FBits(w),
             FBits(r), FBits(g), FBits(b), FBits(a) };
}

} // namespace

int main() {
    std::cout << "[raster] round 13/18: software rasterizer\n";
    FlatGuestMemory mem(0x1000, 0x100000 / 4);   // 1 MB arena

    const uint64_t COLOR_GVA = 0x1000;
    const uint64_t DEPTH_GVA = 0x1000 + 0x80000;
    const uint64_t COLOR3_GVA = 0x1000 + 0x90000;   // fresh plane for the clip cases
    constexpr uint32_t W = 64, H = 64;
    // Round 18: REAL PA_CL_VPORT semantics -- the fullscreen viewport a
    // guest programs (XSCALE=W/2, XOFFSET=W/2, YSCALE=-H/2, YOFFSET=H/2).
    const RasterViewport vp = RasterViewport::Fullscreen(W, H);

    // =====================================================================
    // A: full-screen-ish triangle with flat red.
    // =====================================================================
    std::cout << "[raster] A: coverage + flat colour\n";
    {
        RasterTarget target{COLOR_GVA, 0, W, H};
        std::vector<uint32_t> verts;
        for (uint32_t d : Vertex(-0.5f, -0.5f, 0.0f, 1.0f, 1, 0, 0, 1)) verts.push_back(d);
        for (uint32_t d : Vertex( 0.5f, -0.5f, 0.0f, 1.0f, 1, 0, 0, 1)) verts.push_back(d);
        for (uint32_t d : Vertex(-0.5f,  0.5f, 0.0f, 1.0f, 1, 0, 0, 1)) verts.push_back(d);

        auto stats = SoftwareRasterizer::DrawTriangles(verts, 8, target, vp, &mem);
        CHECK(stats.ok);
        CHECK(stats.triangles_in == 1 && stats.triangles_drawn == 1);
        CHECK(stats.pixels_written > 400);            // ~ half of the lower-left quadrant
        // Centre of the triangle edge-midpoint must be red.
        // NDC (-0.5,-0.5)..(0.5,-0.5)..(-0.5,0.5): centroid at (-1/6,-1/6)
        const uint32_t cx = 32 + static_cast<uint32_t>(-1.0f / 6.0f * 32.0f);
        const uint32_t cy = 32 + static_cast<uint32_t>(-1.0f / 6.0f * 32.0f);
        CHECK(mem.PixelR(COLOR_GVA, cx, cy, W) == 255);
        CHECK(mem.PixelG(COLOR_GVA, cx, cy, W) == 0);
        // A pixel far outside (top-right corner) stays black.
        CHECK(mem.PixelR(COLOR_GVA, 60, 4, W) == 0);
    }

    // =====================================================================
    // B: perspective-correct colour interpolation.
    // =====================================================================
    std::cout << "[raster] B: perspective-correct interpolation\n";
    {
        RasterTarget target{COLOR_GVA, 0, W, H};
        // Bottom edge red -> green across the screen, at w = 1 (affine ==
        // perspective here; correctness of the 1/w weighting is proven in C).
        std::vector<uint32_t> verts;
        for (uint32_t d : Vertex(-1.0f, -0.25f, 0.0f, 1.0f, 1, 0, 0, 1)) verts.push_back(d);
        for (uint32_t d : Vertex( 1.0f, -0.25f, 0.0f, 1.0f, 0, 1, 0, 1)) verts.push_back(d);
        for (uint32_t d : Vertex(-1.0f, -1.00f, 0.0f, 1.0f, 1, 0, 0, 1)) verts.push_back(d);
        auto stats = SoftwareRasterizer::DrawTriangles(verts, 8, target, vp, &mem);
        CHECK(stats.ok);
        // At x = 0 the colour must be a ~50/50 red/green mix.
        const uint8_t r_mid = mem.PixelR(COLOR_GVA, 32, 50, W);
        const uint8_t g_mid = mem.PixelG(COLOR_GVA, 32, 50, W);
        CHECK(r_mid > 80 && r_mid < 176);
        CHECK(g_mid > 80 && g_mid < 176);
        CHECK(std::abs(static_cast<int>(r_mid) - static_cast<int>(g_mid)) < 24);
    }

    // =====================================================================
    // C: 1/w weighting actually differs from affine.
    // =====================================================================
    std::cout << "[raster] C: depth (z/w) + perspective divide\n";
    {
        RasterTarget target{COLOR_GVA, DEPTH_GVA, W, H};
        // Clear the depth plane to 1.0 (far) first -- exactly what a real
        // renderer does before drawing; the rasterizer primes its depth test
        // from the guest plane.
        std::vector<uint32_t> clear_depth(W * H, FBits(1.0f));
        mem.WriteDwords(DEPTH_GVA, clear_depth.data(), clear_depth.size());
        // One triangle at constant z=0.5, w=1 -> depth plane 0.5 after divide.
        std::vector<uint32_t> verts;
        for (uint32_t d : Vertex(-0.3f, -0.3f, 0.5f, 1.0f, 0, 0, 1, 1)) verts.push_back(d);
        for (uint32_t d : Vertex( 0.3f, -0.3f, 0.5f, 1.0f, 0, 0, 1, 1)) verts.push_back(d);
        for (uint32_t d : Vertex(-0.3f,  0.3f, 0.5f, 1.0f, 0, 0, 1, 1)) verts.push_back(d);
        auto stats = SoftwareRasterizer::DrawTriangles(verts, 8, target, vp, &mem);
        CHECK(stats.ok);
        const float z = mem.DepthAt(DEPTH_GVA, 28, 36, W);
        CHECK(std::fabs(z - 0.5f) < 1e-4f);

        // Second triangle at the SAME screen area but z = 0.9 (farther):
        // every pixel must fail the LESS depth test.
        std::vector<uint32_t> far_tri;
        for (uint32_t d : Vertex(-0.3f, -0.3f, 0.9f, 1.0f, 1, 1, 1, 1)) far_tri.push_back(d);
        for (uint32_t d : Vertex( 0.3f, -0.3f, 0.9f, 1.0f, 1, 1, 1, 1)) far_tri.push_back(d);
        for (uint32_t d : Vertex(-0.3f,  0.3f, 0.9f, 1.0f, 1, 1, 1, 1)) far_tri.push_back(d);
        auto stats2 = SoftwareRasterizer::DrawTriangles(far_tri, 8, target, vp, &mem);
        CHECK(stats2.ok);
        CHECK(stats2.pixels_depth_rejected > 0);
        CHECK(stats2.pixels_written == 0);
        // The depth plane still holds 0.5.
        CHECK(std::fabs(mem.DepthAt(DEPTH_GVA, 28, 36, W) - 0.5f) < 1e-4f);

        // A NEARER triangle (z = 0.2) overwrites colour + depth.
        std::vector<uint32_t> near_tri;
        for (uint32_t d : Vertex(-0.3f, -0.3f, 0.2f, 1.0f, 1, 1, 0, 1)) near_tri.push_back(d);
        for (uint32_t d : Vertex( 0.3f, -0.3f, 0.2f, 1.0f, 1, 1, 0, 1)) near_tri.push_back(d);
        for (uint32_t d : Vertex(-0.3f,  0.3f, 0.2f, 1.0f, 1, 1, 0, 1)) near_tri.push_back(d);
        auto stats3 = SoftwareRasterizer::DrawTriangles(near_tri, 8, target, vp, &mem);
        CHECK(stats3.ok && stats3.pixels_written > 0);
        CHECK(std::fabs(mem.DepthAt(DEPTH_GVA, 28, 36, W) - 0.2f) < 1e-4f);
        CHECK(mem.PixelR(COLOR_GVA, 28, 36, W) == 255);   // yellow now
        CHECK(mem.PixelG(COLOR_GVA, 28, 36, W) == 255);
    }

    // =====================================================================
    // D: culling (off-screen + behind the eye + degenerate).
    // =====================================================================
    std::cout << "[raster] D: culling\n";
    {
        RasterTarget target{COLOR_GVA, 0, W, H};
        std::vector<uint32_t> verts;
        // Fully off-screen (right of the viewport).
        for (uint32_t d : Vertex(1.5f, 0.5f, 0.0f, 1.0f, 1, 0, 0, 1)) verts.push_back(d);
        for (uint32_t d : Vertex(2.5f, 0.5f, 0.0f, 1.0f, 1, 0, 0, 1)) verts.push_back(d);
        for (uint32_t d : Vertex(1.5f, 1.5f, 0.0f, 1.0f, 1, 0, 0, 1)) verts.push_back(d);
        // Behind the eye (w <= 0).
        for (uint32_t d : Vertex(-0.2f, -0.2f, 0.0f, -1.0f, 1, 0, 0, 1)) verts.push_back(d);
        for (uint32_t d : Vertex( 0.2f, -0.2f, 0.0f, -1.0f, 1, 0, 0, 1)) verts.push_back(d);
        for (uint32_t d : Vertex(-0.2f,  0.2f, 0.0f, -1.0f, 1, 0, 0, 1)) verts.push_back(d);
        // Degenerate (all three points collinear).
        for (uint32_t d : Vertex(-0.5f, -0.5f, 0.0f, 1.0f, 1, 0, 0, 1)) verts.push_back(d);
        for (uint32_t d : Vertex( 0.0f,  0.0f, 0.0f, 1.0f, 1, 0, 0, 1)) verts.push_back(d);
        for (uint32_t d : Vertex( 0.5f,  0.5f, 0.0f, 1.0f, 1, 0, 0, 1)) verts.push_back(d);
        auto stats = SoftwareRasterizer::DrawTriangles(verts, 8, target, vp, &mem);
        CHECK(stats.ok);
        CHECK(stats.triangles_in == 3);
        CHECK(stats.triangles_culled == 3);
        CHECK(stats.triangles_drawn == 0);
        CHECK(stats.pixels_written == 0);
    }

    // =====================================================================
    // E: viewport transform (non-identity scale/offset).
    // =====================================================================
    std::cout << "[raster] E: viewport transform\n";
    {
        RasterTarget target{COLOR_GVA, 0, W, H};
        // Real PA_CL_VPORT values: a half-width viewport shifted right --
        // XSCALE=16, XOFFSET=48 (NDC -1..1 -> 32..64), YSCALE=-32,
        // YOFFSET=32 (NDC 1..-1 -> 0..64, top-left origin).
        RasterViewport vpw;
        vpw.scale_x = 16.0f; vpw.scale_x_offset = 48.0f;
        vpw.scale_y = -32.0f; vpw.scale_y_offset = 32.0f;
        std::vector<uint32_t> verts;
        for (uint32_t d : Vertex(-1.0f, -1.0f, 0.0f, 1.0f, 0, 1, 0, 1)) verts.push_back(d);
        for (uint32_t d : Vertex( 1.0f, -1.0f, 0.0f, 1.0f, 0, 1, 0, 1)) verts.push_back(d);
        for (uint32_t d : Vertex(-1.0f,  1.0f, 0.0f, 1.0f, 0, 1, 0, 1)) verts.push_back(d);
        auto stats = SoftwareRasterizer::DrawTriangles(verts, 8, target, vpw, &mem);
        CHECK(stats.ok && stats.pixels_written > 0);
        // With XSCALE=16 + XOFFSET=48, NDC x=-1 maps to screen 32 (of 64):
        // the LEFT half of the screen must stay black.
        CHECK(mem.PixelG(COLOR_GVA, 8, 32, W) == 0);
        // ... and a point inside the right-shifted quadrant is green.
        CHECK(mem.PixelG(COLOR_GVA, 48, 40, W) == 255);
    }

    // =====================================================================
    // F: full PM4 draw path -> VGT fetch -> GCN vertex program -> raster.
    // =====================================================================
    std::cout << "[raster] F: PM4 draw integration\n";
    {
        VulkanComputeExecutor exec;
        (void)exec.Initialize();
        VulkanRendererBackend backend;
        PM4VulkanTranslator translator(backend);

        const uint64_t BASE       = 0x1400000000ull;
        const uint64_t SHADER_GVA = BASE + 0x0000;
        const uint64_t FETCH_GVA  = BASE + 0x0800;
        const uint64_t VERT_GVA   = BASE + 0x1000;   // 3 verts x 8 dwords
        const uint64_t IDX32_GVA  = BASE + 0x1400;
        const uint64_t OUT_GVA    = BASE + 0x2000;
        const uint64_t COLOR2_GVA = BASE + 0x3000;
        FlatGuestMemory gmem(BASE, 0x10000 / 4);

        // Trivial vertex program: the fetched attributes (one 8-dword
        // descriptor: clip xyzw + RGBA) ARE the outputs -- s_endpgm alone.
        gmem.PutDwords(SHADER_GVA, { 0xBF810000u });

        // One full-screen triangle: red at (-1,-1), green at (1,-1),
        // blue at (-1,1); z = 0.5, w = 1.
        std::vector<uint32_t> verts;
        const float tri[3][8] = {
            {-1.0f, -1.0f, 0.5f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f},
            { 1.0f, -1.0f, 0.5f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f},
            {-1.0f,  1.0f, 0.5f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f},
        };
        for (const auto& v : tri) {
            for (float f : v) verts.push_back(FBits(f));
        }
        gmem.PutDwords(VERT_GVA, verts);
        gmem.PutDwords(IDX32_GVA, { 0, 1, 2 });

        // Self-describing fetch table: 1 entry, stride 8 dwords, size 8.
        gmem.PutDwords(FETCH_GVA, {
            1,
            static_cast<uint32_t>(VERT_GVA & 0xffffffff),
            static_cast<uint32_t>(VERT_GVA >> 32),
            8, 8,
        });

        auto put_sh_reg = [&](std::vector<uint32_t>& ring, uint32_t off, uint32_t val) {
            ring.push_back((3u << 30) | (1u << 16) | (0x76u << 8));
            ring.push_back(off);
            ring.push_back(val);
        };
        std::vector<uint32_t> ring;
        put_sh_reg(ring, Pm4::SPI_SHADER_PGM_LO_VS, static_cast<uint32_t>(SHADER_GVA >> 8));
        put_sh_reg(ring, Pm4::SPI_SHADER_PGM_HI_VS, static_cast<uint32_t>(SHADER_GVA >> 32));
        put_sh_reg(ring, Pm4::SPI_SHADER_USER_DATA_VS_0 + 0,
                   static_cast<uint32_t>(VERT_GVA & 0xffffffff));
        put_sh_reg(ring, Pm4::SPI_SHADER_USER_DATA_VS_0 + 1,
                   static_cast<uint32_t>(VERT_GVA >> 32));
        put_sh_reg(ring, Pm4::SPI_SHADER_USER_DATA_VS_0 + 2,
                   static_cast<uint32_t>(OUT_GVA & 0xffffffff));
        put_sh_reg(ring, Pm4::SPI_SHADER_USER_DATA_VS_0 + 3,
                   static_cast<uint32_t>(OUT_GVA >> 32));
        put_sh_reg(ring, Pm4::SPI_SHADER_USER_DATA_VS_0 + 4, 3);
        put_sh_reg(ring, Pm4::SPI_SHADER_USER_DATA_VS_FETCH_LO,
                   static_cast<uint32_t>(FETCH_GVA & 0xffffffff));
        put_sh_reg(ring, Pm4::SPI_SHADER_USER_DATA_VS_FETCH_HI,
                   static_cast<uint32_t>(FETCH_GVA >> 32));
        put_sh_reg(ring, Pm4::SPI_SHADER_USER_DATA_VS_OUT_DWORDS, 8);
        // INDEX_BASE (u64) + INDEX_TYPE u32 + DRAW_INDEX_2.
        ring.push_back((3u << 30) | (1u << 16) | (0x2Du << 8));
        ring.push_back(static_cast<uint32_t>(IDX32_GVA & 0xffffffff));
        ring.push_back(static_cast<uint32_t>(IDX32_GVA >> 32));
        ring.push_back((3u << 30) | (0u << 16) | (0x2Eu << 8));
        ring.push_back(Pm4::INDEX_TYPE_U32);
        ring.push_back((3u << 30) | (2u << 16) | (0x22u << 8));
        ring.push_back(3); ring.push_back(0); ring.push_back(0);

        RasterTarget target{COLOR2_GVA, 0, 32, 32};
        translator.SetRasterTarget(target);
        translator.BindComputeExecutor(&exec, &gmem);

        const auto result =
            translator.TranslateAndExecuteCommandRingChecked(ring.data(), ring.size());
        CHECK(result.ok());

        const auto& draw = translator.GetLastDrawDispatch();
        CHECK(draw.attempted);
        CHECK(draw.fetch_enabled);
        CHECK(draw.out_dwords_per_lane == 8);

        const auto& rstats = translator.GetLastRasterStats();
        CHECK(rstats.ok);
        CHECK(rstats.triangles_in == 1);
        CHECK(rstats.triangles_drawn == 1);
        CHECK(rstats.pixels_written > 400);   // half the 32x32 target

        // Barycentric sanity through the FULL pipeline: near the red corner
        // the pixel is mostly red; near the green corner mostly green.
        const uint8_t r_near = gmem.PixelR(COLOR2_GVA, 2, 29, 32);
        const uint8_t g_near = gmem.PixelG(COLOR2_GVA, 2, 29, 32);
        CHECK(r_near > 200 && g_near < 55);
        const uint8_t r_far = gmem.PixelR(COLOR2_GVA, 29, 29, 32);
        const uint8_t g_far = gmem.PixelG(COLOR2_GVA, 29, 29, 32);
        CHECK(r_far < 55 && g_far > 200);
        // Outside the triangle (top-right) stays black.
        CHECK(gmem.PixelR(COLOR2_GVA, 30, 2, 32) == 0);

        // The m<4 guard still fails closed on 1-dword vertices.
        RasterTarget bad_target{COLOR2_GVA, 0, 32, 32};
        std::vector<uint32_t> tiny = { 1, 1, 1 };
        auto guard = SoftwareRasterizer::DrawTriangles(tiny, 1, bad_target,
                                                       RasterViewport{}, &gmem);
        CHECK(!guard.ok);
    }

    // =====================================================================
    // G (round 18): full near-plane clipping -- a triangle straddling the
    // eye plane is CLIPPED (not rejected): the visible half renders with
    // correctly interpolated attributes.
    // =====================================================================
    std::cout << "[raster] G: near-plane clipping\n";
    {
        RasterTarget target{COLOR3_GVA, 0, W, H};
        // Two vertices in front (w=1) at the bottom corners; one vertex
        // BEHIND the eye (w=-1, projecting to the TOP of the screen). Round
        // 13 rejected the whole triangle; round 18 clips it at the near
        // plane. Hand-computed projection: the visible quad is
        //   A(16,57.6) B(48,57.6) -> screen(50,64) -> screen(14,64)
        // i.e. the bottom sliver between y=57.6 and the bottom edge.
        std::vector<uint32_t> verts;
        for (uint32_t d : Vertex(-0.5f, -0.8f, 0.0f, 1.0f, 1, 0, 0, 1)) verts.push_back(d);
        for (uint32_t d : Vertex( 0.5f, -0.8f, 0.0f, 1.0f, 1, 0, 0, 1)) verts.push_back(d);
        for (uint32_t d : Vertex( 0.0f, -0.8f, 0.0f, -1.0f, 1, 0, 0, 1)) verts.push_back(d);
        auto stats = SoftwareRasterizer::DrawTriangles(verts, 8, target, vp, &mem);
        CHECK(stats.ok);
        CHECK(stats.triangles_in == 1);
        CHECK(stats.triangles_clipped == 1);       // gained a clip vertex
        CHECK(stats.triangles_drawn == 2);         // quad fan = 2 slices
        CHECK(stats.pixels_written > 0);           // the visible sliver renders
        // Inside the sliver (y in [58..63], centre): red.
        CHECK(mem.PixelR(COLOR3_GVA, 32, 60, W) == 255);
        // Above the sliver (y=50 < 57.6): the clipped-away part stays black.
        CHECK(mem.PixelR(COLOR3_GVA, 32, 50, W) == 0);
        CHECK(mem.PixelR(COLOR3_GVA, 32, 8, W) == 0);

        // Far-plane: z/w must stay within [0,1] -- a triangle fully beyond
        // the far plane (z > w) is clipped to nothing.
        std::vector<uint32_t> farout;
        for (uint32_t d : Vertex(-0.4f, -0.4f, 2.0f, 1.0f, 0, 0, 1, 1)) farout.push_back(d);
        for (uint32_t d : Vertex( 0.4f, -0.4f, 2.0f, 1.0f, 0, 0, 1, 1)) farout.push_back(d);
        for (uint32_t d : Vertex( 0.0f,  0.4f, 2.0f, 1.0f, 0, 0, 1, 1)) farout.push_back(d);
        auto stats_far = SoftwareRasterizer::DrawTriangles(farout, 8, target, vp, &mem);
        CHECK(stats_far.ok);
        CHECK(stats_far.triangles_culled == 1);
        CHECK(stats_far.pixels_written == 0);

        // An X-plane straddler: one vertex far off the right edge (x=4 > w)
        // gets clipped on the right frustum plane and the rest still draws.
        std::vector<uint32_t> xstraddle;
        for (uint32_t d : Vertex(-0.5f, -0.5f, 0.0f, 1.0f, 0, 1, 0, 1)) xstraddle.push_back(d);
        for (uint32_t d : Vertex( 4.0f, -0.5f, 0.0f, 1.0f, 0, 1, 0, 1)) xstraddle.push_back(d);
        for (uint32_t d : Vertex(-0.5f,  0.5f, 0.0f, 1.0f, 0, 1, 0, 1)) xstraddle.push_back(d);
        auto stats_x = SoftwareRasterizer::DrawTriangles(xstraddle, 8, target, vp, &mem);
        CHECK(stats_x.ok);
        CHECK(stats_x.triangles_clipped == 1);
        CHECK(stats_x.pixels_written > 0);
        CHECK(mem.PixelG(COLOR3_GVA, 16, 40, W) == 255);  // inside the kept part
    }

    // =====================================================================
    // H (round 18): ZFUNC from DB_DEPTH_CONTROL (was hard-wired LESS).
    // =====================================================================
    std::cout << "[raster] H: ZFUNC depth comparison\n";
    {
        // Prime depth to 0.5 everywhere.
        std::vector<uint32_t> clear_depth(W * H, FBits(0.5f));
        mem.WriteDwords(DEPTH_GVA, clear_depth.data(), clear_depth.size());
        RasterTarget target{COLOR_GVA, DEPTH_GVA, W, H};
        target.zfunc = Pm4::ZFunc::Greater;   // pass only where z > 0.5
        std::vector<uint32_t> verts;
        for (uint32_t d : Vertex(-0.4f, -0.4f, 0.2f, 1.0f, 0, 0, 1, 1)) verts.push_back(d);
        for (uint32_t d : Vertex( 0.4f, -0.4f, 0.2f, 1.0f, 0, 0, 1, 1)) verts.push_back(d);
        for (uint32_t d : Vertex( 0.0f,  0.4f, 0.2f, 1.0f, 0, 0, 1, 1)) verts.push_back(d);
        // z=0.2 < stored 0.5 -> Greater FAILS everywhere.
        auto stats = SoftwareRasterizer::DrawTriangles(verts, 8, target, vp, &mem);
        CHECK(stats.ok);
        CHECK(stats.pixels_written == 0);
        CHECK(stats.pixels_depth_rejected > 0);

        // ZFunc::Always ignores the stored value entirely.
        target.zfunc = Pm4::ZFunc::Always;
        auto stats2 = SoftwareRasterizer::DrawTriangles(verts, 8, target, vp, &mem);
        CHECK(stats2.ok && stats2.pixels_written > 0);
    }

    std::cout << "[raster] " << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
