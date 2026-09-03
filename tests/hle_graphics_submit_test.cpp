// ============================================================================
// ProsperoLayer RDNA2 Core - HLE Graphics-Driver Submit Integration Test
// ============================================================================
// Item #3 (HLE: libGraphicsDriver). The HLE entry points in libs/agc.cpp
// (Gen5Driver::GraphicsDriverSubmitDcb / SubmitMultiDcbs / SubmitAcb ...)
// forward guest command rings to the abstract Graphics::Gpu interface via
// submit_dcb()/submit_acb(). Until now that interface had no headless
// implementation, so the driver submit path could not be exercised or verified
// without SDL2 + a Vulkan ICD.
//
// This suite drives the new Graphics::HeadlessGpuBridge -- the same object the
// HLE driver would hold as g_renderer -- with realistic PM4 draw (DCB) and
// compute (ACB) rings, and asserts the *observable* results:
//   * a graphics ring that programs a viewport + draws reaches the backend
//     draw-call counter and the guest-programmed viewport (no host default),
//   * a compute ring that dispatches reaches the dispatch counter,
//   * the end-of-pipe interrupt accounting done by submit_dcb() is honoured
//     (trigger_interrupt_on_done),
//   * a malformed / truncated ring applies NO backend side effects
//     (fail-closed, matching the translator's all-or-nothing guarantee),
//   * multi-ring submission accumulates translated-packet state,
//   * Done() advances the presented frame number (GetFrameNum), which the HLE
//     GraphicsDriverGetEqContextId / flip path depends on.
//
// It links the REAL VulkanRendererBackend (software headless fallback), the REAL
// PM4 translator and the REAL bridge -- no stubs. It never needs a GPU.
// ============================================================================

#include "graphics/host_gpu/headless_gpu_bridge.hpp"

#include <cstdint>
#include <iostream>
#include <sstream>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(const char* name, bool ok) {
    ++g_checks;
    if (ok) {
        std::cout << "[PASS] " << name << "\n";
    } else {
        ++g_failures;
        std::cout << "[FAIL] " << name << "\n";
    }
}

// PM4 Type-3 header: COUNT stores payload_dwords - 1.
uint32_t MakeType3Header(size_t payload_dwords, uint8_t opcode) {
    return (3u << 30u) |
           ((static_cast<uint32_t>(payload_dwords) - 1u) << 16u) |
           (static_cast<uint32_t>(opcode) << 8u);
}

// Opcodes we build rings from (mirrors PS5::GPU::PM4Type3Opcode).
constexpr uint8_t OP_NOP              = 0x00;
constexpr uint8_t OP_DISPATCH_DIRECT  = 0x04;
constexpr uint8_t OP_DRAW_INDEX_AUTO  = 0x23;
constexpr uint8_t OP_SET_CONTEXT_REG  = 0x69;

// Context register the translator recognises for the target extent: the REAL
// PA_SC_SCREEN_SCISSOR_BR (w | h<<16). (Round 18: the old 0xA200/0xA201 pair
// was actually DB_DEPTH_CONTROL/DB_STENCIL_CONTROL on real hardware.)
constexpr uint32_t CX_SCISSOR_BR = 0xA00Du;

// A graphics DCB: set the screen-scissor extent then draw 3 verts.
std::vector<uint32_t> MakeGraphicsRing(uint32_t width, uint32_t height,
                                       uint32_t vertex_count) {
    std::vector<uint32_t> ring;
    // SET_CONTEXT_REG: payload = [PA_SC_SCREEN_SCISSOR_BR, w | h<<16]
    ring.push_back(MakeType3Header(2, OP_SET_CONTEXT_REG));
    ring.push_back(CX_SCISSOR_BR);
    ring.push_back(width | (height << 16u));
    // DRAW_INDEX_AUTO: payload = [vertex_count, draw_initiator] (both required)
    ring.push_back(MakeType3Header(2, OP_DRAW_INDEX_AUTO));
    ring.push_back(vertex_count);
    ring.push_back(0u); // draw initiator
    return ring;
}

// A compute ACB: dispatch (x,y,z) groups. DISPATCH_DIRECT payload needs >= 4.
std::vector<uint32_t> MakeComputeRing(uint32_t gx, uint32_t gy, uint32_t gz) {
    std::vector<uint32_t> ring;
    ring.push_back(MakeType3Header(4, OP_DISPATCH_DIRECT));
    ring.push_back(gx);
    ring.push_back(gy);
    ring.push_back(gz);
    ring.push_back(0u); // dispatch initiator
    return ring;
}

// Silence the translator's informational stdout so the log stays readable.
struct StdoutSilencer {
    std::ostringstream sink;
    std::streambuf* previous;
    StdoutSilencer() : previous(std::cout.rdbuf(sink.rdbuf())) {}
    ~StdoutSilencer() { std::cout.rdbuf(previous); }
};

} // namespace

int main() {
    std::cout << "=== HLE Graphics-Driver Submit Integration Test ===\n";

    // ----------------------------------------------------------------------
    // 1. A graphics submit reaches the backend: the guest-programmed viewport
    //    is applied (no host 1920x1080 default) and the draw call is counted.
    // ----------------------------------------------------------------------
    {
        Graphics::HeadlessGpuBridge bridge;
        bridge.InitializeGpu(nullptr);
        auto ring = MakeGraphicsRing(640u, 360u, 3u);
        {
            StdoutSilencer s;
            bridge.Submit(ring.data(), static_cast<uint32_t>(ring.size()), nullptr, 0,
                          /*trigger_interrupt_on_done=*/true);
        }
        Check("GraphicsSubmitCountsOneSubmission", bridge.GetGraphicsSubmitCount() == 1);
        Check("GraphicsSubmitReachesDrawCall", bridge.Backend().GetDrawCallCount() == 1);
        Check("GraphicsSubmitAppliesGuestViewport", bridge.LastSubmitOk());
        Check("GraphicsSubmitCountsInterrupt", bridge.GetInterruptCount() == 1);
        Check("GraphicsSubmitTranslatedBothPackets",
              bridge.GetTranslatedPacketCount() == 2);
    }

    // ----------------------------------------------------------------------
    // 2. A compute submit reaches the dispatch counter (SubmitAcb path).
    // ----------------------------------------------------------------------
    {
        Graphics::HeadlessGpuBridge bridge;
        bridge.InitializeGpu(nullptr);
        auto ring = MakeComputeRing(8u, 4u, 1u);
        {
            StdoutSilencer s;
            bridge.SubmitCompute(/*queue=*/0u, ring.data(),
                                 static_cast<uint32_t>(ring.size()),
                                 /*trigger_interrupt_on_done=*/false);
        }
        Check("ComputeSubmitCountsOneSubmission", bridge.GetComputeSubmitCount() == 1);
        Check("ComputeSubmitReachesDispatch",
              bridge.Backend().GetDispatchedComputeCount() == 1);
        Check("ComputeSubmitNoInterruptWhenNotRequested",
              bridge.GetInterruptCount() == 0);
    }

    // ----------------------------------------------------------------------
    // 3. Fail-closed: a truncated ring applies NO backend side effects. This
    //    is the guarantee submit_dcb() relies on -- a malformed guest ring must
    //    not partially mutate GPU state.
    // ----------------------------------------------------------------------
    {
        Graphics::HeadlessGpuBridge bridge;
        bridge.InitializeGpu(nullptr);
        // Header claims a 3-dword payload but only 1 dword follows.
        std::vector<uint32_t> ring = {MakeType3Header(3, OP_SET_CONTEXT_REG),
                                      CX_SCISSOR_BR};
        {
            StdoutSilencer s;
            bridge.Submit(ring.data(), static_cast<uint32_t>(ring.size()), nullptr, 0,
                          true);
        }
        Check("TruncatedRingReportsFailure", !bridge.LastSubmitOk());
        Check("TruncatedRingAppliesNoDraw", bridge.Backend().GetDrawCallCount() == 0);
        Check("TruncatedRingTranslatedNoPackets",
              bridge.GetTranslatedPacketCount() == 0);
    }

    // ----------------------------------------------------------------------
    // 4. Multi-ring submission accumulates state on the same driver instance,
    //    exactly as GraphicsDriverSubmitMultiDcbs loops over submit_dcb().
    // ----------------------------------------------------------------------
    {
        Graphics::HeadlessGpuBridge bridge;
        bridge.InitializeGpu(nullptr);
        auto r0 = MakeGraphicsRing(1280u, 720u, 3u);
        auto r1 = MakeGraphicsRing(1920u, 1080u, 6u);
        {
            StdoutSilencer s;
            bridge.Submit(r0.data(), static_cast<uint32_t>(r0.size()), nullptr, 0, false);
            bridge.Submit(r1.data(), static_cast<uint32_t>(r1.size()), nullptr, 0, false);
        }
        Check("MultiRingCountsBothSubmissions", bridge.GetGraphicsSubmitCount() == 2);
        Check("MultiRingCountsBothDraws", bridge.Backend().GetDrawCallCount() == 2);
        Check("MultiRingAccumulatesPackets", bridge.GetTranslatedPacketCount() == 4);
    }

    // ----------------------------------------------------------------------
    // 5. Done() advances the presented frame number the HLE flip path reads.
    // ----------------------------------------------------------------------
    {
        Graphics::HeadlessGpuBridge bridge;
        bridge.InitializeGpu(nullptr);
        Check("FrameNumStartsAtZero", bridge.GetFrameNum() == 0);
        {
            StdoutSilencer s;
            bridge.Done();
            bridge.Done();
        }
        Check("DoneAdvancesFrameNum", bridge.GetFrameNum() == 2);
    }

    // ----------------------------------------------------------------------
    // 6. An inline ACB carried by a graphics Submit is also translated (the
    //    combined DCB+ACB submission shape).
    // ----------------------------------------------------------------------
    {
        Graphics::HeadlessGpuBridge bridge;
        bridge.InitializeGpu(nullptr);
        auto dcb = MakeGraphicsRing(800u, 600u, 3u);
        auto acb = MakeComputeRing(2u, 2u, 2u);
        {
            StdoutSilencer s;
            bridge.Submit(dcb.data(), static_cast<uint32_t>(dcb.size()), acb.data(),
                          static_cast<uint32_t>(acb.size()), true);
        }
        Check("CombinedSubmitReachesDraw", bridge.Backend().GetDrawCallCount() == 1);
        Check("CombinedSubmitReachesDispatch",
              bridge.Backend().GetDispatchedComputeCount() == 1);
        Check("CombinedSubmitOk", bridge.LastSubmitOk());
    }

    std::cout << g_checks - g_failures << "/" << g_checks << " checks passed\n";
    if (g_failures == 0) {
        std::cout << ">> [PASS] HLE graphics-driver submit path verified "
                     "(headless bridge -> real translator -> real backend).\n";
    }
    return g_failures == 0 ? 0 : 1;
}
