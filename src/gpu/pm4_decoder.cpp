#include "gpu/pm4_decoder.hpp"

namespace PS5::GPU {
namespace {

constexpr size_t MinimumPayloadDwords(uint8_t opcode) noexcept {
    switch (static_cast<PM4Type3Opcode>(opcode)) {
    case PM4Type3Opcode::PKT3_SET_SH_REG:
    case PM4Type3Opcode::PKT3_SET_CONTEXT_REG:
    case PM4Type3Opcode::PKT3_SET_UCONFIG_REG:
        return 2;
    case PM4Type3Opcode::PKT3_DISPATCH_DIRECT:
        // x, y, z, and DISPATCH_INITIATOR are required for a complete packet.
        return 4;
    case PM4Type3Opcode::PKT3_DRAW_INDEX_AUTO:
        // Vertex count and draw initiator are both required.
        return 2;
    default:
        return 0;
    }
}

PM4DecodeResult DecodeFailure(PM4DecodeError error, size_t consumed_dwords) {
    PM4DecodeResult result;
    result.error = error;
    result.consumed_dwords = consumed_dwords;
    return result;
}

} // namespace

PM4DecodeResult DecodePM4Type3Stream(const uint32_t* stream, size_t dword_count) {
    if (dword_count == 0) {
        return {};
    }
    if (stream == nullptr) {
        PM4DecodeError error;
        error.code = PM4DecodeErrorCode::NullStream;
        error.available_payload_dwords = dword_count;
        return DecodeFailure(error, 0);
    }

    PM4DecodeResult result;
    size_t cursor = 0;
    size_t packet_index = 0;

    while (cursor < dword_count) {
        const uint32_t header = stream[cursor];
        const uint32_t type = PM4Type3PacketType(header);
        const uint8_t opcode = PM4Type3OpcodeValue(header);

        if (type != 3u) {
            PM4DecodeError error;
            error.code = PM4DecodeErrorCode::UnsupportedPacketType;
            error.dword_offset = cursor;
            error.packet_index = packet_index;
            error.header = header;
            error.opcode = opcode;
            return DecodeFailure(error, cursor);
        }

        const size_t payload_dwords = PM4Type3PayloadDwords(header);
        const size_t available_payload_dwords = dword_count - cursor - 1u;
        if (payload_dwords > available_payload_dwords) {
            PM4DecodeError error;
            error.code = PM4DecodeErrorCode::TruncatedPacket;
            error.dword_offset = cursor;
            error.packet_index = packet_index;
            error.header = header;
            error.opcode = opcode;
            error.required_payload_dwords = payload_dwords;
            error.available_payload_dwords = available_payload_dwords;
            return DecodeFailure(error, cursor);
        }

        const size_t minimum_payload_dwords = MinimumPayloadDwords(opcode);
        if (payload_dwords < minimum_payload_dwords) {
            PM4DecodeError error;
            error.code = PM4DecodeErrorCode::InvalidPacketPayload;
            error.dword_offset = cursor;
            error.packet_index = packet_index;
            error.header = header;
            error.opcode = opcode;
            error.required_payload_dwords = minimum_payload_dwords;
            error.available_payload_dwords = payload_dwords;
            return DecodeFailure(error, cursor);
        }

        PM4Type3Packet packet;
        packet.opcode = static_cast<PM4Type3Opcode>(opcode);
        packet.raw_opcode = opcode;
        packet.header_offset = cursor;
        packet.payload_offset = cursor + 1u;
        packet.payload_dwords = payload_dwords;
        result.packets.push_back(packet);

        cursor += payload_dwords + 1u;
        ++packet_index;
    }

    result.consumed_dwords = cursor;
    return result;
}

const char* PM4DecodeErrorName(PM4DecodeErrorCode code) noexcept {
    switch (code) {
    case PM4DecodeErrorCode::None:
        return "none";
    case PM4DecodeErrorCode::NullStream:
        return "null stream";
    case PM4DecodeErrorCode::UnsupportedPacketType:
        return "unsupported packet type";
    case PM4DecodeErrorCode::TruncatedPacket:
        return "truncated packet";
    case PM4DecodeErrorCode::InvalidPacketPayload:
        return "invalid packet payload";
    }
    return "unknown decode error";
}

const char* PM4Type3OpcodeName(uint8_t opcode) noexcept {
    switch (static_cast<PM4Type3Opcode>(opcode)) {
    case PM4Type3Opcode::PKT3_NOP: return "PKT3_NOP";
    case PM4Type3Opcode::PKT3_SET_BASE: return "PKT3_SET_BASE";
    case PM4Type3Opcode::PKT3_INDEX_BUFFER_SIZE: return "PKT3_INDEX_BUFFER_SIZE";
    case PM4Type3Opcode::PKT3_DISPATCH_DIRECT: return "PKT3_DISPATCH_DIRECT";
    case PM4Type3Opcode::PKT3_DISPATCH_INDIRECT: return "PKT3_DISPATCH_INDIRECT";
    case PM4Type3Opcode::PKT3_WAIT_REG_MEM: return "PKT3_WAIT_REG_MEM";
    case PM4Type3Opcode::PKT3_DRAW_INDEX_2: return "PKT3_DRAW_INDEX_2";
    case PM4Type3Opcode::PKT3_DRAW_INDEX_AUTO: return "PKT3_DRAW_INDEX_AUTO";
    case PM4Type3Opcode::PKT3_DRAW_INDEX_INDIRECT: return "PKT3_DRAW_INDEX_INDIRECT";
    case PM4Type3Opcode::PKT3_DRAW_INDEX_INDIRECT_MULTI: return "PKT3_DRAW_INDEX_INDIRECT_MULTI";
    case PM4Type3Opcode::PKT3_DRAW_INDEX_OFFSET_2: return "PKT3_DRAW_INDEX_OFFSET_2";
    case PM4Type3Opcode::PKT3_DRAW_INDIRECT: return "PKT3_DRAW_INDIRECT";
    case PM4Type3Opcode::PKT3_DRAW_INDEX_INDIRECT_MULTI2: return "PKT3_DRAW_INDEX_INDIRECT_MULTI2";
    case PM4Type3Opcode::PKT3_DISPATCH_DRAW_PREAMBLE: return "PKT3_DISPATCH_DRAW_PREAMBLE";
    case PM4Type3Opcode::PKT3_INDEX_BASE: return "PKT3_INDEX_BASE";
    case PM4Type3Opcode::PKT3_INDEX_TYPE: return "PKT3_INDEX_TYPE";
    case PM4Type3Opcode::PKT3_NUM_INSTANCES: return "PKT3_NUM_INSTANCES";
    case PM4Type3Opcode::PKT3_SET_PREDICATION: return "PKT3_SET_PREDICATION";
    case PM4Type3Opcode::PKT3_DISPATCH_DRAW: return "PKT3_DISPATCH_DRAW";
    case PM4Type3Opcode::PKT3_INDIRECT_BUFFER: return "PKT3_INDIRECT_BUFFER";
    case PM4Type3Opcode::PKT3_COPY_DATA: return "PKT3_COPY_DATA";
    case PM4Type3Opcode::PKT3_WRITE_DATA: return "PKT3_WRITE_DATA";
    case PM4Type3Opcode::PKT3_EVENT_WRITE: return "PKT3_EVENT_WRITE";
    case PM4Type3Opcode::PKT3_EVENT_WRITE_EOP: return "PKT3_EVENT_WRITE_EOP";
    case PM4Type3Opcode::PKT3_EVENT_WRITE_EOS: return "PKT3_EVENT_WRITE_EOS";
    case PM4Type3Opcode::PKT3_RELEASE_MEM: return "PKT3_RELEASE_MEM";
    case PM4Type3Opcode::PKT3_DMA_DATA: return "PKT3_DMA_DATA";
    case PM4Type3Opcode::PKT3_PFP_SYNC_ME: return "PKT3_PFP_SYNC_ME";
    case PM4Type3Opcode::PKT3_ACQUIRE_MEM: return "PKT3_ACQUIRE_MEM";
    case PM4Type3Opcode::PKT3_SET_CONTEXT_REG: return "PKT3_SET_CONTEXT_REG";
    case PM4Type3Opcode::PKT3_SET_CONTEXT_REG_INDIRECT: return "PKT3_SET_CONTEXT_REG_INDIRECT";
    case PM4Type3Opcode::PKT3_SET_SH_REG: return "PKT3_SET_SH_REG";
    case PM4Type3Opcode::PKT3_SET_SH_REG_INDIRECT: return "PKT3_SET_SH_REG_INDIRECT";
    case PM4Type3Opcode::PKT3_SET_UCONFIG_REG: return "PKT3_SET_UCONFIG_REG";
    case PM4Type3Opcode::PKT3_SET_UCONFIG_REG_INDEX: return "PKT3_SET_UCONFIG_REG_INDEX";
    case PM4Type3Opcode::PKT3_SET_UCONFIG_REG_INDIRECT: return "PKT3_SET_UCONFIG_REG_INDIRECT";
    }
    return "PKT3_UNKNOWN";
}

} // namespace PS5::GPU
