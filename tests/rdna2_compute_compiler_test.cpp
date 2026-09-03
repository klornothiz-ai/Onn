// ============================================================================
// ProsperoLayer RDNA2 Core - RDNA2 compute-compiler test
// ----------------------------------------------------------------------------
// Verifies the SPIR-V compute-kernel compiler that a real Vulkan driver can
// consume. Unlike the legacy recompiler test (which checks a dead-code module),
// this asserts the module has *observable* side effects: it loads from an input
// SSBO and stores to an output SSBO. The emitted SPIR-V is also dumped to a
// file so the host can round-trip it through spirv-cross (done by the runner
// wrapper below when spirv-cross is present).
// ============================================================================
#include "gpu/rdna2_compute_compiler.hpp"
#include "gpu/shader_spirv_recompiler.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <vector>

namespace {

using namespace PS5::GPU;

int g_failures = 0;
int g_checks = 0;
bool Check(bool v, const char* e, int line) {
    ++g_checks;
    if (!v) { ++g_failures; std::cerr << "  [FAIL] line " << line << ": " << e << '\n'; }
    return v;
}
#define CHECK(e) Check((e), #e, __LINE__)

constexpr uint32_t EncodeVop1(RDNA2_VOP1_Op op, uint32_t dst, uint32_t src0) {
    // Real VOP1 hardware layout: op[24:17], vdst[16:9], src0[8:0].
    return 0x7e000000U | (static_cast<uint32_t>(op) << 17U) | (dst << 9U) | src0;
}
constexpr uint32_t EncodeVop1Raw(uint32_t op, uint32_t dst, uint32_t src0) {
    // Real VOP1 hardware layout: op[24:17], vdst[16:9], src0[8:0].
    return 0x7e000000U | (op << 17U) | (dst << 9U) | src0;
}
constexpr uint32_t EncodeVop2(uint32_t op, uint32_t dst, uint32_t src1_vgpr, uint32_t src0) {
    return (op << 25U) | (dst << 17U) | (src1_vgpr << 9U) | src0;
}
constexpr uint32_t EncodeSopp(RDNA2_SOPP_Op op, uint32_t simm16 = 0) {
    return 0xbf800000U | (static_cast<uint32_t>(op) << 16U) | simm16;
}
constexpr uint32_t V(uint32_t n) { return 256U + n; } // VGPR source encoding

// Minimal structural sanity: header + a store instruction (OpStore=62) present.
bool HasOpcode(const std::vector<uint32_t>& m, uint16_t opcode) {
    size_t off = 5;
    while (off < m.size()) {
        const uint16_t op = static_cast<uint16_t>(m[off] & 0xffffU);
        const uint16_t wc = static_cast<uint16_t>(m[off] >> 16U);
        if (wc == 0 || off + wc > m.size()) return false;
        if (op == opcode) return true;
        off += wc;
    }
    return false;
}

bool ParsesCleanly(const std::vector<uint32_t>& m) {
    if (m.size() < 5) return false;
    if (m[0] != 0x07230203U) return false;
    size_t off = 5;
    while (off < m.size()) {
        const uint16_t wc = static_cast<uint16_t>(m[off] >> 16U);
        if (wc == 0 || off + wc > m.size()) return false;
        off += wc;
    }
    return off == m.size();
}

void DumpSpirv(const std::vector<uint32_t>& m, const char* path) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(m.data()),
            static_cast<std::streamsize>(m.size() * sizeof(uint32_t)));
}

// program: v1 = v0 * v0 ; v0 = sqrt(v1) ; endpgm  -> out = sqrt(in*in) = |in|
bool TestMulSqrtProgram() {
    std::cout << "[Test] compute: sqrt(in*in) with real SSBO store\n";
    std::vector<uint32_t> code = {
        EncodeVop2(0x08, /*dst v1*/1, /*src1 v0*/0, /*src0*/V(0)), // v1 = v0*v0
        EncodeVop1(RDNA2_VOP1_Op::V_SQRT_F32, /*dst v0*/0, /*src0 v1*/V(1)),
        EncodeSopp(RDNA2_SOPP_Op::S_ENDPGM),
    };
    RDNA2ComputeCompiler cc;
    auto r = cc.Compile(code.data(), code.size());
    CHECK(r.success);
    CHECK(r.instruction_count == 2);
    CHECK(r.alu_op_count == 2);
    CHECK(ParsesCleanly(r.spirv));
    CHECK(HasOpcode(r.spirv, 61)); // OpLoad (reads input SSBO / gid)
    CHECK(HasOpcode(r.spirv, 62)); // OpStore (writes output SSBO) -- NOT dead code
    CHECK(HasOpcode(r.spirv, 65)); // OpAccessChain
    if (r.success) DumpSpirv(r.spirv, "build/tests/out_mulsqrt.spv");
    return true;
}

// program using extended opcodes: add, min, floor, rcp
//   v1 = v0 + 1.0        (inline const 242 = 1.0f)
//   v0 = min(v0, v1)
//   v0 = floor(v0)
//   v0 = rcp(v0)
//   endpgm
// (Round 18: the floor encoding is the REAL GFX10 opcode 0x24 from the
// LLVM-verified table -- the round-10 test used 0x1B, which matched the old
// compiler's private table but contradicts the hardware encoding the round-12
// decoder implements.)
bool TestExtendedOpcodes() {
    std::cout << "[Test] compute: add/min/floor/rcp lowering\n";
    std::vector<uint32_t> code = {
        EncodeVop2(0x03, /*v1*/1, /*src1 v0*/0, /*src0 const 1.0*/242U), // v1 = 1.0 + v0
        EncodeVop2(0x0F, /*v0*/0, /*src1 v1*/1, /*src0 v0*/V(0)),        // v0 = min(v0, v1)
        EncodeVop1Raw(0x24, /*v0*/0, /*src0 v0*/V(0)),                   // v0 = floor(v0)
        EncodeVop1Raw(0x2A, /*v0*/0, /*src0 v0*/V(0)),                   // v0 = rcp(v0)
        EncodeSopp(RDNA2_SOPP_Op::S_ENDPGM),
    };
    RDNA2ComputeCompiler cc;
    auto r = cc.Compile(code.data(), code.size());
    CHECK(r.success);
    CHECK(r.alu_op_count == 4);
    CHECK(ParsesCleanly(r.spirv));
    CHECK(HasOpcode(r.spirv, 136)); // OpFDiv from rcp
    CHECK(HasOpcode(r.spirv, 12));  // OpExtInst from min/floor
    if (r.success) DumpSpirv(r.spirv, "build/tests/out_extended.spv");
    return true;
}

// V_MAC_F32 fused multiply-add accumulation.
//   v1 = 2.0 (mov const)
//   v1 = v0*v0 + v1
bool TestMacAccumulate() {
    std::cout << "[Test] compute: V_MAC_F32 accumulate\n";
    std::vector<uint32_t> code = {
        EncodeVop1(RDNA2_VOP1_Op::V_MOV_B32, /*v1*/1, /*const 2.0*/244U),
        EncodeVop2(0x1F, /*v1*/1, /*src1 v0*/0, /*src0 v0*/V(0)), // v1 = v0*v0 + v1
        EncodeVop1(RDNA2_VOP1_Op::V_MOV_B32, /*v0*/0, /*src v1*/V(1)),
        EncodeSopp(RDNA2_SOPP_Op::S_ENDPGM),
    };
    RDNA2ComputeCompiler cc;
    auto r = cc.Compile(code.data(), code.size());
    CHECK(r.success);
    CHECK(ParsesCleanly(r.spirv));
    CHECK(HasOpcode(r.spirv, 129)); // OpFAdd from MAC
    CHECK(HasOpcode(r.spirv, 133)); // OpFMul from MAC
    if (r.success) DumpSpirv(r.spirv, "build/tests/out_mac.spv");
    return true;
}

// fail-closed cases
bool TestFailClosed() {
    std::cout << "[Test] compute: fail-closed diagnostics\n";
    RDNA2ComputeCompiler cc;
    // uninitialised source VGPR v5
    std::vector<uint32_t> a = {
        EncodeVop1(RDNA2_VOP1_Op::V_SQRT_F32, 0, V(5)),
        EncodeSopp(RDNA2_SOPP_Op::S_ENDPGM),
    };
    auto ra = cc.Compile(a.data(), a.size());
    CHECK(!ra.success);
    CHECK(ra.error == ComputeCompileError::UninitializedRegister);

    // missing S_ENDPGM
    std::vector<uint32_t> b = { EncodeVop1(RDNA2_VOP1_Op::V_MOV_B32, 1, V(0)) };
    auto rb = cc.Compile(b.data(), b.size());
    CHECK(!rb.success);
    CHECK(rb.error == ComputeCompileError::MissingEndProgram);

    // empty input
    auto rc = cc.Compile(nullptr, 0);
    CHECK(!rc.success);
    CHECK(rc.error == ComputeCompileError::InvalidInput);
    return true;
}

} // namespace

int main() {
    std::cout << "=== RDNA2 -> SPIR-V Compute Compiler Test Suite ===\n";
    TestMulSqrtProgram();
    TestExtendedOpcodes();
    TestMacAccumulate();
    TestFailClosed();

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    if (g_failures == 0) {
        std::cout << ">> [PASS] RDNA2 compute compiler verified (real SSBO side effects).\n";
        return 0;
    }
    std::cerr << ">> [FAIL] " << g_failures << " check(s) failed.\n";
    return 1;
}
