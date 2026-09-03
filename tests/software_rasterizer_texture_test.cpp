// ============================================================================
// ProsperoLayer RDNA2 Core - Software Rasterizer Texture Sampling Test
// ----------------------------------------------------------------------------
// Round 21: verifies nearest-neighbour texture sampling added to the
// headless pixel stage. A small 2x2 RGBA8 texture with known colours is
// bound, a triangle covering the texture is rasterized with per-vertex UVs,
// and the sampled texel colours are checked against the render target.
// Also verifies the fail-closed path (null texture -> opaque-black / vertex
// colour fallback) and that a 10-dword vertex without a bound texture keeps
// the round-13/18 vertex-colour behaviour.
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
private:
    uint64_t m_base;
    std::vector<uint32_t> m_storage;
};

// A vertex: clip-space xyzw + RGBA + UV (10 dwords).
std::vector<uint32_t> VertexUV(float x, float y, float z, float w,
                               float r, float g, float b, float a,
                               float u, float v) {
    return { FBits(x), FBits(y), FBits(z), FBits(w),
             FBits(r), FBits(g), FBits(b), FBits(a),
             FBits(u), FBits(v) };
}

} // namespace

int main() {
    std::cout << "[raster-tex] round 21: texture sampling\n";
    FlatGuestMemory mem(0x1000, 0x100000 / 4);   // 1 MB arena

    const uint64_t COLOR_GVA = 0x1000;
    constexpr uint32_t W = 64, H = 64;
    const RasterViewport vp = RasterViewport::Fullscreen(W, H);

    // =====================================================================
    // A: a 2x2 RGBA8 texture; a full-screen triangle samples each corner
    //    texel with nearest-neighbour filtering.
    // =====================================================================
    std::cout << "[raster-tex] A: 2x2 nearest-neighbour sampling\n";
    {
        // Texel layout (RGBA8, row-major):
        //   (0,0)=red    (1,0)=green
        //   (0,1)=blue   (1,1)=white
        uint8_t tex[2 * 2 * 4] = {
            255, 0,   0,   255,    // (0,0) red
            0,   255, 0,   255,     // (1,0) green
            0,   0,   255, 255,     // (0,1) blue
            255, 255, 255, 255,     // (1,1) white
        };
        RasterTexture rtex;
        rtex.width = 2;
        rtex.height = 2;
        rtex.format = Pm4::GuestColorFormat::R8G8B8A8Unorm;
        rtex.data = tex;

        RasterTarget target{COLOR_GVA, 0, W, H};
        target.texture = rtex;

        // Full-screen triangle with white vertex colour (so modulation is a
        // no-op and the texel colour passes through unchanged). UVs map the
        // whole texture across the triangle: v0 -> (0,0), v1 -> (1,0),
        // v2 -> (0,1). With nearest sampling the texel at the v0 corner is
        // red, at the v1 corner green, at the v2 corner blue.
        std::vector<uint32_t> verts;
        for (uint32_t d : VertexUV(-1.0f, -1.0f, 0.0f, 1.0f, 1, 1, 1, 1, 0.0f, 0.0f)) verts.push_back(d);
        for (uint32_t d : VertexUV( 1.0f, -1.0f, 0.0f, 1.0f, 1, 1, 1, 1, 1.0f, 0.0f)) verts.push_back(d);
        for (uint32_t d : VertexUV(-1.0f,  1.0f, 0.0f, 1.0f, 1, 1, 1, 1, 0.0f, 1.0f)) verts.push_back(d);

        auto stats = SoftwareRasterizer::DrawTriangles(verts, 10, target, vp, &mem);
        CHECK(stats.ok);
        CHECK(stats.triangles_in == 1 && stats.triangles_drawn == 1);
        CHECK(stats.pixels_written > 1000);   // most of the 64x64 target

        // The Fullscreen viewport (YSCALE=-H/2, YOFFSET=H/2) flips Y: NDC
        // y=-1 -> screen y=64 (bottom), NDC y=1 -> screen y=0 (top). So:
        //   v0 (-1,-1,UV 0,0) -> bottom-left  -> red
        //   v1 ( 1,-1,UV 1,0) -> bottom-right -> green
        //   v2 (-1, 1,UV 0,1) -> top-left     -> blue
        // Near the v0 corner (bottom-left): UV ~ (0,0) -> red.
        CHECK(mem.PixelR(COLOR_GVA, 2, 60, W) == 255);
        CHECK(mem.PixelG(COLOR_GVA, 2, 60, W) == 0);
        CHECK(mem.PixelB(COLOR_GVA, 2, 60, W) == 0);

        // Near the v1 corner (bottom-right): UV ~ (1,0) -> green.
        CHECK(mem.PixelR(COLOR_GVA, 60, 60, W) == 0);
        CHECK(mem.PixelG(COLOR_GVA, 60, 60, W) == 255);
        CHECK(mem.PixelB(COLOR_GVA, 60, 60, W) == 0);

        // Near the v2 corner (top-left): UV ~ (0,1) -> blue.
        CHECK(mem.PixelR(COLOR_GVA, 2, 2, W) == 0);
        CHECK(mem.PixelG(COLOR_GVA, 2, 2, W) == 0);
        CHECK(mem.PixelB(COLOR_GVA, 2, 2, W) == 255);

        // Centre maps to roughly UV (1/3, 1/3) -> nearest texel is (0,0)=red
        // (since 1/3 * (2-1) + 0.5 = 0.83 -> round to 1? check). Actually
        // with a 2x2 texture the centre texel depends on rounding; just
        // verify the pixel is one of the four known colours (not black).
        const uint8_t cr = mem.PixelR(COLOR_GVA, 32, 32, W);
        const uint8_t cg = mem.PixelG(COLOR_GVA, 32, 32, W);
        const uint8_t cb = mem.PixelB(COLOR_GVA, 32, 32, W);
        const bool known = (cr == 255 && cg == 0 && cb == 0) ||
                           (cr == 0 && cg == 255 && cb == 0) ||
                           (cr == 0 && cg == 0 && cb == 255) ||
                           (cr == 255 && cg == 255 && cb == 255);
        CHECK(known);
    }

    // =====================================================================
    // B: modulation -- a non-white vertex colour tints the texel.
    // =====================================================================
    std::cout << "[raster-tex] B: vertex colour modulation\n";
    {
        uint8_t tex[2 * 2 * 4] = {
            255, 255, 255, 255,    // white texel everywhere
            255, 255, 255, 255,
            255, 255, 255, 255,
            255, 255, 255, 255,
        };
        RasterTexture rtex;
        rtex.width = 2;
        rtex.height = 2;
        rtex.format = Pm4::GuestColorFormat::R8G8B8A8Unorm;
        rtex.data = tex;

        RasterTarget target{COLOR_GVA, 0, W, H};
        target.texture = rtex;

        // Triangle with red vertex colour (255,0,0,1) and flat UVs that all
        // point at the white texel. Modulation: white * red = red.
        std::vector<uint32_t> verts;
        for (uint32_t d : VertexUV(-0.3f, -0.3f, 0.0f, 1.0f, 1, 0, 0, 1, 0.0f, 0.0f)) verts.push_back(d);
        for (uint32_t d : VertexUV( 0.3f, -0.3f, 0.0f, 1.0f, 1, 0, 0, 1, 0.0f, 0.0f)) verts.push_back(d);
        for (uint32_t d : VertexUV(-0.3f,  0.3f, 0.0f, 1.0f, 1, 0, 0, 1, 0.0f, 0.0f)) verts.push_back(d);

        auto stats = SoftwareRasterizer::DrawTriangles(verts, 10, target, vp, &mem);
        CHECK(stats.ok && stats.pixels_written > 0);
        // All covered pixels modulate white * red -> red.
        CHECK(mem.PixelR(COLOR_GVA, 28, 36, W) == 255);
        CHECK(mem.PixelG(COLOR_GVA, 28, 36, W) == 0);
        CHECK(mem.PixelB(COLOR_GVA, 28, 36, W) == 0);
    }

    // =====================================================================
    // C: fail-closed -- null texture data falls back to vertex colour
    //    (the round-13/18 behaviour), never crashes.
    // =====================================================================
    std::cout << "[raster-tex] C: fail-closed null texture\n";
    {
        RasterTarget target{COLOR_GVA, 0, W, H};
        // Default-constructed RasterTexture -> null data -> texturing disabled.
        target.texture = RasterTexture{};

        std::vector<uint32_t> verts;
        for (uint32_t d : VertexUV(-0.3f, -0.3f, 0.0f, 1.0f, 0, 0, 1, 1, 0.0f, 0.0f)) verts.push_back(d);
        for (uint32_t d : VertexUV( 0.3f, -0.3f, 0.0f, 1.0f, 0, 0, 1, 1, 0.0f, 0.0f)) verts.push_back(d);
        for (uint32_t d : VertexUV(-0.3f,  0.3f, 0.0f, 1.0f, 0, 0, 1, 1, 0.0f, 0.0f)) verts.push_back(d);

        auto stats = SoftwareRasterizer::DrawTriangles(verts, 10, target, vp, &mem);
        CHECK(stats.ok && stats.pixels_written > 0);
        // No texture -> vertex blue passes through unchanged.
        CHECK(mem.PixelR(COLOR_GVA, 28, 36, W) == 0);
        CHECK(mem.PixelG(COLOR_GVA, 28, 36, W) == 0);
        CHECK(mem.PixelB(COLOR_GVA, 28, 36, W) == 255);
    }

    // =====================================================================
    // D: fail-closed -- a texture with zero dimensions also falls back to
    //    the vertex colour (SampleTextureNearest returns opaque black, and
    //    with a non-white vertex colour the result is the vertex colour
    //    because the bound-texture gate already sees data != null... verify
    //    the black-tint path instead).
    // =====================================================================
    std::cout << "[raster-tex] D: fail-closed zero-size texture\n";
    {
        uint8_t dummy = 0;
        RasterTarget target{COLOR_GVA, 0, W, H};
        target.texture.width = 0;
        target.texture.height = 0;
        target.texture.format = Pm4::GuestColorFormat::R8G8B8A8Unorm;
        target.texture.data = &dummy;   // non-null but zero-size

        // White vertex colour; the sampler returns opaque black, modulation
        // white*black = black.
        std::vector<uint32_t> verts;
        for (uint32_t d : VertexUV(-0.3f, -0.3f, 0.0f, 1.0f, 1, 1, 1, 1, 0.0f, 0.0f)) verts.push_back(d);
        for (uint32_t d : VertexUV( 0.3f, -0.3f, 0.0f, 1.0f, 1, 1, 1, 1, 0.0f, 0.0f)) verts.push_back(d);
        for (uint32_t d : VertexUV(-0.3f,  0.3f, 0.0f, 1.0f, 1, 1, 1, 1, 0.0f, 0.0f)) verts.push_back(d);

        auto stats = SoftwareRasterizer::DrawTriangles(verts, 10, target, vp, &mem);
        CHECK(stats.ok && stats.pixels_written > 0);
        // Zero-size texture -> sampler returns opaque black; white*black=black.
        CHECK(mem.PixelR(COLOR_GVA, 28, 36, W) == 0);
        CHECK(mem.PixelG(COLOR_GVA, 28, 36, W) == 0);
        CHECK(mem.PixelB(COLOR_GVA, 28, 36, W) == 0);
    }

    std::cout << "[raster-tex] " << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
