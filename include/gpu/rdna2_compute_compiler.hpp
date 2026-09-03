#pragma once

// ============================================================================
// ProsperoLayer RDNA2 Core - RDNA2 -> SPIR-V compute-kernel compiler
// ----------------------------------------------------------------------------
// Round 18: the compiler now decodes programs with the REAL GCN decoder
// (GcnDecoder -- all 12 GFX10 instruction formats) and lowers the full set
// the software executor runs, so a Vulkan host executes the SAME programs:
//
//   SOP1/SOP2/SOPK/SOPC : scalar ALU + SCC, incl. S_CSELECT/S_CMOV and the
//                         CMPK immediate comparisons
//   SOPP                : S_ENDPGM plus STRUCTURED control flow -- forward
//                         conditional branches lower to OpSelectionMerge /
//                         OpBranchConditional (if / if-else, early exit to
//                         S_ENDPGM), backward conditional branches lower to
//                         OpLoopMerge (do-while loops). Anything that is not
//                         a structured pattern fails closed (documented).
//   VOP1/VOP2           : full integer + float ALU the SW executor supports
//   VOPC                : v_cmp_* on VCC (added to the SW executor in the
//                         same round, so both executors agree)
//   VOP3                : V_MAD/V_FMA/V_MED3/V_BFE/V_BFM/V_LDEXP/... with
//                         the real modifiers (neg/abs/omod/clamp)
//   SMEM                : S_LOAD_DWORD[X2/X4/X8] from a scalar-segment
//                         mirror SSBO, indexed by (guest address - mirror
//                         base) >> 2. The base comes from a push-constant
//                         block (round 19) -- the executor pushes it per
//                         dispatch. The mirror occupies the FIRST extra
//                         binding after in/out (binding 2) whenever the
//                         program uses SMEM; the MUBUF descriptor SSBOs
//                         follow it (bindings 3+i in that case, 2+i when no
//                         mirror is used).
//   MUBUF               : BUFFER_LOAD/STORE_DWORD[X2/X3/X4] routed per
//                         descriptor; descriptor i's SSBO mirrors the
//                         descriptor's guest buffer contents from base_gva.
//
// Registers are function-scope variables (VGPR/SGPR/SCC/VCC), so values
// survive across control-flow merges exactly like the architectural state.
//
// Round-18 corrections to historical lowering: V_LOG_F32 lowers to Log2 and
// V_EXP_F32 to Exp2 (GCN semantics -- the round-10 lowering used the
// natural-log/exp ext-insts, disagreeing with the software executor).
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "gpu/gcn_decoder.hpp"  // GcnBufferResource, GcnImageResource

namespace PS5::GPU {

enum class ComputeCompileError {
    None,
    InvalidInput,
    UnsupportedEncoding,
    UnsupportedOpcode,
    UnsupportedOperand,
    UninitializedRegister,
    MissingEndProgram,
    InstructionsAfterEndProgram,
    UnstructuredControlFlow,
    UnmatchedBranchTarget,
};

struct ComputeCompilationResult {
    bool success{false};
    std::vector<uint32_t> spirv;
    ComputeCompileError error{ComputeCompileError::None};
    size_t error_dword{0};
    std::string message;
    size_t instruction_count{0};   // RDNA2 instructions consumed (excl. S_ENDPGM)
    size_t alu_op_count{0};        // ALU ops actually lowered to SPIR-V
    // Round 18 metadata:
    size_t branch_count{0};        // lowered conditional branches (if/loop)
    size_t memory_op_count{0};     // SMEM + MUBUF accesses lowered
    bool used_scalar_mirror{false};    // module declares the SMEM mirror SSBO
    uint32_t buffer_bindings{0};       // MUBUF descriptor SSBOs declared
    // Round 28: MIMG support.
    uint32_t image_bindings{0};        // combined image/sampler descriptor pairs
    size_t image_op_count{0};          // MIMG instructions lowered

    explicit operator bool() const noexcept { return success; }
};

struct ComputeCompilerOptions {
    uint32_t local_size_x{64};
    uint32_t descriptor_set{0};
    uint32_t input_binding{0};
    uint32_t output_binding{1};
    // VGT draw-path lane model (round 10): k input dwords and m output
    // dwords per lane, laid out lane-major in both SSBOs. The defaults (1, 1)
    // keep the compute model exactly as before.
    uint32_t in_dwords_per_lane{1};
    uint32_t out_dwords_per_lane{1};
    // Round 18 -- MUBUF descriptor table (see GcnBufferResource): descriptor
    // i is routed to storage binding (2 + i) and mirrors the guest buffer at
    // base_gva (SSBO element 0 = that address). Empty table: MUBUF fails
    // closed, exactly like the software executor without a table.
    std::vector<GcnBufferResource> buffers;
    // Round 28 -- MIMG image-resource table: image i is routed to descriptor
    // bindings (2 + buffers + 3i .. +2) -- sampled image, storage image,
    // sampler -- and mirrors the guest texel array at base_gva (RGBA32UI,
    // raw dwords). Empty table: MIMG fails closed, exactly like the software
    // executor without images.
    std::vector<GcnImageResource> images;
    // Round 18 -- SMEM scalar-segment mirror: binding (2 + buffers.size())
    // mirrors guest memory starting at scalar_mirror_base_gva (SSBO element
    // (gva - base) / 4). Disabled (base = 0): SMEM fails closed.
    // Round 19: the base VALUE is no longer baked into the module -- the
    // module declares a push-constant block { uint base_lo; uint base_hi; }
    // and the executor pushes the actual mirror base at every dispatch, so
    // one compiled module serves any mirror window.
    uint64_t scalar_mirror_base_gva{0};
    // Round 19 (phase 2) -- emit a VERTEX-stage module (ExecutionModel
    // Vertex) for the real VkGraphicsPipeline raster path instead of the
    // GLCompute dispatch model:
    //   * the lane index is gl_VertexIndex (v0..v{k-1} seeded from
    //     in_data[gl_VertexIndex * k + j]),
    //   * the epilogue still stores v0..v{m-1} to out_data[lane*m + j]
    //     (the transformed-vertex dump the draw ABI promises the guest), and
    //     ADDITIONALLY writes v0..v3 to gl_Position and v4..v7 to a vec4
    //     colour output (Location 0) -- the layout the software rasterizer
    //     consumes ([0..3] clip xyzw, [4..7] RGBA).
    // No LocalSize execution mode is emitted. Default false keeps the
    // compute module byte-for-byte as before.
    bool emit_vertex_stage{false};
};

class RDNA2ComputeCompiler {
public:
    explicit RDNA2ComputeCompiler(ComputeCompilerOptions options = {})
        : m_options(std::move(options)) {}

    // Compile an RDNA2 instruction stream (terminated by S_ENDPGM) into a
    // complete SPIR-V compute module. In the default lane model v0 is seeded
    // from the input buffer and the final v0 is stored to the output buffer;
    // with in_dwords_per_lane = k / out_dwords_per_lane = m (VGT draw path)
    // v0..v{k-1} are seeded and v0..v{m-1} stored per lane.
    // With options.emit_vertex_stage the module is a Vertex-stage shader
    // (see ComputeCompilerOptions) for the VkGraphicsPipeline path.
    ComputeCompilationResult Compile(const uint32_t* bytecode,
                                     size_t dwords_count) const;

    // Round 19 (phase 2): the passthrough FRAGMENT shader for the graphics
    // pipeline -- in vec4 colour (Location 0) -> out vec4 (Location 0),
    // ExecutionModel Fragment + OriginUpperLeft. Hand-assembled with the
    // same verified opcode numbers as Compile(). Every opcode/decoration
    // number here was checked against KhronosGroup/SPIRV-Headers spirv.h.
    static std::vector<uint32_t> BuildPassthroughFragmentShader();

private:
    ComputeCompilerOptions m_options;
};

} // namespace PS5::GPU
