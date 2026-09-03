#include "gpu/shader_spirv_recompiler.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using PS5::GPU::RDNA2OpcodeCategory;
using PS5::GPU::RDNA2_SOPP_Op;
using PS5::GPU::RDNA2_VOP1_Op;
using PS5::GPU::RDNA2_VOP2_Op;
using PS5::GPU::ShaderSpirvRecompiler;
using PS5::GPU::SpirvCompileError;

// Real VOP1 hardware layout: op[24:17], vdst[16:9], src0[8:0].
constexpr uint32_t EncodeVop1(RDNA2_VOP1_Op opcode, uint32_t dst, uint32_t src0) {
    return 0x7e000000U | (static_cast<uint32_t>(opcode) << 17U) |
           (dst << 9U) | src0;
}

constexpr uint32_t EncodeVop2(RDNA2_VOP2_Op opcode, uint32_t dst,
                              uint32_t src1_vgpr, uint32_t src0) {
    return (static_cast<uint32_t>(opcode) << 25U) | (dst << 17U) |
           (src1_vgpr << 9U) | src0;
}

constexpr uint32_t EncodeSopp(RDNA2_SOPP_Op opcode, uint32_t simm16 = 0) {
    return 0xbf800000U | (static_cast<uint32_t>(opcode) << 16U) | simm16;
}

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct ModuleFacts {
    bool capability_shader{false};
    bool memory_model{false};
    bool compute_entry_point{false};
    bool local_size_1{false};
    bool type_void{false};
    bool type_float{false};
    bool type_uint{false};
    bool type_function{false};
    bool function{false};
    bool label{false};
    bool return_instruction{false};
    bool function_end{false};
    bool fadd{false};
    bool fmul{false};
    bool sqrt_ext_inst{false};
};

void AddResultId(uint32_t id, uint32_t bound, uint32_t& max_id,
                 std::unordered_set<uint32_t>& result_ids) {
    Expect(id != 0U, "SPIR-V result ID must not be zero");
    Expect(id < bound, "SPIR-V result ID must be below the header bound");
    Expect(result_ids.insert(id).second, "SPIR-V result ID must be unique");
    if (id > max_id) {
        max_id = id;
    }
}

ModuleFacts ValidateModule(const std::vector<uint32_t>& words) {
    Expect(words.size() >= 5U, "SPIR-V module must contain a header");
    Expect(words[0] == 0x07230203U, "SPIR-V magic is incorrect");
    Expect(words[1] == 0x00010000U, "module must target SPIR-V 1.0");
    Expect(words[3] > 1U, "SPIR-V bound must be nontrivial");
    Expect(words[4] == 0U, "SPIR-V schema word must be zero");

    const uint32_t bound = words[3];
    uint32_t max_id = 0;
    uint32_t entry_point_id = 0;
    std::unordered_set<uint32_t> result_ids;
    ModuleFacts facts;

    size_t offset = 5;
    while (offset < words.size()) {
        const uint32_t first_word = words[offset];
        const uint16_t opcode = static_cast<uint16_t>(first_word & 0xffffU);
        const uint16_t word_count = static_cast<uint16_t>(first_word >> 16U);
        Expect(word_count != 0U, "SPIR-V instruction has zero word count");
        Expect(offset + word_count <= words.size(), "SPIR-V instruction overruns module");

        const auto operand = [&](size_t index) -> uint32_t {
            Expect(index < word_count, "test parser operand out of range");
            return words[offset + index];
        };

        switch (opcode) {
        case 11: // OpExtInstImport
            Expect(word_count >= 3U, "OpExtInstImport is truncated");
            AddResultId(operand(1), bound, max_id, result_ids);
            break;
        case 12: // OpExtInst
            Expect(word_count == 6U, "Sqrt OpExtInst must have one argument");
            AddResultId(operand(2), bound, max_id, result_ids);
            facts.sqrt_ext_inst = operand(4) == 31U;
            break;
        case 14: // OpMemoryModel
            facts.memory_model = word_count == 3U && operand(1) == 0U && operand(2) == 1U;
            break;
        case 15: // OpEntryPoint
            facts.compute_entry_point =
                word_count == 5U && operand(1) == 5U &&
                operand(3) == 0x6e69616dU && operand(4) == 0U;
            entry_point_id = operand(2);
            break;
        case 16: // OpExecutionMode
            facts.local_size_1 =
                word_count == 6U && operand(1) == entry_point_id &&
                operand(2) == 17U && operand(3) == 1U &&
                operand(4) == 1U && operand(5) == 1U;
            break;
        case 17: // OpCapability
            facts.capability_shader = word_count == 2U && operand(1) == 1U;
            break;
        case 19: // OpTypeVoid
            AddResultId(operand(1), bound, max_id, result_ids);
            facts.type_void = true;
            break;
        case 21: // OpTypeInt
            AddResultId(operand(1), bound, max_id, result_ids);
            facts.type_uint = word_count == 4U && operand(2) == 32U && operand(3) == 0U;
            break;
        case 22: // OpTypeFloat
            AddResultId(operand(1), bound, max_id, result_ids);
            facts.type_float = word_count == 3U && operand(2) == 32U;
            break;
        case 33: // OpTypeFunction
            AddResultId(operand(1), bound, max_id, result_ids);
            facts.type_function = true;
            break;
        case 43: // OpConstant
            AddResultId(operand(2), bound, max_id, result_ids);
            break;
        case 54: // OpFunction
            AddResultId(operand(2), bound, max_id, result_ids);
            facts.function = operand(2) == entry_point_id;
            break;
        case 56: // OpFunctionEnd
            facts.function_end = word_count == 1U;
            break;
        case 124: // OpBitcast
        case 129: // OpFAdd
        case 133: // OpFMul
            AddResultId(operand(2), bound, max_id, result_ids);
            facts.fadd = facts.fadd || opcode == 129U;
            facts.fmul = facts.fmul || opcode == 133U;
            break;
        case 248: // OpLabel
            AddResultId(operand(1), bound, max_id, result_ids);
            facts.label = true;
            break;
        case 253: // OpReturn
            facts.return_instruction = word_count == 1U;
            break;
        default:
            throw std::runtime_error("unexpected opcode in deliberately minimal module");
        }

        offset += word_count;
    }

    Expect(offset == words.size(), "SPIR-V parser did not end on a word boundary");
    Expect(bound == max_id + 1U, "SPIR-V bound must equal maximum result ID plus one");
    Expect(facts.capability_shader, "module lacks Shader capability");
    Expect(facts.memory_model, "module lacks Logical GLSL450 memory model");
    Expect(facts.compute_entry_point, "module lacks GLCompute main entry point");
    Expect(facts.local_size_1, "module lacks LocalSize 1 1 1 execution mode");
    Expect(facts.type_void && facts.type_float && facts.type_uint && facts.type_function,
           "module lacks required types");
    Expect(facts.function && facts.label && facts.return_instruction && facts.function_end,
           "module lacks a complete function, label, return, or function end");
    return facts;
}

void TestDecoderPrecedence() {
    Expect(static_cast<uint32_t>(RDNA2_VOP2_Op::V_ADD_F32) == 0x03U &&
               static_cast<uint32_t>(RDNA2_VOP2_Op::V_MUL_F32) == 0x08U &&
               static_cast<uint32_t>(RDNA2_VOP1_Op::V_SQRT_F32) == 0x33U,
           "RDNA2/GFX10 opcode selectors are incorrect");

    const auto vop1 = ShaderSpirvRecompiler::DecodeRDNA2Instruction(
        EncodeVop1(RDNA2_VOP1_Op::V_MOV_B32, 7U, 242U));
    Expect(vop1.category == RDNA2OpcodeCategory::VOP1,
           "VOP1 must be decoded before broad VOP2 encoding");
    Expect(vop1.dst_reg == 7U && vop1.src0 == 242U,
           "VOP1 operands decoded incorrectly");

    const auto sopp = ShaderSpirvRecompiler::DecodeRDNA2Instruction(
        EncodeSopp(RDNA2_SOPP_Op::S_ENDPGM));
    Expect(sopp.category == RDNA2OpcodeCategory::SOPP && sopp.is_terminating,
           "S_ENDPGM decoded incorrectly");
}

void TestMinimalModule() {
    const uint32_t code[] = {EncodeSopp(RDNA2_SOPP_Op::S_ENDPGM)};
    const auto result = ShaderSpirvRecompiler{}.CompileRDNA2ToSpirv(code, 1U);
    Expect(result.success, "S_ENDPGM-only shader must compile");
    ValidateModule(result.spirv);
}

void TestSupportedSubset() {
    const uint32_t code[] = {
        EncodeVop1(RDNA2_VOP1_Op::V_MOV_B32, 0U, 242U), // v0 = 1.0f
        EncodeVop1(RDNA2_VOP1_Op::V_MOV_B32, 1U, 244U), // v1 = 2.0f
        EncodeVop2(RDNA2_VOP2_Op::V_ADD_F32, 2U, 1U, 256U),
        EncodeVop2(RDNA2_VOP2_Op::V_MUL_F32, 3U, 1U, 258U),
        EncodeVop1(RDNA2_VOP1_Op::V_SQRT_F32, 4U, 259U),
        EncodeSopp(RDNA2_SOPP_Op::S_ENDPGM)
    };

    const auto result = ShaderSpirvRecompiler{}.CompileRDNA2ToSpirv(
        code, sizeof(code) / sizeof(code[0]));
    Expect(result.success, "supported RDNA2 subset must compile");
    const auto facts = ValidateModule(result.spirv);
    Expect(facts.fadd && facts.fmul && facts.sqrt_ext_inst,
           "supported float operations were not emitted");
}

void TestUnsupportedOpcodeRejected() {
    const uint32_t code[] = {
        EncodeVop2(RDNA2_VOP2_Op::V_SUB_F32, 0U, 0U, 242U),
        EncodeSopp(RDNA2_SOPP_Op::S_ENDPGM)
    };
    const auto result = ShaderSpirvRecompiler{}.CompileRDNA2ToSpirv(code, 2U);
    Expect(!result.success && result.error == SpirvCompileError::UnsupportedOpcode,
           "unsupported opcode must fail closed");
    Expect(result.error_dword == 0U && result.spirv.empty(),
           "failed compilation must identify its dword and return no module");
    Expect(ShaderSpirvRecompiler{}.RecompileRDNA2ToSpirv(code, 2U).empty(),
           "compatibility API must return no module on failure");

    const uint32_t obsolete_add_selector[] = {
        (1U << 25U) | (0U << 17U) | (0U << 9U) | 242U,
        EncodeSopp(RDNA2_SOPP_Op::S_ENDPGM)
    };
    const auto obsolete_result = ShaderSpirvRecompiler{}.CompileRDNA2ToSpirv(
        obsolete_add_selector, 2U);
    Expect(!obsolete_result.success &&
               obsolete_result.error == SpirvCompileError::UnsupportedOpcode,
           "non-RDNA2 VOP2 selector 1 must not be accepted as V_ADD_F32");
}

void TestUnsupportedInputsRejected() {
    const uint32_t scalar_source[] = {
        EncodeVop1(RDNA2_VOP1_Op::V_MOV_B32, 0U, 1U),
        EncodeSopp(RDNA2_SOPP_Op::S_ENDPGM)
    };
    auto result = ShaderSpirvRecompiler{}.CompileRDNA2ToSpirv(scalar_source, 2U);
    Expect(!result.success && result.error == SpirvCompileError::UnsupportedOperand,
           "unsupported scalar source must be rejected");

    const uint32_t uninitialized_vgpr[] = {
        EncodeVop1(RDNA2_VOP1_Op::V_SQRT_F32, 0U, 256U),
        EncodeSopp(RDNA2_SOPP_Op::S_ENDPGM)
    };
    result = ShaderSpirvRecompiler{}.CompileRDNA2ToSpirv(uninitialized_vgpr, 2U);
    Expect(!result.success && result.error == SpirvCompileError::UninitializedRegister,
           "uninitialized VGPR source must be rejected");

    const uint32_t vop3[] = {0xd4000000U, EncodeSopp(RDNA2_SOPP_Op::S_ENDPGM)};
    result = ShaderSpirvRecompiler{}.CompileRDNA2ToSpirv(vop3, 2U);
    Expect(!result.success && result.error == SpirvCompileError::UnsupportedEncoding,
           "VOP3A must be rejected rather than partially decoded");
}

void TestTerminationChecks() {
    const uint32_t no_end[] = {EncodeVop1(RDNA2_VOP1_Op::V_MOV_B32, 0U, 242U)};
    auto result = ShaderSpirvRecompiler{}.CompileRDNA2ToSpirv(no_end, 1U);
    Expect(!result.success && result.error == SpirvCompileError::MissingEndProgram,
           "missing S_ENDPGM must be rejected");

    const uint32_t trailing[] = {
        EncodeSopp(RDNA2_SOPP_Op::S_ENDPGM),
        EncodeVop1(RDNA2_VOP1_Op::V_MOV_B32, 0U, 242U)
    };
    result = ShaderSpirvRecompiler{}.CompileRDNA2ToSpirv(trailing, 2U);
    Expect(!result.success && result.error == SpirvCompileError::InstructionsAfterEndProgram,
           "trailing instructions must be rejected");

    result = ShaderSpirvRecompiler{}.CompileRDNA2ToSpirv(nullptr, 0U);
    Expect(!result.success && result.error == SpirvCompileError::InvalidInput,
           "null input must be rejected by checked API");
}

} // namespace

int main() {
    try {
        TestDecoderPrecedence();
        TestMinimalModule();
        TestSupportedSubset();
        TestUnsupportedOpcodeRejected();
        TestUnsupportedInputsRejected();
        TestTerminationChecks();
        std::cout << "rdna2_spirv_recompiler_test: 6 test groups passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "rdna2_spirv_recompiler_test: FAILED: " << error.what() << '\n';
        return 1;
    }
}
