// ============================================================================
// ProsperoLayer RDNA2 Core - Software rasterizer (round 13, deepened 18)
// ----------------------------------------------------------------------------
// Round 18 changes:
//   * full homogeneous polygon clipping (Sutherland-Hodgman against the six
//     GCN clip planes) replacing the round-13 guard-band reject + w<=0 cull,
//   * viewport transform now honours the REAL PA_CL_VPORT_* register
//     semantics (screen = ndc * scale + offset) -- and fixes the round-13
//     latent bug where the y-scale term used half_w instead of half_h,
//   * depth comparison driven by DB_DEPTH_CONTROL.ZFUNC (LESS was hard-wired).
// ============================================================================
#include "gpu/software_rasterizer.hpp"

#include <cmath>
#include <cstring>

namespace PS5::GPU {
namespace {

float BitsToF(uint32_t v) { float f; std::memcpy(&f, &v, 4); return f; }

constexpr int kMaxClipVerts = 3 + 6;   // a triangle gains <= 1 vert per plane

struct ClipVertex {
    float x{0}, y{0}, z{0}, w{0};      // homogeneous clip space
    float r{0}, g{0}, b{0}, a{0};      // attributes (interpolate linearly in
                                       // clip space -- correct homogeneous law)
    float u{0}, v{0};                  // Round 21: texture coordinates
};

struct ScreenVertex {
    float x{0}, y{0};        // screen-space (pixel centre at +0.5)
    float z{0};              // NDC depth, written after the depth test
    float inv_w{0};          // 1/w for perspective-correct interpolation
    float r{0}, g{0}, b{0}, a{0};
    float u{0}, v{0};        // Round 21: texture coordinates
};

// Edge function (positive when the point is on the left of a->b).
float Edge(float ax, float ay, float bx, float by, float px, float py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

// Signed distance to a clip plane; >= 0 means inside. The six GCN planes:
//   0: x + w >= 0 (left)      1: w - x >= 0 (right)
//   2: y + w >= 0 (bottom)    3: w - y >= 0 (top)
//   4: z >= 0     (near, D3D/GCN z' in [0,1])   5: w - z >= 0 (far)
float ClipDistance(const ClipVertex& v, int plane) {
    switch (plane) {
        case 0: return v.x + v.w;
        case 1: return v.w - v.x;
        case 2: return v.y + v.w;
        case 3: return v.w - v.y;
        case 4: return v.z;
        default: return v.w - v.z;
    }
}

// One Sutherland-Hodgman pass: clip polygon against a single plane.
void ClipAgainstPlane(const ClipVertex* in, int in_count, int plane,
                      ClipVertex* out, int& out_count) {
    out_count = 0;
    if (in_count == 0) {
        return;
    }
    for (int i = 0; i < in_count; ++i) {
        const ClipVertex& a = in[i];
        const ClipVertex& b = in[(i + 1) % in_count];
        const float da = ClipDistance(a, plane);
        const float db = ClipDistance(b, plane);
        const bool a_in = da >= 0.0f;
        const bool b_in = db >= 0.0f;
        if (a_in && b_in) {
            out[out_count++] = b;                 // both inside: keep b
        } else if (a_in && !b_in) {
            // leaving: emit the boundary intersection
            const float t = da / (da - db);
            ClipVertex v;
            v.x = a.x + (b.x - a.x) * t;
            v.y = a.y + (b.y - a.y) * t;
            v.z = a.z + (b.z - a.z) * t;
            v.w = a.w + (b.w - a.w) * t;
            v.r = a.r + (b.r - a.r) * t;
            v.g = a.g + (b.g - a.g) * t;
            v.b = a.b + (b.b - a.b) * t;
            v.a = a.a + (b.a - a.a) * t;
            v.u = a.u + (b.u - a.u) * t;
            v.v = a.v + (b.v - a.v) * t;
            out[out_count++] = v;
        } else if (!a_in && b_in) {
            // entering: emit the boundary intersection, then b
            const float t = da / (da - db);
            ClipVertex v;
            v.x = a.x + (b.x - a.x) * t;
            v.y = a.y + (b.y - a.y) * t;
            v.z = a.z + (b.z - a.z) * t;
            v.w = a.w + (b.w - a.w) * t;
            v.r = a.r + (b.r - a.r) * t;
            v.g = a.g + (b.g - a.g) * t;
            v.b = a.b + (b.b - a.b) * t;
            v.a = a.a + (b.a - a.a) * t;
            v.u = a.u + (b.u - a.u) * t;
            v.v = a.v + (b.v - a.v) * t;
            out[out_count++] = v;
            out[out_count++] = b;
        }
        // both outside: emit nothing
    }
}

bool ZFuncPass(Pm4::ZFunc func, float z, float ref) {
    switch (func) {
        case Pm4::ZFunc::Never:    return false;
        case Pm4::ZFunc::Less:     return z <  ref;
        case Pm4::ZFunc::Equal:    return z == ref;
        case Pm4::ZFunc::LEqual:   return z <= ref;
        case Pm4::ZFunc::Greater:  return z >  ref;
        case Pm4::ZFunc::NotEqual: return z != ref;
        case Pm4::ZFunc::GEqual:   return z >= ref;
        case Pm4::ZFunc::Always:   return true;
    }
    return false;
}

// ----------------------------------------------------------------------------
// Round 21: nearest-neighbour texture sampling. Reads one RGBA8 texel out of
// a bound RasterTexture, with UV clamped to the [0,1] edge (CLAMP_TO_EDGE).
// Fail-closed: a null data pointer, zero dimensions, or an unsupported
// format returns opaque black (0,0,0,1) so the draw never crashes. Returns
// the sampled RGBA in 0..1 floats ready to feed the colour encoder.
// ----------------------------------------------------------------------------
void SampleTextureNearest(const RasterTexture& tex, float u, float v,
                          float& r, float& g, float& b, float& a) {
    if (tex.data == nullptr || tex.width == 0 || tex.height == 0 ||
        tex.format != Pm4::GuestColorFormat::R8G8B8A8Unorm) {
        r = 0.0f; g = 0.0f; b = 0.0f; a = 1.0f;   // fail closed: opaque black
        return;
    }
    // Clamp UV to [0,1] (CLAMP_TO_EDGE).
    if (!(u >= 0.0f)) u = 0.0f;          // catches NaN too
    if (!(u <= 1.0f)) u = 1.0f;
    if (!(v >= 0.0f)) v = 0.0f;
    if (!(v <= 1.0f)) v = 1.0f;
    // Nearest texel: scale to [0,width-1] x [0,height-1].
    int32_t tx = static_cast<int32_t>(u * static_cast<float>(tex.width - 1) + 0.5f);
    int32_t ty = static_cast<int32_t>(v * static_cast<float>(tex.height - 1) + 0.5f);
    if (tx < 0) tx = 0;
    if (tx > static_cast<int32_t>(tex.width - 1)) tx = tex.width - 1;
    if (ty < 0) ty = 0;
    if (ty > static_cast<int32_t>(tex.height - 1)) ty = tex.height - 1;
    const size_t idx =
        (static_cast<size_t>(ty) * tex.width + static_cast<size_t>(tx)) * 4u;
    // Bounds check: if data_size is known, verify the texel is within bounds
    // to prevent out-of-bounds read when tex.data is smaller than expected.
    if (tex.data_size > 0 && idx + 4u > tex.data_size) {
        r = 0.0f; g = 0.0f; b = 0.0f; a = 1.0f;   // fail closed: opaque black
        return;
    }
    const uint8_t* p = tex.data + idx;
    r = p[0] / 255.0f;
    g = p[1] / 255.0f;
    b = p[2] / 255.0f;
    a = p[3] / 255.0f;
}

// ----------------------------------------------------------------------------
// Round 22: alpha blending. Decodes the existing destination pixel back to
// RGBA floats, then applies result = src*sf <op> dst*df. Only RGBA8 and
// B8G8R8A8 destinations are blended (the formats a software stage realistically
// blends against); other formats fall back to "overwrite" -- the fail-closed
// behaviour. Unknown factors/ops also fall back to overwrite. `dst` points
// at the destination pixel (bpp bytes); the blended result is written back
// there through the per-format encoder so the byte layout stays correct.
// ----------------------------------------------------------------------------
float BlendFactorValue(int factor, float sa) {
    switch (factor) {
        case BlendZero:             return 0.0f;
        case BlendOne:              return 1.0f;
        case BlendSrcAlpha:         return sa;
        case BlendOneMinusSrcAlpha: return 1.0f - sa;
        default:                    return 0.0f;   // fail closed: ZERO
    }
}

// ----------------------------------------------------------------------------
// Round 20: per-format colour encoders. Input is the interpolated RGBA float
// tuple (each in 0..1 for the normalised layouts); output is the guest
// pixel's byte layout. Half-float conversion follows IEEE 754 binary16
// (round-to-nearest-even, denormals preserved); the sRGB curve is the
// IEC 61966-2-1 transfer function; the packed layouts follow the Vulkan
// pack orders (A2B10G10R10 = A2[31:30] B10[29:20] G10[19:10] R10[9:0],
// B10G11R11 = B10[31:22] G11[21:11] R11[10:0], each channel a 5-exponent-bit
// float with NO sign bit).
// ----------------------------------------------------------------------------
uint16_t FloatToHalf(float v) {
    uint32_t bits = 0;
    std::memcpy(&bits, &v, 4);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    int32_t exp = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant = bits & 0x7FFFFFu;
    if (((bits >> 23) & 0xFFu) == 0xFFu) {
        // Inf / NaN
        return static_cast<uint16_t>(sign | 0x7C00u | (mant != 0u ? 0x200u : 0u));
    }
    if (exp <= 0) {
        // too small: zero or a half-denormal (rounded)
        if (exp < -10) return static_cast<uint16_t>(sign);
        const uint32_t half_denorm = mant | 0x800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - exp);
        uint32_t denorm = half_denorm >> shift;
        // round to nearest even
        const uint32_t round_bit = (half_denorm >> (shift - 1)) & 1u;
        const uint32_t sticky = (shift > 1)
            ? ((half_denorm << (32 - (shift - 1))) != 0u ? 1u : 0u) : 0u;
        denorm += round_bit + sticky - ((denorm & 1u) & round_bit);
        return static_cast<uint16_t>(sign | denorm);
    }
    if (exp >= 0x1F) {
        // overflow -> Inf
        return static_cast<uint16_t>(sign | 0x7C00u);
    }
    uint32_t half = sign | (static_cast<uint32_t>(exp) << 10) | (mant >> 13);
    // round to nearest even on the dropped mantissa bits
    const uint32_t dropped = mant & 0x1FFFu;
    if (dropped > 0x1000u || (dropped == 0x1000u && (half & 1u) != 0u)) {
        half++;
    }
    return static_cast<uint16_t>(half);
}

float SrgbEncode(float c) {
    if (c <= 0.0f) return 0.0f;
    if (c >= 1.0f) return 1.0f;
    return c <= 0.0031308f ? c * 12.92f
                           : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// The 11-bit / 10-bit unsigned float channels of B10G11R11 (no sign bit,
// 5-bit exponent, 6/5-bit mantissa; values >= 2^16 saturate to +Inf per the
// VK spec's pack rules -- in practice clamp).
uint32_t FloatToUf11(float v) {
    if (v <= 0.0f) return 0;
    if (v >= 65024.0f) return 0x7C0u;    // largest finite, avoid Inf visual
    uint32_t bits = 0;
    std::memcpy(&bits, &v, 4);
    const int32_t exp = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    const uint32_t mant = bits & 0x7FFFFFu;
    if (exp <= 0) return 0;
    if (exp >= 0x1F) return 0x7C0u;
    // 11-bit channel: 5 exp + 6 mantissa -> keep mant>>17
    return (static_cast<uint32_t>(exp) << 6) | (mant >> 17);
}

uint32_t FloatToUf10(float v) {
    if (v <= 0.0f) return 0;
    if (v >= 65024.0f) return 0x3E0u;
    uint32_t bits = 0;
    std::memcpy(&bits, &v, 4);
    const int32_t exp = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    const uint32_t mant = bits & 0x7FFFFFu;
    if (exp <= 0) return 0;
    if (exp >= 0x1F) return 0x3E0u;
    // 10-bit channel: 5 exp + 5 mantissa -> keep mant>>18
    return (static_cast<uint32_t>(exp) << 5) | (mant >> 18);
}

// Encodes one interpolated RGBA pixel into `dst` according to the target's
// format. `dst` has at least bpp bytes.
void EncodePixel(Pm4::GuestColorFormat fmt, float r, float g, float b, float a,
                 uint8_t* dst) {
    auto u8c = [](float c) {
        const int32_t v = static_cast<int32_t>(c * 255.0f + 0.5f);
        return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
    };
    auto s8c = [](float c) {   // -1..1 -> -128..127 (SNORM)
        float v = c;
        if (v < -1.0f) v = -1.0f;
        if (v > 1.0f) v = 1.0f;
        const int32_t i = static_cast<int32_t>(v * 127.0f + (v >= 0.0f ? 0.5f : -0.5f));
        return static_cast<uint8_t>(static_cast<int8_t>(i));
    };
    auto u16c = [](float c) {
        const int64_t v = static_cast<int64_t>(c * 65535.0f + 0.5f);
        const int64_t cl = v < 0 ? 0 : (v > 65535 ? 65535 : v);
        return static_cast<uint16_t>(cl);
    };
    auto s16c = [](float c) {
        float v = c;
        if (v < -1.0f) v = -1.0f;
        if (v > 1.0f) v = 1.0f;
        const int32_t i = static_cast<int32_t>(v * 32767.0f + (v >= 0.0f ? 0.5f : -0.5f));
        return static_cast<uint16_t>(static_cast<int16_t>(i));
    };
    switch (fmt) {
        case Pm4::GuestColorFormat::R8G8B8A8Unorm:
            dst[0] = u8c(r); dst[1] = u8c(g); dst[2] = u8c(b); dst[3] = u8c(a);
            return;
        case Pm4::GuestColorFormat::B8G8R8A8Unorm:
            dst[0] = u8c(b); dst[1] = u8c(g); dst[2] = u8c(r); dst[3] = u8c(a);
            return;
        case Pm4::GuestColorFormat::R8G8B8A8Srgb:
            dst[0] = u8c(SrgbEncode(r));
            dst[1] = u8c(SrgbEncode(g));
            dst[2] = u8c(SrgbEncode(b));
            dst[3] = u8c(a);   // alpha is linear in sRGB formats
            return;
        case Pm4::GuestColorFormat::R8G8B8A8Snorm: {
            dst[0] = s8c(r); dst[1] = s8c(g); dst[2] = s8c(b); dst[3] = s8c(a);
            return;
        }
        case Pm4::GuestColorFormat::A2B10G10R10UnormPack32: {
            auto c10 = [](float c) {
                const int32_t v = static_cast<int32_t>(c * 1023.0f + 0.5f);
                return static_cast<uint32_t>(v < 0 ? 0 : (v > 1023 ? 1023 : v));
            };
            const auto c2 = [&u8c](float c) { return u8c(c) >> 6; };  // 0..3
            const uint32_t packed = (c2(a) << 30) | (c10(b) << 20) |
                                    (c10(g) << 10) | c10(r);
            std::memcpy(dst, &packed, 4);
            return;
        }
        case Pm4::GuestColorFormat::B10G11R11UfloatPack32: {
            const uint32_t packed = (FloatToUf10(b) << 22) |
                                    (FloatToUf11(g) << 11) | FloatToUf11(r);
            std::memcpy(dst, &packed, 4);
            return;
        }
        case Pm4::GuestColorFormat::R16G16B16A16Sfloat: {
            const uint16_t h[4] = {FloatToHalf(r), FloatToHalf(g),
                                  FloatToHalf(b), FloatToHalf(a)};
            std::memcpy(dst, h, 8);
            return;
        }
        case Pm4::GuestColorFormat::R16G16B16A16Unorm: {
            const uint16_t h[4] = {u16c(r), u16c(g), u16c(b), u16c(a)};
            std::memcpy(dst, h, 8);
            return;
        }
        case Pm4::GuestColorFormat::R16G16B16A16Snorm: {
            const uint16_t h[4] = {s16c(r), s16c(g), s16c(b), s16c(a)};
            std::memcpy(dst, h, 8);
            return;
        }
        case Pm4::GuestColorFormat::R32Sfloat: {
            std::memcpy(dst, &r, 4);   // red-only target
            return;
        }
        default:
            return;   // Invalid: nothing (the gate upstream rejects it)
    }
}

// ----------------------------------------------------------------------------
// Round 22: alpha blending. Decodes the existing destination pixel back to
// RGBA floats, then applies result = src*sf <op> dst*df. Only RGBA8 and
// B8G8R8A8 destinations are blended (the formats a software stage realistically
// blends against); other formats fall back to "overwrite" -- the fail-closed
// behaviour. Unknown factors/ops also fall back to overwrite. `dst` points
// at the destination pixel (bpp bytes); the blended result is written back
// there through the per-format encoder so the byte layout stays correct.
// ----------------------------------------------------------------------------
void BlendAndWrite(const BlendState& blend, Pm4::GuestColorFormat fmt,
                   float r, float g, float b, float a, uint8_t* dst) {
    if (!blend.enabled) {
        EncodePixel(fmt, r, g, b, a, dst);
        return;
    }
    // Decode the destination back to RGBA floats. Only the RGBA8 / BGRA8
    // layouts are blended (a 1:1 byte read); anything else fails closed to
    // a plain overwrite.
    float dr = 0, dg = 0, db = 0, da = 1;
    bool decoded = false;
    if (fmt == Pm4::GuestColorFormat::R8G8B8A8Unorm) {
        dr = dst[0] / 255.0f; dg = dst[1] / 255.0f;
        db = dst[2] / 255.0f; da = dst[3] / 255.0f;
        decoded = true;
    } else if (fmt == Pm4::GuestColorFormat::B8G8R8A8Unorm) {
        db = dst[0] / 255.0f; dg = dst[1] / 255.0f;
        dr = dst[2] / 255.0f; da = dst[3] / 255.0f;
        decoded = true;
    }
    if (!decoded) {
        EncodePixel(fmt, r, g, b, a, dst);   // fail closed: overwrite
        return;
    }
    const float sf = BlendFactorValue(blend.src_factor, a);
    const float df = BlendFactorValue(blend.dst_factor, a);
    float orr, org, orb, ora;
    switch (blend.blend_op) {
        case BlendOpSubtract:
            orr = r * sf - dr * df; org = g * sf - dg * df;
            orb = b * sf - db * df; ora = a * sf - da * df;
            break;
        case BlendOpReverseSubtract:
            orr = dr * df - r * sf; org = dg * df - g * sf;
            orb = db * df - b * sf; ora = da * df - a * sf;
            break;
        case BlendOpAdd:
        default:   // fail closed: ADD on unknown op
            orr = r * sf + dr * df; org = g * sf + dg * df;
            orb = b * sf + db * df; ora = a * sf + da * df;
            break;
    }
    EncodePixel(fmt, orr, org, orb, ora, dst);
}

} // namespace

SoftwareRasterStats SoftwareRasterizer::DrawTriangles(const std::vector<uint32_t>& vertices,
                                                      uint32_t dwords_per_vertex,
                                                      const RasterTarget& target,
                                                      const RasterViewport& viewport,
                                                      GpuGuestMemory* mem,
                                                      bool write_depth) {
    SoftwareRasterStats stats;
    if (mem == nullptr) {
        stats.error = "no guest-memory bridge";
        return stats;
    }
    if (target.width == 0 || target.height == 0 ||
        target.color_gva == 0) {
        stats.error = "invalid render target";
        return stats;
    }
    if (dwords_per_vertex < 4) {
        stats.error = "vertex needs at least clip-space xyzw";
        return stats;
    }
    if (viewport.IsDegenerate()) {
        stats.error = "degenerate viewport (PA_CL_VPORT scale is zero)";
        return stats;
    }
    const bool has_color = dwords_per_vertex >= 8;
    const bool has_uv = dwords_per_vertex >= 10;   // Round 21: UVs at [8..9]
    const size_t vertex_count =
        vertices.size() / dwords_per_vertex;
    if (vertex_count < 3) {
        stats.ok = true;   // nothing to rasterize (still a successful draw)
        return stats;
    }

    // Round 20: the colour plane stride follows the target's format (the
    // historical RGBA8 default keeps every existing caller byte-identical).
    const uint32_t bpp =
        Pm4::GuestColorFormatBytesPerPixel(target.color_format);
    if (bpp == 0) {
        stats.error = "invalid target colour format (GuestColorFormat::Invalid)";
        return stats;
    }
    const size_t color_bytes =
        static_cast<size_t>(target.width) * target.height * bpp;
    // The depth plane is ALWAYS one float32 per pixel (DB 32_FLOAT),
    // independent of the colour format.
    const size_t depth_bytes =
        target.depth_gva != 0
            ? static_cast<size_t>(target.width) * target.height * 4u
            : 0;
    // Scratch surfaces (so reads/writes stay in one host buffer per plane and
    // a partially-covered triangle still sees cleared destination).
    std::vector<uint8_t> color(color_bytes, 0);
    std::vector<float> depth(depth_bytes / 4, 1.0f);
    if (target.depth_gva != 0) {
        // Prime the depth plane from guest memory when one is bound.
        std::vector<uint32_t> depth_bits(depth.size());
        (void)mem->ReadDwords(target.depth_gva, depth_bits.data(), depth_bits.size());
        for (size_t i = 0; i < depth.size(); ++i) {
            std::memcpy(&depth[i], &depth_bits[i], 4);
        }
    }
    if (target.color_write) {
        (void)mem->ReadDwords(target.color_gva,
                              reinterpret_cast<uint32_t*>(color.data()),
                              color_bytes / 4);
    }

    // Round 22: decompose the vertex stream into individual triangles
    // according to the target's topology. Triangles (the historical
    // behaviour) takes consecutive non-overlapping triples; a Strip shares
    // edges and flips winding on odd triangles; a Fan pins vertex 0 as the
    // shared centre. The triples reference indices into the vertex stream
    // and are then fed to the existing clip/raster pipeline unchanged.
    struct TriIdx { size_t i0, i1, i2; };
    std::vector<TriIdx> tris;
    if (target.topology == TopologyTriangleStrip) {
        for (size_t i = 0; i + 2 < vertex_count; ++i) {
            // Odd triangles flip winding so the strip stays consistently
            // front-facing.
            if ((i & 1u) == 0u) {
                tris.push_back({i, i + 1, i + 2});
            } else {
                tris.push_back({i, i + 2, i + 1});
            }
        }
    } else if (target.topology == TopologyTriangleFan) {
        for (size_t i = 1; i + 1 < vertex_count; ++i) {
            tris.push_back({0, i, i + 1});
        }
    } else {   // TopologyTriangles (default)
        for (size_t i = 0; i + 2 < vertex_count; i += 3) {
            tris.push_back({i, i + 1, i + 2});
        }
    }

    for (const TriIdx& tri : tris) {
        stats.triangles_in++;

        // ---- clip stage: homogeneous polygon clipping (6 planes) ----------
        ClipVertex poly[kMaxClipVerts];
        int poly_count = 0;
        bool valid = true;
        for (int i = 0; i < 3; ++i) {
            const size_t vi = (i == 0) ? tri.i0 : (i == 1) ? tri.i1 : tri.i2;
            const uint32_t* v =
                vertices.data() + vi * dwords_per_vertex;
            const float w = BitsToF(v[3]);
            // w == 0 collapses the vertex onto the z=0 boundary line; it can
            // never contribute area, so treat exact-zero w as fully outside.
            if (!(w > 0.0f) && !(w < 0.0f)) {
                valid = false;
                break;
            }
            ClipVertex cv;
            cv.x = BitsToF(v[0]);
            cv.y = BitsToF(v[1]);
            cv.z = BitsToF(v[2]);
            cv.w = w;
            if (has_color) {
                cv.r = BitsToF(v[4]);
                cv.g = BitsToF(v[5]);
                cv.b = BitsToF(v[6]);
                cv.a = BitsToF(v[7]);
            }
            if (has_uv) {                 // Round 21: UV at dwords [8..9]
                cv.u = BitsToF(v[8]);
                cv.v = BitsToF(v[9]);
            }
            poly[poly_count++] = cv;
        }
        if (!valid) {
            stats.triangles_culled++;
            continue;
        }

        for (int plane = 0; plane < 6 && poly_count > 0; ++plane) {
            ClipVertex next[kMaxClipVerts];
            int next_count = 0;
            ClipAgainstPlane(poly, poly_count, plane, next, next_count);
            std::memcpy(poly, next, sizeof(ClipVertex) * static_cast<size_t>(next_count));
            poly_count = next_count;
        }
        if (poly_count < 3) {
            stats.triangles_culled++;   // fully outside the frustum
            continue;
        }
        if (poly_count > 3) {
            stats.triangles_clipped++;  // the geometry crossed a clip plane
        }

        // ---- clip -> NDC -> screen ----------------------------------------
        ScreenVertex sv[kMaxClipVerts];
        bool degenerate = false;
        for (int i = 0; i < poly_count; ++i) {
            const ClipVertex& cv = poly[i];
            if (!(std::fabs(cv.w) > 1e-12f)) {
                degenerate = true;
                break;
            }
            const float inv_w = 1.0f / cv.w;
            const float ndc_x = cv.x * inv_w;
            const float ndc_y = cv.y * inv_w;
            // REAL PA_CL_VPORT semantics: screen = ndc * scale + offset.
            sv[i].x = ndc_x * viewport.scale_x + viewport.scale_x_offset;
            sv[i].y = ndc_y * viewport.scale_y + viewport.scale_y_offset;
            sv[i].z = cv.z * inv_w;
            sv[i].inv_w = inv_w;
            sv[i].r = cv.r;
            sv[i].g = cv.g;
            sv[i].b = cv.b;
            sv[i].a = cv.a;
            sv[i].u = cv.u;       // Round 21: carry UV into screen space
            sv[i].v = cv.v;
        }
        if (degenerate) {
            stats.triangles_culled++;
            continue;
        }

        // Cheap full-reject for polygons entirely off the target.
        float min_x = sv[0].x, max_x = sv[0].x, min_y = sv[0].y, max_y = sv[0].y;
        for (int i = 1; i < poly_count; ++i) {
            min_x = std::min(min_x, sv[i].x);
            max_x = std::max(max_x, sv[i].x);
            min_y = std::min(min_y, sv[i].y);
            max_y = std::max(max_y, sv[i].y);
        }
        if (max_x < 0.0f || max_y < 0.0f ||
            min_x > static_cast<float>(target.width) ||
            min_y > static_cast<float>(target.height)) {
            stats.triangles_culled++;
            continue;
        }

        // ---- fan-triangulate the clipped polygon ---------------------------
        bool any_slice_drawn = false;
        for (int fan = 1; fan + 1 < poly_count && !degenerate; ++fan) {
            const ScreenVertex* t[3] = { &sv[0], &sv[fan], &sv[fan + 1] };

            float area = Edge(t[0]->x, t[0]->y, t[1]->x, t[1]->y,
                              t[2]->x, t[2]->y);
            if (std::fabs(area) < 1e-9f) {
                continue;   // degenerate fan slice
            }

            float tmin_x = std::min(t[0]->x, std::min(t[1]->x, t[2]->x));
            float tmax_x = std::max(t[0]->x, std::max(t[1]->x, t[2]->x));
            float tmin_y = std::min(t[0]->y, std::min(t[1]->y, t[2]->y));
            float tmax_y = std::max(t[0]->y, std::max(t[1]->y, t[2]->y));
            int32_t x0 = std::max<int32_t>(0, static_cast<int32_t>(std::floor(tmin_x)));
            int32_t x1 = std::min<int32_t>(static_cast<int32_t>(target.width) - 1,
                                           static_cast<int32_t>(std::ceil(tmax_x)));
            int32_t y0 = std::max<int32_t>(0, static_cast<int32_t>(std::floor(tmin_y)));
            int32_t y1 = std::min<int32_t>(static_cast<int32_t>(target.height) - 1,
                                           static_cast<int32_t>(std::ceil(tmax_y)));
            if (target.scissor_enable) {
                // Scissor BR is exclusive, while the raster loop is inclusive.
                x0 = std::max<int32_t>(x0, static_cast<int32_t>(target.scissor_left));
                x1 = std::min<int32_t>(x1, static_cast<int32_t>(target.scissor_right) - 1);
                y0 = std::max<int32_t>(y0, static_cast<int32_t>(target.scissor_top));
                y1 = std::min<int32_t>(y1, static_cast<int32_t>(target.scissor_bottom) - 1);
            }
            if (x0 > x1 || y0 > y1) {
                continue;
            }

            const bool ccw = area < 0.0f;
            stats.triangles_drawn++;
            any_slice_drawn = true;

            // ---- pixel stage: barycentric coverage + depth + colour --------
            for (int32_t py = y0; py <= y1; ++py) {
                for (int32_t px = x0; px <= x1; ++px) {
                    const float p_x = static_cast<float>(px) + 0.5f;
                    const float p_y = static_cast<float>(py) + 0.5f;
                    float w0 = Edge(t[1]->x, t[1]->y, t[2]->x, t[2]->y, p_x, p_y);
                    float w1 = Edge(t[2]->x, t[2]->y, t[0]->x, t[0]->y, p_x, p_y);
                    float w2 = Edge(t[0]->x, t[0]->y, t[1]->x, t[1]->y, p_x, p_y);
                    if (ccw) { w0 = -w0; w1 = -w1; w2 = -w2; }
                    if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
                        continue;   // outside the triangle
                    }
                    const float b0 = w0 / area;
                    const float b1 = w1 / area;
                    const float b2 = w2 / area;
                    // NOTE: with the sign fix above, w0+w1+w2 == |area|, so the
                    // barycentrics sum to 1 regardless of winding.

                    // Depth (z is already divided by w per vertex; interpolate
                    // the divided values linearly in screen space -- correct).
                    const float z = b0 * t[0]->z + b1 * t[1]->z + b2 * t[2]->z;
                    const size_t pixel =
                        static_cast<size_t>(py) * target.width + static_cast<size_t>(px);
                    if (target.depth_gva != 0) {
                        if (!ZFuncPass(target.zfunc, z, depth[pixel])) {
                            stats.pixels_depth_rejected++;
                            continue;
                        }
                    }

                    if (target.color_write) {
                        // Perspective-correct colour interpolation:
                        // attr = sum(bi * ai / wi) / sum(bi / wi)
                        if (has_color) {
                            const float iw0 = b0 * t[0]->inv_w;
                            const float iw1 = b1 * t[1]->inv_w;
                            const float iw2 = b2 * t[2]->inv_w;
                            const float iw_sum = iw0 + iw1 + iw2;
                            const auto interp = [&](float a0, float a1, float a2) {
                                return (iw0 * a0 + iw1 * a1 + iw2 * a2) / iw_sum;
                            };
                            float r = interp(t[0]->r, t[1]->r, t[2]->r);
                            float g = interp(t[0]->g, t[1]->g, t[2]->g);
                            float b = interp(t[0]->b, t[1]->b, t[2]->b);
                            float a = interp(t[0]->a, t[1]->a, t[2]->a);
                            // Round 21: nearest-neighbour texture sampling. A
                            // bound texture (non-null data) with UV attributes
                            // samples one RGBA8 texel and modulates the
                            // interpolated vertex colour by it (MODULATE). No
                            // UVs or no texture -> unchanged vertex colour.
                            if (target.texture.data != nullptr && has_uv) {
                                const float u = interp(t[0]->u, t[1]->u, t[2]->u);
                                const float v = interp(t[0]->v, t[1]->v, t[2]->v);
                                float tr, tg, tb, ta;
                                SampleTextureNearest(target.texture, u, v,
                                                     tr, tg, tb, ta);
                                r *= tr; g *= tg; b *= tb; a *= ta;
                            }
                            // Round 20: per-format encode (RGBA8 stays the
                            // historical bytes; 16F/packed/sRGB layouts are
                            // written per the guest's CB_COLOR0_INFO). Round
                            // 22: blend against the existing destination when
                            // a BlendState is enabled (fail-closed overwrite
                            // when disabled or unsupported).
                            BlendAndWrite(target.blend, target.color_format,
                                          r, g, b, a,
                                          color.data() + pixel * bpp);
                        } else {
                            // No colour attributes: white, unless a bound
                            // texture with UVs supplies the colour directly.
                            float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
                            if (target.texture.data != nullptr && has_uv) {
                                const float iw0 = b0 * t[0]->inv_w;
                                const float iw1 = b1 * t[1]->inv_w;
                                const float iw2 = b2 * t[2]->inv_w;
                                const float iw_sum = iw0 + iw1 + iw2;
                                const auto interp = [&](float a0, float a1, float a2) {
                                    return (iw0 * a0 + iw1 * a1 + iw2 * a2) / iw_sum;
                                };
                                const float u = interp(t[0]->u, t[1]->u, t[2]->u);
                                const float v = interp(t[0]->v, t[1]->v, t[2]->v);
                                SampleTextureNearest(target.texture, u, v,
                                                     r, g, b, a);
                            }
                            if (target.color_format == Pm4::GuestColorFormat::R32Sfloat) {
                                std::memcpy(color.data() + pixel * bpp, &r, 4);
                            } else {
                                BlendAndWrite(target.blend, target.color_format,
                                              r, g, b, a,
                                              color.data() + pixel * bpp);
                            }
                        }
                    }
                    if (target.depth_gva != 0 && write_depth) {
                        depth[pixel] = z;
                    }
                    stats.pixels_written++;
                }
            }
        }
        // A polygon whose every fan slice was degenerate never rasterized:
        // account it as culled (the round-13 semantics for zero-area input).
        if (!any_slice_drawn) {
            stats.triangles_culled++;
        }
    }

    // Write the planes back to guest memory.
    if (target.color_write) {
        if (!mem->WriteDwords(target.color_gva,
                              reinterpret_cast<const uint32_t*>(color.data()),
                              color_bytes / 4)) {
            stats.error = "colour-plane write failed";
            return stats;
        }
    }
    if (target.depth_gva != 0 && write_depth) {
        std::vector<uint32_t> depth_bits(depth.size());
        for (size_t i = 0; i < depth.size(); ++i) {
            std::memcpy(&depth_bits[i], &depth[i], 4);
        }
        if (!mem->WriteDwords(target.depth_gva, depth_bits.data(), depth_bits.size())) {
            stats.error = "depth-plane write failed";
            return stats;
        }
    }

    stats.ok = true;
    return stats;
}

} // namespace PS5::GPU
