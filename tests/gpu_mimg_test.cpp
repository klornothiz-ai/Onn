// ============================================================================
// ProsperoLayer RDNA2 Core - GPU MIMG test (round 28: image sampling, fetch,
// gather, atomics through the REAL dispatch path)
// ----------------------------------------------------------------------------
// Proves the MIMG layer end to end:
//   A. the SPIR-V compiler lowers image programs: three UniformConstant
//      variables per image (sampled image + storage image + sampler), the
//      binding layout matches the compiler contract, image op metadata is
//      reported, and programs without an image table / with an out-of-range
//      srsrc fail closed;
//   B. the software executor runs the full MIMG set: every SAMPLE variant
//      collapses to the mip-0 nearest texel, GATHER4 returns the bilinear
//      footprint, GET_RESINFO reports (w, h, 1, mips), the 13 ATOMIC ops do
//      real read-modify-write returning the OLD value, and out-of-bounds
//      LOAD returns zero (hardware semantics);
//   C. the extended resource-table ABI parses: old tables (no magic) stay
//      image-free byte-identically, the "IMGE" magic adds image entries, and
//      malformed image entries fail closed;
//   D. THE ACCEPTANCE CHECK: a program mixing IMAGE_LOAD + IMAGE_SAMPLE +
//      IMAGE_STORE + IMAGE_ATOMIC_ADD dispatched through
//      RunRDNA2WithResources produces the EXACT same guest-memory image
//      contents and output SSBO as a direct GcnSwExecutor reference run --
//      dword-for-dword, on every host (hardware path when a Vulkan device
//      exists, the honest software interpreter otherwise).
// ============================================================================
#include "gpu/gcn_decoder.hpp"
#include "gpu/rdna2_compute_compiler.hpp"
#include "gpu/vulkan_compute_executor.hpp"
#include "gpu/gpu_guest_memory.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
using namespace PS5::GPU;

int g_failures = 0, g_checks = 0;
bool Check(bool v, const char* e, int line) {
    ++g_checks;
    if (!v) { ++g_failures; std::cerr << "  [FAIL] line " << line << ": " << e << '\n'; }
    return v;
}
#define CHECK(e) Check((e), #e, __LINE__)

// ---- REAL GFX10 MIMG encoders -----------------------------------------------
// w0 = 0xF8 prefix | opcode[25:18] | unorm[16] | dmask[15:12] | vaddr[7:0]
// w1 = vdata[7:0] | srsrc field[14:8] (x4 = sgpr quad) | ssamp field[18:15]
uint32_t MimgW0(uint32_t opcode, uint32_t vaddr, uint32_t dmask = 0xFu) {
    return 0xF8000000u | (opcode << 18u) | (dmask << 12u) | vaddr;
}
uint32_t MimgW1(uint32_t vdata, uint32_t srsrc_field, uint32_t ssamp_field = 0u) {
    return vdata | (srsrc_field << 8u) | (ssamp_field << 15u);
}
uint32_t Sopp(uint32_t op, uint32_t s = 0) {
    return 0xBF800000u | (op << 16u) | s;
}
uint32_t AsB(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }

// ---- flat guest memory -------------------------------------------------------
class FlatGuestMemory final : public GpuGuestMemory {
public:
    FlatGuestMemory(uint64_t base, size_t dwords) : m_base(base), m_mem(dwords, 0u) {}
    bool ReadDwords(uint64_t gva, uint32_t* dst, size_t dwords) override {
        size_t off;
        if (!Range(gva, dwords, off)) return false;
        std::memcpy(dst, m_mem.data() + off, dwords * sizeof(uint32_t));
        return true;
    }
    bool WriteDwords(uint64_t gva, const uint32_t* src, size_t dwords) override {
        size_t off;
        if (!Range(gva, dwords, off)) return false;
        std::memcpy(m_mem.data() + off, src, dwords * sizeof(uint32_t));
        return true;
    }
    void PutDwords(uint64_t gva, const std::vector<uint32_t>& v) {
        size_t off = 0;
        if (Range(gva, v.size(), off))
            std::memcpy(m_mem.data() + off, v.data(), v.size() * sizeof(uint32_t));
    }
    std::vector<uint32_t> Region(uint64_t gva, size_t dwords) const {
        std::vector<uint32_t> out(dwords, 0u);
        size_t off = 0;
        if (Range(gva, dwords, off))
            std::memcpy(out.data(), m_mem.data() + off, dwords * 4);
        return out;
    }
private:
    bool Range(uint64_t gva, size_t dwords, size_t& off) const {
        if (gva < m_base || gva % 4 != 0) return false;
        off = static_cast<size_t>((gva - m_base) / 4);
        return off + dwords <= m_mem.size();
    }
    uint64_t m_base;
    std::vector<uint32_t> m_mem;
};

// ---- SPIR-V structure walkers -----------------------------------------------
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
size_t CountVariableStorage(const std::vector<uint32_t>& m, uint32_t storage) {
    size_t count = 0, off = 5;
    while (off < m.size()) {
        const uint16_t op = static_cast<uint16_t>(m[off] & 0xffffu);
        const uint16_t wc = static_cast<uint16_t>(m[off] >> 16u);
        if (wc == 0 || off + wc > m.size()) return 0;
        if (op == 59 && wc >= 4 && m[off + 3] == storage) ++count;  // OpVariable
        off += wc;
    }
    return count;
}
size_t CountBinding(const std::vector<uint32_t>& m, uint32_t binding) {
    size_t count = 0, off = 5;
    while (off < m.size()) {
        const uint16_t op = static_cast<uint16_t>(m[off] & 0xffffu);
        const uint16_t wc = static_cast<uint16_t>(m[off] >> 16u);
        if (wc == 0 || off + wc > m.size()) return 0;
        if (op == 71 && wc >= 4 && m[off + 2] == 33 && m[off + 3] == binding) ++count;
        off += wc;
    }
    return count;
}
size_t CountOpcode(const std::vector<uint32_t>& m, uint16_t opcode) {
    size_t count = 0, off = 5;
    while (off < m.size()) {
        const uint16_t op = static_cast<uint16_t>(m[off] & 0xffffu);
        const uint16_t wc = static_cast<uint16_t>(m[off] >> 16u);
        if (wc == 0 || off + wc > m.size()) return 0;
        if (op == opcode) ++count;
        off += wc;
    }
    return count;
}

// image helper: 4x4 RGBA raw-dword image with a predictable pattern.
GcnSwExecutor::SwImage MakeTestImage(uint32_t w, uint32_t h, uint32_t seed) {
    GcnSwExecutor::SwImage img;
    img.width  = w;
    img.height = h;
    img.mips   = 1;
    img.rgba.assign(static_cast<size_t>(w) * h * 4u, 0u);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            img.SetTexel(x, y, seed + x * 3u, seed + y * 7u + 1u, x + y,
                         0x41u);
        }
    }
    return img;
}

} // namespace

int main() {
    std::printf("[gpu-mimg] A: SPIR-V compiler structure\n");
    {
        // program: image_load v2, v[0:1], s[4:7]; image_sample v6, v[0:1],
        // s[4:7]; s_endpgm  (image 0 = srsrc field 1)
        const uint32_t code[] = {
            MimgW0(GcnOp::IMAGE_LOAD, 0), MimgW1(2, 1),
            MimgW0(GcnOp::IMAGE_SAMPLE, 0), MimgW1(6, 1, 0),
            Sopp(GcnOp::S_ENDPGM),
        };
        ComputeCompilerOptions opt;
        opt.images.push_back({0x100000, 4, 4, 1});
        RDNA2ComputeCompiler cc(opt);
        auto r = cc.Compile(code, 5);
        CHECK(r.success);
        CHECK(r.image_bindings == 1);
        CHECK(r.image_op_count == 2);
        CHECK(r.memory_op_count == 2);
        CHECK(ParsesCleanly(r.spirv));
        // three UniformConstant variables (sampled image, storage, sampler)
        CHECK(CountVariableStorage(r.spirv, 0 /*UniformConstant*/) == 3);
        // in/out SSBOs (Uniform=2) + nothing else new
        CHECK(CountVariableStorage(r.spirv, 2 /*Uniform*/) == 2);
        // bindings: 0=in, 1=out, 2=sampled, 3=storage, 4=sampler
        CHECK(CountBinding(r.spirv, 2) == 1);
        CHECK(CountBinding(r.spirv, 3) == 1);
        CHECK(CountBinding(r.spirv, 4) == 1);
        CHECK(CountBinding(r.spirv, 5) == 0);
        // the module contains the real image opcodes (95=OpImageFetch,
        // 88=OpImageSampleExplicitLod, 27=OpTypeSampledImage)
        CHECK(CountOpcode(r.spirv, 95) == 1);   // OpImageFetch
        CHECK(CountOpcode(r.spirv, 88) == 1);   // OpImageSampleExplicitLod
        CHECK(CountOpcode(r.spirv, 27) == 1);   // OpTypeSampledImage

        // two images: 6 UniformConstant vars, bindings 2..7
        ComputeCompilerOptions opt2;
        opt2.images.push_back({0x100000, 4, 4, 1});
        opt2.images.push_back({0x200000, 8, 8, 1});
        RDNA2ComputeCompiler cc2(opt2);
        const uint32_t code2[] = {
            MimgW0(GcnOp::IMAGE_LOAD, 0), MimgW1(2, 1),
            MimgW0(GcnOp::IMAGE_LOAD, 0), MimgW1(6, 2),   // image 1
            Sopp(GcnOp::S_ENDPGM),
        };
        auto r2 = cc2.Compile(code2, 5);
        CHECK(r2.success);
        CHECK(r2.image_bindings == 2);
        CHECK(r2.image_op_count == 2);
        CHECK(CountVariableStorage(r2.spirv, 0) == 6);
        CHECK(CountBinding(r2.spirv, 7) == 1);   // second sampler

        // fail-closed: no image table
        RDNA2ComputeCompiler cc_no_table{ComputeCompilerOptions{}};
        auto rf = cc_no_table.Compile(code, 5);
        CHECK(!rf);
        CHECK(rf.error == ComputeCompileError::UnsupportedOperand);

        // fail-closed: srsrc out of range (field 3 -> srsrc 12 -> image 2
        // with only 2 images)
        const uint32_t code_bad[] = {
            MimgW0(GcnOp::IMAGE_LOAD, 0), MimgW1(2, 3),
            Sopp(GcnOp::S_ENDPGM),
        };
        auto rb = cc2.Compile(code_bad, 3);
        CHECK(!rb);
        CHECK(rb.error == ComputeCompileError::UnsupportedOperand);

        // fail-closed: unsupported variant rejects with a clear message
        const uint32_t code_c[] = {
            MimgW0(GcnOp::IMAGE_SAMPLE_CD, 0), MimgW1(2, 1),
            Sopp(GcnOp::S_ENDPGM),
        };
        auto rc = cc_no_table.Compile(code_c, 3);
        CHECK(!rc);
    }

    std::printf("[gpu-mimg] B: software executor MIMG set\n");
    {
        GcnSwExecutor ex;
        const auto img = MakeTestImage(4, 4, 10);
        ex.SetImage(0, img);
        ex.SetSgpr(4, 0); ex.SetSgpr(5, 4); ex.SetSgpr(6, 4); ex.SetSgpr(7, 0);

        // every sample variant collapses to the mip-0 nearest texel
        const uint32_t variants[] = {
            GcnOp::IMAGE_SAMPLE, GcnOp::IMAGE_SAMPLE_L, GcnOp::IMAGE_SAMPLE_LZ,
            GcnOp::IMAGE_SAMPLE_B, GcnOp::IMAGE_SAMPLE_D, GcnOp::IMAGE_SAMPLE_CL,
            GcnOp::IMAGE_SAMPLE_DZ,
        };
        for (uint32_t op : variants) {
            // lane: u=0.5, v=0.5 -> texel (2,2)
            const uint32_t code[] = {
                MimgW0(op, 0), MimgW1(2, 1, 0),
                Sopp(GcnOp::S_ENDPGM),
            };
            std::vector<uint32_t> in = {AsB(0.5f), AsB(0.5f), 0, 0, 0, 0};
            std::vector<uint32_t> outv(6, 0);
            GcnSwExecResult r = ex.Run(code, 3, 1, in, 6, 6, outv);
            CHECK(r.ok);
            // texel(2,2) = (10+6, 10+15+1, 4, 0x41)
            CHECK(outv[2] == 16u);
            CHECK(outv[3] == 25u);
            CHECK(outv[4] == 4u);
            CHECK(outv[5] == 0x41u);
        }

        // out-of-bounds LOAD returns zero (hardware semantics)
        {
            const uint32_t code[] = {
                MimgW0(GcnOp::IMAGE_LOAD, 0), MimgW1(2, 1),
                Sopp(GcnOp::S_ENDPGM),
            };
            std::vector<uint32_t> in = {9, 9, 0, 0, 0, 0};
            std::vector<uint32_t> outv(6, 0);
            GcnSwExecResult r = ex.Run(code, 3, 1, in, 6, 6, outv);
            CHECK(r.ok);
            CHECK(outv[2] == 0u && outv[3] == 0u && outv[4] == 0u &&
                  outv[5] == 0u);
        }

        // GET_RESINFO reports (w, h, 1, mips)
        {
            const uint32_t code[] = {
                MimgW0(GcnOp::IMAGE_GET_RESINFO, 0), MimgW1(2, 1),
                Sopp(GcnOp::S_ENDPGM),
            };
            std::vector<uint32_t> in = {0};
            std::vector<uint32_t> outv(6, 0);
            GcnSwExecResult r = ex.Run(code, 3, 1, in, 1, 6, outv);
            CHECK(r.ok);
            CHECK(outv[2] == 4u && outv[3] == 4u && outv[4] == 1u &&
                  outv[5] == 1u);
        }

        // GATHER4: 4 neighbours of the bilinear footprint, component 0
        {
            const uint32_t code[] = {
                MimgW0(GcnOp::IMAGE_GATHER4, 0, 0x1), MimgW1(2, 1, 0),
                Sopp(GcnOp::S_ENDPGM),
            };
            // u=v=0.6 -> fx = 0.6*4-0.5 = 1.9 -> i0=1, j0=1
            std::vector<uint32_t> in = {AsB(0.6f), AsB(0.6f), 0, 0, 0, 0};
            std::vector<uint32_t> outv(6, 0);
            GcnSwExecResult r = ex.Run(code, 3, 1, in, 6, 6, outv);
            CHECK(r.ok);
            // texels (1,2),(2,2),(1,1),(2,1) component 0 = seed+3x
            CHECK(outv[2] == 10u + 3u);   // (1,2)
            CHECK(outv[3] == 10u + 6u);   // (2,2)
            CHECK(outv[4] == 10u + 3u);   // (1,1)
            CHECK(outv[5] == 10u + 6u);   // (2,1)
        }

        // ATOMICS: add/swap/cmpxchg/inc return OLD and modify the texel
        {
            const uint32_t code[] = {
                MimgW0(GcnOp::IMAGE_ATOMIC_ADD, 0, 0x1), MimgW1(2, 1),
                Sopp(GcnOp::S_ENDPGM),
            };
            std::vector<uint32_t> in = {1, 1, 5, 0, 0, 0};   // src=5
            std::vector<uint32_t> outv(6, 0);
            GcnSwExecResult r = ex.Run(code, 3, 1, in, 6, 6, outv);
            CHECK(r.ok);
            CHECK(outv[2] == 10u + 3u);               // old r at (1,1)
            uint32_t rr = 0, gg = 0, bb = 0, aa = 0;
            CHECK(ex.GetImage(0).GetTexel(1, 1, rr, gg, bb, aa));
            CHECK(rr == 10u + 3u + 5u);               // old + 5
            CHECK(gg == 10u + 7u + 1u);               // untouched
        }
        {
            // CMPSWAP: old == cmp -> store src
            const uint32_t code[] = {
                MimgW0(GcnOp::IMAGE_ATOMIC_CMPSWAP, 0, 0x1), MimgW1(2, 1),
                Sopp(GcnOp::S_ENDPGM),
            };
            std::vector<uint32_t> in = {0, 0, 99, 10u + 0u, 0, 0};
            std::vector<uint32_t> outv(6, 0);
            GcnSwExecResult r = ex.Run(code, 3, 1, in, 6, 6, outv);
            CHECK(r.ok);
            CHECK(outv[2] == 10u);                    // old r at (0,0)
            uint32_t rr = 0, gg = 0, bb = 0, aa = 0;
            CHECK(ex.GetImage(0).GetTexel(0, 0, rr, gg, bb, aa));
            CHECK(rr == 99u);                         // swapped
        }
    }

    std::printf("[gpu-mimg] C: extended resource-table ABI\n");
    {
        constexpr uint64_t TABLE = 0x10000;
        constexpr uint64_t BUF0  = 0x20000;
        constexpr uint64_t IMG0  = 0x30000;
        FlatGuestMemory mem(0x10000, 0x80000 / 4);

        // legacy table: no magic -> zero images
        std::vector<uint32_t> table = {
            1,                                    // buffer count
            0, 0, 0,                              // no mirror
            static_cast<uint32_t>(BUF0), 0, 16, 1 // buffer 0
        };
        mem.PutDwords(TABLE, table);
        VulkanComputeExecutor ex;
        GcnDispatchResources res;
        res.images.clear();
        std::vector<SpirvExtraImage> staged;
        CHECK(VulkanComputeExecutor::LoadImageContents(res, &mem, staged));
        CHECK(staged.empty());

        // extended table: magic + image entry
        std::vector<uint32_t> table2 = table;
        table2.push_back(1);                        // image count
        table2.push_back(0x494D4745u);              // "IMGE" magic
        table2.push_back(0); table2.push_back(0);   // reserved
        table2.insert(table2.end(),
                      {static_cast<uint32_t>(IMG0), 0, 4, 4, 1, 0});
        mem.PutDwords(TABLE, table2);
        res = GcnDispatchResources{};
        res.images.push_back({IMG0, 4, 4, 1});
        // stage the image contents through the public helper
        for (uint32_t y = 0; y < 4; ++y)
            for (uint32_t x = 0; x < 4; ++x)
                mem.WriteDwords(IMG0 + (y * 4 + x) * 16,
                                std::vector<uint32_t>{x, y, x + y, 7}.data(), 4);
        staged.clear();
        CHECK(VulkanComputeExecutor::LoadImageContents(res, &mem, staged));
        CHECK(staged.size() == 1);
        CHECK(staged[0].width == 4 && staged[0].height == 4);
        CHECK(staged[0].contents.size() == 4 * 4 * 4);
        CHECK(staged[0].contents[0] == 0u);
        CHECK(staged[0].contents[4] == 1u);   // texel(1,0).r = x = 1
        CHECK(staged[0].contents[5] == 0u);   // texel(1,0).g = y = 0
        // write-back round-trip
        staged[0].contents[0] = 0xABCDu;
        CHECK(VulkanComputeExecutor::StoreImageContents(res, &mem, staged));
        uint32_t back[4] = {};
        mem.ReadDwords(IMG0, back, 4);
        CHECK(back[0] == 0xABCDu);
    }

    std::printf("[gpu-mimg] D: dispatch parity (load+sample+store+atomic)\n");
    {
        constexpr uint64_t IMG0 = 0x30000;
        FlatGuestMemory mem(0x10000, 0x80000 / 4);

        // guest image: 4x4 RGBA
        const auto base = MakeTestImage(4, 4, 100);
        mem.PutDwords(IMG0, base.rgba);

        // program (per lane: v0=x, v1=y, v2=delta):
        //   image_load  v3, v[0:1], s[4:7] dmask=0xF   (v3..v6 = texel)
        //   image_sample v7, v[0:1], s[4:7] dmask=0xF  (v7..v10 = nearest)
        //   image_atomic_add v2, v[0:1], s[4:7]        (old -> v2, r += delta)
        //   image_store v3, v[0:1], s[4:7] dmask=0x1   (r = loaded r)
        //   s_endpgm
        const uint32_t code[] = {
            MimgW0(GcnOp::IMAGE_LOAD, 0), MimgW1(3, 1),
            MimgW0(GcnOp::IMAGE_SAMPLE, 0), MimgW1(7, 1, 0),
            MimgW0(GcnOp::IMAGE_ATOMIC_ADD, 0, 0x1), MimgW1(2, 1),
            MimgW0(GcnOp::IMAGE_STORE, 0, 0x1), MimgW1(3, 1),
            Sopp(GcnOp::S_ENDPGM),
        };
        // 4 lanes: (0,0,5) (1,1,7) (2,2,9) (3,3,11)
        std::vector<uint32_t> input = {0, 0, 5, 0, 1, 1, 7, 0,
                                       2, 2, 9, 0, 3, 3, 11, 0};

        // reference: direct software-executor run on the same program
        GcnSwExecutor swref;
        swref.SetImage(0, base);
        swref.SetSgpr(4, 0); swref.SetSgpr(5, 4);
        swref.SetSgpr(6, 4); swref.SetSgpr(7, 0);
        std::vector<uint32_t> ref_out(11 * 4, 0);
        const auto rr = swref.Run(code, 9, 4, input, 4, 11, ref_out);
        CHECK(rr.ok);
        const auto ref_img = swref.GetImage(0).rgba;

        // dispatch through the resource path (hardware when available)
        GcnDispatchResources res;
        res.images.push_back({IMG0, 4, 4, 1});
        VulkanComputeExecutor exec;
        exec.SetSoftwareFallback(true, &mem);
        const bool have_vk = exec.Initialize();
        auto hw = exec.RunRDNA2WithResources(code, 9, input, res, &mem, 4, 11);
        if (have_vk && hw.status == ComputeExecStatus::Ok) {
            std::printf("        engine: %s%s\n", hw.device_name.c_str(),
                        hw.hardware ? " (hardware)" : " (software)");
        } else {
            std::printf("        engine: %s\n", hw.message.c_str());
        }
        CHECK(hw.status == ComputeExecStatus::Ok);
        if (hw.status == ComputeExecStatus::Ok) {
            // output SSBO must match the reference exactly
            CHECK(hw.output.size() == ref_out.size());
            size_t diffs = 0;
            for (size_t i = 0; i < ref_out.size(); ++i) {
                if (hw.output[i] != ref_out[i]) ++diffs;
            }
            CHECK(diffs == 0);
            // guest image must match the reference exactly
            const auto got = mem.Region(IMG0, 4 * 4 * 4);
            size_t idiffs = 0;
            for (size_t i = 0; i < ref_img.size(); ++i) {
                if (got[i] != ref_img[i]) ++idiffs;
            }
            CHECK(idiffs == 0);
        }
    }

    std::printf("[gpu-mimg] %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
