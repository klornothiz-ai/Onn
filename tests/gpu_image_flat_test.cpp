// gpu_image_flat_test - round 28 GPU expansion: MIMG (image load/store/sample
// with the software image model), FLAT memory ops, EXP exports, and S_BARRIER
// recognition in the software GCN executor, plus decode checks for the new
// instruction formats.
#include "gpu/gcn_decoder.hpp"

#include <cstdio>
#include <cstring>
#include <vector>



using PS5::GPU::GcnDecoder;
using PS5::GPU::GcnFormat;
using namespace PS5::GPU;
using PS5::GPU::GcnInstruction;

using PS5::GPU::GcnSwExecutor;
using PS5::GPU::GpuGuestMemory;

class Mem final : public GpuGuestMemory {
public:
    explicit Mem(size_t n) : d(n, 0) {}
    bool ReadDwords(uint64_t a, uint32_t* out, size_t n) override {
        if ((a & 3u) || a / 4 > d.size() || n > d.size() - a / 4) return false;
        std::memcpy(out, d.data() + a / 4, n * 4);
        return true;
    }
    bool WriteDwords(uint64_t a, const uint32_t* in, size_t n) override {
        if ((a & 3u) || a / 4 > d.size() || n > d.size() - a / 4) return false;
        std::memcpy(d.data() + a / 4, in, n * 4);
        return true;
    }
    std::vector<uint32_t> d;
};

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond)                                      \
    do {                                                 \
        ++g_checks;                                      \
        if (!(cond)) {                                   \
            ++g_failures;                                \
            std::printf("  [FAIL] %s:%d %s\n", __FILE__, __LINE__, #cond); \
        }                                                \
    } while (0)

int main() {
    GcnDecoder dec;

    std::printf("== GPU image/flat/export expansion test ==\n");

    // ---------------- A: MIMG decode --------------------------------------
    std::printf("[gpu-x] A: MIMG decode\n");
    {
        // image_load v4, v[2:3], s[4:7] dmask=0xF
        const uint32_t w0 = 0xF8000000u | (GcnOp::IMAGE_LOAD << 18) | (0xFu << 12) | 2u;
        const uint32_t w1 = (4u & 0xFFu) | (1u << 8);   // vdata=4, srsrc/4=1 -> s[4..7]
        const uint32_t code[2] = {w0, w1};
        GcnInstruction ins{};
        CHECK(dec.Decode(code, 2, 0, ins));
        CHECK(ins.format == GcnFormat::MIMG);
        CHECK(ins.opcode == GcnOp::IMAGE_LOAD);
        CHECK(ins.vaddr == 2);
        CHECK(ins.vdata == 4);
        CHECK(ins.srsrc == 4);
        CHECK(ins.dmask == 0xF);
        CHECK(ins.dwords_consumed == 2);
        CHECK(std::strcmp(GcnDecoder::Mnemonic(ins), "image_load") == 0);
    }
    // ---------------- B: EXP decode ---------------------------------------
    std::printf("[gpu-x] B: EXP decode\n");
    {
        // exp mrt0 v0,v1,v2,v3 en=0xF done=1
        const uint32_t w0 = 0xF8000000u | (0xFu << 8) | (1u << 14) | 0u;
        const uint32_t w1 = (0u) | (1u << 5) | (2u << 10) | (3u << 15);
        const uint32_t code[2] = {w0, w1};
        GcnInstruction ins{};
        CHECK(dec.Decode(code, 2, 0, ins));
        CHECK(ins.format == GcnFormat::EXP);
        CHECK(ins.exp_target == 0);
        CHECK(ins.exp_en == 0xF);
        CHECK(ins.exp_done);
        CHECK(ins.vsrc0 == 0 && ins.vsrc1 == 1 && ins.vsrc2 == 2 && ins.vsrc3 == 3);
        CHECK(ins.is_terminator);   // done=1
    }
    // ---------------- C: FLAT decode --------------------------------------
    std::printf("[gpu-x] C: FLAT decode\n");
    {
        // flat_load_dword v2, v[0:1] offset=8
        const uint32_t w0 = 0xDC800000u | (GcnOp::FLAT_LOAD_DWORD << 16) | 8u;
        const uint32_t w1 = (0u & 0xFFu) | (2u << 8);    // vaddr=0, vdata=2
        const uint32_t code[2] = {w0, w1};
        GcnInstruction ins{};
        CHECK(dec.Decode(code, 2, 0, ins));
        CHECK(ins.format == GcnFormat::FLAT);
        CHECK(ins.opcode == GcnOp::FLAT_LOAD_DWORD);
        CHECK(ins.flat_offset == 8);
        CHECK(ins.vaddr == 0);
        CHECK(ins.vdata == 2);
        CHECK(std::strcmp(GcnDecoder::Mnemonic(ins), "flat_load_dword") == 0);
    }
    // ---------------- D: IMAGE_LOAD + IMAGE_STORE roundtrip ---------------
    std::printf("[gpu-x] D: image load/store roundtrip\n");
    {
        GcnSwExecutor ex;
        GcnSwExecutor::SwImage img;
        img.width = 4;
        img.height = 4;
        img.rgba.assign(4 * 4 * 4, 0);
        for (uint32_t y = 0; y < 4; ++y) {
            for (uint32_t x = 0; x < 4; ++x) {
                img.SetTexel(x, y, x * 17u, y * 23u, 0x40u, 0xFFu);
            }
        }
        ex.SetImage(0, img);
        // descriptor in s[4:7]: image index 0, w, h, fmt
        ex.SetSgpr(4, 0);
        ex.SetSgpr(5, 4);
        ex.SetSgpr(6, 4);
        ex.SetSgpr(7, 0);

        // program (per lane): v0=x, v1=y
        //   image_load v2, v[0:1], s[4:7] dmask=0xF
        //   image_store v2, v[0:1], s[4:7] dmask=0xF  (writes same values back)
        //   s_endpgm
        const uint32_t w0l = 0xF8000000u | (GcnOp::IMAGE_LOAD << 18) | (0xFu << 12) | 0u;
        const uint32_t w1l = (2u) | (1u << 8);
        const uint32_t w0s = 0xF8000000u | (GcnOp::IMAGE_STORE << 18) | (0xFu << 12) | 0u;
        const uint32_t w1s = (2u) | (1u << 8);
        const uint32_t endpgm = 0xBF810000u;   // s_endpgm (opcode 1 << 16)
        const uint32_t code[5] = {w0l, w1l, w0s, w1s, endpgm};

        // lane 0 -> (1,2); lane 1 -> (3,0)
        std::vector<uint32_t> input = {1, 2, 3, 0};
        std::vector<uint32_t> output(12, 0);
        const auto r = ex.Run(code, 5, 2, input, 2, 6, output);
        CHECK(r.ok);
        CHECK(r.terminated);
        CHECK(r.error.empty());
        // lane 0 loaded texel (1,2) into v2/v3
        CHECK(output[2] == 1 * 17u);   // r at x=1
        CHECK(output[3] == 2 * 23u);   // g at y=2
        // lane 1 loaded texel (3,0) into v2/v3
        CHECK(output[8] == 3 * 17u);
        CHECK(output[9] == 0u);
        // image still holds the values (store wrote them back)
        uint32_t rr = 0, gg = 0, bb = 0, aa = 0;
        CHECK(ex.GetImage(0).GetTexel(1, 2, rr, gg, bb, aa));
        CHECK(rr == 17u && gg == 46u);
        // write through IMAGE_STORE from a different lane data:
        GcnSwExecutor::SwImage img2 = ex.GetImage(0);
        CHECK(img2.GetTexel(3, 0, rr, gg, bb, aa));
        CHECK(rr == 51u && gg == 0u);
    }
    // ---------------- E: IMAGE_SAMPLE nearest ------------------------------
    std::printf("[gpu-x] E: image_sample (nearest)\n");
    {
        GcnSwExecutor ex;
        GcnSwExecutor::SwImage img;
        img.width = 2;
        img.height = 2;
        img.rgba.assign(2 * 2 * 4, 0);
        img.SetTexel(0, 0, 10, 20, 30, 40);
        img.SetTexel(1, 0, 50, 60, 70, 80);
        img.SetTexel(0, 1, 90, 100, 110, 120);
        img.SetTexel(1, 1, 130, 140, 150, 160);
        ex.SetImage(0, img);
        ex.SetSgpr(4, 0);
        ex.SetSgpr(5, 2);
        ex.SetSgpr(6, 2);
        ex.SetSgpr(7, 0);

        const uint32_t w0 = 0xF8000000u | (GcnOp::IMAGE_SAMPLE << 18) | (0xFu << 12) | 0u;
        const uint32_t w1 = (2u) | (1u << 8) | (0u << 16);   // vdata=2, srsrc=s[4:7]
        const uint32_t endpgm = 0xBF810000u;   // s_endpgm (opcode 1 << 16)
        const uint32_t code[3] = {w0, w1, endpgm};

        // (u,v) = (0.75, 0.75) -> texel (1,1) = 130,140,150,160
        float uv[2] = {0.75f, 0.75f};
        uint32_t in[2];
        std::memcpy(in, uv, 8);
        std::vector<uint32_t> input = {in[0], in[1]};
        std::vector<uint32_t> output(4, 0);
        const auto r = ex.Run(code, 3, 1, input, 2, 4, output);
        CHECK(r.ok);
        // v2..v5 hold RGBA of texel (1,1); m_out=4 harvests v0..v3
        CHECK(output[2] == 130u && output[3] == 140u);
    }
    // ---------------- F: FLAT load/store ------------------------------------
    std::printf("[gpu-x] F: flat load/store\n");
    {
        Mem mem(0x800000 / 4);
        constexpr uint64_t kBase = 0x400000;
        uint32_t initial[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
        CHECK(mem.WriteDwords(kBase, initial, 4));

        GcnSwExecutor ex;
        // v0 = address low? flat uses a FULL 64-bit address from v[0:1]; the
        // model takes the low vgpr + offset (documented: flat offsets in the
        // research model are 32-bit).
        // program: v0 = addr
        //   flat_load_dwordx4 v2, v[0:1] offset=0
        //   flat_store_dword v6, v[0:1] offset=4  (write 0x99999999)
        const uint32_t w0l = 0xDC800000u | (GcnOp::FLAT_LOAD_DWORDX4 << 16) | 0u;
        const uint32_t w1l = (0u) | (2u << 8);
        // v6 = 0x99999999 via v_mov_b32 (VOP1: opcode [24:17], vdst [16:9],
        // src0 [8:0]; 0xFF = literal follows)
        const uint32_t vmov = 0x7E000000u | (GcnOp::V_MOV_B32 << 17) | (6u << 9) | 0xFFu;
        const uint32_t lit = 0x99999999u;
        const uint32_t w0s = 0xDC800000u | (GcnOp::FLAT_STORE_DWORD << 16) | 4u;
        const uint32_t w1s = (0u) | (6u << 8);
        const uint32_t endpgm = 0xBF810000u;   // s_endpgm (opcode 1 << 16)
        const uint32_t code[8] = {w0l, w1l, vmov, lit, w0s, w1s, endpgm, 0};

        std::vector<uint32_t> input = {static_cast<uint32_t>(kBase), 0};
        std::vector<uint32_t> output(6, 0);
        const auto r = ex.Run(code, 7, 1, input, 2, 6, output, &mem);
        CHECK(r.ok);
        CHECK(output[2] == 0x11111111u);
        CHECK(output[3] == 0x22222222u);
        CHECK(output[4] == 0x33333333u);
        CHECK(output[5] == 0x44444444u);
        uint32_t after = 0;
        CHECK(mem.ReadDwords(kBase + 4, &after, 1));
        CHECK(after == 0x99999999u);
    }
    // ---------------- G: EXP param/MRT export -------------------------------
    std::printf("[gpu-x] G: export (MRT + param)\n");
    {
        GcnSwExecutor ex;
        // v0..v3 = per-lane RGBA; exp mrt0 v0..v3 en=F done; s_endpgm
        const uint32_t w0 = 0xF8000000u | (0xFu << 8) | (1u << 14) | 0u;
        const uint32_t w1 = (0u) | (1u << 5) | (2u << 10) | (3u << 15);
        const uint32_t endpgm = 0xBF810000u;   // s_endpgm (opcode 1 << 16)
        const uint32_t code[3] = {w0, w1, endpgm};
        std::vector<uint32_t> input = {0xAABBCC01u, 0xAABBCC02u, 0xAABBCC03u, 0xAABBCC04u,
                                       0xAABBCC11u, 0xAABBCC12u, 0xAABBCC13u, 0xAABBCC14u};
        std::vector<uint32_t> output(8, 0);
        const auto r = ex.Run(code, 3, 2, input, 4, 4, output);
        CHECK(r.ok);
        CHECK(r.terminated);
        const auto& mrt = ex.MrtExport(0);
        CHECK(mrt.size() == 2 * 4);
        CHECK(mrt[0] == 0xAABBCC01u && mrt[3] == 0xAABBCC04u);
        CHECK(mrt[4] == 0xAABBCC11u && mrt[7] == 0xAABBCC14u);
    }
    // ---------------- H: S_BARRIER recognized -------------------------------
    std::printf("[gpu-x] H: s_barrier + s_waitcnt accepted\n");
    {
        // s_barrier; s_waitcnt vmcnt(0); v_mov v0, 1; s_endpgm
        const uint32_t barrier = 0xBF80000Au;
        const uint32_t waitcnt = 0xBF8C0000u;    // s_waitcnt
        // v_mov v0, 1 -- src0 uses the inline-constant encoding 129 (=1,
        // the 128..192 range maps to integers 0..64); src0=1 would be SGPR1.
        const uint32_t vmov = 0x7E000000u | (GcnOp::V_MOV_B32 << 17) | (0u << 9) | 129u;
        const uint32_t endpgm = 0xBF810000u;   // s_endpgm (opcode 1 << 16)
        const uint32_t code[4] = {barrier, waitcnt, vmov, endpgm};
        GcnSwExecutor ex;
        std::vector<uint32_t> output(1, 0);
        const auto r = ex.Run(code, 4, 1, {}, 0, 1, output);
        CHECK(r.ok);
        CHECK(r.terminated);
        CHECK(output[0] == 1u);
    }

    std::printf("gpu_image_flat_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
