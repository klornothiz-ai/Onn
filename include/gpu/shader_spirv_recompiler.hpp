#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace PS5::GPU {

enum class RDNA2OpcodeCategory {
    VOP1,
    VOP2,
    VOP3A,
    SOPP,
    UNKNOWN
};

enum class RDNA2_VOP2_Op : uint32_t {
    V_ADD_F32 = 0x03,
    V_SUB_F32 = 0x04,
    V_MUL_F32 = 0x08
};

enum class RDNA2_VOP1_Op : uint32_t {
    V_NOP = 0x00,
    V_MOV_B32 = 0x01,
    V_SQRT_F32 = 0x33
};

enum class RDNA2_SOPP_Op : uint32_t {
    S_NOP = 0,
    S_ENDPGM = 1,
    S_BRANCH = 2,
    S_CBRANCH_SCC0 = 4,
    S_CBRANCH_SCC1 = 5
};

struct DecodedRDNA2Instruction {
    RDNA2OpcodeCategory category{RDNA2OpcodeCategory::UNKNOWN};
    uint32_t raw_dword{0};
    uint32_t opcode{0};
    uint32_t dst_reg{0};
    uint32_t src0{0};
    uint32_t src1{0};
    uint32_t src2{0};
    bool is_terminating{false};
};

enum class SpirvCompileError {
    None,
    InvalidInput,
    UnsupportedEncoding,
    UnsupportedOpcode,
    UnsupportedOperand,
    UninitializedRegister,
    MissingEndProgram,
    InstructionsAfterEndProgram
};

struct SpirvCompilationResult {
    bool success{false};
    std::vector<uint32_t> spirv;
    SpirvCompileError error{SpirvCompileError::None};
    size_t error_dword{0};
    std::string message;

    explicit operator bool() const noexcept { return success; }
};

// This is intentionally a small, fail-closed translator, not a general RDNA2 compiler.
class ShaderSpirvRecompiler {
public:
    ShaderSpirvRecompiler() = default;

    static DecodedRDNA2Instruction DecodeRDNA2Instruction(uint32_t dword);

    // Supports V_MOV_B32, V_ADD_F32, V_MUL_F32, V_SQRT_F32 and S_ENDPGM.
    // Source VGPRs must have been defined earlier in the instruction stream.
    SpirvCompilationResult CompileRDNA2ToSpirv(const uint32_t* rdna2_bytecode,
                                               size_t dwords_count) const;

    // Compatibility wrapper. An empty vector means compilation failed.
    std::vector<uint32_t> RecompileRDNA2ToSpirv(const uint32_t* rdna2_bytecode,
                                                size_t dwords_count) const;
};

} // namespace PS5::GPU
