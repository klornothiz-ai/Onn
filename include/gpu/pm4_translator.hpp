#pragma once

#include "gpu/pm4_decoder.hpp"
#include "gpu/software_rasterizer.hpp"
#include "gpu/vulkan_compute_executor.hpp"
#include "graphics/guest_gpu/pm4.h"
#include "vulkan_backend.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace PS5::GPU {

class GpuGuestMemory;          // gpu/gpu_guest_memory.hpp

// Outcome of a real GPU compute dispatch driven from a PM4 DISPATCH_DIRECT
// packet (shader + SSBOs read from guest memory, executed on a Vulkan device,
// results written back). Exposed for tests / diagnostics.
struct ComputeDispatchRecord {
    bool attempted{false};
    bool executed_on_gpu{false};
    uint64_t shader_gva{0};
    uint64_t input_gva{0};
    uint64_t output_gva{0};
    uint32_t element_count{0};
    uint32_t groups_x{0};
    // Round 19: the buffer-resource table (MUBUF descriptors + SMEM mirror)
    // read from the guest's COMPUTE_USER_DATA_0 +5..6 slot.
    bool resources_programmed{false};
    bool resources_parsed{false};
    uint32_t resource_buffer_count{0};
    uint64_t resource_mirror_gva{0};
    uint32_t resource_mirror_dwords{0};
    std::string resource_error;   // parse-failure note (fail-closed)
};

// Outcome of a real GPU draw driven from a PM4 draw packet (DRAW_INDEX_2 /
// DRAW_INDEX_OFFSET_2 / DRAW_INDEX_AUTO). The vertex-stage program GVA comes
// from SPI_SHADER_PGM_LO_VS/HI_VS, the input/output buffers from
// SPI_SHADER_USER_DATA_VS_0 (see graphics/guest_gpu/pm4.h for the ABI). For
// indexed draws the index stream at INDEX_BASE is decoded (u16 zero-extended
// or u32) before execution; the transformed vertices are written back to
// guest memory. Round 10 adds the VGT attribute-fetch fields: when the guest
// programs a fetch-descriptor table (VS user data +5..6) the translator
// gathers several attribute dwords per lane before executing the kernel.
// Exposed for tests / diagnostics.
struct DrawDispatchRecord {
    bool attempted{false};
    bool executed_on_gpu{false};
    bool indexed{false};
    uint64_t shader_gva{0};
    uint64_t input_gva{0};       // non-indexed draws: attribute buffer GVA
    uint64_t output_gva{0};      // transformed-vertex output buffer GVA
    uint64_t index_base_gva{0};  // indexed draws: PKT3_INDEX_BASE value
    uint32_t index_type{0};      // 0 = u16 indices, 1 = u32 (INDEX_TYPE_U16/U32)
    uint32_t element_count{0};   // indices (indexed) / vertices (non-indexed)
    uint32_t instance_count{1};  // from PKT3_NUM_INSTANCES / indirect args
    uint32_t first_index{0};      // indexed draw start element
    int32_t base_vertex{0};       // signed vertex-id bias for indexed draws
    uint32_t first_vertex{0};     // non-indexed draw start vertex
    uint32_t first_instance{0};   // instance-id base
    // VGT attribute fetch (round 10):
    bool fetch_enabled{false};       // fetch-descriptor table programmed
    uint64_t fetch_table_gva{0};     // the VS user data +5..6 value
    uint32_t attribute_count{0};     // descriptor entries used by the gather
    uint32_t in_dwords_per_lane{1};  // k: fetched attribute dwords per lane
    uint32_t out_dwords_per_lane{1}; // m: transformed-vertex dwords per lane
    // Round 19: the buffer-resource table for the vertex stage (VS user data
    // +8..9) -- the same MUBUF/SMEM plumbing as the compute path.
    bool resources_programmed{false};
    bool resources_parsed{false};
    uint32_t resource_buffer_count{0};
    uint64_t resource_mirror_gva{0};
    uint32_t resource_mirror_dwords{0};
    std::string resource_error;
    // Round 19 (phase 2): the real-VkGraphicsPipeline raster attempt.
    bool graphics_raster_attempted{false};
    bool graphics_raster_executed{false};
    std::string graphics_raster_note;
};

struct PM4TranslationResult {
    PM4DecodeError error{};
    size_t consumed_dwords{0};
    size_t executed_packets{0};

    bool ok() const noexcept { return error.code == PM4DecodeErrorCode::None; }
    explicit operator bool() const noexcept { return ok(); }
};

// Round 18: what the draw path derived from the REAL CB/DB/PA context
// registers (see graphics/guest_gpu/pm4.h for the verified offsets). Exposed
// for tests / diagnostics: `bound` is true when CB_COLOR0_BASE was programmed
// and the register-derived target replaced the host-API one. `error` carries
// the fail-closed reason when the guest's binding is malformed (the vertex
// stage still executes -- hardware does not stop the draw on a broken CB).
struct RenderTargetBinding {
    bool programmed{false};     // CB_COLOR0_BASE written by the guest
    bool bound{false};          // a valid PM4-derived target is in effect
    bool color_write{true};     // CB_TARGET_MASK.TARGET0_ENABLE != 0
    bool depth_bound{false};    // DB_Z_WRITE_BASE + Z_ENABLE
    bool depth_write{true};     // DB_DEPTH_CONTROL.Z_WRITE_ENABLE
    Pm4::ZFunc zfunc{Pm4::ZFunc::Less};   // DB_DEPTH_CONTROL.ZFUNC
    // Round 20: the converted CB_COLOR0_INFO layout (FORMAT + NUMBER_TYPE +
    // COMP_SWAP -> GuestColorFormat). Invalid until the gate passes.
    Pm4::GuestColorFormat color_format{Pm4::GuestColorFormat::Invalid};
    uint64_t color_gva{0};
    uint64_t depth_gva{0};
    uint32_t width{0};
    uint32_t height{0};
    bool viewport_from_registers{false};  // PA_CL_VPORT_* programmed
    bool scissor_from_registers{false};
    uint32_t scissor_left{0};
    uint32_t scissor_top{0};
    uint32_t scissor_right{0};
    uint32_t scissor_bottom{0};
    std::string error;
};

class PM4VulkanTranslator {
public:
    explicit PM4VulkanTranslator(VulkanRendererBackend& vulkan_backend);

    // Legacy adapter: malformed streams are rejected and reported to stderr.
    void TranslateAndExecuteCommandRing(const uint32_t* ring_buffer, size_t dwords_count);

    // Validates the complete stream before issuing any backend command.
    PM4TranslationResult TranslateAndExecuteCommandRingChecked(
        const uint32_t* ring_buffer, size_t dwords_count);

    size_t GetProcessedPacketsCount() const { return m_packet_count; }
    const PM4DecodeError& GetLastDecodeError() const { return m_last_error; }

    // Bind the real GPU compute path. When BOTH a guest-memory accessor and a
    // Vulkan compute executor are set, a DISPATCH_DIRECT packet reads the RDNA2
    // shader and input SSBO from guest memory, runs them on the real device via
    // the executor, and writes the output SSBO back -- instead of the legacy
    // backend CPU-sim DispatchCompute() call. The same binding also enables the
    // real GPU draw path (draw packets run the vertex-stage program through the
    // executor; see DrawDispatchRecord). If either is null both paths use the
    // legacy backend calls unchanged (so existing tests keep their behaviour).
    // Round 12: the binding also enables the honest GCN software fallback so
    // draw/compute programs still execute (real instruction semantics,
    // hardware=false) when no Vulkan device exists on the host.
    void BindComputeExecutor(VulkanComputeExecutor* executor, GpuGuestMemory* memory);

    // Round 13: bind a software raster target. When a target is bound, every
    // real-path draw ALSO rasterizes the transformed vertices (lane-major
    // output buffer, m dwords/vertex) into the guest RGBA8 colour plane (+
    // optional float depth plane) with the guest viewport. Unbinding
    // (width = 0) keeps the previous behaviour exactly.
    // Round 18: when the guest programs CB_COLOR0_BASE (SET_CONTEXT_REG) the
    // REGISTER-derived target replaces this host-API one -- the documented
    // "render-target binding from the actual PM4 registers" step.
    void SetRasterTarget(const RasterTarget& target) { m_host_raster_target = target; }
    const SoftwareRasterStats& GetLastRasterStats() const { return m_last_raster; }
    const RasterTarget& GetRasterTarget() const { return m_raster_target; }

    // Round 18: host-API viewport (back-compat; PA_CL_VPORT_* registers take
    // precedence when the guest programs them, and a degenerate host viewport
    // falls back to the fullscreen pass over the active target).
    void SetRasterViewport(const RasterViewport& vp) { m_host_raster_viewport = vp; }
    const RasterViewport& GetRasterViewport() const { return m_raster_viewport; }

    // Round 18: how the active render target + viewport were derived from
    // the REAL CB/DB/PA context registers at the last draw.
    const RenderTargetBinding& GetLastRenderTargetBinding() const { return m_last_rt_binding; }

    // Round 19 (phase 2): opt-in REAL-VkGraphicsPipeline raster path (default
    // OFF -- every existing caller keeps the round-18 behaviour on any host).
    // When enabled AND the executor has a graphics queue AND the last draw
    // bound a register-derived CB target whose format converts, the draw runs
    // the guest VS as the pipeline's VERTEX stage with a passthrough fragment
    // shader into a CB_COLOR0-derived VkImage (pixels + depth read back into
    // guest memory, transformed vertices written to the output buffer).
    // ANY missing piece declines and the software rasterizer runs unchanged.
    void SetGraphicsRasterEnabled(bool enabled) { m_graphics_raster = enabled; }
    bool IsGraphicsRasterEnabled() const { return m_graphics_raster; }

    // Round 19 (phase 2): pure conversion from the register-derived binding
    // to the graphics-pipeline target description -- the extent, the
    // CB_COLOR0_INFO -> GuestColorFormat conversion, DB_DEPTH_CONTROL ->
    // depth/zfunc, and the PA_CL_VPORT numbers the VkViewport will get.
    // Static + pure so the register mapping is unit-testable without a
    // device; returns false when the binding cannot serve the graphics path.
    static bool BuildGraphicsTarget(const RenderTargetBinding& binding,
                                    const RasterViewport& viewport,
                                    VulkanComputeExecutor::GraphicsTargetDesc& out);

    const ComputeDispatchRecord& GetLastComputeDispatch() const { return m_last_dispatch; }

    // Record of the most recent draw packet processed through
    // TryRealDrawDispatch() (whether or not the real path executed).
    const DrawDispatchRecord& GetLastDrawDispatch() const { return m_last_draw; }
    // Round 29: predication/event telemetry (for tests and diagnostics).
    bool IsPredicationActive() const { return m_predication_active; }
    bool PredicationPasses() const { return m_predication_pass; }
    uint64_t EventCounter() const { return m_event_counter; }
    uint32_t GetUconfigReg(uint32_t offset) const {
        const auto it = m_uconfig_regs.find(offset);
        return it != m_uconfig_regs.end() ? it->second : 0u;
    }

private:
    VulkanRendererBackend& m_vulkan;
    size_t m_packet_count{0};
    PM4DecodeError m_last_error{};
    std::unordered_map<uint32_t, uint32_t> m_sh_regs;
    std::unordered_map<uint32_t, uint32_t> m_context_regs;

    // Round 13/18: software raster state. m_raster_target/m_raster_viewport
    // are the ACTIVE values (PM4-derived when the guest programmed
    // CB_COLOR0_BASE, else the host-API pair); the host pair is kept for
    // back-compat and for draws before any CB programming.
    RasterTarget m_raster_target;
    RasterTarget m_host_raster_target;
    RasterViewport m_raster_viewport;
    RasterViewport m_host_raster_viewport;
    RenderTargetBinding m_last_rt_binding;
    SoftwareRasterStats m_last_raster;
    VulkanComputeExecutor* m_executor{nullptr};
    GpuGuestMemory* m_guest_memory{nullptr};
    ComputeDispatchRecord m_last_dispatch{};

    // Draw state recorded from the ring (see graphics/guest_gpu/pm4.h ABI):
    // index buffer location/encoding from PKT3_INDEX_BASE / INDEX_TYPE /
    // INDEX_BUFFER_SIZE, instance count from PKT3_NUM_INSTANCES.
    uint64_t m_index_base_gva{0};
    uint32_t m_index_type{0};       // Pm4::INDEX_TYPE_U16 (default) / _U32
    uint32_t m_index_buffer_dwords{0};
    uint32_t m_num_instances{1};
    uint32_t m_first_index{0};
    int32_t m_base_vertex{0};
    uint32_t m_first_vertex{0};
    uint32_t m_first_instance{0};
    // PM4 SET_BASE establishes the address base used by indirect draw data
    // offsets. Zero keeps the compatibility mode where the packet offset is
    // already a guest virtual address.
    // Round 25: monotonically increasing GPU completion sequence.
    uint64_t m_sync_sequence{0};
    // Round 29: UCONFIG register file (PKT3_SET_UCONFIG_REG), the event
    // counter PKT3_EVENT_WRITE advances, and the draw-predication state
    // PKT3_SET_PREDICATION establishes (draws are SKIPPED while predication
    // fails -- conditional rendering).
    std::unordered_map<uint32_t, uint32_t> m_uconfig_regs;
    uint64_t m_event_counter{0};
    bool m_predication_active{false};
    bool m_predication_pass{true};
    uint64_t m_indirect_base_gva{0};
    DrawDispatchRecord m_last_draw{};

    // Read compute descriptors from the recorded SH regs and, when the real GPU
    // path is bound, execute on the device. Returns true if a real dispatch was
    // performed (so the caller skips the legacy backend call).
    bool TryRealComputeDispatch(uint32_t groups_x, uint32_t groups_y, uint32_t groups_z);

    // Read the vertex-stage descriptors from the recorded SH regs and, when
    // the real GPU path is bound, execute the draw on the device (indexed
    // draws decode the index stream first; a programmed fetch-descriptor
    // table triggers the VGT attribute gather). Returns true if a real draw
    // was performed (so the caller skips the legacy DrawAuto() call).
    bool TryRealDrawDispatch(bool indexed, uint32_t element_count);

    // Round 18: re-derive the active RasterTarget/RasterViewport from the
    // recorded CB/DB/PA context registers (the REAL PM4 render-target
    // binding). Fills m_last_rt_binding; leaves the host-API target active
    // when the guest has not programmed CB_COLOR0_BASE.
    void AssembleRenderTargetFromRegisters();

    // Round 19: parse the guest's buffer-resource table (see pm4.h for the
    // ABI). Returns false + a diagnostic on malformed tables; the dispatch
    // then proceeds WITHOUT resources (MUBUF/SMEM fail closed exactly like
    // round 18 -- never a partially-parsed table).
    bool ParseResourceTable(uint64_t table_gva, GcnDispatchResources& out,
                            std::string& error);

    // Round 19: read the resource-table GVA from the recorded SH registers
    // (compute: COMPUTE_USER_DATA_0 +5..6; draw VS: +8..9) and parse it into
    // `resources`. Fills the record's resource_* fields either way.
    void CollectResourceTable(const uint32_t lo_reg, const uint32_t hi_reg,
                              GcnDispatchResources& resources,
                              bool& programmed, bool& parsed,
                              std::string& error);

    PM4TranslationResult TranslateAndExecuteCommandRingImpl(
        const uint32_t* ring_buffer, size_t dwords_count);
    void ProcessPacketType3(
        PM4Type3Opcode opcode, const uint32_t* payload, size_t payload_dwords);

    // Round 19 (phase 2): the opt-in graphics raster flag (default false).
    bool m_graphics_raster{false};
    uint32_t m_indirect_depth{0};
};

} // namespace PS5::GPU
