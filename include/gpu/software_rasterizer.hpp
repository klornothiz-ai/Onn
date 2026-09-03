#pragma once
// ============================================================================
// ProsperoLayer RDNA2 Core - Software rasterizer (round 13, deepened 18)
// ----------------------------------------------------------------------------
// Headless pixel stage on top of the real vertex path: the draw pipeline
// already executes the guest VS (GCN software interpreter or a Vulkan device)
// and writes transformed vertices back to guest memory in the lane-major
// format (m dwords per vertex). This module consumes those vertices and
// rasterizes triangles into a guest RGBA8 render target with:
//
//   * FULL homogeneous (frustum) polygon clipping -- Sutherland-Hodgman
//     against the six GCN clip planes -w<=x<=w, -w<=y<=w, 0<=z<=w -- with
//     attributes interpolated linearly in clip space at every intersection
//     (the near plane is genuinely clipped now, not "rejected"; round 13
//     only guard-band-rejected triangles with any w <= 0 vertex),
//   * the clip-space -> NDC -> viewport transform driven by the guest's REAL
//     PA_CL_VPORT_* registers (round 18): screen_x = ndc_x * XSCALE +
//     XOFFSET, screen_y = ndc_y * YSCALE + YOFFSET (float bits, pixels; a
//     top-left-origin guest programs a negative YSCALE -- no hidden flip),
//   * perspective-correct barycentric interpolation of vertex attributes,
//   * a z (depth) test whose comparison function comes from the guest's
//     DB_DEPTH_CONTROL.ZFUNC field (LESS/EQUAL/LEQUAL/... -- round 13 was
//     hard-wired to LESS) against a guest 32_FLOAT depth plane,
//   * RGBA8 colour writes through the same GpuGuestMemory bridge the PM4
//     translator uses, gated by CB_TARGET_MASK.TARGET0_ENABLE.
//
// Vertex layout consumed (m dwords per vertex, m >= 8):
//   [0..3] = clip-space x, y, z, w (float bits)
//   [4..7] = RGBA colour (float bits, 0..1)
// m == 4 renders depth-only (no colour attributes interpolated).
// ============================================================================

#include "gpu/gpu_guest_memory.hpp"
#include "graphics/guest_gpu/pm4.h"

#include <cstdint>
#include <string>
#include <vector>

namespace PS5::GPU {

struct SoftwareRasterStats {
    bool ok{false};
    std::string error;
    uint32_t triangles_in{0};
    uint32_t triangles_drawn{0};
    uint32_t triangles_culled{0};     // degenerate / fully clipped / off-screen
    uint32_t triangles_clipped{0};    // gained vertices crossing a clip plane
    uint64_t pixels_written{0};
    uint64_t pixels_depth_rejected{0};
};

// Round 21: a bound texture for nearest-neighbour sampling. The pixel stage
// reads RGBA8 texels (the only format implemented for now) out of a host-side
// buffer -- the guest's texture data is copied here by the caller before the
// draw. A null `data` (or zero dimensions) disables texturing for the draw
// (the rasterizer then falls back to the interpolated vertex colour, the
// round-13/18 behaviour), so existing callers that never bind a texture are
// byte-identical.
struct RasterTexture {
    uint32_t width{0};
    uint32_t height{0};
    Pm4::GuestColorFormat format{Pm4::GuestColorFormat::R8G8B8A8Unorm};
    const uint8_t* data{nullptr};   // not owned; must outlive the draw
    size_t data_size{0};            // size of data in bytes; 0 = unknown (no bounds check)
};

// Round 22: alpha-blend state, mirroring the GCN CB_BLEND0_CONTROL fields the
// PM4 translator extracts. The factors and operation are the small subset a
// software pixel stage needs for now; an extended set can be added later.
// Fail-closed: a disabled or unrecognised blend state means "overwrite the
// destination" exactly as before (no blending), so every existing caller that
// never touches `blend` is byte-identical.
enum BlendFactor : int {
    BlendZero            = 0,   // 0
    BlendOne             = 1,   // 1
    BlendSrcAlpha        = 2,   // src.a
    BlendOneMinusSrcAlpha= 3,  // 1 - src.a
};
enum BlendOp : int {
    BlendOpAdd             = 0,   // src*sf + dst*df
    BlendOpSubtract        = 1,   // src*sf - dst*df
    BlendOpReverseSubtract = 2,   // dst*df - src*sf
};
struct BlendState {
    bool enabled{false};
    int src_factor{BlendOne};     // BlendFactor
    int dst_factor{BlendZero};    // BlendFactor
    int blend_op{BlendOpAdd};     // BlendOp
};

// Round 22: primitive topology for the vertex stream. Triangles (the
// historical behaviour) is the default, so existing callers are unchanged.
enum Topology : int {
    TopologyTriangles     = 0,
    TopologyTriangleStrip = 1,
    TopologyTriangleFan   = 2,
};

struct RasterTarget {
    uint64_t color_gva{0};    // RGBA8, row-major, w*h*4 bytes
    uint64_t depth_gva{0};    // float32 depth, w*h*4 bytes (0 = no depth test)
    uint32_t width{0};
    uint32_t height{0};
    // Round 18: comparison function from DB_DEPTH_CONTROL.ZFUNC (default LESS,
    // the round-13 behaviour). Only consulted when depth_gva != 0.
    Pm4::ZFunc zfunc{Pm4::ZFunc::Less};
    // Round 18: colour-write gate from CB_TARGET_MASK.TARGET0_ENABLE. false
    // renders depth-only (the draw still executes; pixels are only tested
    // and the depth plane updated).
    bool color_write{true};
    // Round 20: the CB_COLOR0_INFO layout (converted through pm4.h). The
    // default keeps the historical RGBA8 UNORM plane for every existing
    // caller; the byte layout per pixel follows the format.
    Pm4::GuestColorFormat color_format{Pm4::GuestColorFormat::R8G8B8A8Unorm};
    // Optional PA_SC_SCREEN_SCISSOR rectangle. Right/bottom are exclusive.
    bool scissor_enable{false};
    uint32_t scissor_left{0};
    uint32_t scissor_top{0};
    uint32_t scissor_right{0};
    uint32_t scissor_bottom{0};
    // Round 21: optional bound texture. A default-constructed RasterTexture
    // (null data) leaves the pixel stage on the interpolated vertex colour
    // exactly as before. Vertex UVs are read from dwords [8..9] when
    // dwords_per_vertex >= 10; without UV attributes texturing is a no-op
    // even if a texture is bound.
    RasterTexture texture{};
    // Round 22: alpha-blend state. The default (disabled) overwrites the
    // destination, the round-13/18/20/21 behaviour.
    BlendState blend{};
    // Round 22: primitive topology. The default (Triangles) matches the
    // historical DrawTriangles behaviour; Strip and Fan decompose into
    // individual triangles before clipping/rasterization.
    Topology topology{TopologyTriangles};
};

struct RasterViewport {
    // Round 18 -- REAL PA_CL_VPORT semantics (float bits from the PM4
    // registers): screen = ndc * scale + offset, in pixels, after the
    // perspective divide. A fullscreen top-left-origin pass programs
    // XSCALE=W/2, XOFFSET=W/2, YSCALE=-H/2, YOFFSET=H/2.
    float scale_x{0.0f};
    float scale_x_offset{0.0f};
    float scale_y{0.0f};
    float scale_y_offset{0.0f};

    bool IsDegenerate() const {
        return scale_x == 0.0f || scale_y == 0.0f;
    }
    // The fullscreen viewport a guest programs for a whole-target pass.
    static RasterViewport Fullscreen(uint32_t width, uint32_t height) {
        RasterViewport vp;
        vp.scale_x = static_cast<float>(width) * 0.5f;
        vp.scale_x_offset = vp.scale_x;
        vp.scale_y = -static_cast<float>(height) * 0.5f;
        vp.scale_y_offset = -vp.scale_y;
        return vp;
    }
};

class SoftwareRasterizer {
public:
    // Rasterizes `vertex_count / 3` triangles from `vertices` (lane-major,
    // `dwords_per_vertex` each) into the target. Each triangle is first
    // clipped against the full frustum in homogeneous clip space (near
    // plane included), the surviving polygon is fan-triangulated, and every
    // fragment is depth-tested with the target's ZFunc before its RGBA8
    // colour is written. Returns per-draw statistics.
    static SoftwareRasterStats DrawTriangles(const std::vector<uint32_t>& vertices,
                                             uint32_t dwords_per_vertex,
                                             const RasterTarget& target,
                                             const RasterViewport& viewport,
                                             GpuGuestMemory* mem,
                                             bool write_depth = true);
};

} // namespace PS5::GPU
