#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace PS5::GPU {

// Values mirror the guest command processor definitions in graphics/guest_gpu/pm4.h.
enum class PM4Type3Opcode : uint32_t {
    PKT3_NOP                        = 0x00,
    PKT3_SET_BASE                   = 0x01,
    PKT3_INDEX_BUFFER_SIZE          = 0x03,
    PKT3_DISPATCH_DIRECT            = 0x04,
    PKT3_DISPATCH_INDIRECT          = 0x05,
    PKT3_WAIT_REG_MEM               = 0x3C,
    PKT3_DRAW_INDEX_2               = 0x22,
    PKT3_DRAW_INDEX_AUTO            = 0x23,
    PKT3_DRAW_INDEX_INDIRECT        = 0x24,
    PKT3_DRAW_INDEX_INDIRECT_MULTI  = 0x25,
    PKT3_DRAW_INDEX_OFFSET_2        = 0x26,
    PKT3_DRAW_INDIRECT              = 0x27,
    PKT3_DRAW_INDEX_INDIRECT_MULTI2 = 0x2B,
    PKT3_DISPATCH_DRAW_PREAMBLE     = 0x2C,
    PKT3_INDEX_BASE                 = 0x2D,
    PKT3_INDEX_TYPE                 = 0x2E,
    PKT3_NUM_INSTANCES              = 0x2F,
    PKT3_SET_PREDICATION            = 0x30,
    PKT3_DISPATCH_DRAW              = 0x31,
    PKT3_INDIRECT_BUFFER            = 0x3F,
    PKT3_COPY_DATA                  = 0x40,
    PKT3_WRITE_DATA                 = 0x41,
    PKT3_EVENT_WRITE                = 0x46,
    PKT3_EVENT_WRITE_EOP            = 0x47,
    PKT3_EVENT_WRITE_EOS            = 0x48,
    PKT3_RELEASE_MEM                = 0x49,
    PKT3_DMA_DATA                   = 0x50,
    PKT3_PFP_SYNC_ME                = 0x51,
    PKT3_SET_CONTEXT_REG            = 0x69,
    PKT3_SET_CONTEXT_REG_INDIRECT   = 0x73,
    PKT3_SET_SH_REG                 = 0x76,
    PKT3_SET_SH_REG_INDIRECT        = 0x77,
    PKT3_SET_UCONFIG_REG            = 0x79,
    PKT3_SET_UCONFIG_REG_INDEX      = 0x7A,
    PKT3_SET_UCONFIG_REG_INDIRECT   = 0x7B,

    // Retained for source compatibility with the original translator.
    PKT3_ACQUIRE_MEM                = 0x58,
};

enum class PM4DecodeErrorCode : uint8_t {
    None,
    NullStream,
    UnsupportedPacketType,
    TruncatedPacket,
    InvalidPacketPayload,
};

struct PM4DecodeError {
    PM4DecodeErrorCode code{PM4DecodeErrorCode::None};
    size_t dword_offset{0};
    size_t packet_index{0};
    uint32_t header{0};
    uint8_t opcode{0};
    size_t required_payload_dwords{0};
    size_t available_payload_dwords{0};
};

struct PM4Type3Packet {
    PM4Type3Opcode opcode{PM4Type3Opcode::PKT3_NOP};
    uint8_t raw_opcode{0};
    size_t header_offset{0};
    size_t payload_offset{0};
    size_t payload_dwords{0};
};

struct PM4DecodeResult {
    std::vector<PM4Type3Packet> packets;
    PM4DecodeError error{};
    size_t consumed_dwords{0};

    bool ok() const noexcept { return error.code == PM4DecodeErrorCode::None; }
    explicit operator bool() const noexcept { return ok(); }
};

constexpr uint32_t PM4Type3PacketType(uint32_t header) noexcept {
    return header >> 30u;
}

// In a standard Type-3 header, COUNT stores payload_dwords - 1.
constexpr size_t PM4Type3PayloadDwords(uint32_t header) noexcept {
    return static_cast<size_t>(((header >> 16u) & 0x3FFFu) + 1u);
}

constexpr uint8_t PM4Type3OpcodeValue(uint32_t header) noexcept {
    return static_cast<uint8_t>((header >> 8u) & 0xFFu);
}

PM4DecodeResult DecodePM4Type3Stream(const uint32_t* stream, size_t dword_count);
const char* PM4DecodeErrorName(PM4DecodeErrorCode code) noexcept;
const char* PM4Type3OpcodeName(uint8_t opcode) noexcept;

} // namespace PS5::GPU
