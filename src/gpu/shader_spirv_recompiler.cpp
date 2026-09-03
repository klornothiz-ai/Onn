#include "gpu/shader_spirv_recompiler.hpp"

#include <initializer_list>
#include <unordered_map>
#include <utility>

namespace PS5::GPU {
namespace {

constexpr uint32_t kSpirvMagic = 0x07230203U;
constexpr uint32_t kSpirvVersion10 = 0x00010000U;

enum SpirvOpcode : uint16_t {
    OpExtInstImport = 11,
    OpExtInst = 12,
    OpMemoryModel = 14,
    OpEntryPoint = 15,
    OpExecutionMode = 16,
    OpCapability = 17,
    OpTypeVoid = 19,
    OpTypeInt = 21,
    OpTypeFloat = 22,
    OpTypeFunction = 33,
    OpConstant = 43,
    OpFunction = 54,
    OpFunctionEnd = 56,
    OpBitcast = 124,
    OpFAdd = 129,
    OpFMul = 133,
    OpLabel = 248,
    OpReturn = 253
};

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

SpirvCompilationResult Failure(SpirvCompileError error, size_t dword,
                               std::string message) {
    SpirvCompilationResult result;
    result.error = error;
    result.error_dword = dword;
    result.message = std::move(message);
    return result;
}

bool DecodeInlineConstant(uint32_t source, uint32_t& bits) {
    if (source >= 128U && source <= 192U) {
        bits = source - 128U;
        return true;
    }
    if (source >= 193U && source <= 208U) {
        bits = 0U - (source - 192U);
        return true;
    }

    switch (source) {
    case 240U: bits = 0x3f000000U; return true; // 0.5f
    case 241U: bits = 0xbf000000U; return true; // -0.5f
    case 242U: bits = 0x3f800000U; return true; // 1.0f
    case 243U: bits = 0xbf800000U; return true; // -1.0f
    case 244U: bits = 0x40000000U; return true; // 2.0f
    case 245U: bits = 0xc0000000U; return true; // -2.0f
    case 246U: bits = 0x40800000U; return true; // 4.0f
    case 247U: bits = 0xc0800000U; return true; // -4.0f
    default: return false;
    }
}

} // namespace

DecodedRDNA2Instruction ShaderSpirvRecompiler::DecodeRDNA2Instruction(uint32_t dword) {
    DecodedRDNA2Instruction decoded;
    decoded.raw_dword = dword;

    // Specific encodings must be checked before VOP2's broad bit-31 test.
    if ((dword & 0xff800000U) == 0xbf800000U) {
        decoded.category = RDNA2OpcodeCategory::SOPP;
        decoded.opcode = (dword >> 16U) & 0x7fU;
        decoded.src0 = dword & 0xffffU;
        decoded.is_terminating =
            decoded.opcode == static_cast<uint32_t>(RDNA2_SOPP_Op::S_ENDPGM);
        return decoded;
    }

    if ((dword & 0xfe000000U) == 0x7e000000U) {
        // Real hardware VOP1 layout (LLVM VOPCe/VOP1e): op(8) at [24:17],
        // vdst(8) at [16:9], src0(9) at [8:0]. (Round 12 fix: the previous
        // decode swapped the op and vdst fields versus the AMD encoding.)
        decoded.category = RDNA2OpcodeCategory::VOP1;
        decoded.opcode = (dword >> 17U) & 0xffU;
        decoded.dst_reg = (dword >> 9U) & 0xffU;
        decoded.src0 = dword & 0x1ffU;
        return decoded;
    }

    if ((dword & 0xfc000000U) == 0xd4000000U) {
        decoded.category = RDNA2OpcodeCategory::VOP3A;
        decoded.opcode = (dword >> 16U) & 0x3ffU;
        decoded.dst_reg = dword & 0xffU;
        return decoded;
    }

    if ((dword & 0x80000000U) == 0U) {
        decoded.category = RDNA2OpcodeCategory::VOP2;
        decoded.opcode = (dword >> 25U) & 0x3fU;
        decoded.dst_reg = (dword >> 17U) & 0xffU;
        decoded.src1 = (dword >> 9U) & 0xffU;
        decoded.src0 = dword & 0x1ffU;
    }

    return decoded;
}

SpirvCompilationResult ShaderSpirvRecompiler::CompileRDNA2ToSpirv(
    const uint32_t* rdna2_bytecode, size_t dwords_count) const {
    if (rdna2_bytecode == nullptr || dwords_count == 0U) {
        return Failure(SpirvCompileError::InvalidInput, 0,
                       "RDNA2 bytecode must contain at least S_ENDPGM");
    }

    IdAllocator ids;
    const uint32_t glsl_ext_inst_id = ids.Allocate();
    const uint32_t void_type_id = ids.Allocate();
    const uint32_t float_type_id = ids.Allocate();
    const uint32_t uint_type_id = ids.Allocate();
    const uint32_t function_type_id = ids.Allocate();
    const uint32_t main_function_id = ids.Allocate();
    const uint32_t entry_label_id = ids.Allocate();

    std::vector<uint32_t> declarations;
    std::vector<uint32_t> body;
    std::unordered_map<uint32_t, uint32_t> constants;
    std::unordered_map<uint32_t, uint32_t> vgpr_values;

    const auto constant_id = [&](uint32_t bits) {
        const auto existing = constants.find(bits);
        if (existing != constants.end()) {
            return existing->second;
        }
        const uint32_t id = ids.Allocate();
        constants.emplace(bits, id);
        Emit(declarations, OpConstant, {uint_type_id, id, bits});
        return id;
    };

    auto read_source = [&](uint32_t source, size_t dword, uint32_t& value_id)
        -> SpirvCompilationResult {
        if (source >= 256U) {
            const uint32_t vgpr = source - 256U;
            const auto value = vgpr_values.find(vgpr);
            if (value == vgpr_values.end()) {
                return Failure(SpirvCompileError::UninitializedRegister, dword,
                               "source VGPR is not defined by an earlier supported instruction");
            }
            value_id = value->second;
            SpirvCompilationResult ok;
            ok.success = true;
            return ok;
        }

        uint32_t bits = 0;
        if (!DecodeInlineConstant(source, bits)) {
            return Failure(SpirvCompileError::UnsupportedOperand, dword,
                           "only VGPR and basic inline-constant sources are supported");
        }
        value_id = constant_id(bits);
        SpirvCompilationResult ok;
        ok.success = true;
        return ok;
    };

    const auto bitcast = [&](uint32_t result_type, uint32_t value) {
        const uint32_t result = ids.Allocate();
        Emit(body, OpBitcast, {result_type, result, value});
        return result;
    };

    bool found_end = false;
    for (size_t i = 0; i < dwords_count; ++i) {
        if (found_end) {
            return Failure(SpirvCompileError::InstructionsAfterEndProgram, i,
                           "instruction data follows S_ENDPGM");
        }

        const auto instruction = DecodeRDNA2Instruction(rdna2_bytecode[i]);
        switch (instruction.category) {
        case RDNA2OpcodeCategory::VOP1: {
            const auto opcode = static_cast<RDNA2_VOP1_Op>(instruction.opcode);
            if (opcode != RDNA2_VOP1_Op::V_MOV_B32 &&
                opcode != RDNA2_VOP1_Op::V_SQRT_F32) {
                return Failure(SpirvCompileError::UnsupportedOpcode, i,
                               "unsupported VOP1 opcode");
            }

            uint32_t source_id = 0;
            auto source_result = read_source(instruction.src0, i, source_id);
            if (!source_result) {
                return source_result;
            }

            if (opcode == RDNA2_VOP1_Op::V_MOV_B32) {
                vgpr_values[instruction.dst_reg] = source_id;
                break;
            }

            const uint32_t float_source = bitcast(float_type_id, source_id);
            const uint32_t float_result = ids.Allocate();
            Emit(body, OpExtInst,
                 {float_type_id, float_result, glsl_ext_inst_id, 31U, float_source});
            vgpr_values[instruction.dst_reg] = bitcast(uint_type_id, float_result);
            break;
        }

        case RDNA2OpcodeCategory::VOP2: {
            const auto opcode = static_cast<RDNA2_VOP2_Op>(instruction.opcode);
            if (opcode != RDNA2_VOP2_Op::V_ADD_F32 &&
                opcode != RDNA2_VOP2_Op::V_MUL_F32) {
                return Failure(SpirvCompileError::UnsupportedOpcode, i,
                               "unsupported VOP2 opcode");
            }

            uint32_t source0_id = 0;
            auto source_result = read_source(instruction.src0, i, source0_id);
            if (!source_result) {
                return source_result;
            }

            const auto source1 = vgpr_values.find(instruction.src1);
            if (source1 == vgpr_values.end()) {
                return Failure(SpirvCompileError::UninitializedRegister, i,
                               "VOP2 VSRC1 VGPR is not defined by an earlier supported instruction");
            }

            const uint32_t float_source0 = bitcast(float_type_id, source0_id);
            const uint32_t float_source1 = bitcast(float_type_id, source1->second);
            const uint32_t float_result = ids.Allocate();
            Emit(body,
                 opcode == RDNA2_VOP2_Op::V_ADD_F32 ? OpFAdd : OpFMul,
                 {float_type_id, float_result, float_source0, float_source1});
            vgpr_values[instruction.dst_reg] = bitcast(uint_type_id, float_result);
            break;
        }

        case RDNA2OpcodeCategory::SOPP:
            if (instruction.opcode != static_cast<uint32_t>(RDNA2_SOPP_Op::S_ENDPGM)) {
                return Failure(SpirvCompileError::UnsupportedOpcode, i,
                               "only S_ENDPGM is supported for SOPP");
            }
            found_end = true;
            break;

        case RDNA2OpcodeCategory::VOP3A:
            return Failure(SpirvCompileError::UnsupportedEncoding, i,
                           "VOP3A instructions are not supported");
        case RDNA2OpcodeCategory::UNKNOWN:
            return Failure(SpirvCompileError::UnsupportedEncoding, i,
                           "unrecognized RDNA2 instruction encoding");
        }
    }

    if (!found_end) {
        return Failure(SpirvCompileError::MissingEndProgram, dwords_count,
                       "instruction stream does not end with S_ENDPGM");
    }

    std::vector<uint32_t> spirv;
    spirv.reserve(64U + declarations.size() + body.size());
    spirv.insert(spirv.end(), {kSpirvMagic, kSpirvVersion10, 0U, ids.next, 0U});

    Emit(spirv, OpCapability, {1U}); // Shader
    spirv.insert(spirv.end(), {
        (6U << 16U) | OpExtInstImport, glsl_ext_inst_id,
        0x4c534c47U, 0x6474732eU, 0x3035342eU, 0U // "GLSL.std.450"
    });
    Emit(spirv, OpMemoryModel, {0U, 1U}); // Logical, GLSL450
    spirv.insert(spirv.end(), {
        (5U << 16U) | OpEntryPoint, 5U, main_function_id,
        0x6e69616dU, 0U // GLCompute, "main"
    });
    Emit(spirv, OpExecutionMode, {main_function_id, 17U, 1U, 1U, 1U});

    Emit(spirv, OpTypeVoid, {void_type_id});
    Emit(spirv, OpTypeFloat, {float_type_id, 32U});
    Emit(spirv, OpTypeInt, {uint_type_id, 32U, 0U});
    Emit(spirv, OpTypeFunction, {function_type_id, void_type_id});
    spirv.insert(spirv.end(), declarations.begin(), declarations.end());

    Emit(spirv, OpFunction,
         {void_type_id, main_function_id, 0U, function_type_id});
    Emit(spirv, OpLabel, {entry_label_id});
    spirv.insert(spirv.end(), body.begin(), body.end());
    Emit(spirv, OpReturn, {});
    Emit(spirv, OpFunctionEnd, {});

    SpirvCompilationResult result;
    result.success = true;
    result.spirv = std::move(spirv);
    return result;
}

std::vector<uint32_t> ShaderSpirvRecompiler::RecompileRDNA2ToSpirv(
    const uint32_t* rdna2_bytecode, size_t dwords_count) const {
    auto result = CompileRDNA2ToSpirv(rdna2_bytecode, dwords_count);
    return result.success ? std::move(result.spirv) : std::vector<uint32_t>{};
}

} // namespace PS5::GPU
