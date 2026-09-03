// ============================================================================
// ProsperoLayer RDNA2 Core - PM4 -> real GPU VGT attribute-fetch test
// ----------------------------------------------------------------------------
// Proves the round 10 "VGT-style attribute fetching" wiring: a draw whose
// VS user data programs an attribute-fetch descriptor table (VS user data
// +5..6, see graphics/guest_gpu/pm4.h) performs a real gather --
//
//   * multiple attributes per lane: each lane's input is the CONCATENATION of
//     every attribute's dwords fetched at attr_gva[a] + vertex_id*stride[a]
//     (the classic VGT index -> address -> gather model), laid out lane-major
//     for the vertex-stage kernel (v0..v{k-1} per lane),
//   * the kernel is compiled in the strided lane model (k in / m out dwords
//     per lane) and executed on the real device via RunRDNA2Strided,
//   * transformed vertices (m dwords per vertex, lane-major) are written BACK
//     to guest memory,
//   * indexed draws (u32 and u16) gather through the decoded index stream --
//     a repeated index re-fetches the same vertex (gather semantics, not a
//     copy), a shuffled order re-fetches in the draw's order,
//   * non-indexed draws gather sequentially (vertex id = lane),
//   * the legacy round-9 single-stream model is untouched: no fetch table ->
//     fetch_enabled = false and the one-dword-per-lane path still runs,
//   * malformed descriptor tables (count 0, zero stride/size) make no real
//     attempt and fall back to the legacy DrawAuto (counter still advances
//     exactly once), and an out-of-range attribute buffer also falls back.
//
// When no Vulkan device is present the value checks are skipped, so the suite
// stays green on a headless host (same discipline as the round-9 draw test).
// ============================================================================
#include "gpu/pm4_translator.hpp"
#include "gpu/gpu_guest_memory.hpp"
#include "gpu/vulkan_compute_executor.hpp"
#include "gpu/shader_spirv_recompiler.hpp"
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

// Flat guest memory: a base GVA mapped to a contiguous dword vector.
class FlatGuestMemory final : public GpuGuestMemory {
public:
    FlatGuestMemory(uint64_t base, size_t dwords) : m_base(base), m_mem(dwords, 0u) {}

    bool ReadDwords(uint64_t gva, uint32_t* dst, size_t dwords) override {
        size_t off;
        if (!Range(gva, dwords, off)) return false;
        std::memcpy(dst, m_mem.data() + off, dwords * sizeof(uint32_t));
        return true;
    }
    bool WriteDwords(uint64_t gva, const uint32_t* src, size_t dwords) override {
        size_t off;
        if (!Range(gva, dwords, off)) return false;
        std::memcpy(m_mem.data() + off, src, dwords * sizeof(uint32_t));
        return true;
    }
    void PutDwords(uint64_t gva, const std::vector<uint32_t>& v) {
        size_t off = 0;
        if (Range(gva, v.size(), off))
            std::memcpy(m_mem.data() + off, v.data(), v.size() * sizeof(uint32_t));
    }
    void PutDword(uint64_t gva, uint32_t v) {
        size_t off = 0;
        if (Range(gva, 1, off)) m_mem[off] = v;
    }
    uint32_t At(uint64_t gva) {
        size_t off = 0;
        return Range(gva, 1, off) ? m_mem[off] : 0u;
    }

private:
    bool Range(uint64_t gva, size_t dwords, size_t& off) {
        if (gva < m_base || (gva - m_base) % 4 != 0) return false;
        off = static_cast<size_t>((gva - m_base) / 4);
        return off + dwords <= m_mem.size();
    }
    uint64_t m_base;
    std::vector<uint32_t> m_mem;
};

// --- RDNA2 encoders (VOP2 / SOPP) -------------------------------------------
uint32_t EncVop2(RDNA2_VOP2_Op op, uint32_t dst, uint32_t src0, uint32_t src1) {
    return (static_cast<uint32_t>(op) << 25U) | (dst << 17U) | (src1 << 9U) | src0;
}
uint32_t EncSopp(RDNA2_SOPP_Op op, uint32_t s = 0) {
    return 0xbf800000U | (static_cast<uint32_t>(op) << 16U) | s;
}
uint32_t V(uint32_t n) { return 256U + n; }
uint32_t Hdr(size_t payload_dwords, uint8_t opcode) {
    return (3u << 30u) | ((static_cast<uint32_t>(payload_dwords) - 1u) << 16u) |
           (static_cast<uint32_t>(opcode) << 8u);
}
float AsF(uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }
uint32_t AsB(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }

constexpr uint8_t OP_SET_SH_REG        = 0x76;
constexpr uint8_t OP_INDEX_BUFFER_SIZE = 0x03;
constexpr uint8_t OP_DRAW_INDEX_2      = 0x22;
constexpr uint8_t OP_DRAW_INDEX_AUTO   = 0x23;
constexpr uint8_t OP_INDEX_BASE        = 0x2D;
constexpr uint8_t OP_INDEX_TYPE        = 0x2E;
constexpr uint8_t OP_NUM_INSTANCES     = 0x2F;

// Vertex stage with TWO input attributes: position (x, y) in v0..v1 and a
// weight w in v2; two output dwords per vertex (m = 2):
//   v0 = x * w,  v1 = y * w
std::vector<uint32_t> MakeFetchVertexShader() {
    return {
        EncVop2(RDNA2_VOP2_Op::V_MUL_F32, 0, V(2), 0),  // v0 = w * x  (src1 is an 8-bit VGPR index)
        EncVop2(RDNA2_VOP2_Op::V_MUL_F32, 1, V(2), 1),  // v1 = w * y
        EncSopp(RDNA2_SOPP_Op::S_ENDPGM),
    };
}

// Round-9 single-stream shader (one dword per lane).
std::vector<uint32_t> MakeSingleStreamShader() {
    return {
        EncVop2(RDNA2_VOP2_Op::V_MUL_F32, 0, V(0), 0),
        EncSopp(RDNA2_SOPP_Op::S_ENDPGM),
    };
}

} // namespace

int main() {
    std::cout << "=== PM4 -> Real GPU VGT Attribute-Fetch Test (round 10) ===\n";

    VulkanComputeExecutor exec;
    const bool available = exec.Initialize();
    std::cout << (available ? "[info] Vulkan device: " + exec.DeviceName()
                            : std::string("[info] no Vulkan device; fallback paths exercised"))
              << "\n";

    // --- guest memory layout --------------------------------------------------
    // Two attributes per vertex: pos = (x, y) floats, stride 2 dwords, size 2;
    // weight = w float, stride 1 dword, size 1. k = 3, m = 2.
    const uint64_t BASE       = 0x1400000000ull;
    const uint64_t SHADER_GVA = BASE + 0x0000;
    const uint64_t FETCH_GVA  = BASE + 0x0800;   // fetch descriptor table
    const uint64_t POS_GVA    = BASE + 0x1000;   // positions (8 verts x 2 dwords)
    const uint64_t WGT_GVA    = BASE + 0x1100;   // weights  (8 verts x 1 dword)
    const uint64_t IDX32_GVA  = BASE + 0x1400;   // u32 index buffer
    const uint64_t IDX16_GVA  = BASE + 0x1800;   // u16 index buffer (packed)
    const uint64_t ATTR_GVA   = BASE + 0x1A00;   // single-stream input buffer
    const uint64_t OUTPUT_GVA = BASE + 0x2000;
    const uint32_t VERTS      = 8;

    FlatGuestMemory mem(BASE, 0x4000 / 4);
    mem.PutDwords(SHADER_GVA, MakeFetchVertexShader());

    // Vertex data: pos[i] = (i+1, (i+1)*10), w[i] = i+1.  => out = ((i+1)^2, (i+1)^2*10)
    std::vector<uint32_t> positions;
    std::vector<uint32_t> weights;
    for (uint32_t i = 0; i < VERTS; ++i) {
        const float f = static_cast<float>(i + 1);
        positions.push_back(AsB(f));
        positions.push_back(AsB(f * 10.0f));
        weights.push_back(AsB(f));
    }
    mem.PutDwords(POS_GVA, positions);
    mem.PutDwords(WGT_GVA, weights);

    // The self-describing fetch table: [count][entries x 4 dwords].
    // Entry a starts at byte offset 4 + a*16 (each entry is 4 DWORDS).
    const auto put_fetch_table = [&](uint32_t count) {
        mem.PutDword(FETCH_GVA, count);
        if (count >= 1) {
            mem.PutDwords(FETCH_GVA + 4, {
                static_cast<uint32_t>(POS_GVA & 0xffffffff),
                static_cast<uint32_t>(POS_GVA >> 32),
                2,   // stride (dwords)
                2,   // size (dwords)
            });
        }
        if (count >= 2) {
            mem.PutDwords(FETCH_GVA + 4 + 4 * sizeof(uint32_t), {
                static_cast<uint32_t>(WGT_GVA & 0xffffffff),
                static_cast<uint32_t>(WGT_GVA >> 32),
                1,   // stride
                1,   // size
            });
        }
    };
    put_fetch_table(2);

    // u32 indices: shuffled order with a repeat (vertex 4 twice) -- gather
    // semantics: lane 3 re-fetches vertex 4's attributes. (Fetch-table draws
    // use the raw index as a vertex id; the single-stream scenario below
    // reuses this buffer with float payloads, so store float bits.)
    std::vector<uint32_t> idx32 = {3, 1, 0, 4, 2, 4, 7, 5};
    mem.PutDwords(IDX32_GVA, idx32);

    // u16 indices {2, 4, 6, 1} packed two per dword.
    std::vector<uint32_t> idx16_packed;
    idx16_packed.push_back(2u | (4u << 16));
    idx16_packed.push_back(6u | (1u << 16));
    mem.PutDwords(IDX16_GVA, idx16_packed);

    // Single-stream attribute buffer (floats 1..32).
    std::vector<uint32_t> attrs(32);
    for (uint32_t i = 0; i < 32; ++i) attrs[i] = AsB(static_cast<float>(i + 1));
    mem.PutDwords(ATTR_GVA, attrs);

    // --- ring builders ----------------------------------------------------------
    auto put_sh_reg = [&](std::vector<uint32_t>& ring, uint32_t off, uint32_t val) {
        ring.push_back(Hdr(2, OP_SET_SH_REG));
        ring.push_back(off);
        ring.push_back(val);
    };
    auto put_index_base = [&](std::vector<uint32_t>& ring, uint64_t gva) {
        ring.push_back(Hdr(2, OP_INDEX_BASE));
        ring.push_back(static_cast<uint32_t>(gva & 0xffffffff));
        ring.push_back(static_cast<uint32_t>(gva >> 32));
    };
    auto put_simple = [&](std::vector<uint32_t>& ring, uint8_t op, uint32_t v) {
        ring.push_back(Hdr(1, op));
        ring.push_back(v);
    };
    // Program the vertex stage: PGM, buffers, count, fetch table, out size.
    const auto put_vs_descriptors = [&](std::vector<uint32_t>& ring, uint32_t count,
                                        uint64_t fetch_gva, uint32_t out_dwords) {
        put_sh_reg(ring, Pm4::SPI_SHADER_PGM_LO_VS, static_cast<uint32_t>(SHADER_GVA >> 8));
        put_sh_reg(ring, Pm4::SPI_SHADER_PGM_HI_VS, static_cast<uint32_t>(SHADER_GVA >> 32));
        put_sh_reg(ring, Pm4::SPI_SHADER_USER_DATA_VS_0 + 0,
                   static_cast<uint32_t>(ATTR_GVA & 0xffffffff));
        put_sh_reg(ring, Pm4::SPI_SHADER_USER_DATA_VS_0 + 1,
                   static_cast<uint32_t>(ATTR_GVA >> 32));
        put_sh_reg(ring, Pm4::SPI_SHADER_USER_DATA_VS_0 + 2,
                   static_cast<uint32_t>(OUTPUT_GVA & 0xffffffff));
        put_sh_reg(ring, Pm4::SPI_SHADER_USER_DATA_VS_0 + 3,
                   static_cast<uint32_t>(OUTPUT_GVA >> 32));
        put_sh_reg(ring, Pm4::SPI_SHADER_USER_DATA_VS_0 + 4, count);
        put_sh_reg(ring, Pm4::SPI_SHADER_USER_DATA_VS_FETCH_LO,
                   static_cast<uint32_t>(fetch_gva & 0xffffffff));
        put_sh_reg(ring, Pm4::SPI_SHADER_USER_DATA_VS_FETCH_HI,
                   static_cast<uint32_t>(fetch_gva >> 32));
        put_sh_reg(ring, Pm4::SPI_SHADER_USER_DATA_VS_OUT_DWORDS, out_dwords);
    };

    // ==========================================================================
    // 1. Indexed draw (u32) with the VGT gather: two attributes per lane,
    //    shuffled + repeated indices, two output dwords per vertex.
    // ==========================================================================
    {
        std::vector<uint32_t> ring;
        put_vs_descriptors(ring, 8, FETCH_GVA, 2);
        put_index_base(ring, IDX32_GVA);
        put_simple(ring, OP_INDEX_TYPE, Pm4::INDEX_TYPE_U32);
        put_simple(ring, OP_INDEX_BUFFER_SIZE, 8);
        put_simple(ring, OP_NUM_INSTANCES, 1);
        ring.push_back(Hdr(3, OP_DRAW_INDEX_2));
        ring.push_back(8); ring.push_back(0); ring.push_back(0);

        VulkanRendererBackend backend;
        backend.Initialize();
        PM4VulkanTranslator translator(backend);
        translator.BindComputeExecutor(&exec, &mem);

        const auto result = translator.TranslateAndExecuteCommandRingChecked(ring.data(), ring.size());
        CHECK(result.ok());

        const auto& draw = translator.GetLastDrawDispatch();
        CHECK(draw.attempted);
        CHECK(draw.fetch_enabled);
        CHECK(draw.fetch_table_gva == FETCH_GVA);
        CHECK(draw.attribute_count == 2);
        CHECK(draw.in_dwords_per_lane == 3);   // pos(2) + weight(1)
        CHECK(draw.out_dwords_per_lane == 2);
        CHECK(draw.element_count == 8);
        CHECK(backend.GetDrawCallCount() == 1);

        if (draw.attempted) {
            int bad = 0;
            for (uint32_t i = 0; i < 8; ++i) {
                const uint32_t v = idx32[i];                    // vertex id
                const float x = static_cast<float>(v + 1);
                const float want_x = x * x;                     // x * w, w = x
                const float want_y = x * x * 10.0f;             // y * w, y = x*10
                const float got_x = AsF(mem.At(OUTPUT_GVA + (i * 2 + 0) * 4));
                const float got_y = AsF(mem.At(OUTPUT_GVA + (i * 2 + 1) * 4));
                if (std::fabs(got_x - want_x) > 1e-2f * want_x) ++bad;
                if (std::fabs(got_y - want_y) > 1e-2f * want_y) ++bad;
            }
            CHECK(bad == 0);
            std::cout << "  [ok] u32 indexed VGT gather: 2 attributes/lane, shuffled+"
                         "repeated indices, 2 dwords/vertex out (" << bad << " bad)\n";
        } else {
            CHECK(!draw.executed_on_gpu);
            std::cout << "  [ok] no device: u32 VGT gather recorded and fell back\n";
        }
    }

    // ==========================================================================
    // 2. Indexed draw (u16) with the VGT gather: packed index stream decoded
    //    first, then gathered lane-major.
    // ==========================================================================
    {
        std::vector<uint32_t> ring;
        put_vs_descriptors(ring, 4, FETCH_GVA, 2);
        put_index_base(ring, IDX16_GVA);
        put_simple(ring, OP_INDEX_TYPE, Pm4::INDEX_TYPE_U16);
        put_simple(ring, OP_INDEX_BUFFER_SIZE, 2);
        ring.push_back(Hdr(3, OP_DRAW_INDEX_2));
        ring.push_back(4); ring.push_back(0); ring.push_back(0);

        VulkanRendererBackend backend;
        backend.Initialize();
        PM4VulkanTranslator translator(backend);
        translator.BindComputeExecutor(&exec, &mem);

        const auto result = translator.TranslateAndExecuteCommandRingChecked(ring.data(), ring.size());
        CHECK(result.ok());

        const auto& draw = translator.GetLastDrawDispatch();
        CHECK(draw.attempted);
        CHECK(draw.fetch_enabled);
        CHECK(draw.index_type == Pm4::INDEX_TYPE_U16);
        CHECK(draw.in_dwords_per_lane == 3);
        CHECK(backend.GetDrawCallCount() == 1);

        if (draw.attempted) {
            const uint16_t idx[4] = {2, 4, 6, 1};
            int bad = 0;
            for (uint32_t i = 0; i < 4; ++i) {
                const float x = static_cast<float>(idx[i] + 1);
                const float want_x = x * x;
                const float want_y = x * x * 10.0f;
                const float got_x = AsF(mem.At(OUTPUT_GVA + (i * 2 + 0) * 4));
                const float got_y = AsF(mem.At(OUTPUT_GVA + (i * 2 + 1) * 4));
                if (std::fabs(got_x - want_x) > 1e-2f * want_x) ++bad;
                if (std::fabs(got_y - want_y) > 1e-2f * want_y) ++bad;
            }
            CHECK(bad == 0);
            std::cout << "  [ok] u16 indexed VGT gather: packed stream decoded then "
                         "gathered (" << bad << " bad)\n";
        } else {
            CHECK(!draw.executed_on_gpu);
            std::cout << "  [ok] no device: u16 VGT gather recorded and fell back\n";
        }
    }

    // ==========================================================================
    // 3. Non-indexed draw with the VGT gather: vertex id = lane, sequential
    //    attribute fetch from both buffers.
    // ==========================================================================
    {
        std::vector<uint32_t> ring;
        put_vs_descriptors(ring, 8, FETCH_GVA, 2);
        ring.push_back(Hdr(2, OP_DRAW_INDEX_AUTO));
        ring.push_back(8); ring.push_back(0);

        VulkanRendererBackend backend;
        backend.Initialize();
        PM4VulkanTranslator translator(backend);
        translator.BindComputeExecutor(&exec, &mem);

        const auto result = translator.TranslateAndExecuteCommandRingChecked(ring.data(), ring.size());
        CHECK(result.ok());

        const auto& draw = translator.GetLastDrawDispatch();
        CHECK(draw.attempted);
        CHECK(draw.fetch_enabled);
        CHECK(!draw.indexed);
        CHECK(backend.GetDrawCallCount() == 1);

        if (draw.attempted) {
            int bad = 0;
            for (uint32_t i = 0; i < 8; ++i) {
                const float x = static_cast<float>(i + 1);
                const float want_x = x * x;
                const float want_y = x * x * 10.0f;
                const float got_x = AsF(mem.At(OUTPUT_GVA + (i * 2 + 0) * 4));
                const float got_y = AsF(mem.At(OUTPUT_GVA + (i * 2 + 1) * 4));
                if (std::fabs(got_x - want_x) > 1e-2f * want_x) ++bad;
                if (std::fabs(got_y - want_y) > 1e-2f * want_y) ++bad;
            }
            CHECK(bad == 0);
            std::cout << "  [ok] non-indexed VGT gather: sequential fetch, "
                         "2 dwords/vertex out (" << bad << " bad)\n";
        } else {
            CHECK(!draw.executed_on_gpu);
            std::cout << "  [ok] no device: non-indexed VGT gather fell back\n";
        }
    }

    // ==========================================================================
    // 4. Back-compat + fallback discipline.
    // ==========================================================================
    {
        // 4a. No fetch table: the round-9 single-stream model is untouched.
        //     The index value IS the lane payload; write FLOAT bits so the
        //     vertex shader's float multiply is observable.
        mem.PutDwords(SHADER_GVA, MakeSingleStreamShader());
        {
            std::vector<uint32_t> payload = {3, 1, 0, 4, 2, 4, 7, 5};
            for (auto& p : payload) p = AsB(static_cast<float>(p));
            mem.PutDwords(IDX32_GVA, payload);
        }
        std::vector<uint32_t> ring;
        put_vs_descriptors(ring, 32, /*fetch_gva=*/0, /*out_dwords=*/1);
        put_index_base(ring, IDX32_GVA);
        put_simple(ring, OP_INDEX_TYPE, Pm4::INDEX_TYPE_U32);
        ring.push_back(Hdr(3, OP_DRAW_INDEX_2));
        ring.push_back(8); ring.push_back(0); ring.push_back(0);

        VulkanRendererBackend backend;
        backend.Initialize();
        PM4VulkanTranslator translator(backend);
        translator.BindComputeExecutor(&exec, &mem);
        auto r = translator.TranslateAndExecuteCommandRingChecked(ring.data(), ring.size());
        CHECK(r.ok());
        const auto& draw = translator.GetLastDrawDispatch();
        CHECK(draw.attempted);
        CHECK(!draw.fetch_enabled);
        CHECK(draw.in_dwords_per_lane == 1);
        CHECK(draw.out_dwords_per_lane == 1);
        CHECK(backend.GetDrawCallCount() == 1);
        if (draw.attempted) {
            // Single-stream semantics: out[i] = idx[i]^2 (the index IS the lane
            // payload, as in round 9).
            int bad = 0;
            for (uint32_t i = 0; i < 8; ++i) {
                const float want = static_cast<float>(idx32[i]) * static_cast<float>(idx32[i]);
                const float got = AsF(mem.At(OUTPUT_GVA + i * 4));
                if (std::fabs(got - want) > 1e-3f) ++bad;
            }
            CHECK(bad == 0);
        }
        std::cout << "  [ok] no fetch table: round-9 single-stream model intact\n";
        mem.PutDwords(SHADER_GVA, MakeFetchVertexShader());

        // 4b. Malformed table (count 0): no real attempt, legacy path.
        std::vector<uint32_t> ring_b;
        put_vs_descriptors(ring_b, 8, FETCH_GVA, 2);
        put_fetch_table(0);
        put_index_base(ring_b, IDX32_GVA);
        put_simple(ring_b, OP_INDEX_TYPE, Pm4::INDEX_TYPE_U32);
        ring_b.push_back(Hdr(3, OP_DRAW_INDEX_2));
        ring_b.push_back(8); ring_b.push_back(0); ring_b.push_back(0);

        VulkanRendererBackend backend_b;
        PM4VulkanTranslator translator_b(backend_b);
        translator_b.BindComputeExecutor(&exec, &mem);
        auto rb = translator_b.TranslateAndExecuteCommandRingChecked(ring_b.data(), ring_b.size());
        CHECK(rb.ok());
        CHECK(!translator_b.GetLastDrawDispatch().attempted);
        CHECK(backend_b.GetDrawCallCount() == 1);
        std::cout << "  [ok] malformed fetch table (count 0): legacy fallback\n";
        put_fetch_table(2);

        // 4c. Malformed entry (zero stride): no real attempt, legacy path.
        std::vector<uint32_t> ring_c;
        put_vs_descriptors(ring_c, 8, FETCH_GVA, 2);
        mem.PutDword(FETCH_GVA + 4 + 2 * sizeof(uint32_t), 0);   // stride of attribute 0 -> 0
        put_index_base(ring_c, IDX32_GVA);
        put_simple(ring_c, OP_INDEX_TYPE, Pm4::INDEX_TYPE_U32);
        ring_c.push_back(Hdr(3, OP_DRAW_INDEX_2));
        ring_c.push_back(8); ring_c.push_back(0); ring_c.push_back(0);

        VulkanRendererBackend backend_c;
        PM4VulkanTranslator translator_c(backend_c);
        translator_c.BindComputeExecutor(&exec, &mem);
        auto rc = translator_c.TranslateAndExecuteCommandRingChecked(ring_c.data(), ring_c.size());
        CHECK(rc.ok());
        CHECK(!translator_c.GetLastDrawDispatch().attempted);
        CHECK(backend_c.GetDrawCallCount() == 1);
        std::cout << "  [ok] malformed fetch entry (stride 0): legacy fallback\n";
        mem.PutDword(FETCH_GVA + 4 + 2 * sizeof(uint32_t), 2);

        // 4d. Out-of-range attribute buffer (index 7 x stride far beyond the
        //     flat window): the gather read fails -> legacy fallback, and the
        //     record keeps attempted = true (descriptors were complete).
        std::vector<uint32_t> ring_d;
        put_vs_descriptors(ring_d, 8, FETCH_GVA, 2);
        // Indices that reach outside the mapped window (vertex_id * stride
        // beyond 0x4000): use u32 index 0x00FFFFFF.
        mem.PutDwords(IDX32_GVA + 0x100, {0x00FFFFFF, 0, 0, 0, 0, 0, 0, 0});
        put_index_base(ring_d, IDX32_GVA + 0x100);
        put_simple(ring_d, OP_INDEX_TYPE, Pm4::INDEX_TYPE_U32);
        ring_d.push_back(Hdr(3, OP_DRAW_INDEX_2));
        ring_d.push_back(8); ring_d.push_back(0); ring_d.push_back(0);

        VulkanRendererBackend backend_d;
        PM4VulkanTranslator translator_d(backend_d);
        translator_d.BindComputeExecutor(&exec, &mem);
        auto rd_ = translator_d.TranslateAndExecuteCommandRingChecked(ring_d.data(), ring_d.size());
        CHECK(rd_.ok());
        CHECK(translator_d.GetLastDrawDispatch().attempted);
        CHECK(!translator_d.GetLastDrawDispatch().executed_on_gpu);
        CHECK(backend_d.GetDrawCallCount() == 1);
        std::cout << "  [ok] out-of-range attribute buffer: gather refused, legacy fallback\n";
        mem.PutDwords(IDX32_GVA, idx32);

        // 4e. All-or-nothing: a tail-truncated stream applies no backend side
        //     effect at all.
        std::vector<uint32_t> ring_e;
        put_vs_descriptors(ring_e, 8, FETCH_GVA, 2);
        ring_e.push_back(Hdr(9, OP_DRAW_INDEX_2));  // promises 9 payload dwords...
        ring_e.push_back(8); ring_e.push_back(0);   // ...only 2 present (truncated)

        VulkanRendererBackend backend_e;
        PM4VulkanTranslator translator_e(backend_e);
        translator_e.BindComputeExecutor(&exec, &mem);
        auto re = translator_e.TranslateAndExecuteCommandRingChecked(ring_e.data(), ring_e.size());
        CHECK(!re.ok());
        CHECK(backend_e.GetDrawCallCount() == 0);
        CHECK(!translator_e.GetLastDrawDispatch().attempted);
        std::cout << "  [ok] truncated stream: zero backend side effects (all-or-nothing)\n";
    }

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    if (g_failures == 0) {
        std::cout << ">> [PASS] VGT attribute fetch: multi-attribute gather per lane "
                     "drives the real GPU vertex stage "
                  << (available ? "(GPU readback via guest memory asserted)."
                                : "(headless-safe fallback).") << "\n";
    }
    return g_failures == 0 ? 0 : 1;
}
