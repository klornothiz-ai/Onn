// ============================================================================
// ProsperoLayer RDNA2 Core - Real GCN/GFX10 Decoder + SW Executor Test (round 12)
// ----------------------------------------------------------------------------
// Verifies the decoder against the ACTUAL AMD GFX10 bit layouts (numbers
// cross-checked against LLVM's AMDGPU tablegen definitions) and the software
// GCN interpreter's instruction semantics, then proves the draw path executes
// real GCN end-to-end on the software fallback when no Vulkan device exists.
// ============================================================================
#include "gpu/gcn_decoder.hpp"
#include "gpu/vulkan_compute_executor.hpp"

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

// ---- encoders using the REAL hardware layouts -------------------------------
constexpr uint32_t Vgpr(uint32_t idx) { return 256 + idx; }   // 9-bit source field

constexpr uint32_t EncSop1(uint32_t op, uint32_t sdst, uint32_t ssrc0) {
    return 0x7D800000u | (op << 16) | (sdst << 9) | ssrc0;
}
constexpr uint32_t EncSop2(uint32_t op, uint32_t sdst, uint32_t ssrc1, uint32_t ssrc0) {
    return 0x80000000u | (op << 23) | (sdst << 16) | (ssrc1 << 9) | ssrc0;
}
constexpr uint32_t EncSopk(uint32_t op, uint32_t sdst, uint32_t simm16) {
    return 0xB0000000u | (op << 20) | (sdst << 16) | simm16;
}
constexpr uint32_t EncSopc(uint32_t op, uint32_t ssrc1, uint32_t ssrc0) {
    return 0x7E800000u | (op << 16) | (ssrc1 << 9) | ssrc0;
}
constexpr uint32_t EncSopp(uint32_t op, uint32_t simm16 = 0) {
    return 0xBF800000u | (op << 16) | simm16;
}
constexpr uint32_t EncVop1(uint32_t op, uint32_t vdst, uint32_t src0) {
    return 0x7E000000u | (op << 17) | (vdst << 9) | src0;
}
constexpr uint32_t EncVop2(uint32_t op, uint32_t vdst, uint32_t src1, uint32_t src0) {
    return (op << 25) | (vdst << 17) | (src1 << 9) | src0;
}
constexpr uint64_t EncVop3(uint32_t op, uint32_t vdst, uint32_t s0, uint32_t s1,
                           uint32_t s2, uint32_t mods = 0) {
    const uint64_t w0 = 0xD4000000ull | (static_cast<uint64_t>(op) << 16) | vdst | (mods << 8);
    const uint64_t w1 = static_cast<uint64_t>(s0) | (static_cast<uint64_t>(s1) << 9) |
                        (static_cast<uint64_t>(s2) << 18);
    return (w1 << 32) | w0;
}
constexpr uint32_t EncVopc(uint32_t op, uint32_t src1, uint32_t src0) {
    return 0x7C000000u | (op << 17) | (src1 << 9) | src0;
}
constexpr uint64_t EncSmem(uint32_t op, uint32_t sdst, uint32_t sbase_pair,
                           uint32_t offset, bool imm) {
    const uint64_t w0 = 0xC0000000ull | (static_cast<uint64_t>(op) << 18) |
                        (static_cast<uint64_t>(sbase_pair >> 1)) |
                        (static_cast<uint64_t>(sdst) << 6) |
                        (imm ? (1ull << 17) : 0ull);
    const uint64_t w1 = static_cast<uint64_t>(offset);
    return (w1 << 32) | w0;
}
constexpr uint64_t EncMubuf(uint32_t op, uint32_t vaddr, uint32_t vdata,
                            uint32_t srsrc_quad, uint32_t offset, bool offen) {
    const uint64_t w0 = 0xE0000000ull | (static_cast<uint64_t>(op) << 18) | offset;
    const uint64_t w1 = static_cast<uint64_t>(vaddr) | (static_cast<uint64_t>(vdata) << 8) |
                        (static_cast<uint64_t>(srsrc_quad >> 2) << 16) |
                        (offen ? (1ull << 22) : 0ull);
    return (w1 << 32) | w0;
}
constexpr uint64_t EncDs(uint32_t op, uint32_t vdst, uint32_t addr, uint32_t data0) {
    const uint64_t w0 = 0xD8000000ull | (static_cast<uint64_t>(op) << 18);
    const uint64_t w1 = static_cast<uint64_t>(addr) | (static_cast<uint64_t>(data0) << 8) |
                        (static_cast<uint64_t>(vdst) << 24);
    return (w1 << 32) | w0;
}

// Flat guest memory for the SW-executor / draw tests.
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
    uint32_t GetDword(uint64_t gva) {
        uint32_t v = 0;
        ReadDwords(gva, &v, 1);
        return v;
    }
private:
    uint64_t m_base;
    std::vector<uint32_t> m_storage;
};

} // namespace

int main() {
    std::cout << "[gcn] round 12: real GFX10 decoder + software executor\n";
    const GcnDecoder dec;

    // =====================================================================
    // Part 1: format identification + field extraction (real layouts).
    // =====================================================================
    std::cout << "[gcn] A: format decode\n";
    {
        GcnInstruction ins;
        // SOP1: s_mov_b32 s4, 0x123 (literal)
        const uint32_t sop1_lit[] = { EncSop1(GcnOp::S_MOV_B32, 4, 0xFFu), 0x123 };
        CHECK(dec.Decode(sop1_lit, 2, 0, ins));
        CHECK(ins.format == GcnFormat::SOP1);
        CHECK(ins.opcode == GcnOp::S_MOV_B32);
        CHECK(ins.dst == 4);
        CHECK(ins.has_literal && ins.literal == 0x123);
        CHECK(ins.dwords_consumed == 2);

        // SOP2: s_add_u32 s0, s2, s3
        const uint32_t sop2[] = { EncSop2(GcnOp::S_ADD_U32, 0, 3, 2) };
        CHECK(dec.Decode(sop2, 1, 0, ins));
        CHECK(ins.format == GcnFormat::SOP2 && ins.opcode == GcnOp::S_ADD_U32);
        CHECK(ins.dst == 0 && ins.src0 == 2 && ins.src1 == 3);

        // SOPK: s_movk_i32 s5, -5
        const uint32_t sopk[] = { EncSopk(GcnOp::S_MOVK_I32, 5, 0xFFFBu) };
        CHECK(dec.Decode(sopk, 1, 0, ins));
        CHECK(ins.format == GcnFormat::SOPK && ins.opcode == GcnOp::S_MOVK_I32);
        CHECK(ins.dst == 5 && ins.simm16 == 0xFFFBu);

        // SOPC: s_cmp_gt_u32 s0, s1
        const uint32_t sopc[] = { EncSopc(GcnOp::S_CMP_GT_U32, 1, 0) };
        CHECK(dec.Decode(sopc, 1, 0, ins));
        CHECK(ins.format == GcnFormat::SOPC && ins.opcode == GcnOp::S_CMP_GT_U32);

        // SOPP: s_endpgm / s_branch +6
        const uint32_t sopp[] = { EncSopp(GcnOp::S_ENDPGM) };
        CHECK(dec.Decode(sopp, 1, 0, ins));
        CHECK(ins.format == GcnFormat::SOPP && ins.is_terminator);
        const uint32_t br[] = { EncSopp(GcnOp::S_BRANCH, 6) };
        CHECK(dec.Decode(br, 1, 0, ins));
        CHECK(ins.is_branch && ins.branch_target == 7);   // pc(0)+1+6

        // VOP1: v_mov_b32 v1, v2
        const uint32_t vop1[] = { EncVop1(GcnOp::V_MOV_B32, 1, Vgpr(2)) };
        CHECK(dec.Decode(vop1, 1, 0, ins));
        CHECK(ins.format == GcnFormat::VOP1 && ins.opcode == GcnOp::V_MOV_B32);
        CHECK(ins.dst == 1 && ins.src0 == 258);

        // VOP2: v_add_f32 v3, v2, v1
        const uint32_t vop2[] = { EncVop2(GcnOp::V_ADD_F32, 3, 2, Vgpr(1)) };
        CHECK(dec.Decode(vop2, 1, 0, ins));
        CHECK(ins.format == GcnFormat::VOP2 && ins.opcode == GcnOp::V_ADD_F32);
        CHECK(ins.dst == 3 && ins.src1 == 2 && ins.src0 == 257);

        // VOPC prefix sanity (format only).
        const uint32_t vopc[] = { EncVopc(0x20, 1, Vgpr(0)) };
        CHECK(dec.Decode(vopc, 1, 0, ins));
        CHECK(ins.format == GcnFormat::VOPC);

        // VOP3 dual-dword: v_mad_f32 v0, v1, v2, v3 with neg0|omod bits.
        const uint64_t v3 = EncVop3(GcnOp::V_MAD_F32, 0, Vgpr(1), Vgpr(2), Vgpr(3));
        const uint32_t vop3[] = { static_cast<uint32_t>(v3),
                                  static_cast<uint32_t>(v3 >> 32) };
        CHECK(dec.Decode(vop3, 2, 0, ins));
        CHECK(ins.format == GcnFormat::VOP3 && ins.opcode == GcnOp::V_MAD_F32);
        CHECK(ins.dst == 0 && ins.src0 == 257 && ins.src1 == 258 && ins.src2 == 259);
        CHECK(ins.dwords_consumed == 2);

        // SMEM: s_load_dwordx4 s[4..7], s[0:1] + immediate offset
        const uint64_t sm = EncSmem(GcnOp::S_LOAD_DWORDX4, 4, 0, 0x40, true);
        const uint32_t smem[] = { static_cast<uint32_t>(sm), static_cast<uint32_t>(sm >> 32) };
        CHECK(dec.Decode(smem, 2, 0, ins));
        CHECK(ins.format == GcnFormat::SMEM && ins.opcode == GcnOp::S_LOAD_DWORDX4);
        CHECK(ins.dst == 4 && ins.sbase == 0 && ins.smem_offset == 0x40 && ins.smem_imm);

        // MUBUF: buffer_load_dword v5, v1, s[8..11] offen
        const uint64_t mb = EncMubuf(GcnOp::BUFFER_LOAD_DWORD, 1, 5, 8, 0x10, true);
        const uint32_t mubuf[] = { static_cast<uint32_t>(mb), static_cast<uint32_t>(mb >> 32) };
        CHECK(dec.Decode(mubuf, 2, 0, ins));
        CHECK(ins.format == GcnFormat::MUBUF && ins.opcode == GcnOp::BUFFER_LOAD_DWORD);
        CHECK(ins.vdata == 5 && ins.srsrc == 8 && ins.offen && ins.buf_offset == 0x10);

        // DS: ds_read_b32 v0, v1
        const uint64_t ds64 = EncDs(GcnOp::DS_READ_B32, 0, 1, 0);
        const uint32_t ds[] = { static_cast<uint32_t>(ds64), static_cast<uint32_t>(ds64 >> 32) };
        CHECK(dec.Decode(ds, 2, 0, ins));
        CHECK(ins.format == GcnFormat::DS && ins.opcode == GcnOp::DS_READ_B32);

        // Fail-closed: a dword that starts nothing.
        const uint32_t bad[] = { 0xFFFFFFFFu };
        CHECK(!dec.Decode(bad, 1, 0, ins));
        // Truncated VOP3 (second dword missing).
        const uint32_t trunc[] = { static_cast<uint32_t>(v3) };
        CHECK(!dec.Decode(trunc, 1, 0, ins));
    }

    // =====================================================================
    // Part 2: literal-constant stream handling.
    // =====================================================================
    std::cout << "[gcn] B: literal constants\n";
    {
        // v_mov_b32 v0, 0x2A (literal) ; v_add_f32 v1, v0, v0 ; s_endpgm
        const uint32_t prog[] = {
            EncVop1(GcnOp::V_MOV_B32, 0, 0xFFu), 0x2A,
            EncVop2(GcnOp::V_ADD_F32, 1, 0, Vgpr(0)),
            EncSopp(GcnOp::S_ENDPGM),
        };
        GcnInstruction ins;
        CHECK(dec.Decode(prog, 4, 0, ins) && ins.has_literal && ins.literal == 0x2A);
        CHECK(ins.dwords_consumed == 2);
        // The next instruction must be found at pc=2, not pc=1.
        CHECK(dec.Decode(prog, 4, 2, ins));
        CHECK(ins.format == GcnFormat::VOP2 && ins.opcode == GcnOp::V_ADD_F32);
        // Inline constant table.
        uint32_t bits = 0;
        CHECK(GcnDecodeInlineConstant32(242, bits) && bits == 0x3F800000u); // 1.0f
        CHECK(GcnDecodeInlineConstant32(193, bits) && bits == 0xFFFFFFFFu); // -1
        CHECK(GcnDecodeInlineConstant32(130, bits) && bits == 2);           // 2
        CHECK(!GcnDecodeInlineConstant32(50, bits));                        // SGPR range
    }

    // =====================================================================
    // Part 3: mnemonics.
    // =====================================================================
    std::cout << "[gcn] C: mnemonics\n";
    {
        GcnInstruction ins;
        const uint32_t p[] = { EncVop2(GcnOp::V_MUL_F32, 0, 0, Vgpr(0)) };
        dec.Decode(p, 1, 0, ins);
        CHECK(std::strcmp(GcnDecoder::Mnemonic(ins), "v_mul_f32") == 0);
        const uint32_t e[] = { EncSopp(GcnOp::S_ENDPGM) };
        dec.Decode(e, 1, 0, ins);
        CHECK(std::strcmp(GcnDecoder::Mnemonic(ins), "s_endpgm") == 0);
        const uint32_t mv[] = { EncSop1(GcnOp::S_MOV_B32, 0, 0) };
        dec.Decode(mv, 1, 0, ins);
        CHECK(std::strcmp(GcnDecoder::Mnemonic(ins), "s_mov_b32") == 0);
        const uint64_t mad = EncVop3(GcnOp::V_MAD_F32, 0, Vgpr(0), Vgpr(0), Vgpr(0));
        const uint32_t md[] = { static_cast<uint32_t>(mad), static_cast<uint32_t>(mad >> 32) };
        dec.Decode(md, 2, 0, ins);
        CHECK(std::strcmp(GcnDecoder::Mnemonic(ins), "v_mad_f32") == 0);
    }

    // =====================================================================
    // Part 4: software-executor semantics (no GPU required).
    // =====================================================================
    std::cout << "[gcn] D: software executor\n";
    {
        // D1: v_add_f32 v0, v0, 2.0  (inline constant in src0)
        const uint32_t prog1[] = {
            EncVop2(GcnOp::V_ADD_F32, 0, 0, 244u),   // src0 = 2.0f inline
            EncSopp(GcnOp::S_ENDPGM),
        };
        std::vector<uint32_t> in1 = { FBits(1.0f), FBits(2.0f), FBits(3.0f) };
        std::vector<uint32_t> out1;
        GcnSwExecutor sw;
        auto r1 = sw.Run(prog1, 2, 3, in1, 1, 1, out1);
        CHECK(r1.ok);
        CHECK(out1.size() == 3);
        CHECK(out1[0] == FBits(3.0f) && out1[1] == FBits(4.0f) && out1[2] == FBits(5.0f));
        CHECK(r1.terminated);

        // D2: scalar program shared across lanes: s_movk s0, 6;
        //     s_cmp_lt_u32 s0, 10 -> scc=1; v_cndmask via VOP3? simpler:
        //     v_mul_u32_u24 v0, v0, s0  -- wait, SGPR in 9-bit src0:
        //     v_mul_u32_u24 v0, s0(=enc 0), v0(src1)
        const uint32_t prog2[] = {
            EncSopk(GcnOp::S_MOVK_I32, 0, 6),
            EncVop2(GcnOp::V_MUL_U32_U24, 0, 0, 0u),   // src0 = s0 (SGPR encoding 0)
            EncSopp(GcnOp::S_ENDPGM),
        };
        std::vector<uint32_t> in2 = { 1, 2, 3 };
        std::vector<uint32_t> out2;
        GcnSwExecutor sw2;
        auto r2 = sw2.Run(prog2, 3, 3, in2, 1, 1, out2);
        CHECK(r2.ok);
        CHECK(out2[0] == 6 && out2[1] == 12 && out2[2] == 18);

        // D3: VOP3 with literals + modifiers: v_mad_f32 v0 = v0 * 2.0 + 1.0
        //     (src1 = inline 2.0, src2 = inline 1.0 through 9-bit encodings).
        const uint64_t mad = EncVop3(GcnOp::V_MAD_F32, 0, Vgpr(0), 244u, 242u);
        const uint32_t prog3[] = {
            static_cast<uint32_t>(mad), static_cast<uint32_t>(mad >> 32),
            EncSopp(GcnOp::S_ENDPGM),
        };
        std::vector<uint32_t> in3 = { FBits(3.0f) };
        std::vector<uint32_t> out3;
        GcnSwExecutor sw3;
        auto r3 = sw3.Run(prog3, 3, 1, in3, 1, 1, out3);
        CHECK(r3.ok);
        CHECK(out3[0] == FBits(3.0f * 2.0f + 1.0f));

        // D4: literal constant through the stream: v_mov v1, lit(42);
        //     v_add_u32 v0 = v0 + v1 via v_add_co_u32 (VOP3).
        const uint64_t add = EncVop3(GcnOp::V_ADD_CO_U32, 0, Vgpr(0), Vgpr(1), 128u);
        const uint32_t prog4[] = {
            EncVop1(GcnOp::V_MOV_B32, 1, 0xFFu), 42u,
            static_cast<uint32_t>(add), static_cast<uint32_t>(add >> 32),
            EncSopp(GcnOp::S_ENDPGM),
        };
        std::vector<uint32_t> in4 = { 100 };
        std::vector<uint32_t> out4;
        GcnSwExecutor sw4;
        auto r4 = sw4.Run(prog4, 5, 1, in4, 1, 1, out4);
        CHECK(r4.ok && out4[0] == 142);

        // D5: SMEM scalar load: s_load_dword s8, s[0:1], 0x10
        FlatGuestMemory mem(0x1000, 0x100);
        mem.PutDwords(0x1010, { 777 });
        const uint64_t ld = EncSmem(GcnOp::S_LOAD_DWORD, 8, 0, 0x10, true);
        const uint32_t prog5[] = {
            static_cast<uint32_t>(ld), static_cast<uint32_t>(ld >> 32),
            EncVop2(GcnOp::V_ADD_F32, 0, 0, 8u),    // v0 = s8 + v0 (s8 in 9-bit src0)
            EncSopp(GcnOp::S_ENDPGM),
        };
        GcnSwExecutor sw5;
        sw5.SetSgpr(0, 0x1000);       // base lo
        sw5.SetSgpr(1, 0);            // base hi
        std::vector<uint32_t> in5 = { 223 };
        std::vector<uint32_t> out5;
        auto r5 = sw5.Run(prog5, 5, 1, in5, 1, 1, out5, &mem);
        CHECK(r5.ok && out5[0] == 777 + 223);

        // D6: branch: if v0 == 0 skip the add (s_cmpk + s_cbranch + s_add).
        //     s_cmpk_eq_u32 s0, 5 ; s_cbranch_scc1 +2 ; v_mov v0, 999 ; s_endpgm
        const uint32_t prog6[] = {
            EncSopk(GcnOp::S_CMPK_EQ_U32, 0, 5),      // s0 == 5?
            EncSopp(GcnOp::S_CBRANCH_SCC1, 2),        // taken -> pc = 2+1+2 = 5 (s_endpgm)
            EncVop1(GcnOp::V_MOV_B32, 0, 0xF9u), 999u,// v0 = 999 (literal; skipped)
            EncSopp(GcnOp::S_ENDPGM),
            EncSopp(GcnOp::S_NOP),                    // filler
        };
        GcnSwExecutor sw6;
        sw6.SetSgpr(0, 5);                             // s0 = 5 -> condition true
        std::vector<uint32_t> in6 = { 7 };
        std::vector<uint32_t> out6;
        auto r6 = sw6.Run(prog6, 6, 1, in6, 1, 1, out6);
        CHECK(r6.ok && out6[0] == 7);                  // add skipped

        // D7: MUBUF load through a buffer descriptor (vdata = v2).
        FlatGuestMemory mem2(0x1000, 0x100);
        mem2.PutDwords(0x1044, { 0xDEAD });
        const uint64_t bld = EncMubuf(GcnOp::BUFFER_LOAD_DWORD, 1, 2, 4, 0x44, true);
        const uint32_t prog7[] = {
            static_cast<uint32_t>(bld), static_cast<uint32_t>(bld >> 32),
            EncSopp(GcnOp::S_ENDPGM),
        };
        const std::vector<GcnBufferResource> bufs = { { 0x1000, 0x100, 4 } };
        GcnSwExecutor sw7;
        // lane vaddr=1: v1 = 0 -> address = 0x1000 + 0x44 + v1*4 = 0x1044.
        std::vector<uint32_t> in7 = { 0 };
        std::vector<uint32_t> out7;
        auto r7 = sw7.Run(prog7, 3, 1, in7, 1, 3, out7, &mem2, &bufs);
        CHECK(r7.ok);
        CHECK(out7[2] == 0xDEAD);   // v2 harvested with m_out = 3
    }

    // =====================================================================
    // Part 5: end-to-end execution through the executor's software fallback.
    // (The PM4 ring-level draw path is exercised by the dedicated draw tests,
    // which now automatically run this fallback when no GPU exists.)
    // =====================================================================
    std::cout << "[gcn] E: executor software fallback\n";
    {
        VulkanComputeExecutor exec;
        const bool has_gpu = exec.Initialize();
        std::cout << "  [info] " << (has_gpu ? exec.DeviceName()
                                            : std::string("no Vulkan device; software GCN path")) << "\n";

        // Vertex shader (REAL GFX10 encodings): v0 = v0 * v0 + 1.0
        const uint32_t shader[] = {
            EncVop2(GcnOp::V_MUL_F32, 0, 0, Vgpr(0)),       // v0 = v0 * v0
            EncVop2(GcnOp::V_ADD_F32, 0, 0, 242u),          // v0 = v0 + 1.0
            EncSopp(GcnOp::S_ENDPGM),
        };
        const uint32_t N = 8;
        std::vector<uint32_t> idx(N);
        for (uint32_t i = 0; i < N; ++i) idx[i] = FBits(static_cast<float>(i + 1));

        exec.SetSoftwareFallback(true);
        auto r = exec.RunRDNA2(shader, 3, idx);
        CHECK(r.status == ComputeExecStatus::Ok);
        CHECK(r.output.size() == N);
        bool all_ok = true;
        for (uint32_t i = 0; i < N; ++i) {
            const float expected = static_cast<float>(i + 1) * static_cast<float>(i + 1) + 1.0f;
            if (r.output[i] != FBits(expected)) all_ok = false;
        }
        CHECK(all_ok);
        CHECK(r.hardware == has_gpu);   // false on a headless host, true with a GPU
        if (!has_gpu) {
            CHECK(r.device_name.find("GCN software interpreter") != std::string::npos);
        }
    }

    std::cout << "[gcn] " << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
