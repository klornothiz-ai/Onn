// ============================================================================
// ProsperoLayer RDNA2 Core - PM4 -> real GPU draw-path integration test
// ----------------------------------------------------------------------------
// Proves the round 9 "draw wiring": a PM4 draw packet (DRAW_INDEX_2 /
// DRAW_INDEX_AUTO), with the vertex-stage GVA in SPI_SHADER_PGM_LO_VS/HI_VS
// and the input/output buffers in SPI_SHADER_USER_DATA_VS_0, drives the REAL
// VulkanComputeExecutor through PM4VulkanTranslator::TryRealDrawDispatch:
//   * indexed draws decode the index stream at INDEX_BASE (u16 zero-extended
//     or u32, per PKT3_INDEX_TYPE) and feed one lane per element,
//   * non-indexed draws pass the attribute buffer through,
//   * the vertex-stage program (an RDNA2 kernel, one lane per element) runs on
//     a Vulkan device and the transformed vertices are written BACK to guest
//     memory. This is the exact path a real game's graphics submission takes
//     for its vertex stage -- only COMPUTE was wired end-to-end before.
//
// The legacy fallback discipline is asserted too:
//   * no device  -> executor reports Unavailable -> legacy DrawAuto() runs and
//     the backend draw counter still advances exactly once per draw,
//   * unbound translator (no BindComputeExecutor) -> unchanged legacy path,
//   * missing vertex-stage descriptors (no PGM regs / no index base) -> no
//     real attempt, legacy path,
//   * PKT3_NUM_INSTANCES is honoured by the legacy call (recorded in the
//     draw record),
//   * a tail-truncated stream applies NO backend side effect (all-or-nothing).
//
// When no Vulkan device is present the value checks are skipped, so the suite
// stays green on a headless host (same discipline as the compute test).
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
    uint32_t At(uint64_t gva) {
        size_t off = 0;
        return Range(gva, 1, off) ? m_mem[off] : 0u;
    }

private:
    bool Range(uint64_t gva, size_t dwords, size_t& off) {
        if (gva < m_base) return false;
        off = static_cast<size_t>((gva - m_base) / 4);
        return off + dwords <= m_mem.size();
    }
    uint64_t m_base;
    std::vector<uint32_t> m_mem;
};

// --- RDNA2 encoders (VOP2 / SOPP) -------------------------------------------
uint32_t EncVop2(RDNA2_VOP2_Op op, uint32_t dst, uint32_t src0, uint32_t src1) {
    // VOP2: bit31=0, opcode [30:25], dst [24:17], src1 [16:9], src0 [8:0].
    return (static_cast<uint32_t>(op) << 25U) | (dst << 17U) | (src1 << 9U) | src0;
}
uint32_t EncSopp(RDNA2_SOPP_Op op, uint32_t s = 0) {
    return 0xbf800000U | (static_cast<uint32_t>(op) << 16U) | s;
}
uint32_t V(uint32_t n) { return 256U + n; }  // VGPR n as a source operand
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

// The "vertex transform": out = in * in (exact in float32 for small values).
std::vector<uint32_t> MakeTransformShader() {
    return {
        EncVop2(RDNA2_VOP2_Op::V_MUL_F32, 0, V(0), 0),  // src1: 8-bit VGPR index
        EncSopp(RDNA2_SOPP_Op::S_ENDPGM),
    };
}

} // namespace

int main() {
    std::cout << "=== PM4 -> Real GPU Draw-Path Integration Test (round 9 wiring) ===\n";

    VulkanComputeExecutor exec;
    const bool available = exec.Initialize();
    std::cout << (available ? "[info] Vulkan device: " + exec.DeviceName()
                            : std::string("[info] no Vulkan device; fallback paths exercised"))
              << "\n";

    // --- guest memory layout --------------------------------------------------
    const uint64_t BASE       = 0x1400000000ull;
    const uint64_t SHADER_GVA = BASE + 0x0000;   // 256-byte aligned
    const uint64_t ATTR_GVA   = BASE + 0x1000;   // non-indexed input buffer
    const uint64_t IDX32_GVA  = BASE + 0x1400;   // u32 index buffer
    const uint64_t IDX16_GVA  = BASE + 0x1800;   // u16 index buffer (packed)
    const uint64_t OUTPUT_GVA = BASE + 0x2000;
    const uint32_t N          = 32;

    FlatGuestMemory mem(BASE, 0x4000 / 4);
    mem.PutDwords(SHADER_GVA, MakeTransformShader());

    // u32 indices: the index value IS the lane payload, written as FLOAT
    // BITS (the vertex shader does float math on it).
    std::vector<uint32_t> idx32(N);
    for (uint32_t i = 0; i < N; ++i) idx32[i] = AsB(static_cast<float>(i + 1));
    mem.PutDwords(IDX32_GVA, idx32);

    // u16 indices {2, 3, 5, 7} repeated: packed two per dword.
    const uint32_t M = 16;
    std::vector<uint32_t> idx16_packed;
    const uint16_t pattern[4] = {2, 3, 5, 7};
    for (uint32_t i = 0; i < M; i += 2) {
        idx16_packed.push_back(static_cast<uint32_t>(pattern[i % 4]) |
                               (static_cast<uint32_t>(pattern[(i + 1) % 4]) << 16));
    }
    mem.PutDwords(IDX16_GVA, idx16_packed);

    // Attribute buffer for the non-indexed draw: floats 1..N.
    std::vector<uint32_t> attrs(N);
    for (uint32_t i = 0; i < N; ++i) attrs[i] = AsB(static_cast<float>(i + 1));
    mem.PutDwords(ATTR_GVA, attrs);

    // --- ring builder ----------------------------------------------------------
    auto put_sh_reg = [&](std::vector<uint32_t>& ring, uint32_t off, uint32_t val) {
        ring.push_back(Hdr(2, OP_SET_SH_REG));
        ring.push_back(off);
        ring.push_back(val);
    };
    auto put_index_base = [&](std::vector<uint32_t>& ring, uint64_t gva) {
        // payload: base lo, base hi (2 dwords)
        ring.push_back(Hdr(2, OP_INDEX_BASE));
        ring.push_back(static_cast<uint32_t>(gva & 0xffffffff));
        ring.push_back(static_cast<uint32_t>(gva >> 32));
    };
    auto put_simple = [&](std::vector<uint32_t>& ring, uint8_t op, uint32_t v) {
        ring.push_back(Hdr(1, op));
        ring.push_back(v);
    };
    auto put_vs_descriptors = [&](std::vector<uint32_t>& ring, uint32_t count) {
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
    };

    // ==========================================================================
    // 1. Indexed draw, u32 indices (DRAW_INDEX_2).
    // ==========================================================================
    {
        std::vector<uint32_t> ring;
        put_vs_descriptors(ring, N);
        put_index_base(ring, IDX32_GVA);
        put_simple(ring, OP_INDEX_TYPE, Pm4::INDEX_TYPE_U32);
        put_simple(ring, OP_INDEX_BUFFER_SIZE, N);
        put_simple(ring, OP_NUM_INSTANCES, 1);
        // payload: index count, dma addr lo, dma addr hi
        ring.push_back(Hdr(3, OP_DRAW_INDEX_2));
        ring.push_back(N); ring.push_back(0); ring.push_back(0);

        VulkanRendererBackend backend;
        backend.Initialize();
        PM4VulkanTranslator translator(backend);
        translator.BindComputeExecutor(&exec, &mem);

        const auto result = translator.TranslateAndExecuteCommandRingChecked(ring.data(), ring.size());
        CHECK(result.ok());

        const auto& draw = translator.GetLastDrawDispatch();
        CHECK(draw.indexed);
        CHECK(draw.attempted);
        CHECK(draw.shader_gva == SHADER_GVA);
        CHECK(draw.output_gva == OUTPUT_GVA);
        CHECK(draw.index_base_gva == IDX32_GVA);
        CHECK(draw.index_type == Pm4::INDEX_TYPE_U32);
        CHECK(draw.element_count == N);
        CHECK(draw.instance_count == 1);
        // Exactly one backend draw call, real or legacy.
        CHECK(backend.GetDrawCallCount() == 1);

        if (draw.attempted) {
            int bad = 0;
            for (uint32_t i = 0; i < N; ++i) {
                const float got = AsF(mem.At(OUTPUT_GVA + i * 4));
                const float want = static_cast<float>(i + 1) * static_cast<float>(i + 1);
                if (std::fabs(got - want) > 1e-2f * want) ++bad;
            }
            CHECK(bad == 0);
            std::cout << "  [ok] u32 indexed draw -> real GPU -> guest memory: "
                      << (N - bad) << "/" << N << " transformed vertices correct\n";
        } else {
            CHECK(!draw.executed_on_gpu);
            std::cout << "  [ok] no device: u32 indexed draw took the legacy fallback "
                         "(counter advanced once)\n";
        }
    }

    // ==========================================================================
    // 2. Indexed draw, u16 indices: the packed stream must be zero-extended
    //    and unpacked lane-per-element before execution.
    // ==========================================================================
    {
        // The u16 payload is a zero-extended INTEGER; use an integer multiply
        // (V_MUL_U32_U24, GFX10 op 0x0B) so the expectation is exact.
        mem.PutDwords(SHADER_GVA, {
            (0x0Bu << 25) | (0u << 17) | (0u << 9) | V(0),   // v_mul_u32_u24 v0, v0, v0
            EncSopp(RDNA2_SOPP_Op::S_ENDPGM),
        });
        std::vector<uint32_t> ring;
        put_vs_descriptors(ring, M);
        put_index_base(ring, IDX16_GVA);
        put_simple(ring, OP_INDEX_TYPE, Pm4::INDEX_TYPE_U16);
        put_simple(ring, OP_INDEX_BUFFER_SIZE, M / 2);
        ring.push_back(Hdr(3, OP_DRAW_INDEX_2));
        ring.push_back(M); ring.push_back(0); ring.push_back(0);

        VulkanRendererBackend backend;
        backend.Initialize();
        PM4VulkanTranslator translator(backend);
        translator.BindComputeExecutor(&exec, &mem);

        const auto result = translator.TranslateAndExecuteCommandRingChecked(ring.data(), ring.size());
        CHECK(result.ok());

        const auto& draw = translator.GetLastDrawDispatch();
        CHECK(draw.indexed);
        CHECK(draw.attempted);
        CHECK(draw.index_type == Pm4::INDEX_TYPE_U16);
        CHECK(draw.element_count == M);
        CHECK(backend.GetDrawCallCount() == 1);

        if (draw.attempted) {
            const uint16_t pattern[4] = {2, 3, 5, 7};
            int bad = 0;
            for (uint32_t i = 0; i < M; ++i) {
                const uint32_t got = mem.At(OUTPUT_GVA + i * 4);
                const uint32_t idx = pattern[i % 4];
                const uint32_t want = idx * idx;
                if (got != want) ++bad;
            }
            CHECK(bad == 0);
            std::cout << "  [ok] u16 indexed draw: packed index stream unpacked and "
                         "transformed (" << (M - bad) << "/" << M << " lanes)\n";
        } else {
            CHECK(!draw.executed_on_gpu);
            std::cout << "  [ok] no device: u16 indexed draw recorded and fell back\n";
        }
        mem.PutDwords(SHADER_GVA, MakeTransformShader());
    }

    // ==========================================================================
    // 3. Non-indexed draw (DRAW_INDEX_AUTO): the attribute buffer is the lane
    //    input, passed through without index decoding.
    // ==========================================================================
    {
        std::vector<uint32_t> ring;
        put_vs_descriptors(ring, N);
        // payload: vertex count + draw initiator (both required by the decoder)
        ring.push_back(Hdr(2, OP_DRAW_INDEX_AUTO));
        ring.push_back(N); ring.push_back(0);

        VulkanRendererBackend backend;
        backend.Initialize();
        PM4VulkanTranslator translator(backend);
        translator.BindComputeExecutor(&exec, &mem);

        const auto result = translator.TranslateAndExecuteCommandRingChecked(ring.data(), ring.size());
        CHECK(result.ok());

        const auto& draw = translator.GetLastDrawDispatch();
        CHECK(!draw.indexed);
        CHECK(draw.attempted);
        CHECK(draw.input_gva == ATTR_GVA);
        CHECK(draw.element_count == N);
        CHECK(backend.GetDrawCallCount() == 1);

        if (draw.attempted) {
            int bad = 0;
            for (uint32_t i = 0; i < N; ++i) {
                const float got = AsF(mem.At(OUTPUT_GVA + i * 4));
                const float want = static_cast<float>(i + 1) * static_cast<float>(i + 1);
                if (std::fabs(got - want) > 1e-2f * want) ++bad;
            }
            CHECK(bad == 0);
            std::cout << "  [ok] non-indexed draw: attribute stream transformed on the GPU ("
                      << (N - bad) << "/" << N << " lanes)\n";
        } else {
            CHECK(!draw.executed_on_gpu);
            std::cout << "  [ok] no device: non-indexed draw fell back to the legacy path\n";
        }
    }

    // ==========================================================================
    // 4. Fallback discipline.
    // ==========================================================================
    {
        // 4a. Unbound translator: unchanged legacy path (back-compat).
        std::vector<uint32_t> ring;
        put_vs_descriptors(ring, N);
        put_index_base(ring, IDX32_GVA);
        put_simple(ring, OP_INDEX_TYPE, Pm4::INDEX_TYPE_U32);
        ring.push_back(Hdr(3, OP_DRAW_INDEX_2));
        ring.push_back(N); ring.push_back(0); ring.push_back(0);

        VulkanRendererBackend backend;
        PM4VulkanTranslator translator(backend);  // no BindComputeExecutor
        auto r = translator.TranslateAndExecuteCommandRingChecked(ring.data(), ring.size());
        CHECK(r.ok());
        CHECK(!translator.GetLastDrawDispatch().attempted);
        CHECK(backend.GetDrawCallCount() == 1);  // legacy DrawAuto()
        std::cout << "  [ok] unbound translator uses legacy DrawAuto (back-compat)\n";

        // 4b. Missing vertex-stage descriptors (no PGM regs programmed).
        std::vector<uint32_t> ring_b;
        put_index_base(ring_b, IDX32_GVA);
        ring_b.push_back(Hdr(3, OP_DRAW_INDEX_2));
        ring_b.push_back(N); ring_b.push_back(0); ring_b.push_back(0);

        VulkanRendererBackend backend_b;
        PM4VulkanTranslator translator_b(backend_b);
        translator_b.BindComputeExecutor(&exec, &mem);
        auto rb = translator_b.TranslateAndExecuteCommandRingChecked(ring_b.data(), ring_b.size());
        CHECK(rb.ok());
        CHECK(!translator_b.GetLastDrawDispatch().attempted);  // descriptors incomplete
        CHECK(backend_b.GetDrawCallCount() == 1);
        std::cout << "  [ok] missing VS descriptors: no real attempt, legacy path\n";

        // 4c. Indexed draw without INDEX_BASE: incomplete -> legacy.
        std::vector<uint32_t> ring_c;
        put_vs_descriptors(ring_c, N);
        ring_c.push_back(Hdr(3, OP_DRAW_INDEX_2));
        ring_c.push_back(N); ring_c.push_back(0); ring_c.push_back(0);

        VulkanRendererBackend backend_c;
        PM4VulkanTranslator translator_c(backend_c);
        translator_c.BindComputeExecutor(&exec, &mem);
        auto rc = translator_c.TranslateAndExecuteCommandRingChecked(ring_c.data(), ring_c.size());
        CHECK(rc.ok());
        CHECK(!translator_c.GetLastDrawDispatch().attempted);
        CHECK(backend_c.GetDrawCallCount() == 1);
        std::cout << "  [ok] indexed draw without INDEX_BASE: legacy path\n";

        // 4d. PKT3_NUM_INSTANCES is recorded and honoured by the legacy call.
        std::vector<uint32_t> ring_d;
        put_vs_descriptors(ring_d, N);
        put_simple(ring_d, OP_NUM_INSTANCES, 3);
        ring_d.push_back(Hdr(3, OP_DRAW_INDEX_2));
        ring_d.push_back(N); ring_d.push_back(0); ring_d.push_back(0);

        VulkanRendererBackend backend_d;
        PM4VulkanTranslator translator_d(backend_d);  // unbound: legacy path
        auto rd_ = translator_d.TranslateAndExecuteCommandRingChecked(ring_d.data(), ring_d.size());
        CHECK(rd_.ok());
        CHECK(translator_d.GetLastDrawDispatch().instance_count == 3);
        CHECK(backend_d.GetDrawCallCount() == 1);
        std::cout << "  [ok] PKT3_NUM_INSTANCES=3 recorded for the draw record\n";

        // 4e. All-or-nothing: a tail-truncated draw stream applies no backend
        //     side effect at all.
        std::vector<uint32_t> ring_e;
        put_vs_descriptors(ring_e, N);
        ring_e.push_back(Hdr(9, OP_DRAW_INDEX_2));  // promises 9 payload dwords...
        ring_e.push_back(N); ring_e.push_back(0);   // ...only 2 present (truncated)

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
        std::cout << ">> [PASS] PM4 draw packets drive the real GPU vertex-stage path "
                  << (available ? "(GPU readback via guest memory asserted)."
                                : "(headless-safe fallback).") << "\n";
    }
    return g_failures == 0 ? 0 : 1;
}
