// ============================================================================
// ProsperoLayer RDNA2 Core - RDNA2 -> SPIR-V compute-kernel compiler
// ----------------------------------------------------------------------------
// Round 18 rewrite: decode via the REAL GcnDecoder (all 12 GFX10 formats),
// lower the full instruction set the GCN software executor runs (SOP1/2/K/C,
// VOP1/2/3, VOPC, SMEM, MUBUF) with structured control flow
// (OpSelectionMerge / OpLoopMerge), registers as function-scope variables.
//
// All SPIR-V opcode numbers and GLSL.std.450 ext-inst numbers were verified
// against KhronosGroup/SPIRV-Headers (spirv.core.grammar.json and
// extinst.glsl.std.450.grammar.json). GFX10 opcode numbers come from the
// round-12 LLVM tablegen extraction.
//
// Structured control-flow contract (fail-closed otherwise):
//   * forward S_CBRANCH_*  -> if / if-else (the then-arm may end with a bare
//     s_branch to a common join = the classic else pattern), including an
//     early exit whose target IS the S_ENDPGM position;
//   * backward S_CBRANCH_* -> a do-while loop (header..back-edge);
//   * S_CBRANCH_EXECZ is never taken (EXEC is all-ones in the lane model)
//     and S_CBRANCH_EXECNZ is an unconditional branch;
//   * any other goto shape reports UnstructuredControlFlow.
// ============================================================================
#include "gpu/rdna2_compute_compiler.hpp"
#include "gpu/gcn_decoder.hpp"

#include <cstring>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace PS5::GPU {
namespace {

constexpr uint32_t kSpirvMagic = 0x07230203U;
constexpr uint32_t kSpirvVersion10 = 0x00010000U;

// SPIR-V core opcodes (verified against spirv.core.grammar.json).
enum Op : uint16_t {
    OpExtInstImport = 11,
    OpExtInst = 12,
    OpMemoryModel = 14,
    OpEntryPoint = 15,
    OpExecutionMode = 16,
    OpCapability = 17,
    OpTypeVoid = 19,
    OpTypeBool = 20,
    OpTypeInt = 21,
    OpTypeFloat = 22,
    OpTypeVector = 23,
    OpTypeImage = 25,
    OpTypeSampler = 26,
    OpTypeSampledImage = 27,
    OpTypeRuntimeArray = 29,
    OpTypeStruct = 30,
    OpTypePointer = 32,
    OpTypeFunction = 33,
    OpConstantTrue = 41,
    OpConstantFalse = 42,
    OpConstant = 43,
    OpVectorShuffle = 79,
    OpCompositeConstruct = 80,
    OpCompositeExtract = 81,
    OpCompositeInsert = 82,
    OpFunction = 54,
    OpFunctionEnd = 56,
    OpVariable = 59,
    OpLoad = 61,
    OpStore = 62,
    OpAccessChain = 65,
    OpDecorate = 71,
    OpMemberDecorate = 72,
    OpConvertFToU = 109,
    OpConvertFToS = 110,
    OpConvertSToF = 111,
    OpConvertUToF = 112,
    OpUConvert = 113,
    OpBitcast = 124,
    OpSNegate = 126,
    OpFNegate = 127,
    OpIAdd = 128,
    OpFAdd = 129,
    OpISub = 130,
    OpFSub = 131,
    OpIMul = 132,
    OpFMul = 133,
    OpFDiv = 136,
    OpSelect = 169,
    OpIEqual = 170,
    OpINotEqual = 171,
    OpUGreaterThan = 172,
    OpSGreaterThan = 173,
    OpUGreaterThanEqual = 174,
    OpSGreaterThanEqual = 175,
    OpULessThan = 176,
    OpSLessThan = 177,
    OpULessThanEqual = 178,
    OpSLessThanEqual = 179,
    OpFOrdEqual = 180,
    OpFUnordEqual = 181,
    OpFOrdNotEqual = 182,
    OpFUnordNotEqual = 183,
    OpFOrdLessThan = 184,
    OpFUnordLessThan = 185,
    OpFOrdGreaterThan = 186,
    OpFUnordGreaterThan = 187,
    OpFOrdLessThanEqual = 188,
    OpFUnordLessThanEqual = 189,
    OpFOrdGreaterThanEqual = 190,
    OpFUnordGreaterThanEqual = 191,
    OpShiftRightLogical = 194,
    OpShiftRightArithmetic = 195,
    OpShiftLeftLogical = 196,
    OpBitwiseOr = 197,
    OpBitwiseXor = 198,
    OpBitwiseAnd = 199,
    OpNot = 200,
    OpBitFieldUExtract = 203,
    OpBitReverse = 204,
    OpBitCount = 205,
    OpControlBarrier = 224,
    // Round 28 (MIMG): opcode numbers verified against
    // KhronosGroup/SPIRV-Headers spirv.h (OpTypeImage=25, OpSampledImage=86,
    // OpImageSampleImplicitLod=87, OpImageFetch=95, OpImageGather=96,
    // OpImageRead=98, OpImageWrite=99, OpImage=100, OpImageQuerySizeLod=103,
    // OpAtomicExchange=229, OpAtomicIAdd=234 .. OpAtomicXor=242).
    OpImageTexelPointer = 60,
    OpSampledImage = 86,
    OpImageSampleImplicitLod = 87,
    OpImageSampleExplicitLod = 88,
    OpImageSampleDrefImplicitLod = 89,
    OpImageSampleDrefExplicitLod = 90,
    OpImageFetch = 95,
    OpImageGather = 96,
    OpImageRead = 98,
    OpImageWrite = 99,
    OpImage = 100,
    OpImageQuerySizeLod = 103,
    OpAtomicExchange = 229,
    OpAtomicCompareExchange = 230,
    OpAtomicIAdd = 234,
    OpAtomicISub = 235,
    OpAtomicSMin = 236,
    OpAtomicUMin = 237,
    OpAtomicSMax = 238,
    OpAtomicUMax = 239,
    OpAtomicAnd = 240,
    OpAtomicOr = 241,
    OpAtomicXor = 242,
    OpPhi = 245,
    OpLoopMerge = 246,
    OpSelectionMerge = 247,
    OpLabel = 248,
    OpBranch = 249,
    OpBranchConditional = 250,
    OpReturn = 253,
};

// GLSL.std.450 ext-inst numbers (verified against
// extinst.glsl.std.450.grammar.json).
enum Glsl : uint32_t {
    GlslRound = 1,
    GlslRoundEven = 2,
    GlslTrunc = 3,
    GlslFAbs = 4,
    GlslSAbs = 5,
    GlslFloor = 8,
    GlslCeil = 9,
    GlslFract = 10,
    GlslSin = 13,
    GlslCos = 14,
    GlslExp2 = 29,
    GlslLog2 = 30,
    GlslSqrt = 31,
    GlslInverseSqrt = 32,
    GlslFMin = 37,
    GlslUMin = 38,
    GlslSMin = 39,
    GlslFMax = 40,
    GlslUMax = 41,
    GlslSMax = 42,
    GlslFClamp = 43,
    GlslFma = 50,
    GlslLdexp = 53,
};

// Decorations / storage classes / builtins / execution models.
// Round 19: every value re-verified against KhronosGroup/SPIRV-Headers
// spirv.h (DecorationBlock=2, DecorationLocation=30, SpvBuiltInVertexIndex=42,
// SpvExecutionModelVertex=0/Fragment=4, OriginUpperLeft=7, PushConstant=9).
enum Decoration : uint32_t {
    DecBlock = 2,
    DecBufferBlock = 3,
    DecArrayStride = 6,
    DecBuiltIn = 11,
    DecNonWritable = 24,
    DecNonReadable = 25,
    DecLocation = 30,
    DecBinding = 33,
    DecDescriptorSet = 34,
    DecOffset = 35,
};
// Round 28: Dim / ImageFormat / ImageOperands operand values (spirv.h:
// Dim2D=1, ImageFormatUnknown=0, ImageFormatRgba32ui=30; Bias/Lod/Grad/
// ConstOffset/Offset/Sample = 1/2/4/8/16/64).
constexpr uint32_t Dim2D = 1;
constexpr uint32_t ImageFormatUnknown = 0;
constexpr uint32_t ImageFormatRgba32ui = 30;
constexpr uint32_t ImgBias = 1;
constexpr uint32_t ImgLod = 2;
constexpr uint32_t ImgGrad = 4;
constexpr uint32_t ImgSample = 64;
constexpr uint32_t StorageUniformConstant = 0;
constexpr uint32_t StorageImage = 12;
// Atomic scope/semantics (spirv.h: Device=1; SequentiallyConsistent=0x10).
constexpr uint32_t ScopeDevice = 1;
constexpr uint32_t SemSeqCst = 0x10;
constexpr uint32_t StorageFunction = 7;
constexpr uint32_t StorageInput = 1;
constexpr uint32_t StorageUniform = 2;
constexpr uint32_t StorageOutput = 3;
constexpr uint32_t StoragePushConstant = 9;
constexpr uint32_t BuiltInGlobalInvocationId = 28;
constexpr uint32_t BuiltInVertexIndex = 42;
constexpr uint32_t BuiltInPosition = 0;
constexpr uint32_t ExecModelGLCompute = 5;
constexpr uint32_t ExecModelVertex = 0;
constexpr uint32_t ExecModelFragment = 4;
constexpr uint32_t ExecModeLocalSize = 17;
constexpr uint32_t ExecModeOriginUpperLeft = 7;
constexpr uint32_t ScopeWorkgroup = 2;

struct IdAllocator {
    uint32_t next{1};
    uint32_t Allocate() { return next++; }
};

void Emit(std::vector<uint32_t>& words, uint16_t opcode,
          std::initializer_list<uint32_t> operands) {
    const auto word_count = static_cast<uint32_t>(operands.size() + 1);
    words.push_back((word_count << 16U) | opcode);
    words.insert(words.end(), operands.begin(), operands.end());
}

ComputeCompilationResult Fail(ComputeCompileError error, size_t dword,
                              std::string message) {
    ComputeCompilationResult r;
    r.error = error;
    r.error_dword = dword;
    r.message = std::move(message);
    return r;
}

// ---------------------------------------------------------------------------
// Program context: decoded instructions + control-flow facts.
// ---------------------------------------------------------------------------
struct Prog {
    const ComputeCompilerOptions& opt;
    std::vector<GcnInstruction> code;
    std::vector<uint32_t> pc_of;    // code[i] starts at dword pc_of[i]
    uint32_t endpgm{0};
    // loop header instruction index -> back-edge instruction index
    std::unordered_map<uint32_t, uint32_t> loop_headers;
};

// Failure report threaded through the lowering helpers.
struct Err {
    ComputeCompileError code{ComputeCompileError::None};
    size_t dword{0};
    std::string msg;
    bool ok() const { return code == ComputeCompileError::None; }
};

// ---------------------------------------------------------------------------
// Emitter: the module under construction.
// ---------------------------------------------------------------------------
struct Emitter {
    IdAllocator ids;
    std::vector<uint32_t> mod;    // pre-function (types/consts/global vars)
    std::vector<uint32_t> deco;   // decorations
    std::vector<uint32_t> body;   // function body under construction

    uint32_t t_void{0}, t_uint{0}, t_ulong{0}, t_float{0}, t_bool{0},
             t_fn{0}, t_v3uint{0}, t_v4float{0}, t_rtarr{0}, t_struct_buf{0};
    uint32_t t_ptr_uni_struct{0}, t_ptr_uni_uint{0};
    uint32_t t_ptr_in_v3uint{0}, t_ptr_in_uint{0}, t_ptr_in_v4float{0};
    uint32_t t_ptr_out_v4float{0}, t_ptr_push_struct{0}, t_struct_push{0};
    uint32_t t_ptr_fn_uint{0}, t_ptr_fn_bool{0};

    uint32_t var_gid{0}, var_in{0}, var_out{0}, var_mirror{0};
    std::vector<uint32_t> var_buf;
    // Round 28 (MIMG): per image resource -- sampled image (Sampled=1),
    // storage image (Sampled=2, Rgba32ui) and sampler descriptors.
    std::vector<uint32_t> var_img_sampled, var_img_storage, var_samp;
    uint32_t t_v2float{0}, t_v2uint{0}, t_v4uint{0};
    uint32_t t_img_sampled{0}, t_img_storage{0}, t_sampled_image{0},
             t_sampler{0};
    uint32_t t_ptr_uc_img_sampled{0}, t_ptr_uc_img_storage{0},
             t_ptr_uc_sampler{0};
    // Round 19: vertex-stage mode + push-constant mirror base.
    uint32_t var_vertindex{0}, var_position{0}, var_color_out{0}, var_push{0};

    uint32_t c_uint0{0}, c_true{0}, c_false{0}, c_ulong0{0}, c_ulong32{0};
    uint32_t c_float0{0};
    std::unordered_map<uint32_t, uint32_t> uint_consts;

    uint32_t fn_main{0}, glsl{0};
    bool block_open{false};

    uint32_t vgpr_var[GcnSwExecutor::kVgprCount]{};
    uint32_t sgpr_var[GcnSwExecutor::kSgprCount]{};
    uint32_t scc_var{0}, vcc_var{0};

    std::unordered_map<uint32_t, uint32_t> label_at;

    // stats
    size_t alu_ops{0}, branches{0}, memory_ops{0}, insn_count{0};
    size_t image_ops{0};                       // Round 28 (MIMG)
    bool used_mirror{false};

    uint32_t UintConst(uint32_t value) {
        if (value == 0u) return c_uint0;
        auto it = uint_consts.find(value);
        if (it != uint_consts.end()) return it->second;
        const uint32_t id = ids.Allocate();
        uint_consts.emplace(value, id);
        Emit(mod, OpConstant, {t_uint, id, value});
        return id;
    }

    void EnsureBlock() {
        if (!block_open) {
            const uint32_t lbl = ids.Allocate();
            Emit(body, OpLabel, {lbl});
            block_open = true;
        }
    }
    void BranchTo(uint32_t label) {
        Emit(body, OpBranch, {label});
        block_open = false;
    }
    uint32_t NewLabel() { return ids.Allocate(); }

    uint32_t ToFloat(uint32_t uint_id) {
        const uint32_t r = ids.Allocate();
        Emit(body, OpBitcast, {t_float, r, uint_id});
        return r;
    }
    uint32_t ToUint(uint32_t float_id) {
        const uint32_t r = ids.Allocate();
        Emit(body, OpBitcast, {t_uint, r, float_id});
        return r;
    }
    uint32_t LoadVar(uint32_t var) {
        const uint32_t r = ids.Allocate();
        Emit(body, OpLoad, {t_uint, r, var});
        return r;
    }
    void StoreVar(uint32_t var, uint32_t value) {
        Emit(body, OpStore, {var, value});
    }
    // bool <-> uint bridges (GCN stores flags in 32-bit registers).
    uint32_t LoadBoolVar(uint32_t var) {
        const uint32_t r = ids.Allocate();
        Emit(body, OpLoad, {t_bool, r, var});
        return r;
    }
    uint32_t BoolToUintId(uint32_t bool_id) {
        const uint32_t r = ids.Allocate();
        Emit(body, OpSelect, {t_uint, r, bool_id, UintConst(1u), c_uint0});
        return r;
    }
    // x & 31 (shift-amount masking, SPIR-V requires < 32).
    uint32_t And31(uint32_t v) {
        const uint32_t r = ids.Allocate();
        Emit(body, OpBitwiseAnd, {t_uint, r, v, UintConst(31u)});
        return r;
    }
};

// ---------------------------------------------------------------------------
// Forward declarations of the lowering helpers (free functions).
// ---------------------------------------------------------------------------
bool LowerRegion(Emitter& em, const Prog& p, uint32_t begin, uint32_t end,
                 Err& err);
bool LowerRegionInner(Emitter& em, const Prog& p, uint32_t begin, uint32_t end,
                      bool skip_header_at_begin, Err& err);
bool LowerAlu(Emitter& em, const Prog& p, uint32_t i, Err& err);
bool LowerMimg(Emitter& em, const Prog& p, uint32_t i, Err& err);
uint32_t LowerIfResumeIndex(const Prog& p, uint32_t i, uint32_t target);
uint32_t LabelFor(Emitter& em, uint32_t index);

// ---------------------------------------------------------------------------
// Module skeleton: types, constants, SSBO variables, entry point.
// Round 19: (a) the SMEM mirror base moved from baked OpConstants to a
// push-constant block { uint base_lo; uint base_hi; } so one module serves
// any mirror window (the executor pushes the base per dispatch); (b) the
// vertex-stage mode (options.emit_vertex_stage) swaps the compute entry for
// a Vertex entry: the lane index is gl_VertexIndex (scalar Input) and
// gl_Position + a Location-0 colour out are declared (written by the
// epilogue alongside the out-SSBO store).
// ---------------------------------------------------------------------------
bool BuildSkeleton(Emitter& em, const Prog& p, bool any_smem, bool any_mubuf,
                   Err& err) {
    (void)err;
    auto& ids = em.ids;
    const bool vertex_mode = p.opt.emit_vertex_stage;

    em.glsl = ids.Allocate();
    em.t_void = ids.Allocate();
    em.t_uint = ids.Allocate();
    em.t_ulong = ids.Allocate();
    em.t_float = ids.Allocate();
    em.t_bool = ids.Allocate();
    em.t_fn = ids.Allocate();
    em.t_v3uint = ids.Allocate();
    if (vertex_mode) em.t_v4float = ids.Allocate();
    em.t_rtarr = ids.Allocate();
    em.t_struct_buf = ids.Allocate();
    em.t_struct_push = ids.Allocate();
    em.t_ptr_uni_struct = ids.Allocate();
    em.t_ptr_uni_uint = ids.Allocate();
    em.t_ptr_in_v3uint = ids.Allocate();
    em.t_ptr_in_uint = ids.Allocate();
    if (vertex_mode) {
        em.t_ptr_in_v4float = ids.Allocate();
        em.t_ptr_out_v4float = ids.Allocate();
    }
    em.t_ptr_push_struct = ids.Allocate();
    em.t_ptr_fn_uint = ids.Allocate();
    em.t_ptr_fn_bool = ids.Allocate();

    em.var_gid = ids.Allocate();
    em.var_in = ids.Allocate();
    em.var_out = ids.Allocate();
    if (any_smem) em.var_mirror = ids.Allocate();
    // em.var_buf backs both MUBUF (srsrc) AND SMEM S_BUFFER_LOAD_* (sbase)
    // descriptor lookups -- see LowerSmem's em.var_buf[desc_index] use. A
    // program with S_BUFFER_LOAD_* but no MUBUF instruction has any_smem set
    // and any_mubuf clear, so gating this allocation on any_mubuf alone left
    // var_buf empty and made that lookup index an empty vector.
    bool any_buffer_smem_load = false;
    for (const auto& ins : p.code) {
        if (ins.format == GcnFormat::SMEM &&
            (ins.opcode == GcnOp::S_BUFFER_LOAD_DWORD ||
             ins.opcode == GcnOp::S_BUFFER_LOAD_DWORDX2 ||
             ins.opcode == GcnOp::S_BUFFER_LOAD_DWORDX4)) {
            any_buffer_smem_load = true;
            break;
        }
    }
    if (any_mubuf || any_buffer_smem_load) {
        em.var_buf.resize(p.opt.buffers.size());
        for (auto& v : em.var_buf) v = ids.Allocate();
    }
    // Round 28 (MIMG): allocate the per-image descriptor variables only when
    // the program actually uses MIMG instructions.
    bool any_mimg = false;
    for (const auto& ins : p.code) {
        if (ins.format == GcnFormat::MIMG) {
            any_mimg = true;
            break;
        }
    }
    if (any_mimg) {
        em.var_img_sampled.resize(p.opt.images.size());
        em.var_img_storage.resize(p.opt.images.size());
        em.var_samp.resize(p.opt.images.size());
        for (auto& v : em.var_img_sampled) v = ids.Allocate();
        for (auto& v : em.var_img_storage) v = ids.Allocate();
        for (auto& v : em.var_samp) v = ids.Allocate();
    }
    if (vertex_mode) {
        em.var_vertindex = ids.Allocate();
        em.var_position = ids.Allocate();
        em.var_color_out = ids.Allocate();
    }
    if (any_smem) em.var_push = ids.Allocate();
    em.c_uint0 = ids.Allocate();
    em.c_true = ids.Allocate();
    em.c_false = ids.Allocate();
    em.c_ulong0 = ids.Allocate();
    em.c_ulong32 = ids.Allocate();
    if (vertex_mode) em.c_float0 = ids.Allocate();
    em.fn_main = ids.Allocate();

    // ---- decorations -------------------------------------------------------
    Emit(em.deco, OpDecorate, {em.t_rtarr, DecArrayStride, 4U});
    Emit(em.deco, OpDecorate, {em.t_struct_buf, DecBufferBlock});
    Emit(em.deco, OpMemberDecorate, {em.t_struct_buf, 0U, DecOffset, 0U});
    Emit(em.deco, OpDecorate, {em.var_in, DecDescriptorSet,
                               p.opt.descriptor_set});
    Emit(em.deco, OpDecorate, {em.var_in, DecBinding, p.opt.input_binding});
    Emit(em.deco, OpDecorate, {em.var_out, DecDescriptorSet,
                               p.opt.descriptor_set});
    Emit(em.deco, OpDecorate, {em.var_out, DecBinding, p.opt.output_binding});
    if (vertex_mode) {
        Emit(em.deco, OpDecorate, {em.var_vertindex, DecBuiltIn,
                                   BuiltInVertexIndex});
        Emit(em.deco, OpDecorate, {em.var_position, DecBuiltIn,
                                   BuiltInPosition});
        Emit(em.deco, OpDecorate, {em.var_color_out, DecLocation, 0U});
    } else {
        Emit(em.deco, OpDecorate, {em.var_gid, DecBuiltIn,
                                   BuiltInGlobalInvocationId});
    }
    uint32_t next_binding = 2;
    if (any_smem) {
        // The push-constant block is NOT decorated with a binding (the
        // push-constant storage class has an implicit single slot); only the
        // Block + member offsets are required.
        Emit(em.deco, OpDecorate, {em.t_struct_push, DecBlock});
        Emit(em.deco, OpMemberDecorate, {em.t_struct_push, 0U, DecOffset, 0U});
        Emit(em.deco, OpMemberDecorate, {em.t_struct_push, 1U, DecOffset, 4U});
        Emit(em.deco, OpDecorate, {em.var_mirror, DecDescriptorSet,
                                   p.opt.descriptor_set});
        Emit(em.deco, OpDecorate, {em.var_mirror, DecBinding, next_binding});
        ++next_binding;
    }
    for (size_t b = 0; b < em.var_buf.size(); ++b) {
        Emit(em.deco, OpDecorate, {em.var_buf[b], DecDescriptorSet,
                                   p.opt.descriptor_set});
        Emit(em.deco, OpDecorate, {em.var_buf[b], DecBinding, next_binding});
        ++next_binding;
    }
    // Round 28 (MIMG): each image resource occupies THREE descriptor slots
    // (sampled image, storage image, sampler) in that order.
    for (size_t g = 0; g < em.var_img_sampled.size(); ++g) {
        Emit(em.deco, OpDecorate, {em.var_img_sampled[g], DecDescriptorSet,
                                   p.opt.descriptor_set});
        Emit(em.deco, OpDecorate, {em.var_img_sampled[g], DecBinding,
                                   next_binding});
        ++next_binding;
        Emit(em.deco, OpDecorate, {em.var_img_storage[g], DecDescriptorSet,
                                   p.opt.descriptor_set});
        Emit(em.deco, OpDecorate, {em.var_img_storage[g], DecBinding,
                                   next_binding});
        ++next_binding;
        Emit(em.deco, OpDecorate, {em.var_samp[g], DecDescriptorSet,
                                   p.opt.descriptor_set});
        Emit(em.deco, OpDecorate, {em.var_samp[g], DecBinding, next_binding});
        ++next_binding;
    }

    // ---- types / constants / global variables ------------------------------
    Emit(em.mod, OpTypeVoid, {em.t_void});
    Emit(em.mod, OpTypeBool, {em.t_bool});
    Emit(em.mod, OpTypeInt, {em.t_uint, 32U, 0U});
    Emit(em.mod, OpTypeInt, {em.t_ulong, 64U, 0U});
    Emit(em.mod, OpTypeFloat, {em.t_float, 32U});
    Emit(em.mod, OpTypeVector, {em.t_v3uint, em.t_uint, 3U});
    if (vertex_mode || any_mimg) {
        Emit(em.mod, OpTypeVector, {em.t_v4float, em.t_float, 4U});
    }
    if (any_mimg) {
        // Coordinates / gather components / fetch results.
        Emit(em.mod, OpTypeVector, {em.t_v2float, em.t_float, 2U});
        Emit(em.mod, OpTypeVector, {em.t_v2uint, em.t_uint, 2U});
        Emit(em.mod, OpTypeVector, {em.t_v4uint, em.t_uint, 4U});
        // OpTypeImage (uint, 2D): Sampled=1 for fetch/gather/sample and
        // Sampled=2 + Rgba32ui for read/write/atomics -- raw-dword semantics
        // that match the software executor bit-for-bit.
        Emit(em.mod, OpTypeImage,
             {em.t_img_sampled, em.t_uint, Dim2D, 0U, 0U, 0U, 1U,
              ImageFormatUnknown});
        Emit(em.mod, OpTypeImage,
             {em.t_img_storage, em.t_uint, Dim2D, 0U, 0U, 0U, 2U,
              ImageFormatRgba32ui});
        Emit(em.mod, OpTypeSampledImage, {em.t_sampled_image,
                                          em.t_img_sampled});
        Emit(em.mod, OpTypeSampler, {em.t_sampler});
        Emit(em.mod, OpTypePointer, {em.t_ptr_uc_img_sampled,
                                     StorageUniformConstant, em.t_img_sampled});
        Emit(em.mod, OpTypePointer, {em.t_ptr_uc_img_storage,
                                     StorageUniformConstant, em.t_img_storage});
        Emit(em.mod, OpTypePointer, {em.t_ptr_uc_sampler,
                                     StorageUniformConstant, em.t_sampler});
    }
    Emit(em.mod, OpTypeFunction, {em.t_fn, em.t_void});
    Emit(em.mod, OpTypeRuntimeArray, {em.t_rtarr, em.t_uint});
    Emit(em.mod, OpTypeStruct, {em.t_struct_buf, em.t_rtarr});
    // Push-constant block: struct Push { uint base_lo; uint base_hi; }.
    Emit(em.mod, OpTypeStruct, {em.t_struct_push, em.t_uint, em.t_uint});
    Emit(em.mod, OpTypePointer, {em.t_ptr_uni_struct, StorageUniform,
                                 em.t_struct_buf});
    Emit(em.mod, OpTypePointer, {em.t_ptr_uni_uint, StorageUniform, em.t_uint});
    Emit(em.mod, OpTypePointer, {em.t_ptr_in_v3uint, StorageInput, em.t_v3uint});
    Emit(em.mod, OpTypePointer, {em.t_ptr_in_uint, StorageInput, em.t_uint});
    if (vertex_mode) {
        Emit(em.mod, OpTypePointer,
             {em.t_ptr_in_v4float, StorageInput, em.t_v4float});
        Emit(em.mod, OpTypePointer,
             {em.t_ptr_out_v4float, StorageOutput, em.t_v4float});
    }
    Emit(em.mod, OpTypePointer,
         {em.t_ptr_push_struct, StoragePushConstant, em.t_struct_push});
    Emit(em.mod, OpTypePointer, {em.t_ptr_fn_uint, StorageFunction, em.t_uint});
    Emit(em.mod, OpTypePointer, {em.t_ptr_fn_bool, StorageFunction, em.t_bool});
    Emit(em.mod, OpConstantTrue, {em.t_bool, em.c_true});
    Emit(em.mod, OpConstantFalse, {em.t_bool, em.c_false});
    Emit(em.mod, OpConstant, {em.t_uint, em.c_uint0, 0U});
    Emit(em.mod, OpConstant, {em.t_ulong, em.c_ulong0, 0U});
    Emit(em.mod, OpConstant, {em.t_ulong, em.c_ulong32, 32U});
    if (vertex_mode) {
        Emit(em.mod, OpConstant, {em.t_float, em.c_float0, 0U});
    }
    Emit(em.mod, OpVariable, {em.t_ptr_uni_struct, em.var_in, StorageUniform});
    Emit(em.mod, OpVariable, {em.t_ptr_uni_struct, em.var_out, StorageUniform});
    if (any_smem) {
        Emit(em.mod, OpVariable, {em.t_ptr_uni_struct, em.var_mirror,
                                  StorageUniform});
        Emit(em.mod, OpVariable,
             {em.t_ptr_push_struct, em.var_push, StoragePushConstant});
    }
    for (uint32_t v : em.var_buf) {
        Emit(em.mod, OpVariable, {em.t_ptr_uni_struct, v, StorageUniform});
    }
    // Round 28 (MIMG): the three descriptor variables per image resource.
    for (uint32_t v : em.var_img_sampled) {
        Emit(em.mod, OpVariable, {em.t_ptr_uc_img_sampled, v,
                                  StorageUniformConstant});
    }
    for (uint32_t v : em.var_img_storage) {
        Emit(em.mod, OpVariable, {em.t_ptr_uc_img_storage, v,
                                  StorageUniformConstant});
    }
    for (uint32_t v : em.var_samp) {
        Emit(em.mod, OpVariable, {em.t_ptr_uc_sampler, v,
                                  StorageUniformConstant});
    }
    if (vertex_mode) {
        Emit(em.mod, OpVariable, {em.t_ptr_in_uint, em.var_vertindex,
                                  StorageInput});
        Emit(em.mod, OpVariable,
             {em.t_ptr_out_v4float, em.var_position, StorageOutput});
        Emit(em.mod, OpVariable,
             {em.t_ptr_out_v4float, em.var_color_out, StorageOutput});
    } else {
        Emit(em.mod, OpVariable, {em.t_ptr_in_v3uint, em.var_gid, StorageInput});
    }
    return true;
}

// ---------------------------------------------------------------------------
// Register variables: pre-scan writes, declare + zero-initialise them.
// ---------------------------------------------------------------------------
bool DeclareRegisters(Emitter& em, const Prog& p, Err& err) {
    (void)err;
    const uint32_t k_in = p.opt.in_dwords_per_lane;
    bool need_scc = false, need_vcc = false;
    auto mark_vgpr = [&](uint32_t idx) {
        if (idx < GcnSwExecutor::kVgprCount && em.vgpr_var[idx] == 0) {
            em.vgpr_var[idx] = em.ids.Allocate();
            Emit(em.mod, OpVariable, {em.t_ptr_fn_uint, em.vgpr_var[idx],
                                      StorageFunction});
        }
    };
    auto mark_sgpr = [&](uint32_t idx) {
        if (idx < GcnSwExecutor::kSgprCount && em.sgpr_var[idx] == 0) {
            em.sgpr_var[idx] = em.ids.Allocate();
            Emit(em.mod, OpVariable, {em.t_ptr_fn_uint, em.sgpr_var[idx],
                                      StorageFunction});
        }
    };

    // The lane seeding writes v0..v{k-1}.
    for (uint32_t j = 0; j < k_in && j < GcnSwExecutor::kVgprCount; ++j) {
        mark_vgpr(j);
    }
    for (uint32_t i = 0; i < p.code.size(); ++i) {
        const GcnInstruction& ins = p.code[i];
        switch (ins.format) {
            case GcnFormat::VOP1:
            case GcnFormat::VOP2:
                mark_vgpr(ins.dst);
                if (ins.format == GcnFormat::VOP2 &&
                    ins.opcode == GcnOp::V_MAC_F32) {
                    mark_vgpr(ins.dst);   // accumulator read as well
                }
                break;
            case GcnFormat::VOP3:
                mark_vgpr(ins.dst);
                break;
            case GcnFormat::VOPC:
                need_vcc = true;
                break;
            case GcnFormat::SOP1:
            case GcnFormat::SOP2:
            case GcnFormat::SOPK:
                if (ins.dst == 106) need_scc = true;       // sdst = SCC
                else mark_sgpr(ins.dst);
                if (ins.format == GcnFormat::SOP1 &&
                    ins.opcode == GcnOp::S_MOV_B64) {
                    mark_sgpr(ins.dst + 1);
                }
                need_scc = true;   // SOP arithmetic may write SCC
                break;
            case GcnFormat::SOPC:
                need_scc = true;
                break;
            case GcnFormat::SMEM: {
                const int dwords_loaded =
                    ins.opcode == GcnOp::S_LOAD_DWORD ? 1 :
                    ins.opcode == GcnOp::S_LOAD_DWORDX2 ? 2 :
                    ins.opcode == GcnOp::S_LOAD_DWORDX4 ? 4 :
                    ins.opcode == GcnOp::S_LOAD_DWORDX8 ? 8 :
                    ins.opcode == GcnOp::S_BUFFER_LOAD_DWORD ? 1 :
                    ins.opcode == GcnOp::S_BUFFER_LOAD_DWORDX2 ? 2 :
                    ins.opcode == GcnOp::S_BUFFER_LOAD_DWORDX4 ? 4 : 0;
                for (int d = 0; d < dwords_loaded; ++d) {
                    mark_sgpr(ins.dst + static_cast<uint8_t>(d));
                }
                // S_BUFFER_LOAD_* addresses via the descriptor table
                // (em.var_buf), not via sbase/sbase+1 as scalar registers --
                // only mark those as live for the plain S_LOAD_* form.
                if (ins.opcode == GcnOp::S_LOAD_DWORD ||
                    ins.opcode == GcnOp::S_LOAD_DWORDX2 ||
                    ins.opcode == GcnOp::S_LOAD_DWORDX4 ||
                    ins.opcode == GcnOp::S_LOAD_DWORDX8) {
                    mark_sgpr(ins.sbase);
                    mark_sgpr(ins.sbase + 1);
                }
                break;
            }
            case GcnFormat::MUBUF: {
                const int count =
                    ins.opcode == GcnOp::BUFFER_LOAD_DWORD ? 1 :
                    ins.opcode == GcnOp::BUFFER_LOAD_DWORDX2 ? 2 :
                    ins.opcode == GcnOp::BUFFER_LOAD_DWORDX3 ? 3 :
                    ins.opcode == GcnOp::BUFFER_LOAD_DWORDX4 ? 4 : 0;
                for (int d = 0; d < count; ++d) {
                    mark_vgpr(ins.vdata + static_cast<uint8_t>(d));
                }
                if (ins.offen || ins.idxen) mark_vgpr(ins.vaddr);
                break;
            }
            case GcnFormat::MIMG: {
                // Round 28: vdata (destination for sample/fetch/gather/
                // resinfo/atomics, source for store) and the address VGPRs.
                const bool is_store =
                    ins.opcode == GcnOp::IMAGE_STORE ||
                    ins.opcode == GcnOp::IMAGE_STORE_MIP ||
                    ins.opcode == GcnOp::IMAGE_STORE_RTN;
                const bool is_gather =
                    ins.opcode == GcnOp::IMAGE_GATHER4 ||
                    ins.opcode == GcnOp::IMAGE_GATHER4_LZ ||
                    ins.opcode == GcnOp::IMAGE_GATHER4_B ||
                    ins.opcode == GcnOp::IMAGE_GATHER4H ||
                    ins.opcode == GcnOp::IMAGE_GATHER4_PO ||
                    ins.opcode == GcnOp::IMAGE_GATHER4_C_LZ;
                if (is_store) {
                    for (uint32_t c = 0; c < 4; ++c) {
                        if ((ins.dmask >> c) & 1u) {
                            mark_vgpr(ins.vdata + static_cast<uint8_t>(c));
                        }
                    }
                } else if (is_gather) {
                    for (uint32_t c = 0; c < 4; ++c) {
                        mark_vgpr(ins.vdata + static_cast<uint8_t>(c));
                    }
                } else if (ins.opcode == GcnOp::IMAGE_GET_RESINFO) {
                    mark_vgpr(ins.vdata);
                    mark_vgpr(ins.vdata + 1);
                    mark_vgpr(ins.vdata + 2);
                    mark_vgpr(ins.vdata + 3);
                } else {
                    // sample/fetch/atomic: dmask selects the written comps
                    // (atomics write vdata[0]).
                    for (uint32_t c = 0; c < 4; ++c) {
                        if ((ins.dmask >> c) & 1u) {
                            mark_vgpr(ins.vdata + static_cast<uint8_t>(c));
                        }
                    }
                    if (ins.opcode >= GcnOp::IMAGE_ATOMIC_SWAP &&
                        ins.opcode <= GcnOp::IMAGE_ATOMIC_DEC) {
                        mark_vgpr(ins.vdata);
                    }
                }
                // address registers: x, y (+ mip / bias / dref / grads).
                mark_vgpr(ins.vaddr);
                mark_vgpr(ins.vaddr + 1);
                if (ins.opcode == GcnOp::IMAGE_LOAD_MIP ||
                    ins.opcode == GcnOp::IMAGE_SAMPLE_L ||
                    ins.opcode == GcnOp::IMAGE_SAMPLE_B ||
                    ins.opcode == GcnOp::IMAGE_SAMPLE_D) {
                    mark_vgpr(ins.vaddr + 2);
                }
                if (ins.opcode == GcnOp::IMAGE_SAMPLE_D) {
                    mark_vgpr(ins.vaddr + 3);
                }
                break;
            }
            case GcnFormat::SOPP:
                if (ins.opcode == GcnOp::S_CBRANCH_SCC0 ||
                    ins.opcode == GcnOp::S_CBRANCH_SCC1) {
                    need_scc = true;
                }
                break;
            default:
                break;
        }
        if (ins.format == GcnFormat::VOP2 &&
            ins.opcode == GcnOp::V_CNDMASK_B32) {
            need_vcc = true;
        }
    }

    if (need_scc) {
        em.scc_var = em.ids.Allocate();
        Emit(em.mod, OpVariable, {em.t_ptr_fn_bool, em.scc_var, StorageFunction});
    }
    if (need_vcc) {
        em.vcc_var = em.ids.Allocate();
        Emit(em.mod, OpVariable, {em.t_ptr_fn_bool, em.vcc_var, StorageFunction});
    }

    // Entry block: label + zero-initialise every register variable so reads
    // are deterministic (mirrors the SW executor's zero-initialised files).
    const uint32_t entry = em.NewLabel();
    Emit(em.body, OpLabel, {entry});
    em.block_open = true;
    for (uint32_t idx = 0; idx < GcnSwExecutor::kVgprCount; ++idx) {
        if (em.vgpr_var[idx] != 0) {
            Emit(em.body, OpStore, {em.vgpr_var[idx], em.c_uint0});
        }
    }
    for (uint32_t idx = 0; idx < GcnSwExecutor::kSgprCount; ++idx) {
        if (em.sgpr_var[idx] != 0) {
            Emit(em.body, OpStore, {em.sgpr_var[idx], em.c_uint0});
        }
    }
    if (em.scc_var != 0) Emit(em.body, OpStore, {em.scc_var, em.c_false});
    if (em.vcc_var != 0) Emit(em.body, OpStore, {em.vcc_var, em.c_false});
    return true;
}

// ---------------------------------------------------------------------------
// Lane seeding: v0..v{k-1} = in_data[gid*k + j].
// ---------------------------------------------------------------------------
bool SeedLanes(Emitter& em, const Prog& p, Err& err) {
    (void)err;
    const uint32_t k_in = p.opt.in_dwords_per_lane;

    // Round 19: the lane index. Compute model: GlobalInvocationId.x; vertex
    // model: gl_VertexIndex (a scalar Input -- loaded directly, no chain).
    uint32_t gid = 0;
    if (p.opt.emit_vertex_stage) {
        gid = em.ids.Allocate();
        Emit(em.body, OpLoad, {em.t_uint, gid, em.var_vertindex});
    } else {
        const uint32_t gid_ptr = em.ids.Allocate();
        Emit(em.body, OpAccessChain, {em.t_ptr_in_uint, gid_ptr, em.var_gid,
                                      em.c_uint0});
        gid = em.ids.Allocate();
        Emit(em.body, OpLoad, {em.t_uint, gid, gid_ptr});
    }

    uint32_t lane_base = gid;
    if (k_in > 1U) {
        const uint32_t ck = em.UintConst(k_in);
        const uint32_t base = em.ids.Allocate();
        Emit(em.body, OpIMul, {em.t_uint, base, gid, ck});
        lane_base = base;
    }
    for (uint32_t j = 0; j < k_in; ++j) {
        uint32_t index = lane_base;
        if (j > 0U) {
            index = em.ids.Allocate();
            Emit(em.body, OpIAdd,
                 {em.t_uint, index, lane_base, em.UintConst(j)});
        }
        const uint32_t ptr = em.ids.Allocate();
        Emit(em.body, OpAccessChain,
             {em.t_ptr_uni_uint, ptr, em.var_in, em.c_uint0, index});
        const uint32_t val = em.ids.Allocate();
        Emit(em.body, OpLoad, {em.t_uint, val, ptr});
        Emit(em.body, OpStore, {em.vgpr_var[j], val});
    }
    return true;
}

// ---------------------------------------------------------------------------
// Operand resolution.
// ---------------------------------------------------------------------------
// Scalar/special source encoding (9-bit; also covers the 7-bit SOP ssrc1
// whose encodings never reach 128+ except inline constants).
bool ResolveScalarSrc(Emitter& em, const GcnInstruction& ins, uint16_t enc,
                      uint32_t& out, Err& err) {
    if (enc == 0xFFu && ins.has_literal) {
        out = em.UintConst(ins.literal);
        return true;
    }
    if (enc <= GcnSwExecutor::kSgprCount - 1) {
        if (em.sgpr_var[enc] != 0) {
            out = em.LoadVar(em.sgpr_var[enc]);
        } else {
            out = em.c_uint0;   // unwritten SGPR reads as 0 (SW parity)
        }
        return true;
    }
    switch (enc) {
        case GCN_SSRC_VCC_LO:
            out = em.BoolToUintId(em.vcc_var != 0
                                      ? em.LoadBoolVar(em.vcc_var)
                                      : em.c_false);
            return true;
        case GCN_SSRC_VCC_HI:
        case GCN_SSRC_M0:
            out = em.c_uint0;
            return true;
        case GCN_SSRC_EXEC_LO:
            out = em.UintConst(0xFFFFFFFFu);   // EXEC is all-ones
            return true;
        case GCN_SSRC_EXEC_HI:
            out = em.c_uint0;
            return true;
        default:
            break;
    }
    uint32_t bits = 0;
    if (GcnDecodeInlineConstant32(enc, bits)) {
        out = em.UintConst(bits);
        return true;
    }
    err.code = ComputeCompileError::UnsupportedOperand;
    err.msg = "unsupported scalar source encoding " + std::to_string(enc);
    return false;
}

// 9-bit VOP-family source: 0..101 SGPR / specials, 255 literal, 256+ VGPR.
bool ResolveSrc9(Emitter& em, const GcnInstruction& ins, uint16_t enc,
                 uint32_t& out, Err& err) {
    if (enc >= 256u) {
        const uint32_t vgpr = enc - 256u;
        if (vgpr >= GcnSwExecutor::kVgprCount || em.vgpr_var[vgpr] == 0) {
            err.code = ComputeCompileError::UninitializedRegister;
            err.msg = "source VGPR v" + std::to_string(vgpr) +
                      " is not defined by an earlier instruction";
            return false;
        }
        out = em.LoadVar(em.vgpr_var[vgpr]);
        return true;
    }
    return ResolveScalarSrc(em, ins, enc, out, err);
}

// ---------------------------------------------------------------------------
// SOP1 / SOP2 / SOPK / SOPC lowering (matches the SW executor's set).
// ---------------------------------------------------------------------------
bool LowerSop1(Emitter& em, const Prog& p, uint32_t i, Err& err) {
    const GcnInstruction& ins = p.code[i];
    uint32_t a = 0;
    if (!ResolveScalarSrc(em, ins, ins.src0, a, err)) return false;
    const bool dst_is_scc = (ins.dst == 106);
    if (ins.dst >= GcnSwExecutor::kSgprCount && !dst_is_scc) {
        err.code = ComputeCompileError::UnsupportedOperand;
        err.msg = "SOP1 sdst out of range";
        return false;
    }
    ++em.alu_ops;
    switch (ins.opcode) {
        case GcnOp::S_MOV_B32:
            if (dst_is_scc) {
                err.code = ComputeCompileError::UnsupportedOperand;
                err.msg = "S_MOV_B32 with sdst=SCC is not modelled";
                return false;
            }
            em.StoreVar(em.sgpr_var[ins.dst], a);
            return true;
        case GcnOp::S_MOV_B64:
            em.StoreVar(em.sgpr_var[ins.dst], a);
            em.StoreVar(em.sgpr_var[ins.dst + 1], em.c_uint0);
            return true;   // documented simplification: hi = 0 (SW parity)
        case GcnOp::S_CMOV_B32: {
            if (em.scc_var == 0) {
                err.code = ComputeCompileError::UninitializedRegister;
                err.msg = "S_CMOV_B32 without an SCC producer";
                return false;
            }
            const uint32_t scc = em.LoadBoolVar(em.scc_var);
            const uint32_t old = em.LoadVar(em.sgpr_var[ins.dst]);
            const uint32_t sel = em.ids.Allocate();
            Emit(em.body, OpSelect, {em.t_uint, sel, scc, a, old});
            em.StoreVar(em.sgpr_var[ins.dst], sel);
            return true;
        }
        case GcnOp::S_NOT_B32: {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpNot, {em.t_uint, r, a});
            em.StoreVar(em.sgpr_var[ins.dst], r);
            if (em.scc_var != 0) {
                const uint32_t nz = em.ids.Allocate();
                Emit(em.body, OpINotEqual, {em.t_bool, nz, a, em.c_uint0});
                Emit(em.body, OpStore, {em.scc_var, nz});
            }
            return true;
        }
        case GcnOp::S_BREV_B32: {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpBitReverse, {em.t_uint, r, a});
            em.StoreVar(em.sgpr_var[ins.dst], r);
            return true;
        }
        case GcnOp::S_SEXT_I32_I8: {
            const uint32_t shl = em.ids.Allocate();
            Emit(em.body, OpShiftLeftLogical,
                 {em.t_uint, shl, a, em.UintConst(24)});
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpShiftRightArithmetic,
                 {em.t_uint, r, shl, em.UintConst(24)});
            em.StoreVar(em.sgpr_var[ins.dst], r);
            return true;
        }
        case GcnOp::S_SEXT_I32_I16: {
            const uint32_t shl = em.ids.Allocate();
            Emit(em.body, OpShiftLeftLogical,
                 {em.t_uint, shl, a, em.UintConst(16)});
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpShiftRightArithmetic,
                 {em.t_uint, r, shl, em.UintConst(16)});
            em.StoreVar(em.sgpr_var[ins.dst], r);
            return true;
        }
        case GcnOp::S_ABS_I32: {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpExtInst, {em.t_uint, r, em.glsl, GlslSAbs, a});
            em.StoreVar(em.sgpr_var[ins.dst], r);
            return true;
        }
        default:
            --em.alu_ops;
            err.code = ComputeCompileError::UnsupportedOpcode;
            err.msg = std::string("unsupported SOP1 opcode ") +
                      GcnDecoder::Mnemonic(ins);
            return false;
    }
}

// Common SOP2 tail: store the result (SGPR or SCC) + optional SCC flag.
bool StoreSopResult(Emitter& em, const GcnInstruction& ins, uint32_t result,
                    uint32_t scc_bool, Err& err) {
    const bool dst_is_scc = (ins.dst == 106);
    if (dst_is_scc) {
        if (em.scc_var == 0) {
            err.code = ComputeCompileError::UnsupportedOperand;
            err.msg = "SOP sdst=SCC without SCC variable";
            return false;
        }
        Emit(em.body, OpStore, {em.scc_var, scc_bool});
        return true;
    }
    if (ins.dst >= GcnSwExecutor::kSgprCount) {
        err.code = ComputeCompileError::UnsupportedOperand;
        err.msg = "SOP sdst out of range";
        return false;
    }
    em.StoreVar(em.sgpr_var[ins.dst], result);
    if (em.scc_var != 0 && scc_bool != 0) {
        Emit(em.body, OpStore, {em.scc_var, scc_bool});
    }
    return true;
}

uint32_t UintCmp(Emitter& em, uint16_t op, uint32_t a, uint32_t b) {
    const uint32_t r = em.ids.Allocate();
    Emit(em.body, op, {em.t_bool, r, a, b});
    return r;
}

bool LowerSop2(Emitter& em, const Prog& p, uint32_t i, Err& err) {
    const GcnInstruction& ins = p.code[i];
    uint32_t a = 0, b = 0;
    if (!ResolveScalarSrc(em, ins, ins.src0, a, err)) return false;
    if (!ResolveScalarSrc(em, ins, ins.src1, b, err)) return false;
    ++em.alu_ops;
    auto bin = [&](uint16_t op) {
        const uint32_t r = em.ids.Allocate();
        Emit(em.body, op, {em.t_uint, r, a, b});
        return r;
    };
    switch (ins.opcode) {
        case GcnOp::S_ADD_U32:
        case GcnOp::S_ADD_I32: {
            const uint32_t r = bin(OpIAdd);
            const uint32_t carry = UintCmp(em, OpULessThan, r, a);
            return StoreSopResult(em, ins, r, carry, err);
        }
        case GcnOp::S_SUB_U32:
        case GcnOp::S_SUB_I32: {
            const uint32_t r = bin(OpISub);
            const uint32_t borrow = UintCmp(em, OpULessThan, a, b);
            return StoreSopResult(em, ins, r, borrow, err);
        }
        case GcnOp::S_MIN_U32: {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpExtInst, {em.t_uint, r, em.glsl, GlslUMin, a, b});
            return StoreSopResult(em, ins, r,
                                  UintCmp(em, OpULessThan, a, b), err);
        }
        case GcnOp::S_MAX_U32: {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpExtInst, {em.t_uint, r, em.glsl, GlslUMax, a, b});
            return StoreSopResult(em, ins, r,
                                  UintCmp(em, OpUGreaterThan, a, b), err);
        }
        case GcnOp::S_MIN_I32: {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpExtInst, {em.t_uint, r, em.glsl, GlslSMin, a, b});
            return StoreSopResult(em, ins, r,
                                  UintCmp(em, OpSLessThan, a, b), err);
        }
        case GcnOp::S_MAX_I32: {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpExtInst, {em.t_uint, r, em.glsl, GlslSMax, a, b});
            return StoreSopResult(em, ins, r,
                                  UintCmp(em, OpSGreaterThan, a, b), err);
        }
        case GcnOp::S_CSELECT_B32: {
            if (em.scc_var == 0) {
                err.code = ComputeCompileError::UninitializedRegister;
                err.msg = "S_CSELECT_B32 without an SCC producer";
                return false;
            }
            const uint32_t scc = em.LoadBoolVar(em.scc_var);
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpSelect, {em.t_uint, r, scc, a, b});
            em.StoreVar(em.sgpr_var[ins.dst], r);
            return true;
        }
        case GcnOp::S_AND_B32: {
            const uint32_t r = bin(OpBitwiseAnd);
            return StoreSopResult(em, ins, r,
                                  UintCmp(em, OpINotEqual, r, em.c_uint0), err);
        }
        case GcnOp::S_OR_B32: {
            const uint32_t r = bin(OpBitwiseOr);
            return StoreSopResult(em, ins, r,
                                  UintCmp(em, OpINotEqual, r, em.c_uint0), err);
        }
        case GcnOp::S_XOR_B32: {
            const uint32_t r = bin(OpBitwiseXor);
            return StoreSopResult(em, ins, r,
                                  UintCmp(em, OpINotEqual, r, em.c_uint0), err);
        }
        case GcnOp::S_ANDN2_B32: {
            const uint32_t nb = em.ids.Allocate();
            Emit(em.body, OpNot, {em.t_uint, nb, b});
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpBitwiseAnd, {em.t_uint, r, a, nb});
            return StoreSopResult(em, ins, r,
                                  UintCmp(em, OpINotEqual, r, em.c_uint0), err);
        }
        case GcnOp::S_ORN2_B32: {
            const uint32_t nb = em.ids.Allocate();
            Emit(em.body, OpNot, {em.t_uint, nb, b});
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpBitwiseOr, {em.t_uint, r, a, nb});
            return StoreSopResult(em, ins, r,
                                  UintCmp(em, OpINotEqual, r, em.c_uint0), err);
        }
        case GcnOp::S_NAND_B32: {
            const uint32_t ab = em.ids.Allocate();
            Emit(em.body, OpBitwiseAnd, {em.t_uint, ab, a, b});
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpNot, {em.t_uint, r, ab});
            return StoreSopResult(em, ins, r,
                                  UintCmp(em, OpINotEqual, r, em.c_uint0), err);
        }
        case GcnOp::S_NOR_B32: {
            const uint32_t ab = em.ids.Allocate();
            Emit(em.body, OpBitwiseOr, {em.t_uint, ab, a, b});
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpNot, {em.t_uint, r, ab});
            return StoreSopResult(em, ins, r,
                                  UintCmp(em, OpINotEqual, r, em.c_uint0), err);
        }
        case GcnOp::S_XNOR_B32: {
            const uint32_t ab = em.ids.Allocate();
            Emit(em.body, OpBitwiseXor, {em.t_uint, ab, a, b});
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpNot, {em.t_uint, r, ab});
            return StoreSopResult(em, ins, r,
                                  UintCmp(em, OpINotEqual, r, em.c_uint0), err);
        }
        case GcnOp::S_LSHL_B32: {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpShiftLeftLogical,
                 {em.t_uint, r, a, em.And31(b)});
            return StoreSopResult(em, ins, r, em.c_false, err);
        }
        case GcnOp::S_LSHR_B32: {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpShiftRightLogical,
                 {em.t_uint, r, a, em.And31(b)});
            return StoreSopResult(em, ins, r, em.c_false, err);
        }
        case GcnOp::S_ASHR_I32: {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpShiftRightArithmetic,
                 {em.t_uint, r, a, em.And31(b)});
            return StoreSopResult(em, ins, r, em.c_false, err);
        }
        case GcnOp::S_MUL_I32: {
            const uint32_t r = bin(OpIMul);
            return StoreSopResult(em, ins, r, 0, err);
        }
        case GcnOp::S_BFM_B32: {
            // width = a & 31, off = b & 31: ((1 << width) - 1) << off
            const uint32_t c1 = em.UintConst(1);
            const uint32_t w = em.And31(a);
            const uint32_t o = em.And31(b);
            const uint32_t m1 = em.ids.Allocate();
            Emit(em.body, OpShiftLeftLogical, {em.t_uint, m1, c1, w});
            const uint32_t m2 = em.ids.Allocate();
            Emit(em.body, OpISub, {em.t_uint, m2, m1, c1});
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpShiftLeftLogical, {em.t_uint, r, m2, o});
            return StoreSopResult(em, ins, r, 0, err);
        }
        default:
            --em.alu_ops;
            err.code = ComputeCompileError::UnsupportedOpcode;
            err.msg = std::string("unsupported SOP2 opcode ") +
                      GcnDecoder::Mnemonic(ins);
            return false;
    }
}

bool LowerSopk(Emitter& em, const Prog& p, uint32_t i, Err& err) {
    const GcnInstruction& ins = p.code[i];
    const uint32_t simm =
        static_cast<uint32_t>(static_cast<int32_t>(
            static_cast<int16_t>(ins.simm16)));
    const uint32_t cimm = em.UintConst(simm);
    if (ins.dst >= GcnSwExecutor::kSgprCount && ins.dst != 106) {
        err.code = ComputeCompileError::UnsupportedOperand;
        err.msg = "SOPK sdst out of range";
        return false;
    }
    ++em.alu_ops;
    switch (ins.opcode) {
        case GcnOp::S_MOVK_I32:
            em.StoreVar(em.sgpr_var[ins.dst], cimm);
            return true;
        case GcnOp::S_CMOVK_I32: {
            if (em.scc_var == 0) {
                err.code = ComputeCompileError::UninitializedRegister;
                err.msg = "S_CMOVK_I32 without an SCC producer";
                return false;
            }
            const uint32_t scc = em.LoadBoolVar(em.scc_var);
            const uint32_t old = em.LoadVar(em.sgpr_var[ins.dst]);
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpSelect, {em.t_uint, r, scc, cimm, old});
            em.StoreVar(em.sgpr_var[ins.dst], r);
            return true;
        }
        case GcnOp::S_ADDK_I32: {
            const uint32_t old = em.LoadVar(em.sgpr_var[ins.dst]);
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpIAdd, {em.t_uint, r, old, cimm});
            em.StoreVar(em.sgpr_var[ins.dst], r);
            return true;
        }
        case GcnOp::S_MULK_I32: {
            const uint32_t old = em.LoadVar(em.sgpr_var[ins.dst]);
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpIMul, {em.t_uint, r, old, cimm});
            em.StoreVar(em.sgpr_var[ins.dst], r);
            return true;
        }
        case GcnOp::S_CMPK_EQ_I32:
        case GcnOp::S_CMPK_LG_I32:
        case GcnOp::S_CMPK_GT_I32:
        case GcnOp::S_CMPK_LT_I32:
        case GcnOp::S_CMPK_EQ_U32:
        case GcnOp::S_CMPK_LG_U32:
        case GcnOp::S_CMPK_GT_U32:
        case GcnOp::S_CMPK_LT_U32: {
            if (em.scc_var == 0) {
                err.code = ComputeCompileError::UnsupportedOperand;
                err.msg = "S_CMPK without SCC";
                return false;
            }
            const uint32_t dst = em.LoadVar(em.sgpr_var[ins.dst]);
            uint32_t r = 0;
            switch (ins.opcode) {
                case GcnOp::S_CMPK_EQ_I32:
                    r = UintCmp(em, OpIEqual, dst, cimm); break;
                case GcnOp::S_CMPK_LG_I32:
                    r = UintCmp(em, OpINotEqual, dst, cimm); break;
                case GcnOp::S_CMPK_GT_I32:
                    r = UintCmp(em, OpSGreaterThan, dst, cimm); break;
                case GcnOp::S_CMPK_LT_I32:
                    r = UintCmp(em, OpSLessThan, dst, cimm); break;
                case GcnOp::S_CMPK_EQ_U32:
                    r = UintCmp(em, OpIEqual, dst, cimm); break;
                case GcnOp::S_CMPK_LG_U32:
                    r = UintCmp(em, OpINotEqual, dst, cimm); break;
                case GcnOp::S_CMPK_GT_U32:
                    r = UintCmp(em, OpUGreaterThan, dst, cimm); break;
                default:
                    r = UintCmp(em, OpULessThan, dst, cimm); break;
            }
            Emit(em.body, OpStore, {em.scc_var, r});
            return true;
        }
        default:
            --em.alu_ops;
            err.code = ComputeCompileError::UnsupportedOpcode;
            err.msg = std::string("unsupported SOPK opcode ") +
                      GcnDecoder::Mnemonic(ins);
            return false;
    }
}

bool LowerSopc(Emitter& em, const Prog& p, uint32_t i, Err& err) {
    const GcnInstruction& ins = p.code[i];
    uint32_t a = 0, b = 0;
    if (!ResolveScalarSrc(em, ins, ins.src0, a, err)) return false;
    if (!ResolveScalarSrc(em, ins, ins.src1, b, err)) return false;
    if (em.scc_var == 0) {
        err.code = ComputeCompileError::UnsupportedOperand;
        err.msg = "SOPC without SCC";
        return false;
    }
    ++em.alu_ops;
    uint32_t r = 0;
    switch (ins.opcode) {
        case GcnOp::S_CMP_EQ_U32: r = UintCmp(em, OpIEqual, a, b); break;
        case GcnOp::S_CMP_LG_U32: r = UintCmp(em, OpINotEqual, a, b); break;
        case GcnOp::S_CMP_GT_U32: r = UintCmp(em, OpUGreaterThan, a, b); break;
        case GcnOp::S_CMP_GE_U32:
            r = UintCmp(em, OpUGreaterThanEqual, a, b); break;
        case GcnOp::S_CMP_LT_U32: r = UintCmp(em, OpULessThan, a, b); break;
        case GcnOp::S_CMP_LE_U32:
            r = UintCmp(em, OpULessThanEqual, a, b); break;
        case GcnOp::S_CMP_EQ_I32: r = UintCmp(em, OpIEqual, a, b); break;
        case GcnOp::S_CMP_LG_I32: r = UintCmp(em, OpINotEqual, a, b); break;
        case GcnOp::S_CMP_GT_I32: r = UintCmp(em, OpSGreaterThan, a, b); break;
        case GcnOp::S_CMP_GE_I32:
            r = UintCmp(em, OpSGreaterThanEqual, a, b); break;
        case GcnOp::S_CMP_LT_I32: r = UintCmp(em, OpSLessThan, a, b); break;
        case GcnOp::S_CMP_LE_I32:
            r = UintCmp(em, OpSLessThanEqual, a, b); break;
        default:
            --em.alu_ops;
            err.code = ComputeCompileError::UnsupportedOpcode;
            err.msg = std::string("unsupported SOPC opcode ") +
                      GcnDecoder::Mnemonic(ins);
            return false;
    }
    Emit(em.body, OpStore, {em.scc_var, r});
    return true;
}

// ---------------------------------------------------------------------------
// VOP1 lowering (float ops in float space; everything through bitcasts).
// ---------------------------------------------------------------------------
bool LowerVop1(Emitter& em, const Prog& p, uint32_t i, Err& err) {
    const GcnInstruction& ins = p.code[i];
    if (ins.opcode == GcnOp::V_NOP) {
        return true;
    }
    uint32_t s = 0;
    if (!ResolveSrc9(em, ins, ins.src0, s, err)) return false;
    if (ins.dst >= GcnSwExecutor::kVgprCount) {
        err.code = ComputeCompileError::UnsupportedOperand;
        err.msg = "VOP1 vdst out of range";
        return false;
    }
    if (ins.opcode == GcnOp::V_MOV_B32) {
        em.StoreVar(em.vgpr_var[ins.dst], s);
        return true;
    }
    ++em.alu_ops;
    auto f = [&]() { return em.ToFloat(s); };
    auto finst = [&](uint32_t glsl_op) {
        const uint32_t r = em.ids.Allocate();
        Emit(em.body, OpExtInst, {em.t_float, r, em.glsl, glsl_op, f()});
        return r;
    };
    auto back = [&](uint32_t fr) { return em.ToUint(fr); };
    uint32_t result = 0;
    switch (ins.opcode) {
        case GcnOp::V_CVT_F32_I32: {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpConvertSToF, {em.t_float, r, s});
            result = back(r);
            break;
        }
        case GcnOp::V_CVT_F32_U32: {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpConvertUToF, {em.t_float, r, s});
            result = back(r);
            break;
        }
        case GcnOp::V_CVT_I32_F32: {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpConvertFToS, {em.t_uint, r, f()});
            result = r;
            break;
        }
        case GcnOp::V_CVT_U32_F32: {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpConvertFToU, {em.t_uint, r, f()});
            result = r;
            break;
        }
        case GcnOp::V_CVT_RPI_I32_F32: {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpConvertFToS, {em.t_uint, r, finst(GlslRoundEven)});
            result = r;
            break;
        }
        case GcnOp::V_CVT_FLR_I32_F32: {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpConvertFToS, {em.t_uint, r, finst(GlslFloor)});
            result = r;
            break;
        }
        case GcnOp::V_FLOOR_F32: result = back(finst(GlslFloor)); break;
        case GcnOp::V_CEIL_F32:  result = back(finst(GlslCeil)); break;
        case GcnOp::V_TRUNC_F32: result = back(finst(GlslTrunc)); break;
        case GcnOp::V_RNDNE_F32: result = back(finst(GlslRoundEven)); break;
        case GcnOp::V_FRACT_F32: result = back(finst(GlslFract)); break;
        case GcnOp::V_SQRT_F32:  result = back(finst(GlslSqrt)); break;
        case GcnOp::V_RCP_F32: {
            const uint32_t one = em.ToFloat(em.UintConst(0x3f800000u));
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpFDiv, {em.t_float, r, one, f()});
            result = back(r);
            break;
        }
        case GcnOp::V_RSQ_F32: result = back(finst(GlslInverseSqrt)); break;
        // GCN semantics: v_log_f32 = log2, v_exp_f32 = exp2 (the round-10
        // lowering used the natural log/exp ext-insts and disagreed with
        // the software executor -- fixed here).
        case GcnOp::V_LOG_F32: result = back(finst(GlslLog2)); break;
        case GcnOp::V_EXP_F32: result = back(finst(GlslExp2)); break;
        case GcnOp::V_SIN_F32: result = back(finst(GlslSin)); break;
        case GcnOp::V_COS_F32: result = back(finst(GlslCos)); break;
        case GcnOp::V_NOT_B32: {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpNot, {em.t_uint, r, s});
            result = r;
            break;
        }
        case GcnOp::V_BFREV_B32: {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpBitReverse, {em.t_uint, r, s});
            result = r;
            break;
        }
        default:
            --em.alu_ops;
            err.code = ComputeCompileError::UnsupportedOpcode;
            err.msg = std::string("unsupported VOP1 opcode ") +
                      GcnDecoder::Mnemonic(ins);
            return false;
    }
    em.StoreVar(em.vgpr_var[ins.dst], result);
    return true;
}

// ---------------------------------------------------------------------------
// VOP2 lowering. src1 is the 8-bit VGPR index field.
// ---------------------------------------------------------------------------
bool LowerVop2(Emitter& em, const Prog& p, uint32_t i, Err& err) {
    const GcnInstruction& ins = p.code[i];
    uint32_t s0 = 0, s1 = 0;
    if (!ResolveSrc9(em, ins, ins.src0, s0, err)) return false;
    const uint32_t vsrc1 = ins.src1;   // 8-bit VGPR index
    if (vsrc1 >= GcnSwExecutor::kVgprCount || em.vgpr_var[vsrc1] == 0) {
        err.code = ComputeCompileError::UninitializedRegister;
        err.msg = "VOP2 VSRC1 VGPR v" + std::to_string(vsrc1) +
                  " is not defined by an earlier instruction";
        return false;
    }
    s1 = em.LoadVar(em.vgpr_var[vsrc1]);
    if (ins.dst >= GcnSwExecutor::kVgprCount) {
        err.code = ComputeCompileError::UnsupportedOperand;
        err.msg = "VOP2 vdst out of range";
        return false;
    }
    if (ins.opcode == GcnOp::V_CNDMASK_B32) {
        if (em.vcc_var == 0) {
            err.code = ComputeCompileError::UninitializedRegister;
            err.msg = "V_CNDMASK_B32 without a VCC producer";
            return false;
        }
        const uint32_t vcc = em.LoadBoolVar(em.vcc_var);
        const uint32_t r = em.ids.Allocate();
        Emit(em.body, OpSelect, {em.t_uint, r, vcc, s1, s0});
        em.StoreVar(em.vgpr_var[ins.dst], r);
        ++em.alu_ops;
        return true;
    }

    ++em.alu_ops;
    auto f0 = [&]() { return em.ToFloat(s0); };
    auto f1 = [&]() { return em.ToFloat(s1); };
    auto fbin = [&](uint16_t op) {
        const uint32_t r = em.ids.Allocate();
        Emit(em.body, op, {em.t_float, r, f0(), f1()});
        return r;
    };
    auto finst = [&](uint32_t glsl_op) {
        const uint32_t r = em.ids.Allocate();
        Emit(em.body, OpExtInst, {em.t_float, r, em.glsl, glsl_op, f0(), f1()});
        return r;
    };
    auto ibin = [&](uint16_t op) {
        const uint32_t r = em.ids.Allocate();
        Emit(em.body, op, {em.t_uint, r, s0, s1});
        return r;
    };
    uint32_t result = 0;
    switch (ins.opcode) {
        case GcnOp::V_ADD_F32:    result = em.ToUint(fbin(OpFAdd)); break;
        case GcnOp::V_SUB_F32:    result = em.ToUint(fbin(OpFSub)); break;
        case GcnOp::V_SUBREV_F32: {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpFSub, {em.t_float, r, f1(), f0()});
            result = em.ToUint(r);
            break;
        }
        case GcnOp::V_MUL_F32:    result = em.ToUint(fbin(OpFMul)); break;
        case GcnOp::V_MUL_U32_U24: {
            const uint32_t mask = em.UintConst(0xFFFFFFu);
            const uint32_t a = em.ids.Allocate();
            Emit(em.body, OpBitwiseAnd, {em.t_uint, a, s0, mask});
            const uint32_t b = em.ids.Allocate();
            Emit(em.body, OpBitwiseAnd, {em.t_uint, b, s1, mask});
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpIMul, {em.t_uint, r, a, b});
            result = r;
            break;
        }
        case GcnOp::V_MIN_F32: result = em.ToUint(finst(GlslFMin)); break;
        case GcnOp::V_MAX_F32: result = em.ToUint(finst(GlslFMax)); break;
        case GcnOp::V_MIN_I32: {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpExtInst, {em.t_uint, r, em.glsl, GlslSMin, s0, s1});
            result = r;
            break;
        }
        case GcnOp::V_MAX_I32: {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpExtInst, {em.t_uint, r, em.glsl, GlslSMax, s0, s1});
            result = r;
            break;
        }
        case GcnOp::V_MIN_U32: {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpExtInst, {em.t_uint, r, em.glsl, GlslUMin, s0, s1});
            result = r;
            break;
        }
        case GcnOp::V_MAX_U32: {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpExtInst, {em.t_uint, r, em.glsl, GlslUMax, s0, s1});
            result = r;
            break;
        }
        case GcnOp::V_AND_B32: result = ibin(OpBitwiseAnd); break;
        case GcnOp::V_OR_B32:  result = ibin(OpBitwiseOr); break;
        case GcnOp::V_XOR_B32: result = ibin(OpBitwiseXor); break;
        case GcnOp::V_LSHLREV_B32: {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpShiftLeftLogical,
                 {em.t_uint, r, s1, em.And31(s0)});
            result = r;
            break;
        }
        case GcnOp::V_LSHRREV_B32: {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpShiftRightLogical,
                 {em.t_uint, r, s1, em.And31(s0)});
            result = r;
            break;
        }
        case GcnOp::V_ASHRREV_I32: {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpShiftRightArithmetic,
                 {em.t_uint, r, s1, em.And31(s0)});
            result = r;
            break;
        }
        case GcnOp::V_MAC_F32: {
            const uint32_t acc = em.LoadVar(em.vgpr_var[ins.dst]);
            const uint32_t mul = em.ids.Allocate();
            Emit(em.body, OpFMul, {em.t_float, mul, f0(), f1()});
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpFAdd, {em.t_float, r, mul, em.ToFloat(acc)});
            result = em.ToUint(r);
            break;
        }
        default:
            --em.alu_ops;
            err.code = ComputeCompileError::UnsupportedOpcode;
            err.msg = std::string("unsupported VOP2 opcode ") +
                      GcnDecoder::Mnemonic(ins);
            return false;
    }
    em.StoreVar(em.vgpr_var[ins.dst], result);
    return true;
}

// ---------------------------------------------------------------------------
// VOPC lowering: v_cmp_* -> VCC (single lane bit, documented).
// ---------------------------------------------------------------------------
bool LowerVopc(Emitter& em, const Prog& p, uint32_t i, Err& err) {
    const GcnInstruction& ins = p.code[i];
    uint32_t s0 = 0;
    if (!ResolveSrc9(em, ins, ins.src0, s0, err)) return false;
    const uint32_t vsrc1 = ins.src1;
    if (vsrc1 >= GcnSwExecutor::kVgprCount || em.vgpr_var[vsrc1] == 0) {
        err.code = ComputeCompileError::UninitializedRegister;
        err.msg = "VOPC VSRC1 VGPR not defined";
        return false;
    }
    const uint32_t s1 = em.LoadVar(em.vgpr_var[vsrc1]);
    ++em.alu_ops;
    uint32_t r = 0;
    const bool is_f = (ins.opcode >= 0x01 && ins.opcode <= 0x06);
    if (is_f) {
        const uint32_t f0 = em.ToFloat(s0);
        const uint32_t f1 = em.ToFloat(s1);
        r = em.ids.Allocate();
        uint16_t op = OpFOrdLessThan;
        switch (ins.opcode) {
            case 0x01: op = OpFOrdLessThan; break;          // v_cmp_lt_f32
            case 0x02: op = OpFOrdEqual; break;             // v_cmp_eq_f32
            case 0x03: op = OpFOrdLessThanEqual; break;     // v_cmp_le_f32
            case 0x04: op = OpFOrdGreaterThan; break;       // v_cmp_gt_f32
            case 0x05: op = OpFOrdNotEqual; break;          // v_cmp_lg_f32
            case 0x06: op = OpFOrdGreaterThanEqual; break;  // v_cmp_ge_f32
        }
        Emit(em.body, op, {em.t_bool, r, f0, f1});
    } else {
        uint16_t op = OpIEqual;
        switch (ins.opcode) {
            case 0x81: op = OpSLessThan; break;             // v_cmp_lt_i32
            case 0x82: op = OpIEqual; break;                // v_cmp_eq_i32
            case 0x83: op = OpSLessThanEqual; break;        // v_cmp_le_i32
            case 0x84: op = OpSGreaterThan; break;          // v_cmp_gt_i32
            case 0x86: op = OpSGreaterThanEqual; break;     // v_cmp_ge_i32
            case 0xC1: op = OpULessThan; break;             // v_cmp_lt_u32
            case 0xC2: op = OpIEqual; break;                // v_cmp_eq_u32
            case 0xC3: op = OpULessThanEqual; break;        // v_cmp_le_u32
            case 0xC4: op = OpUGreaterThan; break;          // v_cmp_gt_u32
            case 0xC6: op = OpUGreaterThanEqual; break;     // v_cmp_ge_u32
            default:
                --em.alu_ops;
                err.code = ComputeCompileError::UnsupportedOpcode;
                err.msg = std::string("unsupported VOPC opcode 0x") +
                          [] (uint32_t v) {
                              char buf[16];
                              std::snprintf(buf, sizeof(buf), "%x", v);
                              return std::string(buf);
                          }(ins.opcode);
                return false;
        }
        r = em.ids.Allocate();
        Emit(em.body, op, {em.t_bool, r, s0, s1});
    }
    Emit(em.body, OpStore, {em.vcc_var, r});
    return true;
}

// ---------------------------------------------------------------------------
// VOP3 lowering (dual-dword, real modifiers: neg/abs per source, omod/clamp
// on the float result -- identical semantics to the SW executor).
// ---------------------------------------------------------------------------
bool LowerVop3(Emitter& em, const Prog& p, uint32_t i, Err& err) {
    const GcnInstruction& ins = p.code[i];
    if (ins.dst >= GcnSwExecutor::kVgprCount) {
        err.code = ComputeCompileError::UnsupportedOperand;
        err.msg = "VOP3 vdst out of range";
        return false;
    }
    uint32_t s0 = 0, s1 = 0, s2 = 0;
    if (!ResolveSrc9(em, ins, ins.src0, s0, err)) return false;
    if (!ResolveSrc9(em, ins, ins.src1, s1, err)) return false;
    if (!ResolveSrc9(em, ins, ins.src2, s2, err)) return false;

    // Float modifier chain: abs then neg (SW apply_f32_mods order).
    auto fmod = [&](uint32_t v, bool neg, bool abs) {
        uint32_t f = em.ToFloat(v);
        if (abs) {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpExtInst, {em.t_float, r, em.glsl, GlslFAbs, f});
            f = r;
        }
        if (neg) {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpFNegate, {em.t_float, r, f});
            f = r;
        }
        return f;
    };
    // Integer "neg" is a bitwise NOT (SW apply_int_mods).
    auto imod = [&](uint32_t v, bool neg) {
        if (!neg) return v;
        const uint32_t r = em.ids.Allocate();
        Emit(em.body, OpNot, {em.t_uint, r, v});
        return r;
    };
    // omod + clamp applied to a float result.
    auto finish_f = [&](uint32_t f) {
        if (ins.omod == 1 || ins.omod == 2 || ins.omod == 3) {
            const uint32_t scale_bits =
                ins.omod == 1 ? 0x40000000u :   // 2.0
                ins.omod == 2 ? 0x40800000u :   // 4.0
                               0x3f000000u;     // 0.5
            const uint32_t c = em.ToFloat(em.UintConst(scale_bits));
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpFMul, {em.t_float, r, f, c});
            f = r;
        }
        if (ins.clamp) {
            const uint32_t lo = em.ToFloat(em.c_uint0);
            const uint32_t hi = em.ToFloat(em.UintConst(0x3f800000u));
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpExtInst, {em.t_float, r, em.glsl, GlslFClamp, f,
                                      lo, hi});
            f = r;
        }
        return f;
    };

    ++em.alu_ops;
    uint32_t result = 0;
    switch (ins.opcode) {
        case GcnOp::V_MAD_F32: {
            const uint32_t a = fmod(s0, ins.neg0, ins.abs0);
            const uint32_t b = fmod(s1, ins.neg1, ins.abs1);
            const uint32_t c = fmod(s2, ins.neg2, ins.abs2);
            const uint32_t mul = em.ids.Allocate();
            Emit(em.body, OpFMul, {em.t_float, mul, a, b});
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpFAdd, {em.t_float, r, mul, c});
            result = em.ToUint(finish_f(r));
            break;
        }
        case GcnOp::V_FMA_F32: {
            const uint32_t a = fmod(s0, ins.neg0, ins.abs0);
            const uint32_t b = fmod(s1, ins.neg1, ins.abs1);
            const uint32_t c = fmod(s2, ins.neg2, ins.abs2);
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpExtInst, {em.t_float, r, em.glsl, GlslFma, a, b, c});
            result = em.ToUint(finish_f(r));
            break;
        }
        case GcnOp::V_MAD_U32_U24: {
            const uint32_t mask = em.UintConst(0xFFFFFFu);
            uint32_t a0 = imod(s0, ins.neg0);
            uint32_t b0 = imod(s1, ins.neg1);
            const uint32_t am = em.ids.Allocate();
            Emit(em.body, OpBitwiseAnd, {em.t_uint, am, a0, mask});
            const uint32_t bm = em.ids.Allocate();
            Emit(em.body, OpBitwiseAnd, {em.t_uint, bm, b0, mask});
            const uint32_t mul = em.ids.Allocate();
            Emit(em.body, OpIMul, {em.t_uint, mul, am, bm});
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpIAdd, {em.t_uint, r, mul, imod(s2, ins.neg2)});
            result = r;
            break;
        }
        case GcnOp::V_MIN3_F32: {
            const uint32_t a = fmod(s0, ins.neg0, ins.abs0);
            const uint32_t b = fmod(s1, ins.neg1, ins.abs1);
            const uint32_t c = fmod(s2, ins.neg2, ins.abs2);
            const uint32_t m1 = em.ids.Allocate();
            Emit(em.body, OpExtInst, {em.t_float, m1, em.glsl, GlslFMin, a, b});
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpExtInst, {em.t_float, r, em.glsl, GlslFMin, m1, c});
            result = em.ToUint(finish_f(r));
            break;
        }
        case GcnOp::V_MAX3_F32: {
            const uint32_t a = fmod(s0, ins.neg0, ins.abs0);
            const uint32_t b = fmod(s1, ins.neg1, ins.abs1);
            const uint32_t c = fmod(s2, ins.neg2, ins.abs2);
            const uint32_t m1 = em.ids.Allocate();
            Emit(em.body, OpExtInst, {em.t_float, m1, em.glsl, GlslFMax, a, b});
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpExtInst, {em.t_float, r, em.glsl, GlslFMax, m1, c});
            result = em.ToUint(finish_f(r));
            break;
        }
        case GcnOp::V_MED3_I32: {
            const uint32_t a = imod(s0, ins.neg0);
            const uint32_t b = imod(s1, ins.neg1);
            const uint32_t c = imod(s2, ins.neg2);
            const uint32_t lo = em.ids.Allocate();
            Emit(em.body, OpExtInst, {em.t_uint, lo, em.glsl, GlslSMin, a, b});
            const uint32_t lo2 = em.ids.Allocate();
            Emit(em.body, OpExtInst, {em.t_uint, lo2, em.glsl, GlslSMin, lo, c});
            const uint32_t hi = em.ids.Allocate();
            Emit(em.body, OpExtInst, {em.t_uint, hi, em.glsl, GlslSMax, a, b});
            const uint32_t hi2 = em.ids.Allocate();
            Emit(em.body, OpExtInst, {em.t_uint, hi2, em.glsl, GlslSMax, hi, c});
            const uint32_t sum = em.ids.Allocate();
            Emit(em.body, OpIAdd, {em.t_uint, sum, a, b});
            const uint32_t sum2 = em.ids.Allocate();
            Emit(em.body, OpIAdd, {em.t_uint, sum2, sum, c});
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpISub, {em.t_uint, r, sum2, lo2});
            const uint32_t r2 = em.ids.Allocate();
            Emit(em.body, OpISub, {em.t_uint, r2, r, hi2});
            result = r2;
            break;
        }
        case GcnOp::V_MUL_LO_U32: {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpIMul, {em.t_uint, r, s0, s1});
            result = r;
            break;
        }
        case GcnOp::V_MUL_HI_U32: {
            // (u64)s0 * s1 >> 32, truncated back to u32.
            const uint32_t a64 = em.ids.Allocate();
            Emit(em.body, OpUConvert, {em.t_ulong, a64, s0});
            const uint32_t b64 = em.ids.Allocate();
            Emit(em.body, OpUConvert, {em.t_ulong, b64, s1});
            const uint32_t m64 = em.ids.Allocate();
            Emit(em.body, OpIMul, {em.t_ulong, m64, a64, b64});
            const uint32_t sh = em.ids.Allocate();
            Emit(em.body, OpShiftRightLogical,
                 {em.t_ulong, sh, m64, em.c_ulong32});
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpUConvert, {em.t_uint, r, sh});
            result = r;
            break;
        }
        case GcnOp::V_BFE_U32: {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpBitFieldUExtract, {em.t_uint, r, s0, s1, s2});
            result = r;
            break;
        }
        case GcnOp::V_BCNT_U32_B32: {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpBitCount, {em.t_uint, r, s0});
            result = r;
            break;
        }
        case GcnOp::V_BFM_B32: {
            const uint32_t c1 = em.UintConst(1);
            const uint32_t w = em.And31(s0);
            const uint32_t o = em.And31(s1);
            const uint32_t m1 = em.ids.Allocate();
            Emit(em.body, OpShiftLeftLogical, {em.t_uint, m1, c1, w});
            const uint32_t m2 = em.ids.Allocate();
            Emit(em.body, OpISub, {em.t_uint, m2, m1, c1});
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpShiftLeftLogical, {em.t_uint, r, m2, o});
            result = r;
            break;
        }
        case GcnOp::V_LDEXP_F32: {
            const uint32_t a = fmod(s0, ins.neg0, ins.abs0);
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpExtInst, {em.t_float, r, em.glsl, GlslLdexp, a, s1});
            result = em.ToUint(finish_f(r));
            break;
        }
        case GcnOp::V_LSHL_OR_B32: {
            const uint32_t sh = em.ids.Allocate();
            Emit(em.body, OpShiftLeftLogical,
                 {em.t_uint, sh, s0, em.And31(s1)});
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpBitwiseOr, {em.t_uint, r, sh, s2});
            result = r;
            break;
        }
        case GcnOp::V_AND_OR_B32: {
            const uint32_t a = em.ids.Allocate();
            Emit(em.body, OpBitwiseAnd, {em.t_uint, a, s0, s1});
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpBitwiseOr, {em.t_uint, r, a, s2});
            result = r;
            break;
        }
        case GcnOp::V_ADD_CO_U32: {
            const uint32_t r = em.ids.Allocate();
            Emit(em.body, OpIAdd, {em.t_uint, r, s0, s1});
            result = r;   // carry in sdst not modelled (SW parity)
            break;
        }
        case GcnOp::V_SUB_CO_U32:
        case GcnOp::V_SUBREV_CO_U32: {
            const uint32_t r = em.ids.Allocate();
            if (ins.opcode == GcnOp::V_SUB_CO_U32) {
                Emit(em.body, OpISub, {em.t_uint, r, s0, s1});
            } else {
                Emit(em.body, OpISub, {em.t_uint, r, s1, s0});
            }
            result = r;
            break;
        }
        default:
            --em.alu_ops;
            err.code = ComputeCompileError::UnsupportedOpcode;
            err.msg = std::string("unsupported VOP3 opcode ") +
                      GcnDecoder::Mnemonic(ins);
            return false;
    }
    em.StoreVar(em.vgpr_var[ins.dst], result);
    return true;
}

// ---------------------------------------------------------------------------
// SSBO element access helpers (runtime index).
// ---------------------------------------------------------------------------
bool BufferLoadIntoRegs(Emitter& em, uint32_t var, uint32_t index_id,
                        uint32_t first_reg, bool is_sgpr, int count,
                        Err& err) {
    for (int d = 0; d < count; ++d) {
        uint32_t index = index_id;
        if (d > 0) {
            index = em.ids.Allocate();
            Emit(em.body, OpIAdd,
                 {em.t_uint, index, index_id, em.UintConst(static_cast<uint32_t>(d))});
        }
        const uint32_t ptr = em.ids.Allocate();
        Emit(em.body, OpAccessChain,
             {em.t_ptr_uni_uint, ptr, var, em.c_uint0, index});
        const uint32_t val = em.ids.Allocate();
        Emit(em.body, OpLoad, {em.t_uint, val, ptr});
        const uint32_t reg = first_reg + static_cast<uint32_t>(d);
        if (is_sgpr) {
            if (reg >= GcnSwExecutor::kSgprCount || em.sgpr_var[reg] == 0) {
                err.code = ComputeCompileError::UnsupportedOperand;
                err.msg = "SMEM sdst register out of range";
                return false;
            }
            em.StoreVar(em.sgpr_var[reg], val);
        } else {
            if (reg >= GcnSwExecutor::kVgprCount || em.vgpr_var[reg] == 0) {
                err.code = ComputeCompileError::UnsupportedOperand;
                err.msg = "MUBUF vdata register out of range";
                return false;
            }
            em.StoreVar(em.vgpr_var[reg], val);
        }
    }
    ++em.memory_ops;
    return true;
}

bool BufferStoreFromRegs(Emitter& em, uint32_t var, uint32_t index_id,
                         uint32_t first_reg, int count, Err& err) {
    for (int d = 0; d < count; ++d) {
        uint32_t index = index_id;
        if (d > 0) {
            index = em.ids.Allocate();
            Emit(em.body, OpIAdd,
                 {em.t_uint, index, index_id, em.UintConst(static_cast<uint32_t>(d))});
        }
        const uint32_t reg = first_reg + static_cast<uint32_t>(d);
        if (reg >= GcnSwExecutor::kVgprCount || em.vgpr_var[reg] == 0) {
            err.code = ComputeCompileError::UnsupportedOperand;
            err.msg = "MUBUF vdata register out of range";
            return false;
        }
        const uint32_t val = em.LoadVar(em.vgpr_var[reg]);
        const uint32_t ptr = em.ids.Allocate();
        Emit(em.body, OpAccessChain,
             {em.t_ptr_uni_uint, ptr, var, em.c_uint0, index});
        Emit(em.body, OpStore, {ptr, val});
    }
    ++em.memory_ops;
    return true;
}

// ---------------------------------------------------------------------------
// SMEM: S_LOAD_DWORD[X2/X4/X8] from the scalar-segment mirror.
//   index = ((sgpr[sbase] | sgpr[sbase+1]<<32) - mirror_base) >> 2
// (the soffset-offset form uses the immediate field -- the same documented
// simplification the software executor makes).
// ---------------------------------------------------------------------------
bool LowerSmem(Emitter& em, const Prog& p, uint32_t i, Err& err) {
    const GcnInstruction& ins = p.code[i];
    const int count =
        ins.opcode == GcnOp::S_LOAD_DWORD ? 1 :
        ins.opcode == GcnOp::S_LOAD_DWORDX2 ? 2 :
        ins.opcode == GcnOp::S_LOAD_DWORDX4 ? 4 :
        ins.opcode == GcnOp::S_LOAD_DWORDX8 ? 8 :
        ins.opcode == GcnOp::S_BUFFER_LOAD_DWORD ? 1 :
        ins.opcode == GcnOp::S_BUFFER_LOAD_DWORDX2 ? 2 :
        ins.opcode == GcnOp::S_BUFFER_LOAD_DWORDX4 ? 4 : 0;
    if (count == 0) {
        err.code = ComputeCompileError::UnsupportedOpcode;
        err.msg = "unsupported SMEM opcode";
        return false;
    }
    if (ins.opcode == GcnOp::S_BUFFER_LOAD_DWORD ||
        ins.opcode == GcnOp::S_BUFFER_LOAD_DWORDX2 ||
        ins.opcode == GcnOp::S_BUFFER_LOAD_DWORDX4) {
        if (ins.sbase < 4 || ins.sbase / 4u > p.opt.buffers.size()) {
            err.code = ComputeCompileError::UnsupportedOperand;
            err.msg = "SMEM buffer descriptor out of range";
            return false;
        }
        const size_t desc_index = ins.sbase / 4u - 1u;
        const auto& buf = p.opt.buffers[desc_index];
        const uint64_t bytes = static_cast<uint64_t>(count) * 4u;
        if (ins.smem_offset > static_cast<uint64_t>(buf.size_dwords) * 4ull ||
            bytes > static_cast<uint64_t>(buf.size_dwords) * 4ull - ins.smem_offset) {
            err.code = ComputeCompileError::UnsupportedOperand;
            err.msg = "SMEM buffer load exceeds descriptor size";
            return false;
        }
        return BufferLoadIntoRegs(em, em.var_buf[desc_index],
                                  em.UintConst(ins.smem_offset / 4u),
                                  ins.dst, true, count, err);
    }
    auto load_sgpr = [&](uint32_t idx) {
        return em.sgpr_var[idx] != 0 ? em.LoadVar(em.sgpr_var[idx])
                                     : em.c_uint0;
    };
    const uint32_t lo = load_sgpr(ins.sbase);
    const uint32_t hi = load_sgpr(ins.sbase + 1);

    // u64 address = hi<<32 | lo. The mirror base comes from the ROUND-19
    // push-constant block { uint base_lo; uint base_hi; } (the executor
    // pushes the actual window base at dispatch time -- the value is no
    // longer baked into the module, so one module serves any window).
    const uint32_t lo64 = em.ids.Allocate();
    Emit(em.body, OpUConvert, {em.t_ulong, lo64, lo});
    const uint32_t hi64 = em.ids.Allocate();
    Emit(em.body, OpUConvert, {em.t_ulong, hi64, hi});
    const uint32_t hi_sh = em.ids.Allocate();
    Emit(em.body, OpShiftLeftLogical, {em.t_ulong, hi_sh, hi64, em.c_ulong32});
    const uint32_t addr64 = em.ids.Allocate();
    Emit(em.body, OpBitwiseOr, {em.t_ulong, addr64, hi_sh, lo64});

    auto push_member = [&](uint32_t member, uint32_t const_id) {
        const uint32_t ptr = em.ids.Allocate();
        Emit(em.body, OpAccessChain,
             {em.t_ptr_push_struct, ptr, em.var_push, const_id});
        const uint32_t val = em.ids.Allocate();
        Emit(em.body, OpLoad, {em.t_uint, val, ptr});
        (void)member;
        return val;
    };
    const uint32_t base_lo = push_member(0, em.c_uint0);
    const uint32_t base_hi = push_member(1, em.UintConst(1u));

    const uint32_t blo64 = em.ids.Allocate();
    Emit(em.body, OpUConvert, {em.t_ulong, blo64, base_lo});
    const uint32_t bhi64 = em.ids.Allocate();
    Emit(em.body, OpUConvert, {em.t_ulong, bhi64, base_hi});
    const uint32_t bhi_sh = em.ids.Allocate();
    Emit(em.body, OpShiftLeftLogical,
         {em.t_ulong, bhi_sh, bhi64, em.c_ulong32});
    const uint32_t base64 = em.ids.Allocate();
    Emit(em.body, OpBitwiseOr, {em.t_ulong, base64, bhi_sh, blo64});

    const uint32_t off64 = em.ids.Allocate();
    Emit(em.body, OpISub, {em.t_ulong, off64, addr64, base64});
    const uint32_t off_sh = em.ids.Allocate();
    Emit(em.body, OpShiftRightLogical, {em.t_ulong, off_sh, off64, em.c_ulong32});
    const uint32_t index = em.ids.Allocate();
    Emit(em.body, OpUConvert, {em.t_uint, index, off_sh});

    em.used_mirror = true;
    return BufferLoadIntoRegs(em, em.var_mirror, index, ins.dst, true, count,
                              err);
}

// ---------------------------------------------------------------------------
// MUBUF: per-descriptor SSBO routing.
//   byte address = buf_offset + (offen ? vaddr*4 : idxen ? vaddr*stride*4 : 0)
//   index = address >> 2   (descriptor i's SSBO mirrors its guest buffer
//   contents from base_gva; hardware semantics: no bounds check).
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Round 28: MIMG lowering (image sample / fetch / gather / write / atomics).
//
// Resource mapping: image i is selected by srsrc = 4*(i+1) -- the same
// "descriptor quad index - 1" convention MUBUF uses -- so the software
// executor (whose guest seeds sgpr[srsrc] = i) and this compiler agree on
// which descriptor a program addresses.
//
// Semantics notes (kept bit-exact vs GcnSwExecutor):
//   * every image is 2D RGBA32UI and MIMG moves RAW dwords, so the module
//     samples a UINT image with a NEAREST sampler (no filtering -> the
//     sample result equals the fetched texel);
//   * IMAGE_SAMPLE_[B/D/CL/O/CD/DZ/PCK*] variants that need a filter model
//     beyond nearest/LOD fail closed with an explicit message;
//   * IMAGE_STORE_MIP / GET_LOD fail closed (fragment-only / no mip write
//     path) -- the software executor still runs them.
// ---------------------------------------------------------------------------
uint32_t LoadVgprValue(Emitter& em, uint8_t idx) {
    if (em.vgpr_var[idx] == 0) return em.c_uint0;
    return em.LoadVar(em.vgpr_var[idx]);
}

// Round 28: store a raw uint value id into a VGPR variable (the variable must
// have been marked live by DeclareRegisters).
void StoreVgprRaw(Emitter& em, uint8_t idx, uint32_t value) {
    if (idx >= GcnSwExecutor::kVgprCount || em.vgpr_var[idx] == 0) {
        return;   // fail-safe: unreachable when DeclareRegisters marked it
    }
    em.StoreVar(em.vgpr_var[idx], value);
}

uint32_t ExtractVecMember(Emitter& em, uint32_t vec_id, uint32_t comp) {
    const uint32_t r = em.ids.Allocate();
    Emit(em.body, OpCompositeExtract, {em.t_uint, r, vec_id, comp});
    return r;
}

bool LowerMimg(Emitter& em, const Prog& p, uint32_t i, Err& err) {
    const GcnInstruction& ins = p.code[i];

    if (p.opt.images.empty()) {
        err.code = ComputeCompileError::UnsupportedOperand;
        err.dword = ins.raw0;
        err.msg = "MIMG needs the image-resource table (options.images)";
        return false;
    }
    if (ins.srsrc < 4 || ins.srsrc % 4 != 0 ||
        ins.srsrc / 4 > p.opt.images.size()) {
        err.code = ComputeCompileError::UnsupportedOperand;
        err.dword = ins.raw0;
        err.msg = "MIMG image descriptor out of range (srsrc=" +
                  std::to_string(ins.srsrc) + ")";
        return false;
    }
    if (ins.a16) {
        err.code = ComputeCompileError::UnsupportedOperand;
        err.dword = ins.raw0;
        err.msg = "MIMG a16 (16-bit address components) not supported";
        return false;
    }
    const size_t img_index = ins.srsrc / 4 - 1;
    const uint32_t var_sampled = em.var_img_sampled[img_index];
    const uint32_t var_storage = em.var_img_storage[img_index];
    const uint32_t var_sampler = em.var_samp[img_index];
    ++em.image_ops;

    em.EnsureBlock();

    // ---- helpers -------------------------------------------------------------
    // float coordinate pair (u, v) from vaddr[0..1].
    auto fcoords = [&]() {
        const uint32_t u_bits = LoadVgprValue(em, ins.vaddr);
        const uint32_t v_bits =
            (ins.vaddr + 1u < static_cast<uint32_t>(GcnSwExecutor::kVgprCount))
                ? LoadVgprValue(em, ins.vaddr + 1)
                : em.c_uint0;
        const uint32_t u = em.ids.Allocate();
        Emit(em.body, OpBitcast, {em.t_float, u, u_bits});
        const uint32_t v = em.ids.Allocate();
        Emit(em.body, OpBitcast, {em.t_float, v, v_bits});
        const uint32_t vec = em.ids.Allocate();
        Emit(em.body, OpCompositeConstruct, {em.t_v2float, vec, u, v});
        return vec;
    };
    // uint coordinate pair (x, y) from vaddr[0..1].
    auto ucoords = [&]() {
        const uint32_t x = LoadVgprValue(em, ins.vaddr);
        const uint32_t y =
            (ins.vaddr + 1u < static_cast<uint32_t>(GcnSwExecutor::kVgprCount))
                ? LoadVgprValue(em, ins.vaddr + 1)
                : em.c_uint0;
        const uint32_t vec = em.ids.Allocate();
        Emit(em.body, OpCompositeConstruct, {em.t_v2uint, vec, x, y});
        return vec;
    };
    // scatter a v4uint result into vdata[c] for every set dmask bit.
    auto scatter_dmask = [&](uint32_t vec4) {
        for (uint32_t c = 0; c < 4; ++c) {
            if ((ins.dmask >> c) & 1u) {
                const uint32_t val = (c == 0)
                    ? vec4
                    : ExtractVecMember(em, vec4, c);  // vec4 id reused for c=0
                StoreVgprRaw(em, ins.vdata + static_cast<uint8_t>(c), val);
            }
        }
    };
    // build OpSampledImage from the sampled-image + sampler descriptors.
    auto sampled_image = [&]() {
        const uint32_t image = em.ids.Allocate();
        Emit(em.body, OpLoad, {em.t_img_sampled, image, var_sampled});
        const uint32_t samp = em.ids.Allocate();
        Emit(em.body, OpLoad, {em.t_sampler, samp, var_sampler});
        const uint32_t si = em.ids.Allocate();
        Emit(em.body, OpSampledImage, {em.t_sampled_image, si, image, samp});
        return si;
    };

    switch (ins.opcode) {
        case GcnOp::IMAGE_SAMPLE:
        case GcnOp::IMAGE_SAMPLE_LZ:
        case GcnOp::IMAGE_SAMPLE_L:
        case GcnOp::IMAGE_SAMPLE_B:
        case GcnOp::IMAGE_SAMPLE_D: {
            const uint32_t coords = fcoords();
            const uint32_t si     = sampled_image();
            const uint32_t res    = em.ids.Allocate();
            // NOTE: every variant lowers to OpImageSampleExplicitLod. The
            // implicit-LOD opcode is only well-defined in the Fragment
            // execution model, and the image model here is single-mip (every
            // LOD clamps to 0) -- explicit Lod 0 is therefore bit-identical
            // to the software executor's nearest sample AND valid in compute.
            if (ins.opcode == GcnOp::IMAGE_SAMPLE_L) {
                // Explicit LOD from vaddr[2].
                const uint32_t lod_bits = LoadVgprValue(em, ins.vaddr + 2);
                const uint32_t lod_f = em.ids.Allocate();
                Emit(em.body, OpBitcast, {em.t_float, lod_f, lod_bits});
                Emit(em.body, OpImageSampleExplicitLod,
                     {em.t_v4uint, res, si, coords, ImgLod, lod_f});
            } else if (ins.opcode == GcnOp::IMAGE_SAMPLE_D) {
                // Explicit gradients dpdx/dpdy from vaddr[2..3] (a no-op on a
                // single-mip image, exactly like the software executor).
                const uint32_t dx_bits = LoadVgprValue(em, ins.vaddr + 2);
                const uint32_t dy_bits = LoadVgprValue(em, ins.vaddr + 3);
                const uint32_t dx = em.ids.Allocate();
                Emit(em.body, OpBitcast, {em.t_float, dx, dx_bits});
                const uint32_t dy = em.ids.Allocate();
                Emit(em.body, OpBitcast, {em.t_float, dy, dy_bits});
                Emit(em.body, OpImageSampleExplicitLod,
                     {em.t_v4uint, res, si, coords, ImgGrad, dx, dy});
            } else {
                // SAMPLE / SAMPLE_LZ / SAMPLE_B: LOD 0 (LZ is defined as
                // level zero; B's bias cannot move a single-mip image).
                const uint32_t zero = em.ids.Allocate();
                Emit(em.body, OpBitcast, {em.t_float, zero, em.c_uint0});
                Emit(em.body, OpImageSampleExplicitLod,
                     {em.t_v4uint, res, si, coords, ImgLod, zero});
            }
            scatter_dmask(res);
            ++em.memory_ops;
            return true;
        }
        case GcnOp::IMAGE_GATHER4:
        case GcnOp::IMAGE_GATHER4_LZ: {
            // Gather the component selected by the lowest set dmask bit
            // (GCN dmask picks the gathered component for gather4).
            const uint32_t coords = fcoords();
            const uint32_t si     = sampled_image();
            uint32_t component    = 0;
            for (uint32_t c = 0; c < 4; ++c) {
                if ((ins.dmask >> c) & 1u) {
                    component = c;
                    break;
                }
            }
            const uint32_t res = em.ids.Allocate();
            Emit(em.body, OpImageGather,
                 {em.t_v4uint, res, si, coords, em.UintConst(component)});
            (void)component;
            // Gather writes all four neighbours into consecutive vdata regs
            // (hardware semantics: result = 4 texels regardless of dmask).
            for (uint32_t c = 0; c < 4; ++c) {
                const uint32_t val =
                    (c == 0 ? res : ExtractVecMember(em, res, c));
                StoreVgprRaw(em, ins.vdata + static_cast<uint8_t>(c), val);
            }
            ++em.memory_ops;
            return true;
        }
        case GcnOp::IMAGE_LOAD:
        case GcnOp::IMAGE_LOAD_MIP: {
            const uint32_t coords = ucoords();
            const uint32_t image  = em.ids.Allocate();
            Emit(em.body, OpLoad, {em.t_img_sampled, image, var_sampled});
            uint32_t lod = em.c_uint0;
            if (ins.opcode == GcnOp::IMAGE_LOAD_MIP) {
                lod = LoadVgprValue(em, ins.vaddr + 2);
            }
            const uint32_t res = em.ids.Allocate();
            Emit(em.body, OpImageFetch,
                 {em.t_v4uint, res, image, coords, ImgLod, lod});
            scatter_dmask(res);
            ++em.memory_ops;
            return true;
        }
        case GcnOp::IMAGE_STORE: {
            const uint32_t coords = ucoords();
            const uint32_t image  = em.ids.Allocate();
            Emit(em.body, OpLoad, {em.t_img_storage, image, var_storage});
            uint32_t comps[4] = {em.c_uint0, em.c_uint0, em.c_uint0,
                                 em.c_uint0};
            for (uint32_t c = 0; c < 4; ++c) {
                if ((ins.dmask >> c) & 1u) {
                    comps[c] = LoadVgprValue(em, ins.vdata + c);
                }
            }
            const uint32_t data = em.ids.Allocate();
            Emit(em.body, OpCompositeConstruct,
                 {em.t_v4uint, data, comps[0], comps[1], comps[2], comps[3]});
            Emit(em.body, OpImageWrite, {image, coords, data});
            ++em.memory_ops;
            return true;
        }
        case GcnOp::IMAGE_GET_RESINFO: {
            // Report (width, height, depth=1, mips) at vdata[0..3]; the LOD
            // comes from vaddr[0].
            const uint32_t lod    = LoadVgprValue(em, ins.vaddr);
            const uint32_t image  = em.ids.Allocate();
            Emit(em.body, OpLoad, {em.t_img_sampled, image, var_sampled});
            const uint32_t size   = em.ids.Allocate();
            Emit(em.body, OpImageQuerySizeLod, {em.t_v2uint, size, image, lod});
            const uint32_t w      = ExtractVecMember(em, size, 0);
            const uint32_t h      = ExtractVecMember(em, size, 1);
            const uint32_t vec    = em.ids.Allocate();
            Emit(em.body, OpCompositeConstruct,
                 {em.t_v4uint, vec, w, h, em.c_uint0,
                  em.UintConst(p.opt.images[img_index].mips)});
            scatter_dmask(vec);
            ++em.memory_ops;
            return true;
        }
        case GcnOp::IMAGE_ATOMIC_SWAP:
        case GcnOp::IMAGE_ATOMIC_CMPSWAP:
        case GcnOp::IMAGE_ATOMIC_ADD:
        case GcnOp::IMAGE_ATOMIC_SUB:
        case GcnOp::IMAGE_ATOMIC_SMIN:
        case GcnOp::IMAGE_ATOMIC_UMIN:
        case GcnOp::IMAGE_ATOMIC_SMAX:
        case GcnOp::IMAGE_ATOMIC_UMAX:
        case GcnOp::IMAGE_ATOMIC_AND:
        case GcnOp::IMAGE_ATOMIC_OR:
        case GcnOp::IMAGE_ATOMIC_XOR:
        case GcnOp::IMAGE_ATOMIC_INC:
        case GcnOp::IMAGE_ATOMIC_DEC: {
            const uint32_t coords = ucoords();
            const uint32_t image  = em.ids.Allocate();
            Emit(em.body, OpLoad, {em.t_img_storage, image, var_storage});
            // OpImageTexelPointer: ptr, image, coords, sample (0 for 2D).
            const uint32_t ptr = em.ids.Allocate();
            Emit(em.body, OpImageTexelPointer,
                 {em.t_ptr_uc_img_storage, ptr, var_storage, coords,
                  em.c_uint0});
            const uint32_t src = LoadVgprValue(em, ins.vdata);
            const uint32_t res = em.ids.Allocate();
            uint16_t op        = OpAtomicExchange;
            switch (ins.opcode) {
                case GcnOp::IMAGE_ATOMIC_SWAP: op = OpAtomicExchange; break;
                case GcnOp::IMAGE_ATOMIC_ADD:
                case GcnOp::IMAGE_ATOMIC_INC:  op = OpAtomicIAdd; break;
                case GcnOp::IMAGE_ATOMIC_SUB:
                case GcnOp::IMAGE_ATOMIC_DEC:  op = OpAtomicISub; break;
                case GcnOp::IMAGE_ATOMIC_SMIN: op = OpAtomicSMin; break;
                case GcnOp::IMAGE_ATOMIC_UMIN: op = OpAtomicUMin; break;
                case GcnOp::IMAGE_ATOMIC_SMAX: op = OpAtomicSMax; break;
                case GcnOp::IMAGE_ATOMIC_UMAX: op = OpAtomicUMax; break;
                case GcnOp::IMAGE_ATOMIC_AND:  op = OpAtomicAnd; break;
                case GcnOp::IMAGE_ATOMIC_OR:   op = OpAtomicOr; break;
                case GcnOp::IMAGE_ATOMIC_XOR:  op = OpAtomicXor; break;
                default: break;
            }
            // INC/DEC are +/-1 around the old value.
            uint32_t value = src;
            if (ins.opcode == GcnOp::IMAGE_ATOMIC_INC ||
                ins.opcode == GcnOp::IMAGE_ATOMIC_DEC) {
                value = em.UintConst(1u);
            }
            if (ins.opcode == GcnOp::IMAGE_ATOMIC_CMPSWAP) {
                const uint32_t cmp = LoadVgprValue(em, ins.vdata + 1);
                Emit(em.body, OpAtomicCompareExchange,
                     {em.t_uint, res, ptr, ScopeDevice, SemSeqCst, SemSeqCst,
                      value, cmp});
            } else {
                Emit(em.body, static_cast<Op>(op),
                     {em.t_uint, res, ptr, ScopeDevice, SemSeqCst, value});
            }
            // The OLD value lands in vdata[0] (dmask bit 0 semantics).
            if ((ins.dmask & 1u) != 0u || ins.dmask == 0u) {
                StoreVgprRaw(em, ins.vdata, res);
            }
            ++em.memory_ops;
            return true;
        }
        default:
            err.code = ComputeCompileError::UnsupportedOpcode;
            err.dword = ins.raw0;
            err.msg = std::string("unsupported MIMG opcode ") +
                      GcnDecoder::Mnemonic(ins) +
                      " (hardware path; the software executor may run it)";
            return false;
    }
}

bool LowerMubuf(Emitter& em, const Prog& p, uint32_t i, Err& err) {
    const GcnInstruction& ins = p.code[i];
    const int load_count =
        ins.opcode == GcnOp::BUFFER_LOAD_DWORD ? 1 :
        ins.opcode == GcnOp::BUFFER_LOAD_DWORDX2 ? 2 :
        ins.opcode == GcnOp::BUFFER_LOAD_DWORDX3 ? 3 :
        ins.opcode == GcnOp::BUFFER_LOAD_DWORDX4 ? 4 : 0;
    const int store_count =
        ins.opcode == GcnOp::BUFFER_STORE_DWORD ? 1 :
        ins.opcode == GcnOp::BUFFER_STORE_DWORDX2 ? 2 :
        ins.opcode == GcnOp::BUFFER_STORE_DWORDX4 ? 4 : 0;

    const size_t desc_index = ins.srsrc / 4 - 1;
    const GcnBufferResource& buf = p.opt.buffers[desc_index];
    const uint32_t var = em.var_buf[desc_index];

    uint32_t addr = em.UintConst(ins.buf_offset);
    if (ins.offen || ins.idxen) {
        const uint32_t vaddr = em.vgpr_var[ins.vaddr] != 0
                                   ? em.LoadVar(em.vgpr_var[ins.vaddr])
                                   : em.c_uint0;
        const uint32_t scale = ins.offen
            ? em.UintConst(4u)
            : em.UintConst((buf.stride ? buf.stride : 1u) * 4u);
        const uint32_t scaled = em.ids.Allocate();
        Emit(em.body, OpIMul, {em.t_uint, scaled, vaddr, scale});
        const uint32_t sum = em.ids.Allocate();
        Emit(em.body, OpIAdd, {em.t_uint, sum, addr, scaled});
        addr = sum;
    }
    const uint32_t index = em.ids.Allocate();
    Emit(em.body, OpShiftRightLogical,
         {em.t_uint, index, addr, em.UintConst(2)});

    if (load_count > 0) {
        return BufferLoadIntoRegs(em, var, index, ins.vdata, false,
                                  load_count, err);
    }
    if (store_count > 0) {
        return BufferStoreFromRegs(em, var, index, ins.vdata, store_count,
                                   err);
    }
    err.code = ComputeCompileError::UnsupportedOpcode;
    err.msg = std::string("unsupported MUBUF opcode ") +
              GcnDecoder::Mnemonic(ins);
    return false;
}

// ---------------------------------------------------------------------------
// Structured control flow.
// ---------------------------------------------------------------------------
uint32_t LabelFor(Emitter& em, uint32_t index) {
    auto it = em.label_at.find(index);
    if (it != em.label_at.end()) return it->second;
    const uint32_t lbl = em.NewLabel();
    em.label_at.emplace(index, lbl);
    return lbl;
}

// The branch condition of a SOPP conditional branch, as a bool id plus a
// "swap" flag (true when the branch is taken on cond == false).
bool BranchCondition(Emitter& em, const GcnInstruction& ins, uint32_t& cond,
                     bool& swap, Err& err) {
    swap = false;
    switch (ins.opcode) {
        case GcnOp::S_CBRANCH_SCC1:
            if (em.scc_var == 0) {
                err.code = ComputeCompileError::UninitializedRegister;
                err.msg = "s_cbranch_scc1 without an SCC producer";
                return false;
            }
            cond = em.LoadBoolVar(em.scc_var);
            return true;
        case GcnOp::S_CBRANCH_SCC0:
            if (em.scc_var == 0) {
                err.code = ComputeCompileError::UninitializedRegister;
                err.msg = "s_cbranch_scc0 without an SCC producer";
                return false;
            }
            cond = em.LoadBoolVar(em.scc_var);
            swap = true;
            return true;
        case GcnOp::S_CBRANCH_VCCNZ:
            if (em.vcc_var == 0) {
                err.code = ComputeCompileError::UninitializedRegister;
                err.msg = "s_cbranch_vccnz without a VCC producer";
                return false;
            }
            cond = em.LoadBoolVar(em.vcc_var);
            return true;
        case GcnOp::S_CBRANCH_VCCZ:
            if (em.vcc_var == 0) {
                err.code = ComputeCompileError::UninitializedRegister;
                err.msg = "s_cbranch_vccz without a VCC producer";
                return false;
            }
            cond = em.LoadBoolVar(em.vcc_var);
            swap = true;
            return true;
        default:
            err.code = ComputeCompileError::UnsupportedOpcode;
            err.msg = "unhandled branch opcode";
            return false;
    }
}

// Lower the if / if-else pattern rooted at the conditional branch `i`
// (target = its instruction index). On success the open block is the merge.
bool LowerIf(Emitter& em, const Prog& p, uint32_t i, uint32_t cond,
             bool swap, uint32_t target, uint32_t end, Err& err) {
    // Detect the classic else pattern: the then-arm's last instruction is a
    // bare s_branch to a common join E >= target.
    uint32_t then_begin = i + 1;
    uint32_t then_end = target;
    uint32_t else_begin = 0, else_end = 0;
    uint32_t merge = target;
    if (target > i + 1) {
        const GcnInstruction& prev = p.code[target - 1];
        if (prev.format == GcnFormat::SOPP &&
            prev.opcode == GcnOp::S_BRANCH) {
            const int64_t e = prev.branch_target;
            if (e < 0 || static_cast<uint32_t>(e) > end) {
                err.code = ComputeCompileError::UnstructuredControlFlow;
                err.msg = "if-else join escapes the enclosing region";
                return false;
            }
            then_end = target - 1;
            else_begin = target;
            else_end = static_cast<uint32_t>(e);
            merge = static_cast<uint32_t>(e);
        }
    }

    const uint32_t merge_label = LabelFor(em, merge);
    const uint32_t then_label = em.NewLabel();
    const uint32_t else_label =
        (else_begin != else_end) ? em.NewLabel() : merge_label;

    em.EnsureBlock();
    Emit(em.body, OpSelectionMerge, {merge_label});
    if (swap) {
        Emit(em.body, OpBranchConditional,
             {cond, else_label, then_label});
    } else {
        Emit(em.body, OpBranchConditional,
             {cond, then_label, else_label});
    }
    em.block_open = false;

    // then-arm
    Emit(em.body, OpLabel, {then_label});
    em.block_open = true;
    if (!LowerRegion(em, p, then_begin, then_end, err)) return false;
    if (em.block_open) em.BranchTo(merge_label);

    // else-arm
    if (else_begin != else_end) {
        Emit(em.body, OpLabel, {else_label});
        em.block_open = true;
        if (!LowerRegion(em, p, else_begin, else_end, err)) return false;
        if (em.block_open) em.BranchTo(merge_label);
    }

    // merge block (open; the caller continues here)
    Emit(em.body, OpLabel, {merge_label});
    em.block_open = true;
    ++em.branches;
    return true;
}

// Lower a do-while loop whose header is `hdr` and back edge is `back`
// (the conditional branch at `back`). Returns with the open block at the
// loop exit.
bool LowerLoop(Emitter& em, const Prog& p, uint32_t hdr, uint32_t back,
               Err& err) {
    const GcnInstruction& branch = p.code[back];
    uint32_t cond = 0;
    bool swap = false;
    if (!BranchCondition(em, branch, cond, swap, err)) return false;

    const uint32_t hdr_label = LabelFor(em, hdr);
    const uint32_t body_label = em.NewLabel();
    const uint32_t cont_label = em.NewLabel();
    const uint32_t exit_label = LabelFor(em, back + 1);

    if (em.block_open) {
        em.BranchTo(hdr_label);
    }
    Emit(em.body, OpLabel, {hdr_label});
    em.block_open = true;
    Emit(em.body, OpLoopMerge, {exit_label, cont_label});
    em.BranchTo(body_label);

    Emit(em.body, OpLabel, {body_label});
    em.block_open = true;
    if (!LowerRegionInner(em, p, hdr, back, true, err)) return false;
    if (em.block_open) em.BranchTo(cont_label);

    // continue target: the back-edge test
    Emit(em.body, OpLabel, {cont_label});
    em.block_open = true;
    if (swap) {
        Emit(em.body, OpBranchConditional, {cond, exit_label, hdr_label});
    } else {
        Emit(em.body, OpBranchConditional, {cond, hdr_label, exit_label});
    }
    em.block_open = false;

    Emit(em.body, OpLabel, {exit_label});
    em.block_open = true;
    ++em.branches;
    return true;
}

// Walk instructions [begin, end) linearly; nested constructs recurse.
bool LowerRegionInner(Emitter& em, const Prog& p, uint32_t begin, uint32_t end,
                     bool skip_header_at_begin, Err& err);

bool LowerRegion(Emitter& em, const Prog& p, uint32_t begin, uint32_t end,
                 Err& err) {
    return LowerRegionInner(em, p, begin, end, false, err);
}

bool LowerRegionInner(Emitter& em, const Prog& p, uint32_t begin, uint32_t end,
                     bool skip_header_at_begin, Err& err) {
    uint32_t i = begin;
    while (i < end) {
        const GcnInstruction& ins = p.code[i];

        // Loop header? (detected in the pre-scan: backward branch target).
        // The body walk of LowerLoop starts AT the header instruction, so it
        // must not re-enter the loop construct for its own header.
        auto lh = p.loop_headers.find(i);
        if (lh != p.loop_headers.end() && (i > begin || !skip_header_at_begin)) {
            const uint32_t back = lh->second;
            if (back >= end) {
                err.code = ComputeCompileError::UnstructuredControlFlow;
                err.msg = "loop back-edge escapes the enclosing region";
                return false;
            }
            if (!LowerLoop(em, p, i, back, err)) return false;
            i = back + 1;
            continue;
        }

        if (ins.format == GcnFormat::SOPP) {
            switch (ins.opcode) {
                case GcnOp::S_NOP:
                case GcnOp::S_WAITCNT:
                    ++i;
                    continue;
                case GcnOp::S_BARRIER: {
                    // Real execution barrier (no memory ordering).
                    em.EnsureBlock();
                    Emit(em.body, OpControlBarrier,
                         {ScopeWorkgroup, ScopeWorkgroup, 0U});
                    ++i;
                    continue;
                }
                case GcnOp::S_CBRANCH_EXECZ:
                    // EXEC is all-ones in this lane model: never taken.
                    ++i;
                    continue;
                case GcnOp::S_CBRANCH_EXECNZ: {
                    // EXEC is all-ones: an unconditional forward jump; the
                    // skipped range is dead code (fail-closed if targeted).
                    const int64_t t = ins.branch_target;
                    if (t < 0 || static_cast<uint32_t>(t) > end) {
                        err.code = ComputeCompileError::UnstructuredControlFlow;
                        err.msg = "s_cbranch_execnz escapes the region";
                        return false;
                    }
                    if (static_cast<uint32_t>(t) <= i + 1) {
                        err.code = ComputeCompileError::UnstructuredControlFlow;
                        err.msg = "s_cbranch_execnz backward jump";
                        return false;
                    }
                    i = static_cast<uint32_t>(t);
                    continue;
                }
                case GcnOp::S_BRANCH: {
                    const int64_t t = ins.branch_target;
                    if (t < 0 || static_cast<uint32_t>(t) > end) {
                        err.code = ComputeCompileError::UnstructuredControlFlow;
                        err.msg = "bare s_branch escapes the enclosing region";
                        return false;
                    }
                    if (static_cast<uint32_t>(t) <= i) {
                        err.code = ComputeCompileError::UnstructuredControlFlow;
                        err.msg = "backward s_branch (unstructured loop)";
                        return false;
                    }
                    i = static_cast<uint32_t>(t);   // dead-code skip
                    continue;
                }
                case GcnOp::S_CBRANCH_SCC0:
                case GcnOp::S_CBRANCH_SCC1:
                case GcnOp::S_CBRANCH_VCCZ:
                case GcnOp::S_CBRANCH_VCCNZ: {
                    const int64_t t = ins.branch_target;
                    if (t < 0 || static_cast<uint32_t>(t) > end) {
                        err.code = ComputeCompileError::UnstructuredControlFlow;
                        err.msg = "conditional branch escapes the enclosing "
                                  "region (early exit past a merge)";
                        return false;
                    }
                    const uint32_t target = static_cast<uint32_t>(t);
                    if (target == i + 1) {
                        ++i;   // branch to the next instruction: no-op
                        continue;
                    }
                    if (target <= i) {
                        // A backward edge not consumed by its loop header.
                        err.code = ComputeCompileError::UnstructuredControlFlow;
                        err.msg = "backward branch to a non-header "
                                  "(unstructured)";
                        return false;
                    }
                    uint32_t cond = 0;
                    bool swap = false;
                    if (!BranchCondition(em, ins, cond, swap, err)) {
                        return false;
                    }
                    if (!LowerIf(em, p, i, cond, swap, target, end, err)) {
                        return false;
                    }
                    i = LowerIfResumeIndex(p, i, target);
                    continue;
                }
                default:
                    err.code = ComputeCompileError::UnsupportedOpcode;
                    err.msg = std::string("unsupported SOPP opcode ") +
                              GcnDecoder::Mnemonic(ins);
                    return false;
            }
        }

        if (!LowerAlu(em, p, i, err)) return false;
        ++em.insn_count;
        ++i;
    }
    return true;
}

// Where the walker resumes after an if rooted at `i` with branch target
// `target`: at the merge (target, or the else-join E).
uint32_t LowerIfResumeIndex(const Prog& p, uint32_t i, uint32_t target) {
    if (target > i + 1) {
        const GcnInstruction& prev = p.code[target - 1];
        if (prev.format == GcnFormat::SOPP &&
            prev.opcode == GcnOp::S_BRANCH) {
            const int64_t e = prev.branch_target;
            if (e >= 0 && static_cast<uint32_t>(e) > target) {
                return static_cast<uint32_t>(e);
            }
        }
    }
    return target;
}

// ALU / memory dispatch.
bool LowerAlu(Emitter& em, const Prog& p, uint32_t i, Err& err) {
    const GcnInstruction& ins = p.code[i];
    switch (ins.format) {
        case GcnFormat::SOP1: return LowerSop1(em, p, i, err);
        case GcnFormat::SOP2: return LowerSop2(em, p, i, err);
        case GcnFormat::SOPK: return LowerSopk(em, p, i, err);
        case GcnFormat::SOPC: return LowerSopc(em, p, i, err);
        case GcnFormat::VOP1: return LowerVop1(em, p, i, err);
        case GcnFormat::VOP2: return LowerVop2(em, p, i, err);
        case GcnFormat::VOPC: return LowerVopc(em, p, i, err);
        case GcnFormat::VOP3: return LowerVop3(em, p, i, err);
        case GcnFormat::SMEM: return LowerSmem(em, p, i, err);
        case GcnFormat::MUBUF: return LowerMubuf(em, p, i, err);
        case GcnFormat::MIMG: return LowerMimg(em, p, i, err);
        default:
            err.code = ComputeCompileError::UnsupportedEncoding;
            err.msg = std::string("format not lowerable: ") +
                      ToString(ins.format);
            return false;
    }
}

// ---------------------------------------------------------------------------
// Epilogue: store v0..v{m-1} to out_data[gid*m + j], then return.
// ---------------------------------------------------------------------------
bool EmitEpilogue(Emitter& em, const Prog& p, Err& err) {
    (void)err;
    const uint32_t m_out = p.opt.out_dwords_per_lane;

    // Round 19: the lane index again (the same lane-seeding rule).
    uint32_t gid = 0;
    if (p.opt.emit_vertex_stage) {
        gid = em.ids.Allocate();
        Emit(em.body, OpLoad, {em.t_uint, gid, em.var_vertindex});
    } else {
        const uint32_t gid_ptr = em.ids.Allocate();
        Emit(em.body, OpAccessChain,
             {em.t_ptr_in_uint, gid_ptr, em.var_gid, em.c_uint0});
        gid = em.ids.Allocate();
        Emit(em.body, OpLoad, {em.t_uint, gid, gid_ptr});
    }

    uint32_t lane_base = gid;
    if (m_out > 1U) {
        const uint32_t cm = em.UintConst(m_out);
        const uint32_t base = em.ids.Allocate();
        Emit(em.body, OpIMul, {em.t_uint, base, gid, cm});
        lane_base = base;
    }
    for (uint32_t j = 0; j < m_out; ++j) {
        uint32_t value = em.c_uint0;
        if (em.vgpr_var[j] != 0) {
            value = em.LoadVar(em.vgpr_var[j]);
        }
        uint32_t index = lane_base;
        if (j > 0U) {
            index = em.ids.Allocate();
            Emit(em.body, OpIAdd,
                 {em.t_uint, index, lane_base, em.UintConst(j)});
        }
        const uint32_t ptr = em.ids.Allocate();
        Emit(em.body, OpAccessChain,
             {em.t_ptr_uni_uint, ptr, em.var_out, em.c_uint0, index});
        Emit(em.body, OpStore, {ptr, value});
    }

    // Round 19 vertex mode: ALSO export the rasterizer interface. The
    // software-rasterizer vertex layout is [0..3] clip-space xyzw and
    // [4..7] RGBA (float bits), so v0..v3 -> gl_Position and v4..v7 -> the
    // Location-0 colour out (bitcast uint->float per component; undefined
    // VGPRs read 0.0 -- deterministic, mirroring the zero-initialised files).
    if (p.opt.emit_vertex_stage) {
        auto comp = [&](uint32_t j) -> uint32_t {
            if (em.vgpr_var[j] != 0) {
                const uint32_t bits = em.LoadVar(em.vgpr_var[j]);
                const uint32_t f = em.ids.Allocate();
                Emit(em.body, OpBitcast, {em.t_float, f, bits});
                return f;
            }
            return em.c_float0;
        };
        const uint32_t pos = em.ids.Allocate();
        Emit(em.body, OpCompositeConstruct,
             {em.t_v4float, pos, comp(0), comp(1), comp(2), comp(3)});
        Emit(em.body, OpStore, {em.var_position, pos});
        const uint32_t col = em.ids.Allocate();
        Emit(em.body, OpCompositeConstruct,
             {em.t_v4float, col, comp(4), comp(5), comp(6), comp(7)});
        Emit(em.body, OpStore, {em.var_color_out, col});
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Compile: decode -> validate -> lower -> assemble.
// ---------------------------------------------------------------------------
ComputeCompilationResult RDNA2ComputeCompiler::Compile(
    const uint32_t* bytecode, size_t dwords_count) const {
    if (bytecode == nullptr || dwords_count == 0U) {
        return Fail(ComputeCompileError::InvalidInput, 0,
                    "bytecode must contain at least S_ENDPGM");
    }
    const uint32_t k_in = m_options.in_dwords_per_lane;
    const uint32_t m_out = m_options.out_dwords_per_lane;
    if (k_in == 0U || k_in > 64U || m_out == 0U || m_out > 16U) {
        return Fail(ComputeCompileError::InvalidInput, 0,
                    "in/out dwords per lane must be 1..64 / 1..16");
    }

    // ---- decode ------------------------------------------------------------
    const GcnDecoder decoder;
    Prog prog{m_options, {}, {}, 0, {}};
    prog.code.reserve(dwords_count);
    size_t pc = 0;
    bool found_end = false;
    std::unordered_map<uint32_t, uint32_t> pc_to_index;
    while (pc < dwords_count) {
        GcnInstruction ins;
        if (!decoder.Decode(bytecode, dwords_count, pc, ins)) {
            return Fail(ComputeCompileError::UnsupportedEncoding, pc,
                        "undecodable GFX10 instruction at dword " +
                            std::to_string(pc));
        }
        if (found_end) {
            return Fail(ComputeCompileError::InstructionsAfterEndProgram, pc,
                        "instruction data follows S_ENDPGM");
        }
        pc_to_index.emplace(static_cast<uint32_t>(pc),
                            static_cast<uint32_t>(prog.code.size()));
        if (ins.format == GcnFormat::SOPP &&
            ins.opcode == GcnOp::S_ENDPGM) {
            found_end = true;
            prog.endpgm = static_cast<uint32_t>(prog.code.size());
        }
        prog.code.push_back(ins);
        prog.pc_of.push_back(static_cast<uint32_t>(pc));
        pc += ins.dwords_consumed;
    }
    if (!found_end) {
        return Fail(ComputeCompileError::MissingEndProgram, dwords_count,
                    "instruction stream does not end with S_ENDPGM");
    }

    // Rewrite branch targets (dword offsets -> instruction indices).
    for (auto& ins : prog.code) {
        if (!ins.is_branch && !ins.is_cond_branch) continue;
        const auto it = pc_to_index.find(static_cast<uint32_t>(ins.branch_target));
        if (it == pc_to_index.end()) {
            return Fail(ComputeCompileError::UnmatchedBranchTarget,
                        ins.raw0,
                        "branch target does not land on an instruction "
                        "boundary");
        }
        ins.branch_target = static_cast<int64_t>(it->second);
    }
    // Loop headers: backward conditional/unconditional branch targets.
    for (uint32_t i = 0; i < prog.code.size(); ++i) {
        const auto& ins = prog.code[i];
        if (ins.format != GcnFormat::SOPP) continue;
        if (ins.opcode != GcnOp::S_BRANCH &&
            ins.opcode != GcnOp::S_CBRANCH_SCC0 &&
            ins.opcode != GcnOp::S_CBRANCH_SCC1 &&
            ins.opcode != GcnOp::S_CBRANCH_VCCZ &&
            ins.opcode != GcnOp::S_CBRANCH_VCCNZ) {
            continue;
        }
        const int64_t t = ins.branch_target;
        if (t >= 0 && t < static_cast<int64_t>(prog.code.size()) &&
            static_cast<int64_t>(i) > t) {
            prog.loop_headers[static_cast<uint32_t>(t)] = i;
        }
    }

    // ---- resource validation ------------------------------------------------
    bool any_mubuf = false, any_smem = false, any_mimg = false;
    for (const auto& ins : prog.code) {
        if (ins.format == GcnFormat::MUBUF) any_mubuf = true;
        if (ins.format == GcnFormat::SMEM) any_smem = true;
        if (ins.format == GcnFormat::MIMG) any_mimg = true;
    }
    if (any_mimg) {
        if (m_options.images.empty()) {
            return Fail(ComputeCompileError::UnsupportedOperand, 0,
                        "MIMG needs an image-resource table (options.images)");
        }
        for (const auto& ins : prog.code) {
            if (ins.format != GcnFormat::MIMG) continue;
            if (ins.srsrc < 4 || ins.srsrc % 4 != 0 ||
                ins.srsrc / 4 > m_options.images.size()) {
                return Fail(ComputeCompileError::UnsupportedOperand, ins.raw0,
                            "MIMG image descriptor out of range (srsrc=" +
                                std::to_string(ins.srsrc) + ")");
            }
        }
    }
    if (any_mubuf) {
        if (m_options.buffers.empty()) {
            return Fail(ComputeCompileError::UnsupportedOperand, 0,
                        "MUBUF needs a buffer-resource table (options.buffers)");
        }
        for (const auto& ins : prog.code) {
            if (ins.format != GcnFormat::MUBUF) continue;
            if (ins.srsrc < 4 ||
                ins.srsrc / 4 > m_options.buffers.size()) {
                return Fail(ComputeCompileError::UnsupportedOperand, ins.raw0,
                            "MUBUF descriptor out of range (srsrc=" +
                                std::to_string(ins.srsrc) + ")");
            }
        }
    }
    bool needs_scalar_smem = false;
    for (const auto& ins : prog.code) {
        if (ins.format != GcnFormat::SMEM) continue;
        if (ins.opcode == GcnOp::S_LOAD_DWORD ||
            ins.opcode == GcnOp::S_LOAD_DWORDX2 ||
            ins.opcode == GcnOp::S_LOAD_DWORDX4 ||
            ins.opcode == GcnOp::S_LOAD_DWORDX8) {
            needs_scalar_smem = true;
            break;
        }
    }
    if (needs_scalar_smem && m_options.scalar_mirror_base_gva == 0) {
        return Fail(ComputeCompileError::UnsupportedOperand, 0,
                    "SMEM scalar load needs the scalar-segment mirror "
                    "(options.scalar_mirror_base_gva)");
    }

    // ---- lower ---------------------------------------------------------------
    Emitter em;
    Err err;
    if (!BuildSkeleton(em, prog, any_smem, any_mubuf, err) ||
        !DeclareRegisters(em, prog, err) ||
        !SeedLanes(em, prog, err)) {
        return Fail(err.code, err.dword, err.msg);
    }
    if (!LowerRegion(em, prog, 0, prog.endpgm, err)) {
        return Fail(err.code, err.dword, err.msg);
    }
    // Fall through to the S_ENDPGM block; epilogue stores + return.
    if (em.block_open) {
        const uint32_t end_label = LabelFor(em, prog.endpgm);
        em.BranchTo(end_label);
    }
    const uint32_t end_label = LabelFor(em, prog.endpgm);
    Emit(em.body, OpLabel, {end_label});
    em.block_open = true;
    if (!EmitEpilogue(em, prog, err)) {
        return Fail(err.code, err.dword, err.msg);
    }
    Emit(em.body, OpReturn, {});
    em.block_open = false;

    // ---- assemble -------------------------------------------------------------
    std::vector<uint32_t> m;
    m.reserve(96U + em.deco.size() + em.mod.size() + em.body.size());
    m.insert(m.end(), {kSpirvMagic, kSpirvVersion10, 0U, em.ids.next, 0U});

    Emit(m, OpCapability, {1U}); // Shader
    m.insert(m.end(), {(6U << 16U) | OpExtInstImport, em.glsl,
                       0x4c534c47U, 0x6474732eU, 0x3035342eU, 0U});
    Emit(m, OpMemoryModel, {0U, 1U}); // Logical, GLSL450

    // OpEntryPoint interface. Round 19 fix (latent round-18 defect): for
    // SPIR-V versions before 1.4 the interface may list ONLY Input/Output
    // storage-class variables -- the round-18 module also listed the Uniform
    // SSBOs, which real validators reject (VUID-StandaloneSpirv-OpEntryPoint)
    // and which serves no purpose pre-1.4. The list below is the honest
    // pre-1.4 interface: the built-in invocation id (compute) or the vertex
    // index + Position + colour outs (vertex model).
    std::vector<uint32_t> iface;
    if (m_options.emit_vertex_stage) {
        iface = {em.var_vertindex, em.var_position, em.var_color_out};
    } else {
        iface = {em.var_gid};
    }
    m.push_back((5U + static_cast<uint32_t>(iface.size())) << 16U |
                OpEntryPoint);
    m.push_back(m_options.emit_vertex_stage ? ExecModelVertex
                                            : ExecModelGLCompute);
    m.push_back(em.fn_main);
    m.push_back(0x6e69616dU); // "main"
    m.push_back(0U);
    m.insert(m.end(), iface.begin(), iface.end());
    if (!m_options.emit_vertex_stage) {
        // LocalSize is compute-only; the Vertex model takes vertex-index
        // draws instead (the pipeline supplies the invocation count).
        Emit(m, OpExecutionMode, {em.fn_main, ExecModeLocalSize,
                                  m_options.local_size_x, 1U, 1U});
    }

    m.insert(m.end(), em.deco.begin(), em.deco.end());
    m.insert(m.end(), em.mod.begin(), em.mod.end());
    Emit(m, OpFunction, {em.t_void, em.fn_main, 0U, em.t_fn});
    m.insert(m.end(), em.body.begin(), em.body.end());
    Emit(m, OpFunctionEnd, {});

    ComputeCompilationResult r;
    r.success = true;
    r.spirv = std::move(m);
    r.instruction_count = em.insn_count;
    r.alu_op_count = em.alu_ops;
    r.branch_count = em.branches;
    r.memory_op_count = em.memory_ops;
    r.used_scalar_mirror = em.used_mirror;
    r.buffer_bindings =
        any_mubuf ? static_cast<uint32_t>(em.var_buf.size()) : 0U;
    r.image_bindings = any_mimg
        ? static_cast<uint32_t>(em.var_img_sampled.size()) : 0U;
    r.image_op_count = em.image_ops;
    return r;
}

// Round 19 (phase 2): the passthrough fragment shader. Hand-assembled SPIR-V
// 1.0 (every opcode/enum value verified against KhronosGroup/SPIRV-Headers
// spirv.h): ExecutionModel Fragment + OriginUpperLeft, in vec4 at Location 0
// straight to the Location-0 attachment output. The vertex module's colour
// output feeds the input; interpolation is the fixed-function default the
// hardware rasterizer performs (the software rasterizer's
// perspective-correct barycentric interpolation models the same thing).
std::vector<uint32_t> RDNA2ComputeCompiler::BuildPassthroughFragmentShader() {
    // IDs: 1 void, 2 float, 3 v4float, 4 fn, 5 ptr_in, 6 ptr_out,
    //     7 in_color, 8 out_color, 9 main, 10 entry label, 11 loaded value.
    std::vector<uint32_t> m;
    m.reserve(48);
    m.insert(m.end(), {kSpirvMagic, kSpirvVersion10, 0U, 12U, 0U});
    Emit(m, OpCapability, {1U});                        // Shader
    Emit(m, OpMemoryModel, {0U, 1U});                   // Logical, GLSL450
    // OpEntryPoint Fragment %main "main" %in %out (interface: I/O only).
    m.push_back((5U + 2U) << 16U | OpEntryPoint);
    m.insert(m.end(), {ExecModelFragment, 9U, 0x6e69616dU, 0U, 7U, 8U});
    Emit(m, OpExecutionMode, {9U, ExecModeOriginUpperLeft});
    Emit(m, OpDecorate, {7U, DecLocation, 0U});
    Emit(m, OpDecorate, {8U, DecLocation, 0U});
    Emit(m, OpTypeVoid, {1U});
    Emit(m, OpTypeFloat, {2U, 32U});
    Emit(m, OpTypeVector, {3U, 2U, 4U});
    Emit(m, OpTypeFunction, {4U, 1U});
    Emit(m, OpTypePointer, {5U, StorageInput, 3U});
    Emit(m, OpTypePointer, {6U, StorageOutput, 3U});
    Emit(m, OpVariable, {5U, 7U, StorageInput});
    Emit(m, OpVariable, {6U, 8U, StorageOutput});
    Emit(m, OpFunction, {1U, 9U, 0U, 4U});
    Emit(m, OpLabel, {10U});
    Emit(m, OpLoad, {3U, 11U, 7U});
    Emit(m, OpStore, {8U, 11U});
    Emit(m, OpReturn, {});
    Emit(m, OpFunctionEnd, {});
    return m;
}

} // namespace PS5::GPU
