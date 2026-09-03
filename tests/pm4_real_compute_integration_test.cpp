// ============================================================================
// ProsperoLayer RDNA2 Core - PM4 -> real GPU compute integration test
// ----------------------------------------------------------------------------
// Proves the item #1 integration the earlier round left as a "next step":
// a PM4 DISPATCH_DIRECT ring, with the compute-shader GVA in COMPUTE_PGM_LO/HI
// and the input/output SSBO GVAs in COMPUTE_USER_DATA, drives the REAL
// VulkanComputeExecutor through PM4VulkanTranslator -- reading the RDNA2 shader
// and input buffer from guest memory, executing on a Vulkan device, and writing
// the output buffer BACK to guest memory. This is the exact path a real game's
// GraphicsDriverSubmit* compute submission takes, no longer the CPU-sim
// fallback.
//
// When no Vulkan device is present the executor reports Unavailable, the
// translator falls back to the legacy path, and the value checks are skipped --
// so the suite stays green on a headless host.
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
#include <map>
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
    uint64_t base() const { return m_base; }
    std::vector<uint32_t>& raw() { return m_mem; }

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

uint32_t EncVop1(RDNA2_VOP1_Op op, uint32_t dst, uint32_t src0) {
    // Real VOP1 hardware layout: op[24:17], vdst[16:9], src0[8:0].
    return 0x7e000000U | (static_cast<uint32_t>(op) << 17U) | (dst << 9U) | src0;
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

constexpr uint8_t OP_SET_SH_REG      = 0x76;
constexpr uint8_t OP_DISPATCH_DIRECT = 0x04;

} // namespace

int main() {
    std::cout << "=== PM4 -> Real GPU Compute Integration Test (item #1 wiring) ===\n";

    VulkanComputeExecutor exec;
    const bool available = exec.Initialize();
    std::cout << (available ? "[info] Vulkan device: " + exec.DeviceName()
                            : std::string("[info] no Vulkan device; fallback path exercised")) << "\n";

    // --- guest memory layout -------------------------------------------------
    const uint64_t BASE       = 0x1200000000ull;
    const uint64_t SHADER_GVA = BASE + 0x0000;   // 256-byte aligned
    const uint64_t INPUT_GVA  = BASE + 0x1000;
    const uint64_t OUTPUT_GVA = BASE + 0x2000;
    const uint32_t N          = 64;

    FlatGuestMemory mem(BASE, 0x4000 / 4);

    // shader: out = sqrt(in)
    std::vector<uint32_t> shader = {
        EncVop1(RDNA2_VOP1_Op::V_SQRT_F32, 0, V(0)),
        EncSopp(RDNA2_SOPP_Op::S_ENDPGM),
    };
    mem.PutDwords(SHADER_GVA, shader);

    std::vector<uint32_t> input(N);
    for (uint32_t i = 0; i < N; ++i) input[i] = AsB(static_cast<float>(i + 1));
    mem.PutDwords(INPUT_GVA, input);

    // --- build a PM4 ring: SET_SH_REG(compute descriptors) + DISPATCH_DIRECT --
    auto put_reg = [&](std::vector<uint32_t>& ring, uint32_t off, uint32_t val) {
        ring.push_back(Hdr(2, OP_SET_SH_REG));
        ring.push_back(off);
        ring.push_back(val);
    };
    std::vector<uint32_t> ring;
    put_reg(ring, Pm4::COMPUTE_PGM_LO, static_cast<uint32_t>(SHADER_GVA >> 8));
    put_reg(ring, Pm4::COMPUTE_PGM_HI, static_cast<uint32_t>(SHADER_GVA >> 32));
    put_reg(ring, Pm4::COMPUTE_USER_DATA_0 + 0, static_cast<uint32_t>(INPUT_GVA & 0xffffffff));
    put_reg(ring, Pm4::COMPUTE_USER_DATA_0 + 1, static_cast<uint32_t>(INPUT_GVA >> 32));
    put_reg(ring, Pm4::COMPUTE_USER_DATA_0 + 2, static_cast<uint32_t>(OUTPUT_GVA & 0xffffffff));
    put_reg(ring, Pm4::COMPUTE_USER_DATA_0 + 3, static_cast<uint32_t>(OUTPUT_GVA >> 32));
    put_reg(ring, Pm4::COMPUTE_USER_DATA_0 + 4, N);
    // DISPATCH_DIRECT payload: groups_x, groups_y, groups_z, dispatch_initiator
    ring.push_back(Hdr(4, OP_DISPATCH_DIRECT));
    ring.push_back(1); ring.push_back(1); ring.push_back(1); ring.push_back(0);

    // --- run through the translator with the real compute path bound ---------
    VulkanRendererBackend backend;
    backend.Initialize();
    PM4VulkanTranslator translator(backend);
    translator.BindComputeExecutor(&exec, &mem);

    auto result = translator.TranslateAndExecuteCommandRingChecked(ring.data(), ring.size());
    CHECK(result.ok());

    const auto& disp = translator.GetLastComputeDispatch();
    CHECK(disp.shader_gva == SHADER_GVA);
    CHECK(disp.input_gva == INPUT_GVA);
    CHECK(disp.output_gva == OUTPUT_GVA);
    CHECK(disp.element_count == N);

    if (available) {
        CHECK(disp.attempted);
        CHECK(disp.executed_on_gpu);
        // read the output SSBO back FROM GUEST MEMORY (written by the GPU path)
        int bad = 0;
        for (uint32_t i = 0; i < N; ++i) {
            const float got = AsF(mem.At(OUTPUT_GVA + i * 4));
            const float want = std::sqrt(static_cast<float>(i + 1));
            if (std::fabs(got - want) > 1e-3f) ++bad;
        }
        CHECK(bad == 0);
        std::cout << "  [ok] PM4 dispatch -> real GPU -> guest memory: "
                  << (N - bad) << "/" << N << " lanes correct\n";

        // The backend dispatch counter still advances (observers rely on it).
        CHECK(backend.GetDispatchedComputeCount() == 1);
    } else {
        // No device: translator used the legacy fallback (backend counter++).
        CHECK(!disp.executed_on_gpu);
        CHECK(backend.GetDispatchedComputeCount() == 1);
        std::cout << "  [ok] no device: legacy fallback path taken, counter advanced\n";
    }

    // --- negative: without binding, the legacy path is used ------------------
    {
        VulkanRendererBackend b2;
        PM4VulkanTranslator t2(b2); // no BindComputeExecutor
        auto r2 = t2.TranslateAndExecuteCommandRingChecked(ring.data(), ring.size());
        CHECK(r2.ok());
        CHECK(!t2.GetLastComputeDispatch().attempted);
        CHECK(b2.GetDispatchedComputeCount() == 1); // legacy DispatchCompute()
        std::cout << "  [ok] unbound translator uses legacy DispatchCompute (back-compat)\n";
    }

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    if (g_failures == 0) {
        std::cout << ">> [PASS] PM4 compute submit path drives the real GPU executor "
                  << (available ? "(GPU readback via guest memory asserted)."
                                : "(headless-safe fallback).") << "\n";
    }
    return g_failures == 0 ? 0 : 1;
}
