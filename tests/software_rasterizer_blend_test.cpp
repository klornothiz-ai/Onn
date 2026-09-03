// ============================================================================
// ProsperoLayer RDNA2 Core - Software Rasterizer Blend & Topology Test
// ----------------------------------------------------------------------------
// Round 22: verifies the alpha-blending and triangle strip/fan topology
// support added to the headless pixel stage.
//   A: alpha blending with SRC_ALPHA / ONE_MINUS_SRC_ALPHA
//   B: triangle strip decomposition (4 vertices -> 2 triangles)
//   C: triangle fan decomposition (5 vertices -> 3 triangles)
//   D: fail-closed -- blending disabled produces the same output as before
//      (a plain overwrite, no blend against the destination).
// ============================================================================
#include "gpu/software_rasterizer.hpp"
#include "graphics/guest_gpu/pm4.h"

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
    // Pre-fill a pixel in the colour plane (RGBA8 little-endian: R in low byte).
    void SetPixel(uint64_t gva, uint32_t x, uint32_t y, uint32_t w,
                  uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        uint32_t v = static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) |
                     (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(a) << 24);
        WriteDwords(gva + (static_cast<uint64_t>(y) * w + x) * 4, &v, 1);
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
    uint8_t PixelB(uint64_t gva, uint32_t x, uint32_t y, uint32_t w) {
        uint32_t v = 0;
        ReadDwords(gva + (static_cast<uint64_t>(y) * w + x) * 4, &v, 1);
        return (v >> 16) & 0xFF;
    }
    uint8_t PixelA(uint64_t gva, uint32_t x, uint32_t y, uint32_t w) {
        uint32_t v = 0;
        ReadDwords(gva + (static_cast<uint64_t>(y) * w + x) * 4, &v, 1);
        return (v >> 24) & 0xFF;
    }
private:
    uint64_t m_base;
    std::vector<uint32_t> m_storage;
};

// A vertex: clip-space xyzw + RGBA (8 dwords). No UVs needed for these tests.
std::vector<uint32_t> Vertex(float x, float y, float z, float w,
                             float r, float g, float b, float a) {
    return { FBits(x), FBits(y), FBits(z), FBits(w),
             FBits(r), FBits(g), FBits(b), FBits(a) };
}

} // namespace

int main() {
    std::cout << "[raster-blend] round 22: blend & topology\n";
    FlatGuestMemory mem(0x1000, 0x100000 / 4);   // 1 MB arena

    const uint64_t COLOR_GVA = 0x1000;
    constexpr uint32_t W = 64, H = 64;
    const RasterViewport vp = RasterViewport::Fullscreen(W, H);

    // =====================================================================
    // A: alpha blending SRC_ALPHA / ONE_MINUS_SRC_ALPHA, ADD.
    //    Pre-fill the target with opaque blue (0,0,255,255). Draw a
    //    half-alpha red triangle (255,0,0,128). Expected:
    //      out = src*src.a + dst*(1-src.a)
    //         = red*0.5 + blue*0.5
    //         = (128, 0, 128, ~192)   (alpha: 128*0.5 + 255*0.5 = 191.5 -> 192)
    // =====================================================================
    std::cout << "[raster-blend] A: SRC_ALPHA / ONE_MINUS_SRC_ALPHA blend\n";
    {
        // Clear the target to opaque blue.
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x)
                mem.SetPixel(COLOR_GVA, x, y, W, 0, 0, 255, 255);

        RasterTarget target{COLOR_GVA, 0, W, H};
        target.blend.enabled = true;
        target.blend.src_factor = BlendSrcAlpha;
        target.blend.dst_factor = BlendOneMinusSrcAlpha;
        target.blend.blend_op = BlendOpAdd;

        // Small full-screen-ish triangle well inside the target so the
        // expected blend applies at its centroid. White vertex colour is
        // modulated by the texture... no texture here, so the vertex
        // colour (half-alpha red) is the source.
        std::vector<uint32_t> verts;
        for (uint32_t d : Vertex(-0.5f, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.5f)) verts.push_back(d);
        for (uint32_t d : Vertex( 0.5f, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.5f)) verts.push_back(d);
        for (uint32_t d : Vertex(-0.5f,  0.5f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.5f)) verts.push_back(d);

        auto stats = SoftwareRasterizer::DrawTriangles(verts, 8, target, vp, &mem);
        CHECK(stats.ok);
        CHECK(stats.pixels_written > 100);

        // Centroid region of the triangle (~26,~37) should be the blended colour.
        const uint8_t r = mem.PixelR(COLOR_GVA, 26, 37, W);
        const uint8_t g = mem.PixelG(COLOR_GVA, 26, 37, W);
        const uint8_t b = mem.PixelB(COLOR_GVA, 26, 37, W);
        const uint8_t a = mem.PixelA(COLOR_GVA, 26, 37, W);
        // 0.5*255 = 127.5 -> 128 for R; G stays 0; B: 0.5*255 = 128;
        // A: 0.5*0.5 + 1.0*0.5 = 0.75 -> 191 (0.75*255=191.25->191).
        CHECK(r == 128);
        CHECK(g == 0);
        CHECK(b == 128);
        CHECK(a == 191);

        // A pixel outside the triangle stays the destination (blue, opaque).
        CHECK(mem.PixelR(COLOR_GVA, 60, 4, W) == 0);
        CHECK(mem.PixelB(COLOR_GVA, 60, 4, W) == 255);
    }

    // =====================================================================
    // B: triangle strip decomposition. 4 vertices -> 2 triangles.
    //    Two adjacent triangles drawn as a strip with distinct colours per
    //    triangle; verify both regions are covered (2 triangles drawn).
    // =====================================================================
    std::cout << "[raster-blend] B: triangle strip (4 verts -> 2 tris)\n";
    {
        RasterTarget target{COLOR_GVA, 0, W, H};
        target.topology = TopologyTriangleStrip;

        // A strip forming a quad split into two triangles:
        //   v0 (-0.6,-0.6)  v1 ( 0.6,-0.6)
        //   v2 (-0.6, 0.6)  v3 ( 0.6, 0.6)
        // Triangles: (v0,v1,v2) and (v1,v3,v2) [winding flipped on odd].
        // Flat white colour so we just count coverage.
        std::vector<uint32_t> verts;
        for (uint32_t d : Vertex(-0.6f, -0.6f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f)) verts.push_back(d);
        for (uint32_t d : Vertex( 0.6f, -0.6f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f)) verts.push_back(d);
        for (uint32_t d : Vertex(-0.6f,  0.6f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f)) verts.push_back(d);
        for (uint32_t d : Vertex( 0.6f,  0.6f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f)) verts.push_back(d);

        auto stats = SoftwareRasterizer::DrawTriangles(verts, 8, target, vp, &mem);
        CHECK(stats.ok);
        // 4 vertices -> 2 triangles in.
        CHECK(stats.triangles_in == 2);
        // Both triangles are on-screen, non-degenerate -> both drawn.
        CHECK(stats.triangles_drawn == 2);
        CHECK(stats.pixels_written > 1000);
        // A point in the right half of the quad (covered only by the 2nd
        // triangle of the strip) must be white.
        CHECK(mem.PixelR(COLOR_GVA, 40, 32, W) == 255);
        // A point in the left half (covered by the 1st triangle) also white.
        CHECK(mem.PixelR(COLOR_GVA, 20, 32, W) == 255);
    }

    // =====================================================================
    // C: triangle fan decomposition. 5 vertices -> 3 triangles.
    //    A fan around vertex 0 with 5 vertices yields 3 triangles.
    // =====================================================================
    std::cout << "[raster-blend] C: triangle fan (5 verts -> 3 tris)\n";
    {
        RasterTarget target{COLOR_GVA, 0, W, H};
        target.topology = TopologyTriangleFan;

        // A fan centred at v0 (the screen centre) sweeping a full disc.
        // v0 = centre, v1..v4 around it. Flat white.
        std::vector<uint32_t> verts;
        for (uint32_t d : Vertex(0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f)) verts.push_back(d); // v0 centre
        for (uint32_t d : Vertex(-0.8f, -0.8f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f)) verts.push_back(d); // v1
        for (uint32_t d : Vertex( 0.8f, -0.8f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f)) verts.push_back(d); // v2
        for (uint32_t d : Vertex( 0.8f,  0.8f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f)) verts.push_back(d); // v3
        for (uint32_t d : Vertex(-0.8f,  0.8f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f)) verts.push_back(d); // v4

        auto stats = SoftwareRasterizer::DrawTriangles(verts, 8, target, vp, &mem);
        CHECK(stats.ok);
        // 5 vertices -> 3 triangles in.
        CHECK(stats.triangles_in == 3);
        CHECK(stats.triangles_drawn == 3);
        CHECK(stats.pixels_written > 1000);
        // The centre (v0) is shared by all three fan triangles -> covered.
        CHECK(mem.PixelR(COLOR_GVA, 32, 32, W) == 255);
    }

    // =====================================================================
    // D: fail-closed -- blending disabled overwrites the destination
    //    exactly as before (no blend against the existing pixel).
    // =====================================================================
    std::cout << "[raster-blend] D: fail-closed blend disabled\n";
    {
        // Pre-fill the target with opaque green.
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x)
                mem.SetPixel(COLOR_GVA, x, y, W, 0, 255, 0, 255);

        // Default RasterTarget -> blend disabled (overwrite), topology
        // Triangles (the historical behaviour).
        RasterTarget target{COLOR_GVA, 0, W, H};

        // Half-alpha red triangle: with blending OFF the destination is
        // overwritten with the source colour (red), NOT blended.
        std::vector<uint32_t> verts;
        for (uint32_t d : Vertex(-0.5f, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.5f)) verts.push_back(d);
        for (uint32_t d : Vertex( 0.5f, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.5f)) verts.push_back(d);
        for (uint32_t d : Vertex(-0.5f,  0.5f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.5f)) verts.push_back(d);

        auto stats = SoftwareRasterizer::DrawTriangles(verts, 8, target, vp, &mem);
        CHECK(stats.ok);
        CHECK(stats.pixels_written > 100);

        // Overwrite: source red passes through unchanged (full opacity of
        // the encoded source, alpha 0.5 -> 128).
        const uint8_t r = mem.PixelR(COLOR_GVA, 26, 37, W);
        const uint8_t g = mem.PixelG(COLOR_GVA, 26, 37, W);
        const uint8_t b = mem.PixelB(COLOR_GVA, 26, 37, W);
        const uint8_t a = mem.PixelA(COLOR_GVA, 26, 37, W);
        CHECK(r == 255);
        CHECK(g == 0);
        CHECK(b == 0);
        CHECK(a == 128);   // source alpha written through, no blend

        // Outside the triangle the destination green is untouched.
        CHECK(mem.PixelG(COLOR_GVA, 60, 4, W) == 255);
    }

    // =====================================================================
    // E: default topology is Triangles (regression). A 6-vertex triangle
    //    list with topology left at the default produces exactly 2
    //    triangles -- not a strip's 4.
    // =====================================================================
    std::cout << "[raster-blend] E: default topology is Triangles\n";
    {
        RasterTarget target{COLOR_GVA, 0, W, H};   // default topology
        std::vector<uint32_t> verts;
        for (int t = 0; t < 2; ++t) {
            for (uint32_t d : Vertex(-0.3f, -0.3f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f)) verts.push_back(d);
            for (uint32_t d : Vertex( 0.3f, -0.3f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f)) verts.push_back(d);
            for (uint32_t d : Vertex(-0.3f,  0.3f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f)) verts.push_back(d);
        }
        auto stats = SoftwareRasterizer::DrawTriangles(verts, 8, target, vp, &mem);
        CHECK(stats.ok);
        CHECK(stats.triangles_in == 2);   // 6 verts / 3, not a strip's 4
    }

    std::cout << "[raster-blend] " << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
