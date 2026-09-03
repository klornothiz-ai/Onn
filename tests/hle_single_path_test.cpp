// ============================================================================
// ProsperoLayer RDNA2 Core - HLE-to-GPU single-path integration test
// ----------------------------------------------------------------------------
// Round 34: previously no real guest submission ever reached the GPU. The HLE
// graphics driver routes every guest PM4 ring through the global g_renderer,
// which was only ever set by the (never-invoked) Graphics subsystem init, and
// Graphics::WindowInit was undefined in the headless build. So a guest DCB
// died at EXIT_IF(g_renderer == nullptr) -> abort.
//
// This suite drives the REAL HLE entry point
// (Libs::Graphics::Gen5Driver::GraphicsDriverSubmitCommandBuffer), which lazily
// bootstraps g_renderer to the headless GPU bridge and flows the ring through
// the PM4 translator into the GPU backend. It proves the chain works without
// touching the bridge object directly.
// ============================================================================
#include "libs/agc.h"
#include "graphics/presentation/window.h"
#include "graphics/host_gpu/headless_gpu_bridge.hpp"
#include "gpu/pm4_decoder.hpp"

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <vector>

namespace {

using ::Graphics::HeadlessGpuBridge;
using ::Graphics::WindowInit;

int g_failures = 0;
bool Check(bool v, const char* e) {
    if (!v) { ++g_failures; std::cerr << "  [FAIL] " << e << '\n'; }
    return v;
}

// PM4 Type-3 header: COUNT stores payload_dwords - 1 (bits 31-30 = type 3).
uint32_t MakeType3Header(size_t payload_dwords, uint8_t opcode) {
    return (3u << 30u) |
           ((static_cast<uint32_t>(payload_dwords) - 1u) << 16u) |
           (static_cast<uint32_t>(opcode) << 8u);
}

// A minimal graphics DCB: SET_CONTEXT_REG screen scissor, then DRAW_INDEX_AUTO.
std::vector<uint32_t> MakeGraphicsRing() {
    constexpr uint32_t CX_SCISSOR_BR = 0xA00Du;
    std::vector<uint32_t> ring;
    ring.push_back(MakeType3Header(2, 0x69)); // SET_CONTEXT_REG, 2 payload dwords
    ring.push_back(CX_SCISSOR_BR);
    ring.push_back(640u | (360u << 16u));     // w | h<<16
    ring.push_back(MakeType3Header(2, 0x23)); // DRAW_INDEX_AUTO, 2 payload dwords
    ring.push_back(3u);                        // vertex count
    ring.push_back(0u);                        // draw initiator
    return ring;
}

} // namespace

int main() {
    std::cout << "[hle-single-path] round 34: real HLE submit -> PM4 -> GPU\n";

    // The bridge is the process-lifetime headless window singleton. We obtain
    // it through the SAME public entry the graphics driver bootstrap uses, so
    // the check observes exactly what g_renderer points at.
    auto& bridge = static_cast<HeadlessGpuBridge&>(WindowInit(640, 360));

    auto ring = MakeGraphicsRing();
    // Drive the REAL HLE entry point (not the bridge directly).
    const int rc = Libs::Graphics::Gen5Driver::GraphicsDriverSubmitCommandBuffer(
        /*queue=*/0u, ring.data(), static_cast<uint32_t>(ring.size()));
    Check(rc == 0, "HLE submit returned OK (no abort on null g_renderer)");
    Check(bridge.LastSubmitOk(), "submit reported ok");
    std::cout << "  [diag] submit_ok=" << bridge.LastSubmitOk()
              << " submits=" << bridge.GetGraphicsSubmitCount()
              << " draws=" << bridge.Backend().GetDrawCallCount()
              << " packets=" << bridge.GetTranslatedPacketCount()
              << " err=" << PM4DecodeErrorName(bridge.LastDecodeError().code) << '\n';
    Check(bridge.GetGraphicsSubmitCount() == 1, "backend saw one graphics submission");
    Check(bridge.Backend().GetDrawCallCount() == 1, "draw reached the GPU backend");
    Check(bridge.GetTranslatedPacketCount() == 2, "both PM4 packets translated");

    const bool pass = g_failures == 0;
    std::cout << (pass ? "PASS" : "FAIL") << ": "
              << "submits=" << bridge.GetGraphicsSubmitCount()
              << " draws=" << bridge.Backend().GetDrawCallCount()
              << " packets=" << bridge.GetTranslatedPacketCount()
              << " failures=" << g_failures << '\n';
    return pass ? 0 : 1;
}
