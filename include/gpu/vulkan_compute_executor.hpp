#pragma once
// ============================================================================
// ProsperoLayer RDNA2 Core - Real Vulkan compute executor (item #1)
// ----------------------------------------------------------------------------
// A genuine end-to-end GPU compute path. Unlike the legacy backend -- which
// created a device but fell back to a CPU "simulation" (an XOR over a memory
// framebuffer) because it never built pipelines, buffers or descriptor sets --
// this executor performs the full real pipeline:
//
//   RDNA2 bytecode --(RDNA2ComputeCompiler)--> SPIR-V
//     -> vkCreateShaderModule
//     -> vkCreateDescriptorSetLayout (2x storage buffer / SSBO)
//     -> vkCreatePipelineLayout + vkCreateComputePipelines
//     -> vkCreateBuffer (input SSBO) + vkCreateBuffer (output SSBO)
//     -> vkAllocateMemory (host-visible|coherent) + vkBindBufferMemory
//     -> vkAllocateDescriptorSets + vkUpdateDescriptorSets
//     -> vkCmdBindPipeline + vkCmdBindDescriptorSets + vkCmdDispatch
//     -> vkQueueSubmit + vkQueueWaitIdle
//     -> map output memory and read the results BACK from GPU memory.
//
// The result is provable without hand-waving: the caller supplies an input
// vector, the RDNA2 kernel runs on a real Vulkan device (llvmpipe/lavapipe on a
// GPU-less host, or a physical GPU on the user's machine), and the executor
// returns exactly what the GPU wrote to the output SSBO.
//
// When no Vulkan loader/driver is available the executor reports Unavailable
// (never a silently-wrong CPU fake): callers decide how to degrade.
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "gpu/gcn_decoder.hpp"  // GcnBufferResource, GcnImageResource (software fallback)
#include "graphics/guest_gpu/pm4.h"  // Pm4::ZFunc / GuestColorFormat (round 19)

namespace PS5::GPU {

class GpuGuestMemory;

// Round 19 -- the per-dispatch GCN resource tables: the MUBUF descriptor
// table + the SMEM scalar-mirror window, read from the guest's PM4
// resource-table ABI (see graphics/guest_gpu/pm4.h). The executor uploads
// every buffer's guest contents into its own SSBO before the dispatch and
// writes modified buffers back afterwards -- exactly the "routed per
// descriptor to its own SSBO" model the compiler emits.
struct GcnDispatchResources {
    std::vector<GcnBufferResource> buffers;   // MUBUF descriptors
    std::vector<GcnImageResource> images;     // MIMG descriptors (round 28)
    uint64_t scalar_mirror_base_gva{0};       // 0 = no mirror (SMEM fails)
    uint32_t scalar_mirror_dwords{0};
};

// One extra SSBO bound at the bindings that follow in/out (2, 3, ...): the
// scalar mirror first (when the module uses SMEM), then one per MUBUF
// descriptor. `contents` is uploaded before the dispatch; when read_back is
// set the downloaded contents replace it afterwards.
struct SpirvExtraSsbo {
    std::vector<uint32_t> contents;
    bool read_back{false};
};

// Round 28: an image bound at the THREE bindings that follow the last SSBO
// (sampled image, storage image, sampler -- the layout the compiler emits).
// `contents` is width*height*4 raw RGBA32UI dwords uploaded before the
// dispatch and always downloaded back afterwards (MIMG stores/atomics must
// land in guest memory).
struct SpirvExtraImage {
    std::vector<uint32_t> contents;
    uint32_t width{0};
    uint32_t height{0};
    uint32_t mips{1};
    bool read_back{true};
};

enum class ComputeExecStatus {
    Ok,
    Unavailable,        // no Vulkan loader / no device / no compute queue
    CompileFailed,      // RDNA2 -> SPIR-V rejected the program
    PipelineFailed,     // shader module / pipeline creation failed
    ResourceFailed,     // buffer / memory / descriptor allocation failed
    DispatchFailed,     // command recording / submission failed
};

struct ComputeDispatchResult {
    ComputeExecStatus status{ComputeExecStatus::Unavailable};
    std::vector<uint32_t> output;   // raw uint bits read back from the out SSBO
    std::string message;
    std::string device_name;        // e.g. "llvmpipe (LLVM 19.1.7, 256 bits)"
    bool hardware{false};           // true when executed on a real Vulkan device
    size_t spirv_dwords{0};
    // Round 20: true when the pipeline objects (dsl/layout/module/pipeline)
    // came from the cross-dispatch cache instead of the vkCreate* chain.
    bool pipeline_cache_hit{false};

    explicit operator bool() const noexcept { return status == ComputeExecStatus::Ok; }
};

// Result of a real VkImage lifecycle: the image is created, backed by
// device-local memory, transitioned through layouts, written on the GPU, then
// copied to a host-visible staging buffer and read back as RGBA8 pixels.
struct ImageOpResult {
    ComputeExecStatus status{ComputeExecStatus::Unavailable};
    std::vector<uint8_t> pixels;    // width*height*4 RGBA8, read back from GPU
    uint32_t width{0};
    uint32_t height{0};
    std::string message;
    bool hardware{false};

    explicit operator bool() const noexcept { return status == ComputeExecStatus::Ok; }
};

// Owns a Vulkan instance/device for the lifetime of the object. Cheap to reuse
// across dispatches. All methods are safe to call even when Vulkan is missing;
// they simply report Unavailable.
class VulkanComputeExecutor {
public:
    VulkanComputeExecutor();
    ~VulkanComputeExecutor();

    VulkanComputeExecutor(const VulkanComputeExecutor&) = delete;
    VulkanComputeExecutor& operator=(const VulkanComputeExecutor&) = delete;

    // Bring up the Vulkan instance + logical device + compute queue. Returns
    // false (and leaves IsAvailable() == false) on a GPU-less/loaderless host.
    bool Initialize();
    bool IsAvailable() const { return m_ready; }
    const std::string& DeviceName() const { return m_device_name; }

    // Compile an RDNA2 stream (terminated by S_ENDPGM) to SPIR-V and execute it
    // over `input` on the GPU. The kernel seeds v0 from in_data[gid] and stores
    // final v0 to out_data[gid]; one thread per input element is dispatched
    // (rounded up to the shader's local_size_x work-group size).
    ComputeDispatchResult RunRDNA2(const uint32_t* rdna2_code, size_t dwords,
                                   const std::vector<uint32_t>& input);

    // VGT draw-path lane model (round 10): the kernel is a vertex-stage
    // program. `input` holds in_dwords_per_lane fetched attribute dwords per
    // lane (lane-major); v0..v{k-1} are seeded from in_data[gid*k + j] and
    // v0..v{m-1} stored to out_data[gid*m + j]. One thread per lane is
    // dispatched and the result carries threads*out_dwords_per_lane dwords.
    // (in_dwords_per_lane = out_dwords_per_lane = 1 reduces to RunRDNA2.)
    ComputeDispatchResult RunRDNA2Strided(const uint32_t* rdna2_code, size_t dwords,
                                          const std::vector<uint32_t>& input,
                                          uint32_t in_dwords_per_lane,
                                          uint32_t out_dwords_per_lane);

    // Convenience float overloads: bit-reinterpret to/from uint SSBO storage.
    ComputeDispatchResult RunRDNA2Float(const uint32_t* rdna2_code, size_t dwords,
                                        const std::vector<float>& input);

    // Execute an already-compiled SPIR-V compute module (set 0, binding 0 = in,
    // binding 1 = out storage buffers; entry point "main").
    // threads (0 = input.size()) overrides the dispatched lane count and
    // out_elements (0 = input.size()) the output element count, for kernels
    // whose lane consumes/produces several dwords (the VGT vertex model).
    // Round 19: `extra` binds additional SSBOs at bindings 2, 3, ... (the
    // scalar mirror + the per-descriptor MUBUF SSBOs the compiler emits) and
    // `push_constants` (when non-null) is pushed before the dispatch -- the
    // SMEM mirror base { uint base_lo; uint base_hi; }. Both default to
    // nothing, preserving the round-18 behaviour exactly.
    ComputeDispatchResult RunSpirv(const std::vector<uint32_t>& spirv,
                                   const std::vector<uint32_t>& input,
                                   uint32_t local_size_x = 64,
                                   uint32_t threads = 0,
                                   uint32_t out_elements = 0,
                                   std::vector<SpirvExtraSsbo>* extra = nullptr,
                                   const uint32_t* push_constants = nullptr,
                                   uint32_t push_constant_dwords = 0,
                                   std::vector<SpirvExtraImage>* extra_images = nullptr);

    // Round 19 -- the resource-aware dispatch: reads every buffer's guest
    // contents (and the mirror window) via `mem`, compiles the program with
    // the resource tables, dispatches on the device with the mirror base
    // pushed as constants, and writes modified buffer contents back to guest
    // memory. When the hardware path cannot run, the SAME program + tables
    // execute on the honest GCN software interpreter (hardware=false) --
    // which writes guest memory live through the same bridge -- so the
    // guest-visible result is identical whichever path served the dispatch.
    // Empty tables reduce to the round-18 behaviour (MUBUF/SMEM fail closed
    // at compile time on the hardware path).
    ComputeDispatchResult RunRDNA2WithResources(
        const uint32_t* rdna2_code, size_t dwords,
        const std::vector<uint32_t>& input,
        const GcnDispatchResources& resources,
        GpuGuestMemory* mem,
        uint32_t in_dwords_per_lane = 1,
        uint32_t out_dwords_per_lane = 1);

    // Round 19: guest-memory <-> SSBO staging for the resource tables.
    // Public statics so the upload/write-back plumbing is unit-testable on
    // a headless host without a Vulkan device (both fail closed on any
    // unreadable/unwritable range -- no partial staging).
    static bool LoadResourceContents(const GcnDispatchResources& resources,
                                     GpuGuestMemory* mem,
                                     std::vector<std::vector<uint32_t>>& buffer_contents,
                                     std::vector<uint32_t>& mirror_contents);
    static bool StoreResourceContents(const GcnDispatchResources& resources,
                                      GpuGuestMemory* mem,
                                      const std::vector<std::vector<uint32_t>>& buffer_contents);
    // Round 28: image texel staging (fail-closed, like the buffer pair).
    static bool LoadImageContents(const GcnDispatchResources& resources,
                                  GpuGuestMemory* mem,
                                  std::vector<SpirvExtraImage>& image_contents);
    static bool StoreImageContents(const GcnDispatchResources& resources,
                                    GpuGuestMemory* mem,
                                    const std::vector<SpirvExtraImage>& image_contents);

    // Real VkImage path (item #1, the "images" half): allocate a 2D RGBA8
    // device-local image, transition UNDEFINED->TRANSFER_DST, clear it to the
    // requested colour on the GPU, transition ->TRANSFER_SRC, copy to a
    // host-visible buffer, and read the pixels back. Proves the image +
    // memory + layout-barrier + transfer machinery, not just buffers.
    ImageOpResult ClearImage(uint32_t width, uint32_t height,
                             float r, float g, float b, float a);

    // ------------------------------------------------------------------
    // Round 19 (phase 2) -- the REAL VkGraphicsPipeline raster path.
    // ------------------------------------------------------------------
    // Target description assembled from the register-derived binding (see
    // PM4VulkanTranslator::AssembleRenderTargetFromRegisters): extent +
    // converted colour format (GuestColorFormat carries the real VkFormat
    // value), optional 32_FLOAT depth with its ZFUNC, and the guest
    // PA_CL_VPORT_* viewport (screen = ndc * scale + offset, pixels).
    struct GraphicsTargetDesc {
        uint32_t width{0};
        uint32_t height{0};
        Pm4::GuestColorFormat color_format{Pm4::GuestColorFormat::Invalid};
        bool depth_enabled{false};
        bool depth_write{true};
        Pm4::ZFunc zfunc{Pm4::ZFunc::Less};
        float vport_scale_x{0.0f}, vport_off_x{0.0f};
        float vport_scale_y{0.0f}, vport_off_y{0.0f};
        bool color_write{true};   // CB_TARGET_MASK.TARGET0_ENABLE
    };

    struct GraphicsRasterResult {
        ComputeExecStatus status{ComputeExecStatus::Unavailable};
        bool executed{false};          // true ONLY on a GPU rasterization
        std::string message;
        std::string device_name;
        // The transformed vertices the vertex stage wrote to the out SSBO
        // (lane-major, out_dwords_per_lane each) -- the caller writes them to
        // the guest output buffer, keeping the round-9 draw ABI intact.
        std::vector<uint32_t> transformed_vertices;
        // Round 20: true when the graphics pipeline chain (dsl / layout /
        // modules / render pass / pipeline) came from the cross-dispatch
        // cache (only the per-target framebuffer was rebuilt).
        bool pipeline_cache_hit{false};
    };

    // True when the initialized device has a GRAPHICS-capable queue (the
    // compute path works without one; the graphics raster needs one).
    bool HasGraphicsQueue() const { return m_graphics_queue; }

    // Rasterizes `draw_vertex_count` vertices through a REAL
    // VkGraphicsPipeline: the guest's RDNA2 vertex-stage program is compiled
    // with options.emit_vertex_stage and becomes the pipeline's VERTEX stage
    // (attributes fetched from the in SSBO by gl_VertexIndex -- the same
    // lane model), a passthrough fragment shader colours the fragments, and
    // the colour attachment is a VkImage created with the extent/format the
    // CB registers named (converted via Pm4::CbInfoToGuestColorFormat) plus
    // an optional D32_SFLOAT depth attachment driven by DB_DEPTH_CONTROL's
    // ZFUNC. The guest's CURRENT colour/depth plane contents are uploaded
    // first (loadOp LOAD -- merge semantics, like the software rasterizer),
    // and the rendered planes are read back and written to guest memory at
    // color_gva / depth_gva. MUBUF/SMEM resources for the vertex stage are
    // staged exactly like RunRDNA2WithResources.
    // Fail-closed: ANY missing piece (no device, no graphics queue,
    // unsupported format, unreadable planes, pipeline rejection) returns
    // executed=false and touches nothing -- the caller falls back to the
    // software rasterizer.
    GraphicsRasterResult DrawVerticesToGuest(
        const uint32_t* rdna2_code, size_t rdna2_dwords,
        const std::vector<uint32_t>& input,
        uint32_t in_dwords_per_lane, uint32_t out_dwords_per_lane,
        uint32_t draw_vertex_count,
        const GcnDispatchResources& resources,
        const GraphicsTargetDesc& target,
        uint64_t color_gva, uint64_t depth_gva,
        GpuGuestMemory* mem);

    // Round 12: honest software fallback. When enabled and the hardware path
    // cannot run (no Vulkan device, or the SPIR-V compiler rejects the
    // program), the raw GFX10 bytecode executes on the REAL GCN software
    // interpreter (same lane model, real instruction semantics) instead of
    // failing outright. Results are flagged hardware=false with a message
    // naming the interpreter -- they are never faked.
    void SetSoftwareFallback(bool enabled, GpuGuestMemory* mem = nullptr,
                              const std::vector<GcnBufferResource>* buffers = nullptr);

private:
    struct Impl;
    Impl* m_impl{nullptr};
    bool m_ready{false};
    bool m_graphics_queue{false};
    std::string m_device_name;

    // Round 12 software-fallback state (not part of the Vulkan Impl).
    bool m_sw_fallback{false};
    GpuGuestMemory* m_sw_mem{nullptr};
    const std::vector<GcnBufferResource>* m_sw_buffers{nullptr};
    // Round 28: the fallback now consumes the full resource table (buffers
    // + images); the legacy SetSoftwareFallback buffers-only entry wraps
    // them into this owned struct.
    GcnDispatchResources m_sw_resources;
    const GcnDispatchResources* m_sw_res_ptr{nullptr};
};

} // namespace PS5::GPU
