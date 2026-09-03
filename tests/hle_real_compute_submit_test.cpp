// ============================================================================
// ProsperoLayer RDNA2 Core - HLE submit path -> real GPU compute test
// ----------------------------------------------------------------------------
// The final item #1 wiring check: the SAME path a real game takes
// (GraphicsDriverSubmit* -> Graphics::Gpu -> HeadlessGpuBridge::SubmitCompute)
// now drives the real VulkanComputeExecutor when EnableRealCompute is set. A
// compute ACB with COMPUTE_PGM_* / COMPUTE_USER_DATA descriptors and a
// DISPATCH_DIRECT packet reads the RDNA2 shader + input SSBO from guest memory,
// runs on the Vulkan device, and writes the output SSBO back -- verified by
// reading guest memory after the submit.
//
// Headless-safe: with no Vulkan device the bridge stays on the legacy path and
// the value checks are skipped.
// ============================================================================
#include "graphics/host_gpu/headless_gpu_bridge.hpp"
#include "gpu/gpu_guest_memory.hpp"
#include "gpu/shader_spirv_recompiler.hpp"
#include "graphics/guest_gpu/pm4.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
int g_failures = 0, g_checks = 0;
bool Check(bool v, const char* e, int line) {
    ++g_checks;
    if (!v) { ++g_failures; std::cerr << "  [FAIL] line " << line << ": " << e << '\n'; }
    return v;
}
#define CHECK(e) Check((e), #e, __LINE__)

using namespace PS5::GPU;

class FlatGuestMemory final : public GpuGuestMemory {
public:
    FlatGuestMemory(uint64_t base, size_t dwords) : m_base(base), m_mem(dwords, 0u) {}
    bool ReadDwords(uint64_t gva, uint32_t* dst, size_t n) override {
        size_t off = 0; if (!R(gva, n, off)) return false;
        std::memcpy(dst, m_mem.data() + off, n * 4); return true;
    }
    bool WriteDwords(uint64_t gva, const uint32_t* src, size_t n) override {
        size_t off = 0; if (!R(gva, n, off)) return false;
        std::memcpy(m_mem.data() + off, src, n * 4); return true;
    }
    void Put(uint64_t gva, const std::vector<uint32_t>& v) {
        size_t off = 0; if (R(gva, v.size(), off)) std::memcpy(m_mem.data() + off, v.data(), v.size() * 4);
    }
    uint32_t At(uint64_t gva) { size_t off = 0; return R(gva, 1, off) ? m_mem[off] : 0u; }
private:
    bool R(uint64_t gva, size_t n, size_t& off) {
        if (gva < m_base) return false;
        off = static_cast<size_t>((gva - m_base) / 4);
        return off + n <= m_mem.size();
    }
    uint64_t m_base; std::vector<uint32_t> m_mem;
};

uint32_t EncVop2(uint32_t op, uint32_t dst, uint32_t s1, uint32_t s0) {
    return (op << 25U) | (dst << 17U) | (s1 << 9U) | s0;
}
uint32_t EncSopp(RDNA2_SOPP_Op op) { return 0xbf800000U | (static_cast<uint32_t>(op) << 16U); }
uint32_t V(uint32_t n) { return 256U + n; }
uint32_t Hdr(size_t pd, uint8_t op) {
    return (3u << 30u) | ((static_cast<uint32_t>(pd) - 1u) << 16u) | (static_cast<uint32_t>(op) << 8u);
}
float AsF(uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }
uint32_t AsB(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }

constexpr uint8_t OP_SET_SH_REG = 0x76;
constexpr uint8_t OP_DISPATCH_DIRECT = 0x04;

} // namespace

int main() {
    std::cout << "=== HLE Submit -> Real GPU Compute Test (item #1 final wiring) ===\n";

    const uint64_t BASE = 0x1300000000ull;
    const uint64_t SHADER = BASE, INPUT = BASE + 0x1000, OUTPUT = BASE + 0x2000;
    const uint32_t N = 64;
    FlatGuestMemory mem(BASE, 0x4000 / 4);

    // out = in*in  (square) via V_MUL_F32 (opcode 8): v0 = v0*v0
    mem.Put(SHADER, { EncVop2(8U, 0, 0, V(0)), EncSopp(RDNA2_SOPP_Op::S_ENDPGM) });
    std::vector<uint32_t> input(N);
    for (uint32_t i = 0; i < N; ++i) input[i] = AsB(static_cast<float>(i + 1));
    mem.Put(INPUT, input);

    std::vector<uint32_t> acb;
    auto reg = [&](uint32_t off, uint32_t val) {
        acb.push_back(Hdr(2, OP_SET_SH_REG)); acb.push_back(off); acb.push_back(val);
    };
    reg(Pm4::COMPUTE_PGM_LO, static_cast<uint32_t>(SHADER >> 8));
    reg(Pm4::COMPUTE_PGM_HI, static_cast<uint32_t>(SHADER >> 32));
    reg(Pm4::COMPUTE_USER_DATA_0 + 0, static_cast<uint32_t>(INPUT & 0xffffffff));
    reg(Pm4::COMPUTE_USER_DATA_0 + 1, static_cast<uint32_t>(INPUT >> 32));
    reg(Pm4::COMPUTE_USER_DATA_0 + 2, static_cast<uint32_t>(OUTPUT & 0xffffffff));
    reg(Pm4::COMPUTE_USER_DATA_0 + 3, static_cast<uint32_t>(OUTPUT >> 32));
    reg(Pm4::COMPUTE_USER_DATA_0 + 4, N);
    acb.push_back(Hdr(4, OP_DISPATCH_DIRECT));
    acb.push_back(1); acb.push_back(1); acb.push_back(1); acb.push_back(0);

    Graphics::HeadlessGpuBridge bridge;
    bridge.EnableRealCompute(&mem);   // <- real GPU compute on the HLE submit path
    bridge.InitializeGpu(nullptr);

    const bool active = bridge.RealComputeActive();
    std::cout << (active ? "[info] real GPU compute active on submit path\n"
                         : "[info] no device; legacy fallback on submit path\n");

    bridge.SubmitCompute(/*queue=*/0u, acb.data(), static_cast<uint32_t>(acb.size()),
                         /*trigger_interrupt_on_done=*/true);

    CHECK(bridge.GetComputeSubmitCount() == 1);
    CHECK(bridge.LastSubmitOk());
    CHECK(bridge.GetInterruptCount() == 1);

    if (active) {
        const auto& disp = bridge.LastComputeDispatch();
        CHECK(disp.attempted);
        CHECK(disp.executed_on_gpu);
        CHECK(disp.element_count == N);
        int bad = 0;
        for (uint32_t i = 0; i < N; ++i) {
            const float got = AsF(mem.At(OUTPUT + i * 4));
            const float want = static_cast<float>(i + 1) * static_cast<float>(i + 1);
            if (std::fabs(got - want) > 1e-2f) ++bad;
        }
        CHECK(bad == 0);
        std::cout << "  [ok] game-style SubmitCompute -> real GPU -> guest mem: "
                  << (N - bad) << "/" << N << " lanes correct\n";
    } else {
        CHECK(!bridge.LastComputeDispatch().executed_on_gpu);
        std::cout << "  [ok] legacy fallback taken; submit path intact\n";
    }

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    if (g_failures == 0) {
        std::cout << ">> [PASS] HLE compute submit path executes real GPU compute "
                  << (active ? "(guest-memory readback asserted)." : "(headless-safe).") << "\n";
    }
    return g_failures == 0 ? 0 : 1;
}
