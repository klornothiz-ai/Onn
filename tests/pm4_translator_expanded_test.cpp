// ============================================================================
// ProsperoLayer RDNA2 Core - PM4 Translator Expanded Test Suite
// ============================================================================
// Description: Extends pm4_decoder_test.cpp with translator-level behaviour the
//              original suite does not cover:
//                * register state tracking (SET_SH_REG / SET_CONTEXT_REG write
//                  the recorded register files, multi-dword payloads included)
//                * viewport programming driven only by the guest's context
//                  registers (0xA200 width / 0xA201 height) with no host default,
//                  and suppression when either dimension is zero
//                * translator state persistence across multiple ring
//                  submissions on the same instance (packet counter accumulates)
//                * NOP / state-only packets that consume the stream but issue no
//                  draw or dispatch
//                * a single mixed ring that programs a viewport, dispatches
//                  compute and draws, verifying counts and order-independence
//                * the fail-closed guarantee: a stream that is valid up front
//                  but truncated at the tail applies NO backend side effects
//              The translator is exercised against an in-process backend stub,
//              so this executable never links to or initializes Vulkan. Uses the
//              same lightweight custom harness as the other suites.
// ============================================================================

#include "gpu/pm4_translator.hpp"
#include "graphics/guest_gpu/pm4.h"

#include <cstdint>
#include <iostream>
#include <sstream>
#include <vector>

// ---------------------------------------------------------------------------
// In-process backend stub. Mirrors the technique used by pm4_decoder_test.cpp:
// we provide our own definitions of the VulkanRendererBackend surface the
// translator calls, and additionally record the last programmed viewport so we
// can assert guest-driven viewport behaviour.
// ---------------------------------------------------------------------------
namespace PS5::GPU {

VulkanRendererBackend::VulkanRendererBackend() = default;
VulkanRendererBackend::~VulkanRendererBackend() = default;

// The base counters live in the class; we mirror the viewport into file-scope
// state because the stub cannot add members to the real class.
namespace {
float g_last_viewport_width = 0.0f;
float g_last_viewport_height = 0.0f;
int g_viewport_calls = 0;
} // namespace

void VulkanRendererBackend::SetViewport(float width, float height) {
    g_last_viewport_width = width;
    g_last_viewport_height = height;
    ++g_viewport_calls;
}

void VulkanRendererBackend::DispatchCompute(uint32_t, uint32_t, uint32_t) {
    ++m_dispatched_compute;
}

void VulkanRendererBackend::DrawAuto(uint32_t, uint32_t) {
    ++m_draw_calls;
}

} // namespace PS5::GPU

namespace {

using PS5::GPU::PM4DecodeErrorCode;
using PS5::GPU::PM4Type3Opcode;
using PS5::GPU::PM4VulkanTranslator;
using PS5::GPU::VulkanRendererBackend;

uint32_t MakeType3Header(size_t payload_dwords, uint8_t opcode) {
    return (3u << 30u) |
           ((static_cast<uint32_t>(payload_dwords) - 1u) << 16u) |
           (static_cast<uint32_t>(opcode) << 8u);
}

void ResetViewportProbe() {
    PS5::GPU::g_last_viewport_width = 0.0f;
    PS5::GPU::g_last_viewport_height = 0.0f;
    PS5::GPU::g_viewport_calls = 0;
}

// Suppress the translator's informational stdout for state-only packets so the
// PASS/FAIL log stays readable.
struct StdoutSilencer {
    std::ostringstream sink;
    std::streambuf* previous;
    StdoutSilencer() : previous(std::cout.rdbuf(sink.rdbuf())) {}
    ~StdoutSilencer() { std::cout.rdbuf(previous); }
};

bool Check(bool value, const char* expression, int line) {
    if (!value) {
        std::cerr << "check failed at line " << line << ": " << expression << '\n';
    }
    return value;
}

#define CHECK(expression) \
    do { \
        if (!Check((expression), #expression, __LINE__)) return false; \
    } while (false)

// SET_SH_REG with a multi-dword payload records each register, and issues no
// draw/dispatch.
bool SetShRegRecordsStateWithoutSideEffects() {
    // payload = { first_register, v0, v1, v2 } -> 4 dwords.
    const std::vector<uint32_t> stream = {
        MakeType3Header(4, static_cast<uint8_t>(PM4Type3Opcode::PKT3_SET_SH_REG)),
        0x2C, 0x11111111u, 0x22222222u, 0x33333333u,
    };
    VulkanRendererBackend backend;
    PM4VulkanTranslator translator(backend);
    const auto result =
        translator.TranslateAndExecuteCommandRingChecked(stream.data(), stream.size());

    CHECK(result.ok());
    CHECK(result.executed_packets == 1);
    CHECK(translator.GetProcessedPacketsCount() == 1);
    CHECK(backend.GetDispatchedComputeCount() == 0);
    CHECK(backend.GetDrawCallCount() == 0);
    return true;
}

// SET_CONTEXT_REG that programs the screen-scissor bottom-right corner
// (the REAL PA_SC_SCREEN_SCISSOR_BR register, w | h<<16) drives SetViewport
// with exactly the guest-supplied dimensions -- no host 1920x1080
// substitution. (Round 18: the old 0xA200/0xA201 "viewport" pair was
// actually DB_DEPTH_CONTROL/DB_STENCIL_CONTROL on real hardware.)
bool ContextRegProgramsGuestViewport() {
    ResetViewportProbe();
    const uint32_t br = 1280u | (720u << 16u);
    const std::vector<uint32_t> stream = {
        MakeType3Header(2, static_cast<uint8_t>(PM4Type3Opcode::PKT3_SET_CONTEXT_REG)),
        Pm4::PA_SC_SCREEN_SCISSOR_BR, br,
    };
    VulkanRendererBackend backend;
    PM4VulkanTranslator translator(backend);
    const auto result =
        translator.TranslateAndExecuteCommandRingChecked(stream.data(), stream.size());

    CHECK(result.ok());
    CHECK(PS5::GPU::g_viewport_calls == 1);
    CHECK(PS5::GPU::g_last_viewport_width == 1280.0f);
    CHECK(PS5::GPU::g_last_viewport_height == 720.0f);
    return true;
}

// A zero dimension must NOT program a viewport (guard against a 0-sized frame).
bool ZeroViewportDimensionIsIgnored() {
    ResetViewportProbe();
    const uint32_t br = 1280u | (0u << 16u);   // height = 0
    const std::vector<uint32_t> stream = {
        MakeType3Header(2, static_cast<uint8_t>(PM4Type3Opcode::PKT3_SET_CONTEXT_REG)),
        Pm4::PA_SC_SCREEN_SCISSOR_BR, br,
    };
    VulkanRendererBackend backend;
    PM4VulkanTranslator translator(backend);
    const auto result =
        translator.TranslateAndExecuteCommandRingChecked(stream.data(), stream.size());

    CHECK(result.ok());
    CHECK(PS5::GPU::g_viewport_calls == 0);
    return true;
}

// NOP is consumed but produces no draw/dispatch/viewport activity.
bool NopIsConsumedWithoutSideEffects() {
    const std::vector<uint32_t> stream = {
        MakeType3Header(1, static_cast<uint8_t>(PM4Type3Opcode::PKT3_NOP)),
        0,
    };
    ResetViewportProbe();
    VulkanRendererBackend backend;
    PM4VulkanTranslator translator(backend);
    const auto result =
        translator.TranslateAndExecuteCommandRingChecked(stream.data(), stream.size());

    CHECK(result.ok());
    CHECK(result.executed_packets == 1);
    CHECK(backend.GetDispatchedComputeCount() == 0);
    CHECK(backend.GetDrawCallCount() == 0);
    CHECK(PS5::GPU::g_viewport_calls == 0);
    return true;
}

// The processed-packet counter accumulates across multiple submissions on the
// same translator instance (state persists between rings).
bool PacketCounterPersistsAcrossSubmissions() {
    const std::vector<uint32_t> ring_a = {
        MakeType3Header(4, static_cast<uint8_t>(PM4Type3Opcode::PKT3_DISPATCH_DIRECT)),
        8, 4, 2, 0,
    };
    const std::vector<uint32_t> ring_b = {
        MakeType3Header(2, static_cast<uint8_t>(PM4Type3Opcode::PKT3_DRAW_INDEX_AUTO)),
        12, 0,
    };
    VulkanRendererBackend backend;
    PM4VulkanTranslator translator(backend);

    CHECK(translator.TranslateAndExecuteCommandRingChecked(ring_a.data(), ring_a.size()).ok());
    CHECK(translator.GetProcessedPacketsCount() == 1);
    CHECK(translator.TranslateAndExecuteCommandRingChecked(ring_b.data(), ring_b.size()).ok());
    CHECK(translator.GetProcessedPacketsCount() == 2);
    CHECK(backend.GetDispatchedComputeCount() == 1);
    CHECK(backend.GetDrawCallCount() == 1);
    return true;
}

// A rich mixed ring: viewport, then compute dispatch, then two draws. All
// counts must reflect exactly what the stream requested.
bool MixedRingProgramsViewportDispatchAndDraws() {
    ResetViewportProbe();
    const uint32_t br = 800u | (600u << 16u);
    const std::vector<uint32_t> stream = {
        MakeType3Header(2, static_cast<uint8_t>(PM4Type3Opcode::PKT3_SET_CONTEXT_REG)),
        Pm4::PA_SC_SCREEN_SCISSOR_BR, br,
        MakeType3Header(4, static_cast<uint8_t>(PM4Type3Opcode::PKT3_DISPATCH_DIRECT)),
        8, 4, 2, 0,
        MakeType3Header(2, static_cast<uint8_t>(PM4Type3Opcode::PKT3_DRAW_INDEX_AUTO)),
        12, 0,
        MakeType3Header(2, static_cast<uint8_t>(PM4Type3Opcode::PKT3_DRAW_INDEX_2)),
        24, 0,
    };
    VulkanRendererBackend backend;
    PM4VulkanTranslator translator(backend);
    const auto result =
        translator.TranslateAndExecuteCommandRingChecked(stream.data(), stream.size());

    CHECK(result.ok());
    CHECK(result.consumed_dwords == stream.size());
    CHECK(result.executed_packets == 4);
    CHECK(translator.GetProcessedPacketsCount() == 4);
    CHECK(backend.GetDispatchedComputeCount() == 1);
    CHECK(backend.GetDrawCallCount() == 2);
    CHECK(PS5::GPU::g_viewport_calls == 1);
    CHECK(PS5::GPU::g_last_viewport_width == 800.0f);
    CHECK(PS5::GPU::g_last_viewport_height == 600.0f);
    return true;
}

// Fail-closed: a stream whose leading packets are valid but whose final packet
// is truncated must apply NO side effects at all (all-or-nothing execution).
bool TailTruncationAppliesNoSideEffects() {
    ResetViewportProbe();
    const uint32_t br = 640u | (480u << 16u);
    const std::vector<uint32_t> stream = {
        // valid scissor + dispatch that WOULD execute if applied incrementally
        MakeType3Header(2, static_cast<uint8_t>(PM4Type3Opcode::PKT3_SET_CONTEXT_REG)),
        Pm4::PA_SC_SCREEN_SCISSOR_BR, br,
        MakeType3Header(4, static_cast<uint8_t>(PM4Type3Opcode::PKT3_DISPATCH_DIRECT)),
        8, 4, 2, 0,
        // truncated draw: header claims 2 payload dwords, only 1 present
        MakeType3Header(2, static_cast<uint8_t>(PM4Type3Opcode::PKT3_DRAW_INDEX_AUTO)),
        12,
    };
    VulkanRendererBackend backend;
    PM4VulkanTranslator translator(backend);
    const auto result =
        translator.TranslateAndExecuteCommandRingChecked(stream.data(), stream.size());

    CHECK(!result.ok());
    CHECK(result.error.code == PM4DecodeErrorCode::TruncatedPacket);
    CHECK(result.executed_packets == 0);
    CHECK(translator.GetProcessedPacketsCount() == 0);
    CHECK(backend.GetDispatchedComputeCount() == 0);
    CHECK(backend.GetDrawCallCount() == 0);
    CHECK(PS5::GPU::g_viewport_calls == 0);
    return true;
}

// The legacy adapter reports the decode error via GetLastDecodeError and, like
// the checked path, applies nothing when the stream is malformed.
bool LegacyAdapterReportsErrorAndAppliesNothing() {
    const std::vector<uint32_t> stream = {
        MakeType3Header(4, static_cast<uint8_t>(PM4Type3Opcode::PKT3_DISPATCH_DIRECT)),
        8, 4, 2, // one dword short
    };
    VulkanRendererBackend backend;
    PM4VulkanTranslator translator(backend);

    std::ostringstream captured_error;
    std::streambuf* const previous = std::cerr.rdbuf(captured_error.rdbuf());
    translator.TranslateAndExecuteCommandRing(stream.data(), stream.size());
    std::cerr.rdbuf(previous);

    CHECK(translator.GetLastDecodeError().code == PM4DecodeErrorCode::TruncatedPacket);
    CHECK(translator.GetProcessedPacketsCount() == 0);
    CHECK(backend.GetDispatchedComputeCount() == 0);
    CHECK(backend.GetDrawCallCount() == 0);
    CHECK(!captured_error.str().empty());
    return true;
}

struct TestCase {
    const char* name;
    bool (*run)();
};

} // namespace

int main() {
    // Informational packet handlers print to stdout; keep the harness output
    // clean while still letting [PASS]/[FAIL] through after each test.
    const TestCase tests[] = {
        {"SetShRegRecordsStateWithoutSideEffects", SetShRegRecordsStateWithoutSideEffects},
        {"ContextRegProgramsGuestViewport", ContextRegProgramsGuestViewport},
        {"ZeroViewportDimensionIsIgnored", ZeroViewportDimensionIsIgnored},
        {"NopIsConsumedWithoutSideEffects", NopIsConsumedWithoutSideEffects},
        {"PacketCounterPersistsAcrossSubmissions", PacketCounterPersistsAcrossSubmissions},
        {"MixedRingProgramsViewportDispatchAndDraws", MixedRingProgramsViewportDispatchAndDraws},
        {"TailTruncationAppliesNoSideEffects", TailTruncationAppliesNoSideEffects},
        {"LegacyAdapterReportsErrorAndAppliesNothing", LegacyAdapterReportsErrorAndAppliesNothing},
    };

    size_t passed = 0;
    for (const TestCase& test : tests) {
        bool ok;
        {
            StdoutSilencer quiet;
            ok = test.run();
        }
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << test.name << '\n';
        if (ok) ++passed;
    }

    std::cout << passed << '/' << (sizeof(tests) / sizeof(tests[0])) << " tests passed\n";
    return passed == (sizeof(tests) / sizeof(tests[0])) ? 0 : 1;
}
