#include "gpu/pm4_translator.hpp"
#include "graphics/guest_gpu/pm4.h"

#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// The translator is tested with an in-process backend stub, so this executable
// does not link to or initialize Vulkan.
namespace PS5::GPU {

VulkanRendererBackend::VulkanRendererBackend() = default;
VulkanRendererBackend::~VulkanRendererBackend() = default;

void VulkanRendererBackend::SetViewport(float, float) {}

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

static_assert(static_cast<uint32_t>(PM4Type3Opcode::PKT3_NOP) == Pm4::IT_NOP);
static_assert(static_cast<uint32_t>(PM4Type3Opcode::PKT3_SET_BASE) == Pm4::IT_SET_BASE);
static_assert(static_cast<uint32_t>(PM4Type3Opcode::PKT3_DISPATCH_DIRECT) == Pm4::IT_DISPATCH_DIRECT);
static_assert(static_cast<uint32_t>(PM4Type3Opcode::PKT3_DISPATCH_INDIRECT) == Pm4::IT_DISPATCH_INDIRECT);
static_assert(static_cast<uint32_t>(PM4Type3Opcode::PKT3_DRAW_INDEX_2) == Pm4::IT_DRAW_INDEX_2);
static_assert(static_cast<uint32_t>(PM4Type3Opcode::PKT3_DRAW_INDEX_AUTO) == Pm4::IT_DRAW_INDEX_AUTO);
static_assert(static_cast<uint32_t>(PM4Type3Opcode::PKT3_COPY_DATA) == Pm4::IT_COPY_DATA);
static_assert(static_cast<uint32_t>(PM4Type3Opcode::PKT3_WRITE_DATA) == Pm4::IT_WRITE_DATA);
static_assert(static_cast<uint32_t>(PM4Type3Opcode::PKT3_EVENT_WRITE) == Pm4::IT_EVENT_WRITE);
static_assert(static_cast<uint32_t>(PM4Type3Opcode::PKT3_RELEASE_MEM) == Pm4::IT_RELEASE_MEM);
static_assert(static_cast<uint32_t>(PM4Type3Opcode::PKT3_SET_CONTEXT_REG) == Pm4::IT_SET_CONTEXT_REG);
static_assert(static_cast<uint32_t>(PM4Type3Opcode::PKT3_SET_SH_REG) == Pm4::IT_SET_SH_REG);
static_assert(static_cast<uint32_t>(PM4Type3Opcode::PKT3_SET_UCONFIG_REG) == Pm4::IT_SET_UCONFIG_REG);

uint32_t MakeType3Header(size_t payload_dwords, uint8_t opcode) {
    return (3u << 30u) |
           ((static_cast<uint32_t>(payload_dwords) - 1u) << 16u) |
           (static_cast<uint32_t>(opcode) << 8u);
}

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "    check failed at line " << __LINE__ << ": " #condition "\n"; \
            return false; \
        } \
    } while (false)

bool EmptyStreamIsValid() {
    const auto result = PS5::GPU::DecodePM4Type3Stream(nullptr, 0);
    CHECK(result.ok());
    CHECK(result.packets.empty());
    CHECK(result.consumed_dwords == 0);
    return true;
}

bool NullNonEmptyStreamIsRejected() {
    const auto result = PS5::GPU::DecodePM4Type3Stream(nullptr, 1);
    CHECK(!result.ok());
    CHECK(result.error.code == PM4DecodeErrorCode::NullStream);
    CHECK(result.packets.empty());
    return true;
}

bool StandardCountMathIsUsed() {
    const uint32_t header = MakeType3Header(
        16384, static_cast<uint8_t>(PM4Type3Opcode::PKT3_NOP));
    CHECK(PS5::GPU::PM4Type3PacketType(header) == 3);
    CHECK(PS5::GPU::PM4Type3PayloadDwords(header) == 16384);
    CHECK(PS5::GPU::PM4Type3OpcodeValue(header) ==
          static_cast<uint8_t>(PM4Type3Opcode::PKT3_NOP));
    return true;
}

bool MultiplePacketsDecodeWithCorrectOffsets() {
    const std::vector<uint32_t> stream = {
        MakeType3Header(1, static_cast<uint8_t>(PM4Type3Opcode::PKT3_NOP)),
        0,
        MakeType3Header(4, static_cast<uint8_t>(PM4Type3Opcode::PKT3_DISPATCH_DIRECT)),
        8, 4, 2, 0,
    };

    const auto result = PS5::GPU::DecodePM4Type3Stream(stream.data(), stream.size());
    CHECK(result.ok());
    CHECK(result.packets.size() == 2);
    CHECK(result.consumed_dwords == stream.size());
    CHECK(result.packets[0].header_offset == 0);
    CHECK(result.packets[0].payload_offset == 1);
    CHECK(result.packets[0].payload_dwords == 1);
    CHECK(result.packets[1].header_offset == 2);
    CHECK(result.packets[1].payload_offset == 3);
    CHECK(result.packets[1].payload_dwords == 4);
    return true;
}

bool TruncationRejectsTheWholeStream() {
    const std::vector<uint32_t> stream = {
        MakeType3Header(1, static_cast<uint8_t>(PM4Type3Opcode::PKT3_NOP)),
        0,
        MakeType3Header(4, static_cast<uint8_t>(PM4Type3Opcode::PKT3_DISPATCH_DIRECT)),
        8, 4, 2,
    };

    const auto result = PS5::GPU::DecodePM4Type3Stream(stream.data(), stream.size());
    CHECK(!result.ok());
    CHECK(result.error.code == PM4DecodeErrorCode::TruncatedPacket);
    CHECK(result.error.dword_offset == 2);
    CHECK(result.error.packet_index == 1);
    CHECK(result.error.required_payload_dwords == 4);
    CHECK(result.error.available_payload_dwords == 3);
    CHECK(result.packets.empty());
    CHECK(result.consumed_dwords == 2);
    return true;
}

bool HeaderWithoutPayloadIsRejected() {
    const uint32_t stream[] = {
        MakeType3Header(1, static_cast<uint8_t>(PM4Type3Opcode::PKT3_NOP)),
    };
    const auto result = PS5::GPU::DecodePM4Type3Stream(stream, 1);
    CHECK(!result.ok());
    CHECK(result.error.code == PM4DecodeErrorCode::TruncatedPacket);
    CHECK(result.error.required_payload_dwords == 1);
    CHECK(result.error.available_payload_dwords == 0);
    return true;
}

bool EveryCountFieldRejectsAMissingPayload() {
    for (uint32_t count_field = 0; count_field <= 0x3FFFu; ++count_field) {
        const uint32_t stream[] = {
            (3u << 30u) | (count_field << 16u) |
                (static_cast<uint32_t>(PM4Type3Opcode::PKT3_NOP) << 8u),
        };
        const auto result = PS5::GPU::DecodePM4Type3Stream(stream, 1);
        CHECK(result.error.code == PM4DecodeErrorCode::TruncatedPacket);
        CHECK(result.error.required_payload_dwords == count_field + 1u);
        CHECK(result.error.available_payload_dwords == 0);
    }
    return true;
}

bool NonType3PacketIsRejected() {
    const uint32_t stream[] = {2u << 30u};
    const auto result = PS5::GPU::DecodePM4Type3Stream(stream, 1);
    CHECK(!result.ok());
    CHECK(result.error.code == PM4DecodeErrorCode::UnsupportedPacketType);
    CHECK(result.error.dword_offset == 0);
    return true;
}

bool ShortKnownPacketIsRejected() {
    const uint32_t stream[] = {
        MakeType3Header(1, static_cast<uint8_t>(PM4Type3Opcode::PKT3_SET_SH_REG)),
        0x20,
    };
    const auto result = PS5::GPU::DecodePM4Type3Stream(stream, 2);
    CHECK(!result.ok());
    CHECK(result.error.code == PM4DecodeErrorCode::InvalidPacketPayload);
    CHECK(result.error.required_payload_dwords == 2);
    CHECK(result.error.available_payload_dwords == 1);
    return true;
}

bool ShortDispatchIsRejected() {
    const uint32_t stream[] = {
        MakeType3Header(3, static_cast<uint8_t>(PM4Type3Opcode::PKT3_DISPATCH_DIRECT)),
        8, 4, 2,
    };
    const auto result = PS5::GPU::DecodePM4Type3Stream(stream, 4);
    CHECK(!result.ok());
    CHECK(result.error.code == PM4DecodeErrorCode::InvalidPacketPayload);
    CHECK(result.error.required_payload_dwords == 4);
    CHECK(result.error.available_payload_dwords == 3);
    return true;
}

bool ShortDrawIsRejected() {
    const uint32_t stream[] = {
        MakeType3Header(1, static_cast<uint8_t>(PM4Type3Opcode::PKT3_DRAW_INDEX_AUTO)),
        12,
    };
    const auto result = PS5::GPU::DecodePM4Type3Stream(stream, 2);
    CHECK(!result.ok());
    CHECK(result.error.code == PM4DecodeErrorCode::InvalidPacketPayload);
    CHECK(result.error.required_payload_dwords == 2);
    CHECK(result.error.available_payload_dwords == 1);
    return true;
}

bool UnknownOpcodeRemainsStructurallyDecodable() {
    const uint32_t stream[] = {MakeType3Header(1, 0xFE), 0x12345678};
    const auto result = PS5::GPU::DecodePM4Type3Stream(stream, 2);
    CHECK(result.ok());
    CHECK(result.packets.size() == 1);
    CHECK(result.packets[0].raw_opcode == 0xFE);
    CHECK(std::string(PS5::GPU::PM4Type3OpcodeName(0xFE)) == "PKT3_UNKNOWN");
    return true;
}

bool InvalidStreamHasNoBackendSideEffects() {
    const std::vector<uint32_t> stream = {
        MakeType3Header(3, static_cast<uint8_t>(PM4Type3Opcode::PKT3_DISPATCH_DIRECT)),
        8, 4, 2,
        MakeType3Header(1, static_cast<uint8_t>(PM4Type3Opcode::PKT3_SET_SH_REG)),
        0x20,
    };

    PS5::GPU::VulkanRendererBackend backend;
    PS5::GPU::PM4VulkanTranslator translator(backend);
    const auto result =
        translator.TranslateAndExecuteCommandRingChecked(stream.data(), stream.size());

    CHECK(!result.ok());
    CHECK(result.error.code == PM4DecodeErrorCode::InvalidPacketPayload);
    CHECK(result.executed_packets == 0);
    CHECK(translator.GetProcessedPacketsCount() == 0);
    CHECK(backend.GetDispatchedComputeCount() == 0);
    CHECK(backend.GetDrawCallCount() == 0);
    return true;
}

bool ValidStreamExecutesAfterValidation() {
    const std::vector<uint32_t> stream = {
        MakeType3Header(4, static_cast<uint8_t>(PM4Type3Opcode::PKT3_DISPATCH_DIRECT)),
        8, 4, 2, 0,
        MakeType3Header(2, static_cast<uint8_t>(PM4Type3Opcode::PKT3_DRAW_INDEX_AUTO)),
        12, 0,
    };

    PS5::GPU::VulkanRendererBackend backend;
    PS5::GPU::PM4VulkanTranslator translator(backend);
    const auto result =
        translator.TranslateAndExecuteCommandRingChecked(stream.data(), stream.size());

    CHECK(result.ok());
    CHECK(result.consumed_dwords == stream.size());
    CHECK(result.executed_packets == 2);
    CHECK(translator.GetProcessedPacketsCount() == 2);
    CHECK(backend.GetDispatchedComputeCount() == 1);
    CHECK(backend.GetDrawCallCount() == 1);
    return true;
}

bool MixedPacketStreamExecutesAfterValidation() {
    const std::vector<uint32_t> stream = {
        2u << 30u,
        0u, 0xCAFEu,
        1u << 30u,
        MakeType3Header(4, static_cast<uint8_t>(PM4Type3Opcode::PKT3_DISPATCH_DIRECT)),
        8, 4, 2, 0,
        MakeType3Header(2, static_cast<uint8_t>(PM4Type3Opcode::PKT3_DRAW_INDEX_AUTO)),
        12, 0,
    };

    PS5::GPU::VulkanRendererBackend backend;
    PS5::GPU::PM4VulkanTranslator translator(backend);
    translator.TranslateAndExecuteCommandRing(stream.data(), stream.size());

    CHECK(translator.GetLastDecodeError().code == PM4DecodeErrorCode::None);
    CHECK(translator.GetProcessedPacketsCount() == 2);
    CHECK(backend.GetDispatchedComputeCount() == 1);
    CHECK(backend.GetDrawCallCount() == 1);
    return true;
}

bool InvalidLegacyRingHasNoBackendSideEffects() {
    const std::vector<uint32_t> stream = {
        MakeType3Header(4, static_cast<uint8_t>(PM4Type3Opcode::PKT3_DISPATCH_DIRECT)),
        8, 4, 2, 0,
        1u << 16u,
        0x1234u,
    };

    PS5::GPU::VulkanRendererBackend backend;
    PS5::GPU::PM4VulkanTranslator translator(backend);
    std::ostringstream captured_error;
    std::streambuf* const previous_error_buffer = std::cerr.rdbuf(captured_error.rdbuf());
    translator.TranslateAndExecuteCommandRing(stream.data(), stream.size());
    std::cerr.rdbuf(previous_error_buffer);

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
    const TestCase tests[] = {
        {"EmptyStreamIsValid", EmptyStreamIsValid},
        {"NullNonEmptyStreamIsRejected", NullNonEmptyStreamIsRejected},
        {"StandardCountMathIsUsed", StandardCountMathIsUsed},
        {"MultiplePacketsDecodeWithCorrectOffsets", MultiplePacketsDecodeWithCorrectOffsets},
        {"TruncationRejectsTheWholeStream", TruncationRejectsTheWholeStream},
        {"HeaderWithoutPayloadIsRejected", HeaderWithoutPayloadIsRejected},
        {"EveryCountFieldRejectsAMissingPayload", EveryCountFieldRejectsAMissingPayload},
        {"NonType3PacketIsRejected", NonType3PacketIsRejected},
        {"ShortKnownPacketIsRejected", ShortKnownPacketIsRejected},
        {"ShortDispatchIsRejected", ShortDispatchIsRejected},
        {"ShortDrawIsRejected", ShortDrawIsRejected},
        {"UnknownOpcodeRemainsStructurallyDecodable", UnknownOpcodeRemainsStructurallyDecodable},
        {"InvalidStreamHasNoBackendSideEffects", InvalidStreamHasNoBackendSideEffects},
        {"ValidStreamExecutesAfterValidation", ValidStreamExecutesAfterValidation},
        {"MixedPacketStreamExecutesAfterValidation", MixedPacketStreamExecutesAfterValidation},
        {"InvalidLegacyRingHasNoBackendSideEffects", InvalidLegacyRingHasNoBackendSideEffects},
    };

    size_t passed = 0;
    for (const TestCase& test : tests) {
        const bool ok = test.run();
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << test.name << '\n';
        if (ok) {
            ++passed;
        }
    }

    std::cout << passed << '/' << (sizeof(tests) / sizeof(tests[0])) << " tests passed\n";
    return passed == (sizeof(tests) / sizeof(tests[0])) ? 0 : 1;
}
