#include "gpu/pm4_translator.hpp"
#include "gpu/gpu_guest_memory.hpp"
#include "gpu/vulkan_compute_executor.hpp"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/renderer/sync.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

namespace PS5::GPU {
namespace {

PM4DecodeResult LegacyDecodeFailure(PM4DecodeError error, size_t consumed_dwords) {
    PM4DecodeResult result;
    result.error = error;
    result.consumed_dwords = consumed_dwords;
    return result;
}

PM4DecodeResult DecodeLegacyCommandRing(const uint32_t* stream, size_t dword_count) {
    if (dword_count == 0 || stream == nullptr) {
        return DecodePM4Type3Stream(stream, dword_count);
    }

    PM4DecodeResult result;
    size_t cursor = 0;
    while (cursor < dword_count) {
        const uint32_t header = stream[cursor];
        const uint32_t type = PM4Type3PacketType(header);

        if (type == 3u) {
            const size_t payload_dwords = PM4Type3PayloadDwords(header);
            const size_t available_payload_dwords = dword_count - cursor - 1u;
            if (payload_dwords > available_payload_dwords) {
                PM4DecodeError error;
                error.code = PM4DecodeErrorCode::TruncatedPacket;
                error.dword_offset = cursor;
                error.packet_index = result.packets.size();
                error.header = header;
                error.opcode = PM4Type3OpcodeValue(header);
                error.required_payload_dwords = payload_dwords;
                error.available_payload_dwords = available_payload_dwords;
                return LegacyDecodeFailure(error, cursor);
            }

            PM4DecodeResult packet_result =
                DecodePM4Type3Stream(stream + cursor, payload_dwords + 1u);
            if (!packet_result) {
                packet_result.error.dword_offset += cursor;
                packet_result.error.packet_index = result.packets.size();
                return LegacyDecodeFailure(packet_result.error, cursor);
            }

            PM4Type3Packet packet = packet_result.packets.front();
            packet.header_offset += cursor;
            packet.payload_offset += cursor;
            result.packets.push_back(packet);
            cursor += payload_dwords + 1u;
            continue;
        }

        if (type == 0u) {
            const size_t payload_dwords =
                static_cast<size_t>(((header >> 16u) & 0x3FFFu) + 1u);
            const size_t available_payload_dwords = dword_count - cursor - 1u;
            if (payload_dwords > available_payload_dwords) {
                PM4DecodeError error;
                error.code = PM4DecodeErrorCode::TruncatedPacket;
                error.dword_offset = cursor;
                error.packet_index = result.packets.size();
                error.header = header;
                error.required_payload_dwords = payload_dwords;
                error.available_payload_dwords = available_payload_dwords;
                return LegacyDecodeFailure(error, cursor);
            }
            cursor += payload_dwords + 1u;
            continue;
        }

        // The original adapter treated Type-1 and Type-2 words as one-dword packets.
        ++cursor;
    }

    result.consumed_dwords = cursor;
    return result;
}

} // namespace

PM4VulkanTranslator::PM4VulkanTranslator(VulkanRendererBackend& vulkan_backend)
    : m_vulkan(vulkan_backend) {}

void PM4VulkanTranslator::BindComputeExecutor(VulkanComputeExecutor* executor,
                                              GpuGuestMemory* memory) {
    m_executor = executor;
    m_guest_memory = memory;
    // Round 12: enable the honest GCN software fallback so draw/compute
    // programs still execute (real instruction semantics, hardware=false)
    // when no Vulkan device exists on the host.
    if (m_executor != nullptr) {
        m_executor->SetSoftwareFallback(true, memory);
    }
}

void PM4VulkanTranslator::TranslateAndExecuteCommandRing(
    const uint32_t* ring_buffer, size_t dwords_count) {
    const PM4TranslationResult result =
        TranslateAndExecuteCommandRingImpl(ring_buffer, dwords_count);
    if (!result) {
        std::cerr << "[PM4 Translator] " << PM4DecodeErrorName(result.error.code)
                  << " at dword " << result.error.dword_offset
                  << " (packet " << result.error.packet_index
                  << ", header 0x" << std::hex << result.error.header << std::dec << ")\n";
    }
}

PM4TranslationResult PM4VulkanTranslator::TranslateAndExecuteCommandRingChecked(
    const uint32_t* ring_buffer, size_t dwords_count) {
    return TranslateAndExecuteCommandRingImpl(ring_buffer, dwords_count);
}

PM4TranslationResult PM4VulkanTranslator::TranslateAndExecuteCommandRingImpl(
    const uint32_t* ring_buffer, size_t dwords_count) {
    PM4TranslationResult translation;
    const PM4DecodeResult decoded = DecodeLegacyCommandRing(ring_buffer, dwords_count);
    translation.error = decoded.error;
    translation.consumed_dwords = decoded.consumed_dwords;
    m_last_error = decoded.error;

    if (!decoded) {
        return translation;
    }

    // The decoder has validated every packet, so execution cannot partially apply
    // a stream that later turns out to be malformed.
    for (const PM4Type3Packet& packet : decoded.packets) {
        ProcessPacketType3(packet.opcode, ring_buffer + packet.payload_offset,
                           packet.payload_dwords);
        ++m_packet_count;
        ++translation.executed_packets;
    }

    return translation;
}

// Round 19: parse the guest's self-describing buffer-resource table (the ABI
// is documented in graphics/guest_gpu/pm4.h). Fail-closed: any malformed
// entry rejects the WHOLE table -- the dispatch then proceeds without
// resources and MUBUF/SMEM fail closed exactly like round 18.
bool PM4VulkanTranslator::ParseResourceTable(uint64_t table_gva,
                                             GcnDispatchResources& out,
                                             std::string& error) {
    out = GcnDispatchResources{};
    error.clear();
    if (m_guest_memory == nullptr || table_gva == 0) {
        error = "no guest memory bridge";
        return false;
    }
    // Header: count, mirror base lo/hi, mirror dwords.
    uint32_t header[4] = {};
    if (!m_guest_memory->ReadDwords(table_gva, header, 4u)) {
        error = "resource table header unreadable";
        return false;
    }
    const uint32_t count = header[0];
    if (count == 0 || count > Pm4::GCN_MAX_BUFFER_RESOURCES) {
        error = "resource table buffer count out of range (1.." +
                std::to_string(Pm4::GCN_MAX_BUFFER_RESOURCES) + ")";
        return false;
    }
    const uint64_t mirror_base =
        (static_cast<uint64_t>(header[2]) << 32) | header[1];
    const uint32_t mirror_dwords = header[3];
    if (mirror_base == 0 && mirror_dwords != 0) {
        error = "resource table mirror size without a base";
        return false;
    }
    if (mirror_base != 0 &&
        (mirror_dwords == 0 || mirror_dwords > Pm4::GCN_MAX_MIRROR_DWORDS)) {
        error = "resource table mirror size out of range (1.." +
                std::to_string(Pm4::GCN_MAX_MIRROR_DWORDS) + ")";
        return false;
    }
    // Entries: 4 dwords each (base lo/hi, size dwords, stride dwords).
    std::vector<uint32_t> entries(count * 4u, 0u);
    if (!m_guest_memory->ReadDwords(table_gva + 16, entries.data(),
                                    entries.size())) {
        error = "resource table entries unreadable";
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t* e = entries.data() + i * 4u;
        GcnBufferResource buf{};
        buf.base_gva = (static_cast<uint64_t>(e[1]) << 32) | e[0];
        buf.size_dwords = e[2];
        buf.stride = e[3];
        if (buf.base_gva == 0 || buf.size_dwords == 0 ||
            buf.size_dwords > Pm4::GCN_MAX_BUFFER_DWORDS || buf.stride == 0) {
            error = "resource table entry " + std::to_string(i) +
                    " malformed (base/size/stride)";
            return false;
        }
        out.buffers.push_back(buf);
    }
    out.scalar_mirror_base_gva = mirror_base;
    out.scalar_mirror_dwords = mirror_dwords;

    // Round 28: optional image-extension header (dwords 4..7). A magic guards
    // old tables, which simply end after the buffer entries: anything without
    // the magic parses with zero images, byte-identically to round 19.
    uint32_t ext_header[4] = {};
    if (m_guest_memory->ReadDwords(table_gva + 16, ext_header, 4u)) {
        if (ext_header[1] == Pm4::GCN_RESOURCE_TABLE_IMAGE_MAGIC) {
            const uint32_t image_count = ext_header[0];
            if (image_count == 0 || image_count > Pm4::GCN_MAX_IMAGE_RESOURCES) {
                error = "resource table image count out of range (1.." +
                        std::to_string(Pm4::GCN_MAX_IMAGE_RESOURCES) + ")";
                return false;
            }
            const uint64_t entries_gva =
                table_gva + 16 + static_cast<uint64_t>(count) * 16u;
            std::vector<uint32_t> img_entries(image_count * 6u, 0u);
            if (!m_guest_memory->ReadDwords(entries_gva, img_entries.data(),
                                            img_entries.size())) {
                error = "resource table image entries unreadable";
                return false;
            }
            for (uint32_t i = 0; i < image_count; ++i) {
                const uint32_t* e = img_entries.data() + i * 6u;
                GcnImageResource img{};
                img.base_gva = (static_cast<uint64_t>(e[1]) << 32) | e[0];
                img.width  = e[2];
                img.height = e[3];
                img.mips   = e[4];
                if (img.base_gva == 0 || img.width == 0 || img.height == 0 ||
                    img.width > Pm4::GCN_MAX_IMAGE_DIM ||
                    img.height > Pm4::GCN_MAX_IMAGE_DIM ||
                    img.mips == 0 || img.mips > Pm4::GCN_MAX_IMAGE_MIPS) {
                    error = "resource table image entry " + std::to_string(i) +
                            " malformed (base/dims/mips)";
                    return false;
                }
                out.images.push_back(img);
            }
        }
    }
    return true;
}

void PM4VulkanTranslator::CollectResourceTable(
        const uint32_t lo_reg, const uint32_t hi_reg,
        GcnDispatchResources& resources, bool& programmed, bool& parsed,
        std::string& error) {
    resources = GcnDispatchResources{};
    programmed = false;
    parsed = false;
    error.clear();
    const auto it_lo = m_sh_regs.find(lo_reg);
    const auto it_hi = m_sh_regs.find(hi_reg);
    if (it_lo == m_sh_regs.end() && it_hi == m_sh_regs.end()) return;
    const uint64_t table_gva =
        (static_cast<uint64_t>(it_hi != m_sh_regs.end() ? it_hi->second : 0u)
         << 32) |
        (it_lo != m_sh_regs.end() ? it_lo->second : 0u);
    if (table_gva == 0) return;   // slot programmed to zero = no table
    programmed = true;
    parsed = ParseResourceTable(table_gva, resources, error);
    if (!parsed) {
        std::cerr << "[PM4 Translator] resource table rejected: " << error
                  << " (proceeding without resources)\n";
        resources = GcnDispatchResources{};
    }
}

// Round 19 (phase 2): the pure register->graphics-target conversion.
bool PM4VulkanTranslator::BuildGraphicsTarget(
        const RenderTargetBinding& binding, const RasterViewport& viewport,
        VulkanComputeExecutor::GraphicsTargetDesc& out) {
    out = VulkanComputeExecutor::GraphicsTargetDesc{};
    // Only the register-derived binding serves the graphics path: a
    // host-API target has no CB_COLOR0_INFO to convert from.
    if (!binding.programmed || !binding.bound || !binding.error.empty()) {
        return false;
    }
    if (!binding.color_write) return false;   // depth-only: deliberately next
    if (binding.color_format == Pm4::GuestColorFormat::Invalid) return false;
    if (binding.width == 0 || binding.height == 0) return false;
    out.width = binding.width;
    out.height = binding.height;
    out.color_format = binding.color_format;
    out.color_write = binding.color_write;
    out.depth_enabled = binding.depth_bound;
    out.depth_write = binding.depth_write;
    out.zfunc = binding.zfunc;
    // PA_CL_VPORT numbers (pixels): screen = ndc * scale + offset. The
    // executor derives the VkViewport from the same pair.
    out.vport_scale_x = viewport.scale_x;
    out.vport_off_x = viewport.scale_x_offset;
    out.vport_scale_y = viewport.scale_y;
    out.vport_off_y = viewport.scale_y_offset;
    return viewport.scale_x != 0.0f && viewport.scale_y != 0.0f;
}

bool PM4VulkanTranslator::TryRealComputeDispatch(uint32_t groups_x,
                                                 uint32_t groups_y,
                                                 uint32_t groups_z) {
    m_last_dispatch = ComputeDispatchRecord{};
    m_last_dispatch.groups_x = groups_x;

    if (m_executor == nullptr || m_guest_memory == nullptr) {
        return false; // real path not bound -> legacy DispatchCompute()
    }

    // Assemble the compute-shader GVA and SSBO descriptors from the recorded
    // SH registers (see graphics/guest_gpu/pm4.h for the ProsperoLayer ABI).
    const auto reg = [&](uint32_t off, uint32_t fallback = 0u) -> uint32_t {
        auto it = m_sh_regs.find(off);
        return it != m_sh_regs.end() ? it->second : fallback;
    };

    const uint64_t pgm_lo = reg(Pm4::COMPUTE_PGM_LO);
    const uint64_t pgm_hi = reg(Pm4::COMPUTE_PGM_HI);
    // GCN convention: PGM_LO is 256-byte aligned (stored >> 8).
    const uint64_t shader_gva = (pgm_hi << 32) | (pgm_lo << 8);

    const uint64_t in_lo  = reg(Pm4::COMPUTE_USER_DATA_0 + 0);
    const uint64_t in_hi  = reg(Pm4::COMPUTE_USER_DATA_0 + 1);
    const uint64_t out_lo = reg(Pm4::COMPUTE_USER_DATA_0 + 2);
    const uint64_t out_hi = reg(Pm4::COMPUTE_USER_DATA_0 + 3);
    const uint32_t count  = reg(Pm4::COMPUTE_USER_DATA_0 + 4);
    const uint64_t input_gva  = (in_hi << 32) | in_lo;
    const uint64_t output_gva = (out_hi << 32) | out_lo;

    m_last_dispatch.attempted = true;
    m_last_dispatch.shader_gva = shader_gva;
    m_last_dispatch.input_gva = input_gva;
    m_last_dispatch.output_gva = output_gva;
    m_last_dispatch.element_count = count;

    // Round 19: the buffer-resource table (COMPUTE_USER_DATA_0 +5..6). When
    // programmed AND well-formed, MUBUF/SMEM operations run on the real
    // device through per-descriptor SSBOs; a malformed table drops the
    // resources (fail-closed -- round-18 behaviour) and the memory ops
    // fail honestly from there.
    GcnDispatchResources resources;
    CollectResourceTable(Pm4::COMPUTE_USER_DATA_RESOURCE_LO,
                         Pm4::COMPUTE_USER_DATA_RESOURCE_HI, resources,
                         m_last_dispatch.resources_programmed,
                         m_last_dispatch.resources_parsed,
                         m_last_dispatch.resource_error);
    m_last_dispatch.resource_buffer_count =
        static_cast<uint32_t>(resources.buffers.size());
    m_last_dispatch.resource_mirror_gva = resources.scalar_mirror_base_gva;
    m_last_dispatch.resource_mirror_dwords = resources.scalar_mirror_dwords;

    if (shader_gva == 0 || input_gva == 0 || output_gva == 0 || count == 0) {
        // Descriptors incomplete -> fall back to the legacy path.
        m_last_dispatch.attempted = false;
        return false;
    }

    // Read the RDNA2 shader from guest memory. The shader length is not encoded
    // in the packet; read a bounded window and let the compiler stop at
    // S_ENDPGM. 256 dwords is ample for the kernels this path supports.
    constexpr size_t kMaxShaderDwords = 256;
    std::vector<uint32_t> shader(kMaxShaderDwords, 0u);
    if (!m_guest_memory->ReadDwords(shader_gva, shader.data(), kMaxShaderDwords)) {
        return false;
    }
    // Trim to and including the first S_ENDPGM so the compiler sees a clean end.
    size_t shader_len = 0;
    for (size_t i = 0; i < shader.size(); ++i) {
        ++shader_len;
        // S_ENDPGM encoding used by the compute compiler (SOPP S_ENDPGM=1).
        // Round 13 fix: S_ENDPGM terminates the program for ANY simm16
        // value (hardware semantics); the old check only matched simm=1.
        if ((shader[i] & 0xFFFF0000u) == 0xBF810000u) break;
    }

    // Read the input SSBO.
    std::vector<uint32_t> input(count, 0u);
    if (!m_guest_memory->ReadDwords(input_gva, input.data(), count)) {
        return false;
    }

    // Execute on the real device. Round 19: a parsed resource table routes
    // the MUBUF/SMEM memory ops through the same dispatch (per-descriptor
    // SSBOs + the push-constant mirror base); otherwise the round-18 call.
    auto result = m_last_dispatch.resources_parsed
        ? m_executor->RunRDNA2WithResources(shader.data(), shader_len, input,
                                            resources, m_guest_memory)
        : m_executor->RunRDNA2(shader.data(), shader_len, input);
    if (!result) {
        std::cerr << "[PM4 Translator] real compute dispatch failed: "
                  << result.message << "\n";
        return false;
    }

    // Write the output SSBO back to guest memory.
    if (!m_guest_memory->WriteDwords(output_gva, result.output.data(),
                                     result.output.size())) {
        return false;
    }

    m_last_dispatch.executed_on_gpu = result.hardware;
    // Keep the backend dispatch counter meaningful for observers.
    m_vulkan.DispatchCompute(groups_x, groups_y, groups_z);
    (void)groups_y;
    (void)groups_z;
    return true;
}

bool PM4VulkanTranslator::TryRealDrawDispatch(bool indexed, uint32_t element_count) {
    m_last_draw = DrawDispatchRecord{};
    m_last_draw.indexed = indexed;
    m_last_draw.index_base_gva = m_index_base_gva;
    m_last_draw.index_type = m_index_type;
    m_last_draw.element_count = element_count;
    m_last_draw.instance_count = m_num_instances;
    m_last_draw.first_index = m_first_index;
    m_last_draw.base_vertex = m_base_vertex;
    m_last_draw.first_vertex = m_first_vertex;
    m_last_draw.first_instance = m_first_instance;

    if (m_executor == nullptr || m_guest_memory == nullptr) {
        return false; // real path not bound -> legacy DrawAuto()
    }

    // Round 18: the REAL render-target binding. Whatever the guest
    // programmed into the CB/DB/PA context registers decides the raster
    // stage's target + viewport (replacing the round-13 host-API coupling).
    AssembleRenderTargetFromRegisters();

    // Assemble the vertex-stage GVA and buffer descriptors from the recorded
    // SH registers (see graphics/guest_gpu/pm4.h for the ProsperoLayer ABI).
    const auto reg = [&](uint32_t off, uint32_t fallback = 0u) -> uint32_t {
        auto it = m_sh_regs.find(off);
        return it != m_sh_regs.end() ? it->second : fallback;
    };

    // Same GCN convention as the compute program: PGM_LO is 256-byte aligned
    // (stored >> 8).
    const uint64_t pgm_lo = reg(Pm4::SPI_SHADER_PGM_LO_VS);
    const uint64_t pgm_hi = reg(Pm4::SPI_SHADER_PGM_HI_VS);
    const uint64_t shader_gva = (pgm_hi << 32) | (pgm_lo << 8);

    const uint64_t in_lo  = reg(Pm4::SPI_SHADER_USER_DATA_VS_0 + 0);
    const uint64_t in_hi  = reg(Pm4::SPI_SHADER_USER_DATA_VS_0 + 1);
    const uint64_t out_lo = reg(Pm4::SPI_SHADER_USER_DATA_VS_0 + 2);
    const uint64_t out_hi = reg(Pm4::SPI_SHADER_USER_DATA_VS_0 + 3);
    const uint64_t input_gva  = (in_hi << 32) | in_lo;
    const uint64_t output_gva = (out_hi << 32) | out_lo;

    // Round 10 VGT attribute fetch: the fetch-descriptor table GVA (VS user
    // data +5..6) and the transformed-vertex output size (+7). Zero table GVA
    // keeps the round-9 single-stream lane model exactly as before.
    const uint64_t fetch_lo = reg(Pm4::SPI_SHADER_USER_DATA_VS_FETCH_LO);
    const uint64_t fetch_hi = reg(Pm4::SPI_SHADER_USER_DATA_VS_FETCH_HI);
    const uint64_t fetch_table_gva = (fetch_hi << 32) | fetch_lo;
    const uint32_t out_dwords = reg(Pm4::SPI_SHADER_USER_DATA_VS_OUT_DWORDS, 1u);

    struct FetchAttr {
        uint64_t gva{0};
        uint32_t stride{0};   // dwords between consecutive vertices
        uint32_t size{0};     // dwords per attribute
    };
    FetchAttr attrs[Pm4::VGT_MAX_ATTRIBUTES];
    uint32_t attr_count = 0;
    uint32_t k_in = 1;   // fetched dwords per lane (single-stream default)
    GcnDispatchResources resources;   // round 19: MUBUF/SMEM tables (VS +8..9)

    if (fetch_table_gva != 0) {
        // Read + validate the self-describing descriptor table (see pm4.h).
        uint32_t header[1 + Pm4::VGT_MAX_ATTRIBUTES * 4] = {};
        if (!m_guest_memory->ReadDwords(fetch_table_gva, header, 1u)) {
            return false;  // unreadable table -> legacy fallback
        }
        const uint32_t n = header[0];
        if (n == 0 || n > Pm4::VGT_MAX_ATTRIBUTES) {
            m_last_draw.attempted = false;  // malformed -> descriptors incomplete
            return false;
        }
        if (!m_guest_memory->ReadDwords(fetch_table_gva + 4, header + 1, n * 4u)) {
            return false;
        }
        uint32_t total = 0;
        for (uint32_t a = 0; a < n; ++a) {
            const uint32_t* e = header + 1 + a * 4;
            const uint64_t gva = (static_cast<uint64_t>(e[1]) << 32) | e[0];
            const uint32_t stride = e[2];
            const uint32_t size = e[3];
            if (gva == 0 || stride == 0 || size == 0 ||
                size > Pm4::VGT_MAX_ATTRIBUTE_DWORDS) {
                m_last_draw.attempted = false;  // malformed entry
                return false;
            }
            attrs[a] = {gva, stride, size};
            total += size;
        }
        if (total == 0) {
            m_last_draw.attempted = false;
            return false;
        }
        attr_count = n;
        k_in = total;
        m_last_draw.fetch_enabled = true;
        m_last_draw.fetch_table_gva = fetch_table_gva;
        m_last_draw.attribute_count = n;
        m_last_draw.in_dwords_per_lane = k_in;
    }

    m_last_draw.attempted = true;
    m_last_draw.shader_gva = shader_gva;
    m_last_draw.input_gva = input_gva;
    m_last_draw.output_gva = output_gva;
    m_last_draw.out_dwords_per_lane = fetch_table_gva != 0 ? out_dwords : 1u;
    const uint32_t m_out = m_last_draw.out_dwords_per_lane;

    // Round 19: the vertex stage's buffer-resource table (VS user data +8..9)
    // -- the same MUBUF/SMEM plumbing as the compute path.
    CollectResourceTable(Pm4::SPI_SHADER_USER_DATA_VS_RESOURCE_LO,
                         Pm4::SPI_SHADER_USER_DATA_VS_RESOURCE_HI, resources,
                         m_last_draw.resources_programmed,
                         m_last_draw.resources_parsed,
                         m_last_draw.resource_error);
    m_last_draw.resource_buffer_count =
        static_cast<uint32_t>(resources.buffers.size());
    m_last_draw.resource_mirror_gva = resources.scalar_mirror_base_gva;
    m_last_draw.resource_mirror_dwords = resources.scalar_mirror_dwords;

    if (shader_gva == 0 || output_gva == 0 || element_count == 0 ||
        m_out == 0 || m_out > 16u) {
        // Descriptors incomplete -> fall back to the legacy path.
        m_last_draw.attempted = false;
        return false;
    }

    // Read the RDNA2 vertex-stage program from guest memory (same bounded
    // window + S_ENDPGM trim discipline as the compute path).
    constexpr size_t kMaxShaderDwords = 256;
    std::vector<uint32_t> shader(kMaxShaderDwords, 0u);
    if (!m_guest_memory->ReadDwords(shader_gva, shader.data(), kMaxShaderDwords)) {
        return false;
    }
    size_t shader_len = 0;
    for (size_t i = 0; i < shader.size(); ++i) {
        ++shader_len;
        // Round 13 fix: S_ENDPGM terminates the program for ANY simm16
        // value (hardware semantics); the old check only matched simm=1.
        if ((shader[i] & 0xFFFF0000u) == 0xBF810000u) break; // S_ENDPGM
    }

    // Build the per-lane input stream.
    //   Round-9 model (no fetch table): one dword per lane -- indexed draws
    //   decode the index buffer (u16 zero-extended or u32 per INDEX_TYPE);
    //   non-indexed draws pass the guest's attribute buffer through.
    //   Round-10 VGT model: per lane, gather every attribute's dwords at
    //   attr_gva + vertex_id * stride and concatenate them lane-major.
    std::vector<uint32_t> input;
    std::vector<uint32_t> indices;   // decoded once, reused per lane
    if (indexed) {
        if (m_index_base_gva == 0) {
            m_last_draw.attempted = false;
            return false;
        }
        if (m_index_buffer_dwords != 0) {
            const uint64_t index_bytes = static_cast<uint64_t>(m_index_buffer_dwords) * 4u;
            const uint64_t start_bytes = static_cast<uint64_t>(m_first_index) *
                                         (m_index_type == Pm4::INDEX_TYPE_U32 ? 4u : 2u);
            const uint64_t need_bytes = static_cast<uint64_t>(element_count) *
                                        (m_index_type == Pm4::INDEX_TYPE_U32 ? 4u : 2u);
            if (start_bytes > index_bytes || need_bytes > index_bytes - start_bytes) {
                m_last_draw.attempted = false;
                return false;
            }
        }
        if (m_index_type == Pm4::INDEX_TYPE_U32) {
            indices.resize(element_count, 0u);
            const uint64_t index_addr = m_index_base_gva + static_cast<uint64_t>(m_first_index) * 4u;
            if (!m_guest_memory->ReadDwords(index_addr, indices.data(),
                                            element_count)) {
                return false;
            }
        } else {
            // u16: two indices per dword; the trailing half-word of an odd
            // count is padding and never observed.
            const uint64_t index_addr = m_index_base_gva + static_cast<uint64_t>(m_first_index) * 2u;
            const size_t packed_dwords = (element_count + 1u + ((index_addr & 2u) != 0 ? 1u : 0u)) / 2u;
            std::vector<uint32_t> packed(packed_dwords, 0u);
            if (!m_guest_memory->ReadDwords(index_addr & ~3ull, packed.data(), packed_dwords)) {
                return false;
            }
            indices.reserve(element_count);
            const bool halfword_offset = (index_addr & 2u) != 0;
            for (uint32_t i = 0; i < element_count; ++i) {
                const uint32_t half = i + (halfword_offset ? 1u : 0u);
                const uint32_t word = packed[half >> 1u];
                indices.push_back((half & 1u) != 0u ? (word >> 16u) : (word & 0xffffu));
            }
        }
    }

    if (fetch_table_gva != 0) {
        // VGT gather: lane-major concatenation of every attribute's dwords.
        input.reserve(static_cast<size_t>(element_count) * k_in);
        std::vector<uint32_t> scratch;
        for (uint32_t i = 0; i < element_count; ++i) {
            const uint32_t vertex_id = indexed
                ? static_cast<uint32_t>(static_cast<int64_t>(indices[i]) + m_base_vertex)
                : (m_first_vertex + i);
            for (uint32_t a = 0; a < attr_count; ++a) {
                const FetchAttr& attr = attrs[a];
                scratch.assign(attr.size, 0u);
                // Round 13 fix (latent round-10 defect): the descriptor stride
                // is in DWORDS (see the pm4.h ABI), so the byte address steps
                // by stride*4. The old code added the dword count directly to
                // the byte GVA, corrupting every gather past vertex 0.
                const uint64_t attr_addr =
                    attr.gva + static_cast<uint64_t>(vertex_id) * attr.stride * 4u;
                if (!m_guest_memory->ReadDwords(attr_addr, scratch.data(), attr.size)) {
                    return false;  // attribute buffer out of range -> legacy
                }
                input.insert(input.end(), scratch.begin(), scratch.end());
            }
        }
    } else if (indexed) {
        input.resize(indices.size());
        for (size_t i = 0; i < indices.size(); ++i) {
            input[i] = static_cast<uint32_t>(static_cast<int64_t>(indices[i]) + m_base_vertex);
        }
    } else {
        if (input_gva == 0) {
            m_last_draw.attempted = false;
            return false;
        }
        input.resize(element_count, 0u);
        const uint64_t vertex_input_gva = input_gva + static_cast<uint64_t>(m_first_vertex) * 4u;
        if (!m_guest_memory->ReadDwords(vertex_input_gva, input.data(), element_count)) {
            return false;
        }
    }

    // Round 19 (phase 2): the REAL VkGraphicsPipeline raster path -- opt-in.
    // When enabled AND the register-derived binding converts, the guest VS
    // runs as the pipeline's VERTEX stage (gl_VertexIndex lane model) with a
    // passthrough fragment shader into a CB_COLOR0-derived VkImage; the
    // pixels + depth land back in guest memory and the transformed vertices
    // go to the output buffer (the round-9 ABI). Any missing piece declines
    // (executed=false) and the compute + software-raster path runs instead --
    // exactly the round-18 behaviour.
    if (m_graphics_raster && m_last_rt_binding.bound &&
        m_last_rt_binding.error.empty()) {
        VulkanComputeExecutor::GraphicsTargetDesc target;
        if (BuildGraphicsTarget(m_last_rt_binding, m_raster_viewport, target)) {
            m_last_draw.graphics_raster_attempted = true;
            auto gfx = m_executor->DrawVerticesToGuest(
                shader.data(), shader_len, input, k_in, m_out, element_count,
                resources, target, m_raster_target.color_gva,
                m_raster_target.depth_gva, m_guest_memory);
            if (gfx.executed) {
                m_last_draw.graphics_raster_executed = true;
                m_last_draw.graphics_raster_note = gfx.message;
                // The transformed-vertex dump (round-9 draw ABI).
                if (!m_guest_memory->WriteDwords(
                        output_gva, gfx.transformed_vertices.data(),
                        gfx.transformed_vertices.size())) {
                    return false;
                }
                m_last_draw.executed_on_gpu = true;
                m_last_raster = SoftwareRasterStats{};
                m_vulkan.DrawAuto(element_count, m_num_instances);
                return true;
            }
            m_last_draw.graphics_raster_note =
                "declined: " + gfx.message + " (software rasterizer instead)";
        } else {
            m_last_draw.graphics_raster_note =
                "declined: binding does not convert to a graphics target";
        }
    }

    // Execute the vertex stage on the real device (one lane per element).
    // Strided lane geometry only when the VGT gather is enabled; otherwise the
    // exact round-9 dispatch. Round 19: a parsed resource table routes the
    // VS's MUBUF/SMEM memory ops through per-descriptor SSBOs.
    const bool use_resources = m_last_draw.resources_parsed;
    auto result = use_resources
        ? m_executor->RunRDNA2WithResources(shader.data(), shader_len, input,
                                            resources, m_guest_memory,
                                            k_in, m_out)
        : (fetch_table_gva != 0
               ? m_executor->RunRDNA2Strided(shader.data(), shader_len,
                                             input, k_in, m_out)
               : m_executor->RunRDNA2(shader.data(), shader_len, input));
    if (!result) {
        std::cerr << "[PM4 Translator] real draw dispatch failed: "
                  << result.message << "\n";
        return false;
    }

    // Write the transformed vertices back to guest memory (lane-major:
    // lane i occupies [i*m, i*m+m) in the VGT model; one dword per lane
    // otherwise).
    if (!m_guest_memory->WriteDwords(output_gva, result.output.data(),
                                     result.output.size())) {
        return false;
    }

    // Round 13/18: software raster stage. When a target is active (the
    // PM4-derived one when CB_COLOR0_BASE was programmed, else the host-API
    // binding), the SAME transformed vertices feed the headless pixel stage
    // (full frustum clipping, PA_CL_VPORT transform, ZFUNC depth test)
    // writing the guest RGBA8 colour plane. Runs on every real-path draw --
    // GPU or GCN software interpreter -- because the vertex data is
    // identical.
    if (!m_last_rt_binding.error.empty() && m_last_rt_binding.programmed) {
        // Malformed guest binding: fail the RASTER stage closed (the vertex
        // stage above already executed, exactly like hardware keeps going
        // with a broken CB binding).
        m_last_raster = SoftwareRasterStats{};
        m_last_raster.error = m_last_rt_binding.error;
        std::cerr << "[PM4 Translator] render-target binding rejected: "
                  << m_last_rt_binding.error << "\n";
    } else if (m_raster_target.width != 0 && m_raster_target.color_gva != 0) {
        m_last_raster = SoftwareRasterizer::DrawTriangles(
            result.output,
            fetch_table_gva != 0 ? m_out : 1u,
            m_raster_target, m_raster_viewport, m_guest_memory,
            m_last_rt_binding.bound ? m_last_rt_binding.depth_write : true);
        if (!m_last_raster.ok) {
            std::cerr << "[PM4 Translator] software raster failed: "
                      << m_last_raster.error << "\n";
        }
    } else {
        m_last_raster = SoftwareRasterStats{};
    }

    m_last_draw.executed_on_gpu = result.hardware;
    // Keep the backend draw counter meaningful for observers (the counter
    // advances exactly once per draw, whether real or legacy).
    m_vulkan.DrawAuto(element_count, m_num_instances);
    return true;
}

void PM4VulkanTranslator::AssembleRenderTargetFromRegisters() {
    m_last_rt_binding = RenderTargetBinding{};

    const auto reg = [&](uint32_t off, uint32_t fallback = 0u) -> uint32_t {
        auto it = m_context_regs.find(off);
        return it != m_context_regs.end() ? it->second : fallback;
    };
    const auto reg_programmed = [&](uint32_t off) -> bool {
        return m_context_regs.find(off) != m_context_regs.end();
    };

    // --- viewport first (needed by both binding sources) -------------------
    // REAL PA_CL_VPORT_* registers (float bits). All four must be programmed
    // together (a driver writes them as one block); anything else keeps the
    // host viewport / the fullscreen default.
    RasterViewport vp = m_host_raster_viewport;
    if (reg_programmed(Pm4::PA_CL_VPORT_XSCALE)) {
        if (!reg_programmed(Pm4::PA_CL_VPORT_XOFFSET) ||
            !reg_programmed(Pm4::PA_CL_VPORT_YSCALE) ||
            !reg_programmed(Pm4::PA_CL_VPORT_YOFFSET)) {
            m_last_rt_binding.error =
                "partial PA_CL_VPORT_* programming (all four required)";
            m_last_rt_binding.programmed = reg_programmed(Pm4::CB_COLOR0_BASE);
            return;
        }
        auto bits = [](uint32_t raw) { float f; std::memcpy(&f, &raw, 4); return f; };
        vp.scale_x = bits(reg(Pm4::PA_CL_VPORT_XSCALE));
        vp.scale_x_offset = bits(reg(Pm4::PA_CL_VPORT_XOFFSET));
        vp.scale_y = bits(reg(Pm4::PA_CL_VPORT_YSCALE));
        vp.scale_y_offset = bits(reg(Pm4::PA_CL_VPORT_YOFFSET));
        m_last_rt_binding.viewport_from_registers = true;
    }

    // --- colour target: the REAL CB_COLOR0_* registers ---------------------
    if (!reg_programmed(Pm4::CB_COLOR0_BASE)) {
        // No PM4 binding: keep the host-API target (round-13 back-compat).
        m_raster_target = m_host_raster_target;
        if (vp.IsDegenerate() && m_raster_target.width != 0) {
            vp = RasterViewport::Fullscreen(m_raster_target.width,
                                            m_raster_target.height);
        }
        m_raster_viewport = vp;
        return;
    }

    m_last_rt_binding.programmed = true;

    // Format gate (round 20): CB_COLOR0_INFO names the target's layout.
    // FORMAT[6:2] is the SQIMG DataFormat, NUMBER_TYPE[10:8] the number
    // format, COMP_SWAP[12:11] the channel swap -- the SAME fields shadPS4
    // decodes (refs/regs_color.h Color0Info). The conversion table (pm4.h)
    // accepts the ten formats the model implements; anything else fails
    // closed with the raw values in the error string so a guest dump can be
    // diagnosed.
    const uint32_t info = reg(Pm4::CB_COLOR0_INFO);
    const uint32_t format = (info & Pm4::CB_INFO_FORMAT_MASK) >> Pm4::CB_INFO_FORMAT_SHIFT;
    const uint32_t number = (info & Pm4::CB_INFO_NUMBER_MASK) >> Pm4::CB_INFO_NUMBER_SHIFT;
    const uint32_t comp_swap =
        (info & Pm4::CB_INFO_COMP_SWAP_MASK) >> Pm4::CB_INFO_COMP_SWAP_SHIFT;
    if (!reg_programmed(Pm4::CB_COLOR0_INFO)) {
        m_last_rt_binding.error = "CB_COLOR0_INFO not programmed (format unknown)";
        return;
    }
    Pm4::GuestColorFormat guest_format = Pm4::GuestColorFormat::Invalid;
    if (!Pm4::CbInfoToGuestColorFormat(format, number, guest_format, comp_swap)) {
        std::ostringstream os;
        os << "unsupported CB_COLOR0_INFO format 0x" << std::hex << format
           << " number_type 0x" << number << " comp_swap 0x" << comp_swap
           << " (see pm4.h CbInfoToGuestColorFormat for the accepted set)";
        m_last_rt_binding.error = os.str();
        return;
    }
    m_last_rt_binding.color_format = guest_format;

    // Geometry from the linear-mode tile-count registers (see pm4.h):
    //   pitch_px  = (CB_COLOR0_PITCH.TILE_MAX + 1) * 8
    //   pixels    = (CB_COLOR0_SLICE.TILE_MAX + 1) * 64
    //   height    = pixels / pitch_px   (must divide exactly)
    const uint32_t pitch =
        ((reg(Pm4::CB_COLOR0_PITCH) & Pm4::CB_PITCH_TILE_MAX_MASK) + 1u) *
        Pm4::CB_LINEAR_TILE_PIXELS;
    const uint64_t pixels =
        static_cast<uint64_t>(
            (reg(Pm4::CB_COLOR0_SLICE) & Pm4::CB_SLICE_TILE_MAX_MASK) + 1u) *
        Pm4::CB_LINEAR_TILE_AREA;
    if (pitch == 0 || pixels == 0 || pixels % pitch != 0 ||
        pixels / pitch > 16384u) {
        m_last_rt_binding.error =
            "malformed CB_COLOR0_PITCH/SLICE geometry (linear tile math)";
        return;
    }
    const uint32_t height = static_cast<uint32_t>(pixels / pitch);
    if (pitch == 0 || pitch > 16384u || height == 0) {
        m_last_rt_binding.error = "degenerate CB target extent";
        return;
    }

    RasterTarget target{};
    // 256-byte alignment convention: low 8 bits of BASE are reserved.
    target.color_gva =
        (static_cast<uint64_t>(reg(Pm4::CB_COLOR0_BASE_EXT)) << 32) |
        (reg(Pm4::CB_COLOR0_BASE) & ~0xFFu);
    target.width = pitch;
    target.height = height;
    target.color_format = guest_format;   // round 20: the converted layout
    target.color_write =
        (reg(Pm4::CB_TARGET_MASK, 0xFu) & Pm4::CB_TARGET0_ENABLE_MASK) != 0;

    // REAL PA_SC_SCREEN_SCISSOR rectangle. TL/BR pack 16-bit x/y values;
    // BR is an exclusive edge in the guest ABI. A malformed/inverted
    // rectangle is rejected rather than silently widening the draw.
    const bool have_scissor_tl = reg_programmed(Pm4::PA_SC_SCREEN_SCISSOR_TL);
    const bool have_scissor_br = reg_programmed(Pm4::PA_SC_SCREEN_SCISSOR_BR);
    if (have_scissor_tl || have_scissor_br) {
        if (!have_scissor_tl || !have_scissor_br) {
            m_last_rt_binding.error =
                "partial PA_SC_SCREEN_SCISSOR programming (TL and BR required)";
            return;
        }
        const uint32_t tl = reg(Pm4::PA_SC_SCREEN_SCISSOR_TL);
        const uint32_t br = reg(Pm4::PA_SC_SCREEN_SCISSOR_BR);
        const uint32_t left = tl & 0xFFFFu;
        const uint32_t top = (tl >> 16u) & 0xFFFFu;
        const uint32_t right = br & 0xFFFFu;
        const uint32_t bottom = (br >> 16u) & 0xFFFFu;
        if (right <= left || bottom <= top) {
            m_last_rt_binding.error =
                "malformed PA_SC_SCREEN_SCISSOR rectangle";
            return;
        }
        target.scissor_enable = true;
        target.scissor_left = left;
        target.scissor_top = top;
        target.scissor_right = right;
        target.scissor_bottom = bottom;
        m_last_rt_binding.scissor_from_registers = true;
        m_last_rt_binding.scissor_left = left;
        m_last_rt_binding.scissor_top = top;
        m_last_rt_binding.scissor_right = right;
        m_last_rt_binding.scissor_bottom = bottom;
    }

    // --- depth target: DB_Z_WRITE_BASE + DB_DEPTH_CONTROL ------------------
    const uint32_t db_control = reg(Pm4::DB_DEPTH_CONTROL);
    const bool z_enable = (db_control & Pm4::DB_CONTROL_Z_ENABLE_MASK) != 0;
    if (reg_programmed(Pm4::DB_Z_WRITE_BASE) && z_enable) {
        const uint32_t z_info = reg(Pm4::DB_Z_INFO);
        if ((z_info & 0x3u) != Pm4::DB_Z_FORMAT_32_FLOAT) {
            m_last_rt_binding.error =
                "unsupported DB_Z_INFO format (only 32_FLOAT)";
            return;
        }
        target.depth_gva =
            (static_cast<uint64_t>(reg(Pm4::DB_Z_WRITE_BASE_HI)) << 32) |
            (reg(Pm4::DB_Z_WRITE_BASE) & ~0xFFu);
        target.zfunc = static_cast<Pm4::ZFunc>(
            (db_control & Pm4::DB_CONTROL_ZFUNC_MASK) >> Pm4::DB_CONTROL_ZFUNC_SHIFT);
        m_last_rt_binding.depth_bound = true;
        m_last_rt_binding.depth_write =
            (db_control & Pm4::DB_CONTROL_Z_WRITE_MASK) != 0;
    }

    // Cross-check the depth extent when the guest programmed it.
    if (m_last_rt_binding.depth_bound && reg_programmed(Pm4::DB_DEPTH_SIZE_XY)) {
        const uint32_t size = reg(Pm4::DB_DEPTH_SIZE_XY);
        const uint32_t dw = ((size & Pm4::DB_SIZE_X_MAX_MASK) + 1u);
        const uint32_t dh =
            ((size & Pm4::DB_SIZE_Y_MAX_MASK) >> Pm4::DB_SIZE_Y_MAX_SHIFT) + 1u;
        if (dw != target.width || dh != target.height) {
            m_last_rt_binding.error =
                "DB_DEPTH_SIZE_XY extent disagrees with the CB target";
            return;
        }
    }

    m_raster_target = target;
    if (vp.IsDegenerate()) {
        vp = RasterViewport::Fullscreen(target.width, target.height);
    }
    m_raster_viewport = vp;

    m_last_rt_binding.bound = true;
    m_last_rt_binding.color_write = target.color_write;
    m_last_rt_binding.zfunc = target.zfunc;
    m_last_rt_binding.color_gva = target.color_gva;
    m_last_rt_binding.depth_gva = target.depth_gva;
    m_last_rt_binding.width = target.width;
    m_last_rt_binding.height = target.height;
}

void PM4VulkanTranslator::ProcessPacketType3(
    PM4Type3Opcode opcode, const uint32_t* payload, size_t payload_dwords) {
    switch (opcode) {
    case PM4Type3Opcode::PKT3_SET_SH_REG: {
        const uint32_t first_register = payload[0];
        for (size_t index = 1; index < payload_dwords; ++index) {
            m_sh_regs[first_register + static_cast<uint32_t>(index - 1u)] = payload[index];
        }
        break;
    }
    case PM4Type3Opcode::PKT3_SET_CONTEXT_REG: {
        const uint32_t first_register = payload[0];
        for (size_t index = 1; index < payload_dwords; ++index) {
            const uint32_t register_offset =
                first_register + static_cast<uint32_t>(index - 1u);
            m_context_regs[register_offset] = payload[index];
        }
        // Round 18: the legacy backend viewport now follows the REAL
        // PA_SC_SCREEN_SCISSOR_BR (exclusive extent) instead of the old
        // 0xA200/0xA201 pair -- those offsets are actually DB_DEPTH_CONTROL
        // and DB_STENCIL_CONTROL on real hardware, and round 13's reading of
        // them as viewport dimensions misread any depth-control programming.
        const auto br = m_context_regs.find(Pm4::PA_SC_SCREEN_SCISSOR_BR);
        if (br != m_context_regs.end()) {
            const uint32_t w = br->second & 0xFFFFu;
            const uint32_t h = (br->second >> 16u) & 0xFFFFu;
            if (w > 0 && h > 0) {
                m_vulkan.SetViewport(static_cast<float>(w),
                                     static_cast<float>(h));
            }
        }
        break;
    }
    case PM4Type3Opcode::PKT3_DISPATCH_DIRECT:
        if (payload_dwords >= 4) {
            // Prefer the real GPU compute path when a device + guest memory are
            // bound and the compute descriptors are present; otherwise keep the
            // legacy backend call so existing behaviour is unchanged.
            if (!TryRealComputeDispatch(payload[0], payload[1], payload[2])) {
                m_vulkan.DispatchCompute(payload[0], payload[1], payload[2]);
            }
        }
        break;
    case PM4Type3Opcode::PKT3_DISPATCH_INDIRECT: {
        // Indirect dispatch arguments are three uint32 group counts at a
        // guest address. Keep the operation bounded and fail-closed when the
        // argument buffer is unreadable.
        if (payload_dwords < 2 || m_guest_memory == nullptr) break;
        const uint64_t addr = (static_cast<uint64_t>(payload[1]) << 32) | payload[0];
        uint32_t groups[3] = {};
        if (!m_guest_memory->ReadDwords(addr, groups, 3)) break;
        if (groups[0] == 0 || groups[1] == 0 || groups[2] == 0) break;
        if (!TryRealComputeDispatch(groups[0], groups[1], groups[2]))
            m_vulkan.DispatchCompute(groups[0], groups[1], groups[2]);
        break;
    }
    case PM4Type3Opcode::PKT3_DRAW_INDEX_2:
    case PM4Type3Opcode::PKT3_DRAW_INDEX_OFFSET_2:
    case PM4Type3Opcode::PKT3_DRAW_INDEX_AUTO: {
        // Round 29: conditional rendering -- an active, failing predication
        // suppresses the draw entirely (the hardware behaviour).
        if (m_predication_active && !m_predication_pass) {
            break;
        }
        const uint32_t element_count = payload_dwords > 0 ? payload[0] : 0u;
        const bool indexed = opcode != PM4Type3Opcode::PKT3_DRAW_INDEX_AUTO;
        if (opcode == PM4Type3Opcode::PKT3_DRAW_INDEX_OFFSET_2 && payload_dwords >= 3) {
            m_first_index = payload[1];
            m_base_vertex = static_cast<int32_t>(payload[2]);
            m_first_vertex = 0;
            m_first_instance = 0;
        } else {
            m_first_index = 0;
            m_base_vertex = 0;
            m_first_vertex = 0;
            m_first_instance = 0;
        }
        // Prefer the real GPU draw path when a device + guest memory are
        // bound and the vertex-stage descriptors are present; otherwise keep
        // the legacy backend call so existing behaviour is unchanged.
        if (!TryRealDrawDispatch(indexed, element_count)) {
            // Legacy path: the recorded PKT3_NUM_INSTANCES count is honoured
            // (previously hardcoded to 1).
            m_vulkan.DrawAuto(element_count, m_num_instances);
        }
        break;
    }
    case PM4Type3Opcode::PKT3_DRAW_INDEX_INDIRECT:
    case PM4Type3Opcode::PKT3_DRAW_INDIRECT: {
        // Prospero/AGC encoding: payload[0] is a byte offset from SET_BASE,
        // payload[1..2] are patch metadata, payload[3] is the draw initiator.
        // The referenced indirect argument record follows the standard
        // indexed/non-indexed layout: count, instance_count, first, ... .
        // This core currently models the first two fields; first-index and
        // base-vertex patching are deliberately left for the VGT address model.
        if (payload_dwords < 4 || m_guest_memory == nullptr) {
            break;
        }
        const uint64_t data_gva =
            m_indirect_base_gva + static_cast<uint64_t>(payload[0]);
        uint32_t args[5] = {};
        const bool indexed = opcode == PM4Type3Opcode::PKT3_DRAW_INDEX_INDIRECT;
        const size_t arg_words = indexed ? 5u : 4u;
        if (!m_guest_memory->ReadDwords(data_gva, args, arg_words)) {
            std::cerr << "[PM4 Translator] indirect draw arguments unreadable\n";
            break;
        }
        const uint32_t count = args[0];
        const uint32_t instances = args[1];
        m_first_index = indexed ? args[2] : 0u;
        m_base_vertex = indexed ? static_cast<int32_t>(args[3]) : 0;
        m_first_vertex = indexed ? 0u : args[2];
        m_first_instance = indexed ? args[4] : args[3];
        if (count == 0 || instances == 0 || instances > 0x100000u) {
            break;
        }
        const uint32_t old_instances = m_num_instances;
        m_num_instances = instances;
        const bool executed = TryRealDrawDispatch(indexed, count);
        if (!executed) {
            m_vulkan.DrawAuto(count, instances);
        }
        m_num_instances = old_instances;
        m_first_index = 0;
        m_base_vertex = 0;
        m_first_vertex = 0;
        m_first_instance = 0;
        break;
    }
    case PM4Type3Opcode::PKT3_DRAW_INDEX_INDIRECT_MULTI:
    case PM4Type3Opcode::PKT3_DRAW_INDEX_INDIRECT_MULTI2: {
        // Multi-draw ABI emitted by AGC: offset, patch_lo, patch_hi,
        // count-mode, max_count, count_address_lo, count_address_hi, stride,
        // initiator. The count buffer contains the number of records.
        if (payload_dwords < 9 || m_guest_memory == nullptr) {
            break;
        }
        const uint64_t data_base =
            m_indirect_base_gva + static_cast<uint64_t>(payload[0]);
        const uint32_t max_count = payload[4];
        const uint64_t count_gva =
            (static_cast<uint64_t>(payload[6]) << 32) | payload[5];
        const uint32_t stride = payload[7];
        const bool indexed = opcode == PM4Type3Opcode::PKT3_DRAW_INDEX_INDIRECT_MULTI ||
                             opcode == PM4Type3Opcode::PKT3_DRAW_INDEX_INDIRECT_MULTI2;
        const uint32_t min_stride = indexed ? 20u : 16u;
        if (max_count == 0 || stride < min_stride || (stride & 3u) != 0) {
            break;
        }
        uint32_t draw_count = 0;
        if (!m_guest_memory->ReadDwords(count_gva, &draw_count, 1)) {
            break;
        }
        draw_count = std::min(draw_count, max_count);
        const uint32_t old_instances = m_num_instances;
        for (uint32_t i = 0; i < draw_count; ++i) {
            uint32_t args[5] = {};
            const uint64_t gva = data_base + static_cast<uint64_t>(i) * stride;
            const size_t arg_words = indexed ? 5u : 4u;
            if (!m_guest_memory->ReadDwords(gva, args, arg_words)) {
                break;
            }
            if (args[0] == 0 || args[1] == 0 || args[1] > 0x100000u) {
                continue;
            }
            m_num_instances = args[1];
            m_first_index = indexed ? args[2] : 0u;
            m_base_vertex = indexed ? static_cast<int32_t>(args[3]) : 0;
            m_first_vertex = indexed ? 0u : args[2];
            m_first_instance = indexed ? args[4] : args[3];
            if (!TryRealDrawDispatch(indexed, args[0])) {
                m_vulkan.DrawAuto(args[0], args[1]);
            }
        }
        m_num_instances = old_instances;
        break;
    }
    case PM4Type3Opcode::PKT3_NUM_INSTANCES:
        // Instance count applied to every subsequent draw until reprogrammed.
        if (payload_dwords >= 1 && payload[0] > 0) {
            m_num_instances = payload[0];
        }
        break;
    case PM4Type3Opcode::PKT3_INDEX_BASE:
        // payload[0..1] = 64-bit index buffer GVA (lo, hi).
        if (payload_dwords >= 2) {
            m_index_base_gva =
                (static_cast<uint64_t>(payload[1]) << 32) | payload[0];
        }
        break;
    case PM4Type3Opcode::PKT3_INDEX_TYPE:
        // 0 = 16-bit indices, 1 = 32-bit (VGT_INDEX_TYPE semantics).
        if (payload_dwords >= 1) {
            m_index_type = payload[0] & 1u;
        }
        break;
    case PM4Type3Opcode::PKT3_INDEX_BUFFER_SIZE:
        // Size of the index buffer in dwords (informational bound).
        if (payload_dwords >= 1) {
            m_index_buffer_dwords = payload[0];
        }
        break;
    case PM4Type3Opcode::PKT3_INDIRECT_BUFFER: {
        // IB packet: base GVA in payload[0..1], control in payload[2]. The
        // low 20 bits are the dword count in the ProsperoLayer model. A small
        // recursion guard prevents malformed guest command streams from
        // exhausting the host stack.
        if (payload_dwords < 3 || m_guest_memory == nullptr || m_indirect_depth >= 8)
            break;
        const uint64_t addr = (static_cast<uint64_t>(payload[1]) << 32) | payload[0];
        const size_t count = static_cast<size_t>(payload[2] & 0xFFFFFu);
        if (addr == 0 || count == 0 || count > (1u << 20)) break;
        std::vector<uint32_t> nested(count);
        if (!m_guest_memory->ReadDwords(addr, nested.data(), count)) break;
        ++m_indirect_depth;
        (void)TranslateAndExecuteCommandRingImpl(nested.data(), nested.size());
        --m_indirect_depth;
        break;
    }
    case PM4Type3Opcode::PKT3_SET_BASE:
        // AGC emits SET_BASE as: base selector, address_lo, address_hi.
        if (payload_dwords >= 3) {
            m_indirect_base_gva =
                (static_cast<uint64_t>(payload[2]) << 32) | payload[1];
        }
        break;
    case PM4Type3Opcode::PKT3_WRITE_DATA: {
        // ProsperoLayer memory-write adapter ABI:
        //   payload[1..2] = destination GVA, payload[3..] = dwords.
        // The control word in payload[0] is intentionally ignored; this keeps
        // the implementation usable for the common AGC host-memory variant
        // while remaining fail-closed when the address/data are absent.
        if (m_guest_memory == nullptr || payload_dwords < 3) break;
        const uint64_t addr = (static_cast<uint64_t>(payload[2]) << 32) | payload[1];
        const size_t count = payload_dwords - 3u;
        if (addr == 0 || count == 0 || count > (1u << 20)) break;
        (void)m_guest_memory->WriteDwords(addr, payload + 3, count);
        break;
    }
    case PM4Type3Opcode::PKT3_COPY_DATA: {
        // ProsperoLayer copy adapter ABI:
        // payload[1..2] = source GVA, payload[3..4] = destination GVA,
        // payload[5] = dword count. No partial copy on unreadable source.
        if (m_guest_memory == nullptr || payload_dwords < 6) break;
        const uint64_t src = (static_cast<uint64_t>(payload[2]) << 32) | payload[1];
        const uint64_t dst = (static_cast<uint64_t>(payload[4]) << 32) | payload[3];
        const size_t count = static_cast<size_t>(payload[5]);
        if (src == 0 || dst == 0 || count == 0 || count > (1u << 20)) break;
        std::vector<uint32_t> tmp(count);
        if (!m_guest_memory->ReadDwords(src, tmp.data(), count)) break;
        (void)m_guest_memory->WriteDwords(dst, tmp.data(), count);
        break;
    }
    case PM4Type3Opcode::PKT3_WAIT_REG_MEM: {
        // Bounded guest-memory wait adapter ABI:
        // payload[1..2] address, payload[3..4] reference (64-bit),
        // payload[5] mask; bit0 of payload[0] selects EQUAL(0)/NOT_EQUAL(1).
        if (m_guest_memory == nullptr || payload_dwords < 6) break;
        const uint64_t addr = (static_cast<uint64_t>(payload[2]) << 32) | payload[1];
        const uint64_t ref = (static_cast<uint64_t>(payload[4]) << 32) | payload[3];
        const uint64_t mask = payload[5] == 0 ? ~0ull : static_cast<uint64_t>(payload[5]);
        const bool not_equal = (payload[0] & 1u) != 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        uint32_t words[2] = {};
        while (std::chrono::steady_clock::now() < deadline) {
            if (!m_guest_memory->ReadDwords(addr, words, 2)) break;
            const uint64_t cur = static_cast<uint64_t>(words[0]) |
                                 (static_cast<uint64_t>(words[1]) << 32);
            const bool matched = ((cur & mask) == (ref & mask));
            if (matched != not_equal) break;
            std::this_thread::yield();
        }
        break;
    }
    case PM4Type3Opcode::PKT3_EVENT_WRITE_EOP:
    case PM4Type3Opcode::PKT3_RELEASE_MEM: {
        // Treat EOP/release as a real monotonically increasing completion
        // point. If the packet supplies an address/value, publish it to guest
        // memory after the local fence advances.
        ++m_sync_sequence;
        if (m_guest_memory != nullptr && payload_dwords >= 5) {
            const uint64_t addr = (static_cast<uint64_t>(payload[2]) << 32) | payload[1];
            const uint64_t value = (static_cast<uint64_t>(payload[4]) << 32) | payload[3];
            const uint32_t out[2] = {static_cast<uint32_t>(value),
                                     static_cast<uint32_t>(value >> 32)};
            (void)m_guest_memory->WriteDwords(addr, out, 2);
        }
        break;
    }
    case PM4Type3Opcode::PKT3_ACQUIRE_MEM:
        // Device-scope acquire is represented by the ordered command-stream
        // execution itself. Nothing is silently skipped or fabricated here.
        std::atomic_thread_fence(std::memory_order_acquire);
        break;
    case PM4Type3Opcode::PKT3_EVENT_WRITE: {
        // Round 29: a real event advances the CP event counter and, when the
        // packet carries an address, publishes the counter as the fence
        // value (the documented behaviour of EVENT_WRITE's data selector:
        // fences observe the counter, and memory-only packets still count).
        ++m_event_counter;
        if (m_guest_memory != nullptr && payload_dwords >= 3) {
            const uint64_t addr =
                (static_cast<uint64_t>(payload[2]) << 32) | payload[1];
            const uint32_t out[2] = {static_cast<uint32_t>(m_event_counter),
                                     static_cast<uint32_t>(m_event_counter >> 32)};
            (void)m_guest_memory->WriteDwords(addr, out, 2);
        }
        break;
    }
    case PM4Type3Opcode::PKT3_EVENT_WRITE_EOS: {
        // Round 29: EOS publishes its immediate 64-bit payload value to the
        // guest address once the stream reaches it (signalling semantics).
        ++m_event_counter;
        if (m_guest_memory != nullptr && payload_dwords >= 5) {
            const uint64_t addr =
                (static_cast<uint64_t>(payload[2]) << 32) | payload[1];
            const uint64_t value =
                (static_cast<uint64_t>(payload[4]) << 32) | payload[3];
            const uint32_t out[2] = {static_cast<uint32_t>(value),
                                     static_cast<uint32_t>(value >> 32)};
            (void)m_guest_memory->WriteDwords(addr, out, 2);
        }
        break;
    }
    case PM4Type3Opcode::PKT3_DMA_DATA: {
        // Round 29: a REAL memory-to-memory copy through the guest bridge.
        // Layout: [0]=CTL, [1..2]=SRC address, [3..4]=DST address,
        // [5]=size in bytes. Fail-closed when the bridge is missing, the
        // operands are not guest memory, or the ranges are unmapped --
        // nothing is copied and nothing is fabricated.
        if (m_guest_memory == nullptr || payload_dwords < 6) break;
        const uint64_t src =
            (static_cast<uint64_t>(payload[2]) << 32) | payload[1];
        const uint64_t dst =
            (static_cast<uint64_t>(payload[4]) << 32) | payload[3];
        const uint64_t bytes = payload[5];
        if (bytes == 0 || (bytes & 3u) != 0) break;   // dword copies only
        const size_t words = static_cast<size_t>(bytes / 4);
        std::vector<uint32_t> tmp(words, 0);
        if (!m_guest_memory->ReadDwords(src, tmp.data(), words)) break;
        if (!m_guest_memory->WriteDwords(dst, tmp.data(), words)) break;
        break;
    }
    case PM4Type3Opcode::PKT3_SET_UCONFIG_REG: {
        // Round 29: the UCONFIG register file (CP-level registers) gets the
        // same real storage SET_SH_REG / SET_CONTEXT_REG have.
        if (payload_dwords >= 2) {
            const uint32_t first = payload[0];
            for (size_t i = 1; i < payload_dwords; ++i) {
                m_uconfig_regs[first + static_cast<uint32_t>(i - 1u)] = payload[i];
            }
        }
        break;
    }
    case PM4Type3Opcode::PKT3_SET_PREDICATION: {
        // Round 29: draw predication. The 64-bit guest word at the given
        // address decides PASS/FAIL (hardware PRED_OP: bit 63 set = fail);
        // bit 0 of the header keeps the previous decision when CLEAR is not
        // requested. Draws are skipped while predication fails (below).
        if (payload_dwords >= 3 && m_guest_memory != nullptr) {
            const uint64_t addr =
                (static_cast<uint64_t>(payload[2]) << 32) | payload[1];
            const bool keep_previous = (payload[0] & 1u) != 0;
            uint32_t words[2] = {};
            if (m_guest_memory->ReadDwords(addr, words, 2)) {
                const uint64_t value = static_cast<uint64_t>(words[0]) |
                                       (static_cast<uint64_t>(words[1]) << 32);
                const bool pass = (value & 0x8000000000000000ull) == 0;
                if (!keep_previous || !m_predication_active) {
                    m_predication_pass = pass;
                }
                m_predication_active = true;
            } else {
                m_predication_active = false;   // unreadable: no predication
                m_predication_pass = true;
            }
        }
        break;
    }
    case PM4Type3Opcode::PKT3_PFP_SYNC_ME:
    case PM4Type3Opcode::PKT3_DISPATCH_DRAW_PREAMBLE:
    case PM4Type3Opcode::PKT3_NOP:
        std::atomic_thread_fence(std::memory_order_seq_cst);
        break;
    default:
        std::cout << "[PM4 Translator] Ignoring "
                  << PM4Type3OpcodeName(static_cast<uint8_t>(opcode)) << " (0x"
                  << std::hex << static_cast<uint32_t>(opcode) << std::dec << ")\n";
        break;
    }
}

} // namespace PS5::GPU
