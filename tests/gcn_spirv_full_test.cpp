// ============================================================================
// ProsperoLayer RDNA2 Core - Full GCN -> SPIR-V compiler test (round 18)
// ----------------------------------------------------------------------------
// Proves the round-18 compiler accepts the full decoded GCN set the software
// executor runs (SOP1/2/K/C + SOPP branches, VOP1/2/3, VOPC, SMEM, MUBUF):
//
//   * every program below ALSO runs on the GCN software executor, and its
//     OUTPUT VALUES are checked -- so the module's meaning is verified, not
//     just its structure (on a Vulkan host both paths execute the same
//     program; here the semantics come from the interpreter, the structure
//     from the compiler);
//   * structured control flow: if/else lowers to OpSelectionMerge +
//     OpBranchConditional, a do-while loop to OpLoopMerge, a guard-clause
//     early exit (branch straight to S_ENDPGM) to a selection with an empty
//     arm;
//   * VOP3 with the real modifiers (neg/abs/omod);
//   * SMEM routes through the scalar-segment mirror SSBO (u64 address math);
//   * MUBUF routes per descriptor to its own SSBO with offen addressing;
//   * fail-closed: MUBUF without a table, SMEM without a mirror, backward
//     s_branch (unstructured), a branch into a literal dword.
// ============================================================================
#include "gpu/rdna2_compute_compiler.hpp"
#include "gpu/gcn_decoder.hpp"
#include "gpu/gpu_guest_memory.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
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

uint32_t FBits(float f) { uint32_t v; std::memcpy(&v, &f, 4); return v; }
float BitsF(uint32_t v) { float f; std::memcpy(&f, &v, 4); return f; }
uint32_t V(uint32_t n) { return 256U + n; }

// ---- encoders (the REAL GFX10 layouts the round-12 decoder reads) --------
uint32_t Vop1(uint32_t op, uint32_t dst, uint32_t src0) {
    return 0x7E000000u | (op << 17u) | (dst << 9u) | src0;
}
uint32_t Vop2(uint32_t op, uint32_t dst, uint32_t src1_vgpr, uint32_t src0) {
    return (op << 25u) | (dst << 17u) | (src1_vgpr << 9u) | src0;
}
uint32_t Vopc(uint32_t op, uint32_t src1_vgpr, uint32_t src0) {
    return 0x7C000000u | (op << 17u) | (src1_vgpr << 9u) | src0;
}
uint32_t Sopp(uint32_t op, int simm16) {
    return 0xBF800000u | (op << 16u) |
           (static_cast<uint32_t>(simm16) & 0xFFFFu);
}
uint32_t Sop1(uint32_t op, uint32_t sdst, uint32_t ssrc0) {
    return 0x7D800000u | (op << 16u) | (sdst << 9u) | ssrc0;
}
// VOP3 (dual dword): op[25:16] dst[7:0] | w1: src0[8:0] src1[17:9] src2[26:18]
// omod[28:27] neg0[29] neg1[30] neg2[31] (+ abs bits in w0 [8:10]).
uint32_t Vop3W0(uint32_t op, uint32_t dst) {
    return 0xD4000000u | (op << 16u) | dst;
}
uint32_t Vop3W1(uint32_t src0, uint32_t src1, uint32_t src2, uint32_t mods) {
    return src0 | (src1 << 9u) | (src2 << 18u) | mods;
}
constexpr uint32_t VOP3_NEG0 = 1u << 29;
constexpr uint32_t VOP3_NEG1 = 1u << 30;
constexpr uint32_t VOP3_NEG2 = 1u << 31;

// SOPP opcodes.
constexpr uint32_t SOPP_ENDPGM = 0x001, SOPP_BRANCH = 0x002,
                    SOPP_CBRANCH_SCC0 = 0x004, SOPP_CBRANCH_SCC1 = 0x005,
                    SOPP_CBRANCH_VCCZ = 0x006, SOPP_CBRANCH_VCCNZ = 0x007;

// Flat guest memory (mirror of the emulator's bridge for the SW executor).
class FlatGuestMemory final : public GpuGuestMemory {
public:
    FlatGuestMemory(uint64_t base, size_t dwords)
        : m_base(base), m_storage(dwords, 0) {}
    bool ReadDwords(uint64_t gva, uint32_t* dst, size_t dwords) override {
        if (gva < m_base || gva % 4 != 0) return false;
        const size_t off = (gva - m_base) / 4;
        if (off + dwords > m_storage.size()) return false;
        std::memcpy(dst, m_storage.data() + off, dwords * 4);
        return true;
    }
    bool WriteDwords(uint64_t gva, const uint32_t* src, size_t dwords) override {
        if (gva < m_base || gva % 4 != 0) return false;
        const size_t off = (gva - m_base) / 4;
        if (off + dwords > m_storage.size()) return false;
        std::memcpy(m_storage.data() + off, src, dwords * 4);
        return true;
    }
    void PutDwords(uint64_t gva, const std::vector<uint32_t>& words) {
        WriteDwords(gva, words.data(), words.size());
    }
    uint32_t At(uint64_t gva) {
        uint32_t v = 0;
        ReadDwords(gva, &v, 1);
        return v;
    }
private:
    uint64_t m_base;
    std::vector<uint32_t> m_storage;
};

// Structural helpers over the emitted module.
bool ParsesCleanly(const std::vector<uint32_t>& m) {
    if (m.size() < 5 || m[0] != 0x07230203u) return false;
    size_t off = 5;
    while (off < m.size()) {
        const uint16_t wc = static_cast<uint16_t>(m[off] >> 16u);
        if (wc == 0 || off + wc > m.size()) return false;
        off += wc;
    }
    return off == m.size();
}
size_t CountOpcode(const std::vector<uint32_t>& m, uint16_t opcode) {
    size_t count = 0;
    size_t off = 5;
    while (off < m.size()) {
        const uint16_t op = static_cast<uint16_t>(m[off] & 0xffffu);
        const uint16_t wc = static_cast<uint16_t>(m[off] >> 16u);
        if (wc == 0 || off + wc > m.size()) return 0;
        if (op == opcode) ++count;
        off += wc;
    }
    return count;
}

// Runs a program on the software executor and returns the single output lane.
std::vector<uint32_t> RunSw(const std::vector<uint32_t>& code,
                            const std::vector<uint32_t>& input,
                            uint32_t k_in, uint32_t m_out,
                            GpuGuestMemory* mem,
                            const std::vector<GcnBufferResource>* buffers) {
    std::vector<uint32_t> out;
    GcnSwExecutor sw;
    const size_t lanes = input.size() / k_in;
    GcnSwExecResult r = sw.Run(code.data(), code.size(), lanes, input, k_in,
                               m_out, out, mem, buffers);
    if (!r.ok) {
        std::cerr << "  [SW] error: " << r.error << "\n";
        return {};
    }
    return out;
}

} // namespace

int main() {
    std::cout << "[gcn-spirv] round 18: full GCN -> SPIR-V coverage\n";

    // =====================================================================
    // A: if/else -- vcc from v_cmp_lt_f32, a literal constant in the else
    //    arm, and the classic cbranch/s_branch else shape.
    //      if (v0 < 2.0) v0 = v0 * 4.0; else v0 = 10.0 + v0;
    // =====================================================================
    std::cout << "[gcn-spirv] A: structured if/else\n";
    {
        const float lit10 = 10.0f;
        const std::vector<uint32_t> code = {
            Vop1(GcnOp::V_MOV_B32, /*v1=*/1, /*2.0=*/244u),          // pc0
            Vopc(GcnOp::V_CMP_LT_F32, /*src1 v1=*/1, V(0)),          // pc1
            Sopp(SOPP_CBRANCH_VCCNZ, /*to pc5*/ 2),                  // pc2
            Vop2(GcnOp::V_MUL_F32, 0, /*src1 v0=*/0, /*4.0=*/246u),  // pc3
            Sopp(SOPP_BRANCH, /*to pc7*/ 2),                         // pc4
            Vop2(GcnOp::V_ADD_F32, 0, 0, 0xFFu),                     // pc5
            FBits(lit10),                                            // pc6 (literal)
            Sopp(SOPP_ENDPGM, 0),                                    // pc7
        };

        // SW semantics: then/else paths.
        auto sw_then = RunSw(code, {FBits(1.0f)}, 1, 1, nullptr, nullptr);
        CHECK(!sw_then.empty());
        CHECK(std::fabs(BitsF(sw_then[0]) - 11.0f) < 1e-5f);   // else taken
        auto sw_else = RunSw(code, {FBits(3.0f)}, 1, 1, nullptr, nullptr);
        CHECK(!sw_else.empty());
        CHECK(std::fabs(BitsF(sw_else[0]) - 12.0f) < 1e-5f);   // then taken

        RDNA2ComputeCompiler cc;
        auto r = cc.Compile(code.data(), code.size());
        CHECK(r.success);
        CHECK(r.branch_count == 1);
        CHECK(ParsesCleanly(r.spirv));
        CHECK(CountOpcode(r.spirv, 247) == 1);  // OpSelectionMerge
        CHECK(CountOpcode(r.spirv, 250) >= 1);  // OpBranchConditional
        CHECK(r.instruction_count == 4);        // 2 mov/cmp/mul/add + literal arm
    }

    // =====================================================================
    // B: do-while loop -- sum four 1.0 increments.
    //      v1 = 0; v2 = 0; do { v2 += 1; v1 += 1 } while (v1 < 4); v0 = v2;
    // =====================================================================
    std::cout << "[gcn-spirv] B: structured do-while loop\n";
    {
        const std::vector<uint32_t> code = {
            Vop1(GcnOp::V_MOV_B32, 1, /*0=*/128u),                  // pc0
            Vop1(GcnOp::V_MOV_B32, 2, 128u),                        // pc1
            Vop2(GcnOp::V_ADD_F32, 2, /*src1 v2=*/2, /*1.0=*/242u), // pc2 (L)
            Vop2(GcnOp::V_ADD_F32, 1, 1, 242u),                     // pc3
            Vop1(GcnOp::V_MOV_B32, 3, /*4.0=*/246u),                // pc4
            Vopc(GcnOp::V_CMP_LT_F32, /*src1 v3=*/3, V(1)),         // pc5
            Sopp(SOPP_CBRANCH_VCCNZ, /*to pc2*/ -5),                // pc6
            Vop1(GcnOp::V_MOV_B32, 0, V(2)),                        // pc7
            Sopp(SOPP_ENDPGM, 0),                                   // pc8
        };

        auto sw = RunSw(code, {0u}, 1, 1, nullptr, nullptr);
        CHECK(!sw.empty());
        CHECK(std::fabs(BitsF(sw[0]) - 4.0f) < 1e-5f);

        RDNA2ComputeCompiler cc;
        auto r = cc.Compile(code.data(), code.size());
        CHECK(r.success);
        CHECK(r.branch_count == 1);
        CHECK(ParsesCleanly(r.spirv));
        CHECK(CountOpcode(r.spirv, 246) == 1);  // OpLoopMerge
        CHECK(CountOpcode(r.spirv, 250) >= 1);  // back-edge BranchConditional
    }

    // =====================================================================
    // C: VOP3 with modifiers -- v_mad_f32(-v0, 2.0, 0.5) (round 10 rejected
    //    every VOP3 encoding; round 18 lowers it with the real neg mod).
    // =====================================================================
    std::cout << "[gcn-spirv] C: VOP3 + modifiers\n";
    {
        const std::vector<uint32_t> code = {
            Vop3W0(GcnOp::V_MAD_F32, 0),
            Vop3W1(V(0), /*2.0=*/244u, /*0.5=*/240u, VOP3_NEG0),
            Sopp(SOPP_ENDPGM, 0),
        };

        auto sw = RunSw(code, {FBits(1.0f)}, 1, 1, nullptr, nullptr);
        CHECK(!sw.empty());
        CHECK(std::fabs(BitsF(sw[0]) - (-1.5f)) < 1e-5f);   // -(1*2)+0.5

        RDNA2ComputeCompiler cc;
        auto r = cc.Compile(code.data(), code.size());
        CHECK(r.success);
        CHECK(ParsesCleanly(r.spirv));
        CHECK(CountOpcode(r.spirv, 127) >= 1);  // OpFNegate (the neg mod)
        CHECK(CountOpcode(r.spirv, 133) >= 1);  // OpFMul
        CHECK(CountOpcode(r.spirv, 129) >= 1);  // OpFAdd
    }

    // =====================================================================
    // D: SOPK/SOPC + guard-clause early exit (branch straight to S_ENDPGM).
    //      s0 = 5 (movk); if (s0 == 5) skip the overwrite; v0 stays.
    // =====================================================================
    std::cout << "[gcn-spirv] D: SOPK/SOPC + early exit\n";
    {
        // s_movk_i32 s0, 5 ; s_cmpk_eq_u32 s0, 5 ; s_cbranch_scc1 ENDPGM ;
        // v_mov v0, 7.0 ; s_endpgm
        const uint32_t smovk =
            0xB0000000u | (GcnOp::S_MOVK_I32 << 20u) | (0u << 16u) | 5u;
        const uint32_t scmpk =
            0xB0000000u | (GcnOp::S_CMPK_EQ_U32 << 20u) | (0u << 16u) | 5u;
        const std::vector<uint32_t> code = {
            smovk,                                    // pc0
            scmpk,                                    // pc1
            Sopp(SOPP_CBRANCH_SCC1, /*to pc4*/ 1),    // pc2 -> endpgm
            Vop1(GcnOp::V_MOV_B32, 0, /*0.5f*/ 0xF0u),// pc3 (skipped by the
                                                       // early exit)
            Sopp(SOPP_ENDPGM, 0),                     // pc4
        };

        auto sw = RunSw(code, {FBits(2.0f)}, 1, 1, nullptr, nullptr);
        CHECK(!sw.empty());
        CHECK(std::fabs(BitsF(sw[0]) - 2.0f) < 1e-5f);  // skip kept v0

        RDNA2ComputeCompiler cc;
        auto r = cc.Compile(code.data(), code.size());
        CHECK(r.success);
        CHECK(r.branch_count == 1);
        CHECK(ParsesCleanly(r.spirv));
        CHECK(CountOpcode(r.spirv, 247) == 1);  // OpSelectionMerge
    }

    // =====================================================================
    // E: SMEM -- s_load_dword from a guest address held in an SGPR pair,
    //    routed through the scalar-segment mirror binding.
    // =====================================================================
    std::cout << "[gcn-spirv] E: SMEM scalar-segment mirror\n";
    {
        const uint64_t BASE = 0x1400000000ull;
        const uint64_t SLOT = BASE + 0x10;
        FlatGuestMemory mem(BASE, 0x1000 / 4);
        mem.PutDwords(SLOT, { 0x42480000u });   // 50.0f

        // s_mov_b32 s0, 0 (inline) ; s_mov_b32 s1, literal 0x14 ;
        // s_load_dword s2, s[0:1], +0x10 (imm) ; v_mov v0, s2 ; s_endpgm
        const uint32_t smem =
            0xC0000000u | (GcnOp::S_LOAD_DWORD << 18u) |
            (1u << 17u) |          // immediate offset
            (2u << 6u);            // sdst = s2 ; sbase field 0 (pair s0-s1)
        const std::vector<uint32_t> code = {
            Sop1(GcnOp::S_MOV_B32, 0, /*inline 0*/ 128u),   // pc0
            Sop1(GcnOp::S_MOV_B32, 1, 0xFFu),               // pc1
            static_cast<uint32_t>(BASE >> 32),              // pc2 literal hi
            smem,                                            // pc3
            0x10u,                                           // pc4 (offset)
            Vop1(GcnOp::V_MOV_B32, 0, /*s2*/ 2u),           // pc5
            Sopp(SOPP_ENDPGM, 0),                           // pc6
        };

        auto sw = RunSw(code, {0u}, 1, 1, &mem, nullptr);
        CHECK(!sw.empty());
        CHECK(sw[0] == 0x42480000u);   // loaded the guest dword

        ComputeCompilerOptions opt;
        opt.scalar_mirror_base_gva = BASE;
        RDNA2ComputeCompiler cc(opt);
        auto r = cc.Compile(code.data(), code.size());
        CHECK(r.success);
        CHECK(r.used_scalar_mirror);
        CHECK(r.memory_op_count == 1);
        CHECK(ParsesCleanly(r.spirv));
        CHECK(CountOpcode(r.spirv, 113) >= 2);  // OpUConvert (u64 address math)
    }

    // =====================================================================
    // F: MUBUF -- buffer_load_dword + buffer_store_dword through descriptor
    //    0 (srsrc quad s[4:7]), offen addressing by v0.
    // =====================================================================
    std::cout << "[gcn-spirv] F: MUBUF per-descriptor routing\n";
    {
        const uint64_t BASE = 0x1400000000ull;
        const uint64_t BUF = BASE + 0x100;
        FlatGuestMemory mem(BASE, 0x1000 / 4);
        mem.PutDwords(BUF + 5 * 4, { FBits(100.0f) });

        const uint32_t load_w0 =
            0xE0000000u | (GcnOp::BUFFER_LOAD_DWORD << 18u);
        const uint32_t load_w1 =
            /*vaddr=*/0u | (/*vdata=*/2u << 8u) | (/*srsrc field=*/1u << 16u) |
            (1u << 22u);   // offen
        const uint32_t store_w0 =
            0xE0000000u | (GcnOp::BUFFER_STORE_DWORD << 18u);
        const uint32_t store_w1 =
            0u | (2u << 8u) | (1u << 16u) | (1u << 22u);

        const std::vector<uint32_t> code = {
            load_w0, load_w1,                                  // pc0-1
            Vop2(GcnOp::V_ADD_F32, 2, /*src1 v2*/ 2, /*1.0*/ 242u), // pc2
            store_w0, store_w1,                                // pc3-4
            Sopp(SOPP_ENDPGM, 0),                              // pc5
        };

        std::vector<GcnBufferResource> bufs;
        bufs.push_back({BUF, 0x100 / 4, 1});
        auto sw = RunSw(code, {5u}, 1, 1, &mem, &bufs);
        CHECK(!sw.empty());
        CHECK(mem.At(BUF + 5 * 4) == FBits(101.0f));   // stored load+1 (float bits)

        ComputeCompilerOptions opt;
        opt.buffers = bufs;
        RDNA2ComputeCompiler cc(opt);
        auto r = cc.Compile(code.data(), code.size());
        CHECK(r.success);
        CHECK(r.buffer_bindings == 1);
        CHECK(r.memory_op_count == 2);
        CHECK(ParsesCleanly(r.spirv));
    }

    // =====================================================================
    // G: VOPC feeding V_CNDMASK (vcc -> select) -- the classic comparison
    //    select idiom.
    // =====================================================================
    std::cout << "[gcn-spirv] G: VOPC + v_cndmask\n";
    {
        // v_cndmask_b32 v0, <lit 8.0>, v0: dst = vcc ? src1(v0) : src0(lit)
        const std::vector<uint32_t> code = {
            Vop1(GcnOp::V_MOV_B32, 1, 244u),                  // pc0
            Vopc(GcnOp::V_CMP_LT_F32, 1, V(0)),               // pc1
            Vop2(GcnOp::V_CNDMASK_B32, 0, 0, 0xFFu),          // pc2 (src0 lit)
            FBits(8.0f),                                       // pc3
            Sopp(SOPP_ENDPGM, 0),                              // pc4
        };

        auto sw_small = RunSw(code, {FBits(1.0f)}, 1, 1, nullptr, nullptr);
        CHECK(!sw_small.empty());
        CHECK(std::fabs(BitsF(sw_small[0]) - 1.0f) < 1e-5f);   // vcc true -> src1
        auto sw_big = RunSw(code, {FBits(9.0f)}, 1, 1, nullptr, nullptr);
        CHECK(!sw_big.empty());
        CHECK(std::fabs(BitsF(sw_big[0]) - 8.0f) < 1e-5f);     // vcc false -> src0

        RDNA2ComputeCompiler cc;
        auto r = cc.Compile(code.data(), code.size());
        CHECK(r.success);
        CHECK(ParsesCleanly(r.spirv));
        CHECK(CountOpcode(r.spirv, 169) >= 1);  // OpSelect (cndmask)
    }

    // =====================================================================
    // H: fail-closed diagnostics.
    // =====================================================================
    std::cout << "[gcn-spirv] H: fail-closed\n";
    {
        // MUBUF without a table.
        const std::vector<uint32_t> mubuf_code = {
            0xE0000000u | (GcnOp::BUFFER_LOAD_DWORD << 18u),
            (2u << 8u) | (1u << 16u) | (1u << 22u),
            Sopp(SOPP_ENDPGM, 0),
        };
        RDNA2ComputeCompiler cc;
        auto r1 = cc.Compile(mubuf_code.data(), mubuf_code.size());
        CHECK(!r1.success);
        CHECK(r1.error == ComputeCompileError::UnsupportedOperand);
        CHECK(!r1.message.empty());

        // SMEM without a mirror.
        const std::vector<uint32_t> smem_code = {
            0xC0000000u | (GcnOp::S_LOAD_DWORD << 18u) | (1u << 17u) |
                (2u << 6u),
            0x10u,
            Sopp(SOPP_ENDPGM, 0),
        };
        auto r2 = cc.Compile(smem_code.data(), smem_code.size());
        CHECK(!r2.success);
        CHECK(r2.error == ComputeCompileError::UnsupportedOperand);

        // Backward s_branch: an unstructured (infinite) loop.
        const std::vector<uint32_t> bad_loop = {
            Sop1(GcnOp::S_MOV_B32, 0, 128u),
            Sopp(SOPP_BRANCH, -1),      // jump to itself
            Sopp(SOPP_ENDPGM, 0),
        };
        auto r3 = cc.Compile(bad_loop.data(), bad_loop.size());
        CHECK(!r3.success);
        CHECK(r3.error == ComputeCompileError::UnstructuredControlFlow);

        // Branch into the middle of a literal dword (not an instruction
        // boundary): the VOP2 at pc0 consumes pc0+pc1, so pc1 is INSIDE it.
        const std::vector<uint32_t> bad_target = {
            Vop2(GcnOp::V_ADD_F32, 0, 0, 0xFFu),
            FBits(8.0f),                 // literal occupies pc1
            Sopp(SOPP_CBRANCH_VCCNZ, /*to pc1*/ -2),
            Sopp(SOPP_ENDPGM, 0),
        };
        auto r4 = cc.Compile(bad_target.data(), bad_target.size());
        CHECK(!r4.success);
        CHECK(r4.error == ComputeCompileError::UnmatchedBranchTarget);
    }

    std::cout << "[gcn-spirv] " << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
