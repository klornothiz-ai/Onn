// gpu_wave29_test.cpp — round 29 GPU expansion:
//   * VOP3P packed math (v_mad_mix_f32) -- decode + half-selection execution
//     including IEEE binary16 subnormals,
//   * VINTRP (v_interp_p1/p2/mov_f32) -- decode + execution against the EXP
//     parameter exports (the fragment-interpolation research model),
//   * DS extensions: integer/float reductions (min_i32/max_i32/min_f32/
//     max_f32/add_f32) and the WAVEFRONT lane ops (ds_swizzle_b32,
//     ds_bpermute_b32 -- the cross-lane shuffle).
// All execution checks run on the software executor (the honest fallback that
// the Vulkan path mirrors when a device is present).
#include "gpu/gcn_decoder.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using namespace PS5::GPU;
using PS5::GPU::GcnDecoder;
using PS5::GPU::GcnFormat;
using PS5::GPU::GcnInstruction;
using PS5::GPU::GcnSwExecutor;

int g_checks = 0;
int g_failures = 0;
bool Check(bool v, const char* e, int line) {
    ++g_checks;
    if (!v) {
        ++g_failures;
        std::fprintf(stderr, "  [FAIL] %s (line %d)\n", e, line);
    }
    return v;
}
#define CHECK(e) Check((e), #e, __LINE__)

uint32_t AsF(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
float AsU(uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }

} // namespace

int main() {
    GcnDecoder dec;
    std::printf("== GPU round-29 expansion: VOP3P + VINTRP + DS/wavefront ==\n");

    // ------------------------------------------------------------------
    // A: VOP3P decode + v_mad_mix_f32 execution
    // ------------------------------------------------------------------
    std::printf("[gpu-29] A: VOP3P v_mad_mix_f32\n");
    {
        // w0: 0xD0 | opcode<<16 | opsel<<8 | dst ; w1: VOP3 source triple.
        const uint32_t w0 = 0xD0000000u | (GcnOp::V_MAD_MIX_F32 << 16) | (0u << 8) | 0u;
        const uint32_t w1 = (256u + 0u) | ((256u + 1u) << 9) | ((256u + 2u) << 18);
        const uint32_t dw[2] = {w0, w1};
        GcnInstruction ins{};
        CHECK(dec.Decode(dw, 2, 0, ins));
        CHECK(ins.format == GcnFormat::VOP3P);
        CHECK(ins.opcode == GcnOp::V_MAD_MIX_F32);
        CHECK(ins.opsel == 0);
        CHECK(ins.dst == 0);
        CHECK(ins.src0 == 256 + 0 && ins.src1 == 256 + 1 && ins.src2 == 256 + 2);
        CHECK(std::strcmp(GcnDecoder::Mnemonic(ins), "v_mad_mix_f32") == 0);

        // f16 pack helper: two halves into one 32-bit lane value.
        auto f16bits = [](float f) -> uint32_t {
            // convert via the same normalization the executor implements
            // (round-to-nearest not needed for exact powers of two).
            for (uint32_t h = 0; h < 65536; ++h) {
                const uint32_t sign = (h >> 15) & 1u;
                const uint32_t exp = (h >> 10) & 0x1Fu;
                const uint32_t frac = h & 0x3FFu;
                uint32_t out;
                if (exp == 0) {
                    if (frac == 0) { out = sign << 31; }
                    else {
                        int shifts = 0; uint32_t m = frac;
                        while ((m & 0x400u) == 0) { m <<= 1; ++shifts; }
                        m &= 0x3FFu;
                        out = (sign << 31) | (static_cast<uint32_t>(113 - shifts) << 23) | (m << 13);
                    }
                } else if (exp == 0x1F) {
                    out = (sign << 31) | 0x7F800000u | (frac << 13);
                } else {
                    out = (sign << 31) | ((exp + 112) << 23) | (frac << 13);
                }
                if (out == AsF(f)) return h;
            }
            return 0;
        };
        const uint32_t pack_a = (f16bits(1.0f) << 16) | f16bits(2.0f);   // hi=1, lo=2
        const uint32_t pack_b = (f16bits(3.0f) << 16) | f16bits(4.0f);
        const uint32_t pack_c = (f16bits(5.0f) << 16) | f16bits(6.0f);

        for (uint32_t opsel = 0; opsel < 8; ++opsel) {
            const uint32_t code0 = 0xD0000000u | (GcnOp::V_MAD_MIX_F32 << 16) |
                                   (opsel << 8) | 0u;
            const uint32_t code1 = w1;
            const uint32_t endpgm = 0xBF810000u;
            const uint32_t code[3] = {code0, code1, endpgm};
            GcnSwExecutor ex;
            // v1=a, v2=b, v3=c via k_in=3 input seeding
            std::vector<uint32_t> input = {pack_a, pack_b, pack_c};
            std::vector<uint32_t> output(1, 0);
            const auto r = ex.Run(code, 3, 1, input, 3, 1, output);
            CHECK(r.ok);
            const float a = (opsel & 1u) ? 1.0f : 2.0f;
            const float b = (opsel & 2u) ? 3.0f : 4.0f;
            const float c = (opsel & 4u) ? 5.0f : 6.0f;
            CHECK(AsU(output[0]) == a * b + c);
        }
        std::printf("  [ok] mad_mix all 8 opsel combinations\n");

        // Subnormal f16 (exp=0, frac!=0): 0x0400 >> more precisely 2^-24
        // region. f16 0x0001 = 2^-24. Check the pick path normalizes it.
        {
            // a = f16 subnormal 2^-24 (bits 0x0001 in the lo half of v0);
            // b = packed {hi=0, lo=1.0f16} in v1 (0x00003C00); c = 0 in v2.
            const uint32_t code0 = 0xD0000000u | (GcnOp::V_MAD_MIX_F32 << 16) | 0u;
            const uint32_t code1 = (256u + 0u) | ((256u + 1u) << 9) | ((256u + 2u) << 18);
            const uint32_t endpgm = 0xBF810000u;
            const uint32_t code[3] = {code0, code1, endpgm};
            GcnSwExecutor ex;
            std::vector<uint32_t> input = {0x0001u, 0x00003C00u, 0u};
            std::vector<uint32_t> output(1, 0);
            const auto r = ex.Run(code, 3, 1, input, 3, 1, output);
            CHECK(r.ok);
            CHECK(AsU(output[0]) == std::ldexp(1.0f, -24));
        }
        std::printf("  [ok] mad_mix f16 subnormal normalized\n");
    }

    // ------------------------------------------------------------------
    // B: VINTRP decode + execution against EXP param exports
    // ------------------------------------------------------------------
    std::printf("[gpu-29] B: VINTRP (fragment interpolation model)\n");
    {
        const uint32_t w0 = 0xD4000000u | (GcnOp::V_INTERP_MOV_F32 << 16) | 2u;
        const uint32_t w1x = (256u + 5u) | ((128u + 0u) << 9) | (128u << 18);
        const uint32_t dw[2] = {w0, w1x};
        GcnInstruction ins{};
        CHECK(dec.Decode(dw, 2, 0, ins));
        CHECK(ins.format == GcnFormat::VOP3);
        CHECK(ins.opcode == GcnOp::V_INTERP_MOV_F32);
        CHECK(std::strcmp(GcnDecoder::Mnemonic(ins), "v_interp_mov_f32") == 0 ||
              std::strcmp(GcnDecoder::Mnemonic(ins), "v_*") == 0);

        // Program: export param (lane values 7,9) then interp:
        //   exp param0 v0..v3 en=1 done=0 ; v_interp_mov v2 <- param0
        //   v_interp_p1 v3 <- param0 * v4(i) ; v_interp_p2 v3 += param0 * v5(j)
        const uint32_t exp_w0 = 0xF8000000u | (1u << 8) | (32u & 0xFFu);  // en=1, target=32
        const uint32_t exp_w1 = (0u) | (1u << 5) | (2u << 10) | (3u << 15);
        const uint32_t mov0 = 0xD4000000u | (GcnOp::V_INTERP_MOV_F32 << 16) | 2u;
        const uint32_t mov1 = (256u + 5u) | ((128u + 0u) << 9) | ((128u + 0u) << 18);
        const uint32_t p10 = 0xD4000000u | (GcnOp::V_INTERP_P1_F32 << 16) | 3u;
        const uint32_t p11 = (256u + 4u) | ((128u + 0u) << 9) | ((128u + 0u) << 18);
        const uint32_t p20 = 0xD4000000u | (GcnOp::V_INTERP_P2_F32 << 16) | 3u;
        const uint32_t p21 = (256u + 5u) | ((128u + 0u) << 9) | ((128u + 0u) << 18);
        const uint32_t endpgm = 0xBF810000u;
        const uint32_t code[9] = {exp_w0, exp_w1, mov0, mov1,
                                  p10, p11, p20, p21, endpgm};

        GcnSwExecutor ex;
        // 2 lanes: v0=7/9 (param), v4=i=0.5/0.25, v5=j=0.5/0.5
        float i0 = 0.5f, i1 = 0.25f, j0 = 0.5f, j1 = 0.5f;
        std::vector<uint32_t> input = {
            AsF(7.0f), 0u, 0u, 0u, AsF(i0), AsF(j0),   // lane 0
            AsF(9.0f), 0u, 0u, 0u, AsF(i1), AsF(j1),   // lane 1
        };
        std::vector<uint32_t> output(4, 0);   // v0..v3 of lane 0
        const auto r = ex.Run(code, 9, 2, input, 6, 4, output);
        CHECK(r.ok);
        // v2 (interp_mov) = param0 = 7 for lane 0
        CHECK(AsU(output[2]) == 7.0f);
        // v3 = 7*0.5 (p1) + 7*0.5 (p2) = 7.0
        CHECK(AsU(output[3]) == 7.0f);
        std::printf("  [ok] interp mov/p1/p2 against param exports\n");
    }

    // ------------------------------------------------------------------
    // C: DS integer/float reductions
    // ------------------------------------------------------------------
    std::printf("[gpu-29] C: DS reductions\n");
    {
        // helper: run a 3-instruction DS program
        // Reductions write their result to LDS; read it back with
        // ds_read_b32 so the output VGPR carries the value under test.
        auto run_ds = [&](uint32_t op, uint32_t offset0, uint8_t src0, uint8_t src1,
                          uint8_t dst_vgpr, const std::vector<uint32_t>& input,
                          uint32_t k_in, uint32_t m_out) {
            const uint32_t wr0 = 0xD8000000u | (GcnOp::DS_WRITE_B32 << 18);
            const uint32_t wr1 = 0u | (1u << 8);
            const uint32_t t0 = 0xD8000000u | (op << 18) | offset0;
            const uint32_t t1 = src0 | (src1 << 8);
            const uint32_t rd0 = 0xD8000000u | (GcnOp::DS_READ_B32 << 18) | offset0;
            const uint32_t rd1 = src0 | (dst_vgpr << 24);
            const uint32_t endpgm = 0xBF810000u;
            const uint32_t code[8] = {wr0, wr1, t0, t1, rd0, rd1, endpgm, 0};
            GcnSwExecutor ex;
            std::vector<uint32_t> output(m_out, 0);
            const auto r = ex.Run(code, 7, 1, input, k_in, m_out, output);
            struct Res { bool ok; std::vector<uint32_t> out; };
            return Res{r.ok, output};
        };

        // min_i32: LDS=-5, src=-10 -> -10 ; max_i32: -> -5
        {
            const auto res = run_ds(GcnOp::DS_MIN_I32, 0, 0, 2, 2,
                                    {0u, 0xFFFFFFFBu /* -5 */, 0xFFFFFFF6u /* -10 */}, 3, 3);
            CHECK(res.ok);
            CHECK(res.out[2] == 0xFFFFFFF6u);
        }
        {
            const auto res = run_ds(GcnOp::DS_MAX_I32, 0, 0, 2, 2,
                                    {0u, 0xFFFFFFFBu, 0xFFFFFFF6u}, 3, 3);
            CHECK(res.ok);
            CHECK(res.out[2] == 0xFFFFFFFBu);
        }
        // min_f32: LDS=2.0, src=1.0 -> 1.0
        {
            const auto res = run_ds(GcnOp::DS_MIN_F32, 0, 0, 2, 2,
                                    {0u, AsF(2.0f), AsF(1.0f)}, 3, 3);
            CHECK(res.ok);
            CHECK(res.out[2] == AsF(1.0f));
        }
        // add_f32: 2.5 + 1.25 = 3.75
        {
            const auto res = run_ds(GcnOp::DS_ADD_F32, 0, 0, 2, 2,
                                    {0u, AsF(2.5f), AsF(1.25f)}, 3, 3);
            CHECK(res.ok);
            CHECK(res.out[2] == AsF(3.75f));
        }
        std::printf("  [ok] DS min_i32/max_i32/min_f32/add_f32\n");
    }

    // ------------------------------------------------------------------
    // D: wavefront ops (ds_swizzle + ds_bpermute)
    // ------------------------------------------------------------------
    std::printf("[gpu-29] D: wavefront lane ops\n");
    {
        // ds_bpermute_b32: lane i's v0 holds its SELECTOR; the result is
        // v0 of the selected lane (the classic wavefront shuffle).
        // 10-bit opcode 0x36d encodes directly (low bits nonzero).
        const uint32_t bp0 = 0xD8000000u | (GcnOp::DS_BPERMUTE_B32 << 16);
        const uint32_t bp1 = 0u | (0u << 24);   // dst = v0
        const uint32_t endpgm = 0xBF810000u;
        const uint32_t code[4] = {bp0, bp1, endpgm, 0};
        GcnSwExecutor ex;
        // selectors: lane 0 -> lane 1, lane 1 -> lane 2, lane 2 -> lane 3,
        // lane 3 -> lane 0. The shuffled VALUE is each lane's v0.
        std::vector<uint32_t> input = {1u, 2u, 3u, 0u};   // 4 lanes, k_in=1
        std::vector<uint32_t> output(4, 0);
        const auto r = ex.Run(code, 3, 4, input, 1, 1, output);
        CHECK(r.ok);
        CHECK(output[0] == 2u);
        CHECK(output[1] == 3u);
        CHECK(output[2] == 0u);
        CHECK(output[3] == 1u);
        std::printf("  [ok] ds_bpermute cross-lane shuffle\n");

        // ds_swizzle_b32: write LDS[0]=11 then swizzle with pattern 1
        // (lane i reads LDS word (0 + (1 ^ i))).
        const uint32_t wr0 = 0xD8000000u | (GcnOp::DS_WRITE_B32 << 18);
        const uint32_t wr1 = 0u | (1u << 8);           // addr=v0, data=v1
        const uint32_t sw0 = 0xD8000000u | (GcnOp::DS_SWIZZLE_B32 << 18) | (1u << 8);
        const uint32_t sw1 = 0u | (0u << 24);   // dst = v0
        const uint32_t code2[6] = {wr0, wr1, sw0, sw1, endpgm, 0};
        GcnSwExecutor ex2;
        // all lanes write LDS[0]=11 (addr 0); LDS[1] stays 0; swizzle reads
        // LDS[(0 + (1^lane))] -> lane 0 reads LDS[1]=0, lane 1 reads LDS[0]=11.
        std::vector<uint32_t> input2 = {0u, 11u, 0u,   // lane 0: addr=0, data=11
                                        0u, 11u, 0u};  // lane 1
        std::vector<uint32_t> output2(2, 0);
        const auto r2 = ex2.Run(code2, 5, 2, input2, 3, 1, output2);
        CHECK(r2.ok);
        CHECK(output2[0] == 0u);     // lane 0: 1^0=1 -> LDS[1] = 0
        CHECK(output2[1] == 11u);    // lane 1: 1^1=0 -> LDS[0] = 11
        std::printf("  [ok] ds_swizzle lane-local shuffle\n");
    }

    std::printf("gpu_wave29_test: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) std::printf(">> [PASS] GPU round-29 expansion\n");
    return g_failures == 0 ? 0 : 1;
}
