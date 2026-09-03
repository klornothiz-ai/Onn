// ============================================================================
// ProsperoLayer RDNA2 Core - PM4 resource-table dispatch test (round 19,
// phase 1: MUBUF/SMEM -> the real dispatch path)
// ----------------------------------------------------------------------------
// Proves the guest's buffer-resource tables now PLUMB into the dispatch:
//   * a PM4 ring with the resource table GVA in COMPUTE_USER_DATA_0 +5..6
//     (compute) / SPI_SHADER_USER_DATA_VS_0 +8..9 (draw) parses from guest
//     memory, and the dispatch executes the SAME program the software
//     executor runs -- MUBUF loads/stores through per-descriptor SSBOs, SMEM
//     through the push-constant mirror base;
//   * THE ACCEPTANCE CHECK: the final guest state (output SSBO + the MUBUF
//     buffer the program stored into) is compared dword-for-dword against a
//     direct GcnSwExecutor reference run -- the exact same VALUES, not just
//     structure. The comparison runs on EVERY host: when a Vulkan device
//     exists the dispatched path is the hardware one, otherwise the honest
//     GCN software interpreter serves it -- either way the guest-visible
//     result must equal the reference exactly;
//   * the mirror base rides a push-constant block (one module serves any
//     window): the compiled module declares exactly one PushConstant
//     variable and the mirror/buffer binding layout matches the compiler's
//     contract (mirror at 2, descriptors after it);
//   * staging plumbing (LoadResourceContents / StoreResourceContents) moves
//     exact contents and fails closed on unreadable ranges;
//   * fail-closed: malformed tables (count 0, count > max, zero stride,
//     mirror without base, unreadable header) drop the resources and the
//     MUBUF program fails honestly -- legacy backend call, guest memory
//     untouched, never a partially-parsed table;
//   * back-compat: no resource slots programmed keeps the round-18 dispatch
//     exactly.
// ============================================================================
#include "gpu/pm4_translator.hpp"
#include "gpu/gpu_guest_memory.hpp"
#include "gpu/vulkan_compute_executor.hpp"
#include "gpu/rdna2_compute_compiler.hpp"
#include "gpu/gcn_decoder.hpp"
#include "graphics/guest_gpu/pm4.h"

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

// Flat guest memory: a base GVA mapped to a contiguous dword vector.
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
    uint32_t At(uint64_t gva) const {
        size_t off = 0;
        return Range(gva, 1, off) ? m_mem[off] : 0u;
    }
    std::vector<uint32_t> Region(uint64_t gva, size_t dwords) const {
        std::vector<uint32_t> out(dwords, 0u);
        size_t off = 0;
        if (Range(gva, dwords, off))
            std::memcpy(out.data(), m_mem.data() + off, dwords * 4);
        return out;
    }
    bool RegionEquals(const FlatGuestMemory& other, uint64_t gva,
                      size_t dwords) const {
        return Region(gva, dwords) == other.Region(gva, dwords);
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

// ---- REAL GFX10 encoders (the layouts gcn_decoder.cpp decodes) ------------
uint32_t Sop1(uint32_t op, uint32_t sdst, uint32_t ssrc0) {
    return 0x7D800000u | (op << 16u) | (sdst << 9u) | ssrc0;
}
uint32_t Vop1(uint32_t op, uint32_t dst, uint32_t src0) {
    return 0x7E000000u | (op << 17u) | (dst << 9u) | src0;
}
uint32_t Vop2(uint32_t op, uint32_t dst, uint32_t src1_vgpr, uint32_t src0) {
    return (op << 25u) | (dst << 17u) | (src1_vgpr << 9u) | src0;
}
uint32_t Sopp(uint32_t op, uint32_t s = 0) {
    return 0xBF800000u | (op << 16u) | s;
}
uint32_t V(uint32_t n) { return 256U + n; }

// SMEM: s_load_dword sdst, s[sbase:sbase+1], +offset (immediate).
uint32_t SmemLoadW0(uint32_t sdst, uint32_t sbase_field) {
    return 0xC0000000u | (GcnOp::S_LOAD_DWORD << 18u) | (1u << 17u) |
           (sdst << 6u) | sbase_field;
}
// MUBUF w0/w1 pair (vaddr in w1[7:0], vdata w1[15:8], srsrc w1[20:16],
// offen w1[22]); srsrc field 1 == the s[4:7] quad == descriptor 0.
uint32_t MubufW0(uint32_t op, uint32_t buf_offset = 0) {
    return 0xE0000000u | (op << 18u) | buf_offset;
}
uint32_t MubufW1(uint32_t vaddr, uint32_t vdata, uint32_t srsrc_field,
                 bool offen) {
    return vaddr | (vdata << 8u) | (srsrc_field << 16u) |
           (offen ? (1u << 22u) : 0u);
}

uint32_t Hdr(size_t payload_dwords, uint8_t opcode) {
    return (3u << 30u) | ((static_cast<uint32_t>(payload_dwords) - 1u) << 16u) |
           (static_cast<uint32_t>(opcode) << 8u);
}
uint32_t AsB(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }

constexpr uint8_t OP_SET_SH_REG      = 0x76;
constexpr uint8_t OP_DISPATCH_DIRECT = 0x04;
constexpr uint8_t OP_DRAW_INDEX_AUTO = 0x23;

// ---- SPIR-V structure walkers ----------------------------------------------
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
        // OpDecorate target, Decoration(33 = Binding), value...
        if (op == 71 && wc >= 4 && m[off + 2] == 33 && m[off + 3] == binding) {
            ++count;
        }
        off += wc;
    }
    return count;
}
size_t CountDecoration(const std::vector<uint32_t>& m, uint32_t decoration) {
    size_t count = 0, off = 5;
    while (off < m.size()) {
        const uint16_t op = static_cast<uint16_t>(m[off] & 0xffffu);
        const uint16_t wc = static_cast<uint16_t>(m[off] >> 16u);
        if (wc == 0 || off + wc > m.size()) return 0;
        if (op == 71 && wc >= 3 && m[off + 2] == decoration) ++count;
        off += wc;
    }
    return count;
}

// The round-19 memory program: per lane, MUBUF-load BUF[v0] (descriptor 0,
// offen), add the SMEM-loaded scalar, store back, and export the result.
//   s_mov_b32 s0, lo(MIRROR) ; s_mov_b32 s1, hi(MIRROR)
//   s_load_dword s2, s[0:1], +0x40
//   buffer_load_dword v2, v[0], s[4:7], 0 offen
//   v_add_f32 v2, v2, s2
//   buffer_store_dword v2, v[0], s[4:7], 0 offen
//   v_mov_b32 v0, v2
//   s_endpgm
std::vector<uint32_t> MakeMemoryProgram(uint64_t mirror_gva) {
    return {
        Sop1(GcnOp::S_MOV_B32, 0, 0xFFu),              // s0 = literal lo
        static_cast<uint32_t>(mirror_gva & 0xffffffffu),
        Sop1(GcnOp::S_MOV_B32, 1, 0xFFu),              // s1 = literal hi
        static_cast<uint32_t>(mirror_gva >> 32),
        SmemLoadW0(/*sdst=*/2, /*sbase=*/0),           // s_load_dword s2, s[0:1]
        0x40u,                                          // +0x40 (offset)
        MubufW0(GcnOp::BUFFER_LOAD_DWORD),             // buffer_load_dword
        MubufW1(/*vaddr=*/0, /*vdata=*/2, /*srsrc=*/1, /*offen=*/true),
        Vop2(GcnOp::V_ADD_F32, /*dst=*/2, /*src1 v2=*/2, /*src0 s2=*/2),
        MubufW0(GcnOp::BUFFER_STORE_DWORD),            // buffer_store_dword
        MubufW1(0, 2, 1, true),
        Vop1(GcnOp::V_MOV_B32, /*dst=*/0, V(2)),       // v0 = v2
        Sopp(GcnOp::S_ENDPGM),
    };
}

// Builds the resource-table ABI image in guest memory (see pm4.h).
void PutResourceTable(FlatGuestMemory& mem, uint64_t table_gva,
                      const std::vector<GcnBufferResource>& buffers,
                      uint64_t mirror_base, uint32_t mirror_dwords) {
    std::vector<uint32_t> words;
    words.push_back(static_cast<uint32_t>(buffers.size()));
    words.push_back(static_cast<uint32_t>(mirror_base & 0xffffffffu));
    words.push_back(static_cast<uint32_t>(mirror_base >> 32));
    words.push_back(mirror_dwords);
    for (const auto& b : buffers) {
        words.push_back(static_cast<uint32_t>(b.base_gva & 0xffffffffu));
        words.push_back(static_cast<uint32_t>(b.base_gva >> 32));
        words.push_back(b.size_dwords);
        words.push_back(b.stride);
    }
    mem.PutDwords(table_gva, words);
}

} // namespace

int main() {
    std::cout << "[res-dispatch] round 19 phase 1: MUBUF/SMEM resource tables "
                 "on the real dispatch path\n";

    VulkanComputeExecutor exec;
    const bool available = exec.Initialize();
    std::cout << (available ? "[info] Vulkan device: " + exec.DeviceName()
                            : std::string("[info] no Vulkan device; the honest GCN "
                                          "software interpreter serves the dispatch"))
              << "\n";

    const uint64_t BASE        = 0x1400000000ull;
    const uint64_t SHADER_GVA  = BASE + 0x0000;
    const uint64_t INPUT_GVA   = BASE + 0x1000;
    const uint64_t OUTPUT_GVA  = BASE + 0x1400;
    const uint64_t BUF_GVA     = BASE + 0x2000;
    const uint64_t MIRROR_GVA  = BASE + 0x4000;
    const uint64_t RTABLE_GVA  = BASE + 0x6000;
    const uint64_t ATTR_GVA    = BASE + 0x7000;   // draw-path input buffer
    const uint64_t OUT2_GVA    = BASE + 0x7400;   // draw-path output
    const uint32_t N           = 16;
    const uint32_t MIRROR_DWORDS = 0x400;
    const uint32_t SMEM_CONST = 0x42480000u;      // 50.0f

    const std::vector<uint32_t> program = MakeMemoryProgram(MIRROR_GVA);

    // Initial guest state, shared by every scenario.
    auto make_mem = [&]() {
        FlatGuestMemory mem(BASE, 0x10000 / 4);
        mem.PutDwords(SHADER_GVA, program);
        // lane inputs: the integer MUBUF index i (offen -> BUF + i*4).
        std::vector<uint32_t> input(N);
        for (uint32_t i = 0; i < N; ++i) input[i] = i;
        mem.PutDwords(INPUT_GVA, input);
        // the MUBUF buffer: float bits 100..115 (exactly representable).
        std::vector<uint32_t> buf(N);
        for (uint32_t i = 0; i < N; ++i) buf[i] = AsB(100.0f + static_cast<float>(i));
        mem.PutDwords(BUF_GVA, buf);
        // the SMEM scalar inside the mirror window.
        mem.PutDwords(MIRROR_GVA + 0x40, {SMEM_CONST});
        return mem;
    };

    const std::vector<GcnBufferResource> table_buffers = {
        {BUF_GVA, N, 1},
    };
    struct ReferenceRun {
        FlatGuestMemory mem;
        std::vector<uint32_t> output;
    };
    auto make_reference = [&]() -> ReferenceRun {
        // Direct GcnSwExecutor run over an identical guest image: the value
        // reference the dispatched path must reproduce EXACTLY. The lane
        // outputs come back in `output`; MUBUF stores land in the guest
        // image itself (exactly like the dispatched path).
        FlatGuestMemory ref = make_mem();
        GcnSwExecutor sw;
        std::vector<uint32_t> input(N);
        for (uint32_t i = 0; i < N; ++i) input[i] = i;
        std::vector<uint32_t> output;
        GcnSwExecResult r = sw.Run(program.data(), program.size(), N, input,
                                   1, 1, output, &ref, &table_buffers);
        if (!r.ok) {
            std::cerr << "  [SW reference] error: " << r.error << "\n";
        }
        return {std::move(ref), std::move(output)};
    };

    // =====================================================================
    // A: compute dispatch with the resource table -- exact-value parity.
    // =====================================================================
    std::cout << "[res-dispatch] A: compute dispatch, exact guest-state parity\n";
    {
        FlatGuestMemory mem = make_mem();
        PutResourceTable(mem, RTABLE_GVA, table_buffers, MIRROR_GVA,
                         MIRROR_DWORDS);

        std::vector<uint32_t> ring;
        auto put_reg = [&](uint32_t off, uint32_t val) {
            ring.push_back(Hdr(2, OP_SET_SH_REG));
            ring.push_back(off);
            ring.push_back(val);
        };
        put_reg(Pm4::COMPUTE_PGM_LO, static_cast<uint32_t>(SHADER_GVA >> 8));
        put_reg(Pm4::COMPUTE_PGM_HI, static_cast<uint32_t>(SHADER_GVA >> 32));
        put_reg(Pm4::COMPUTE_USER_DATA_0 + 0,
                static_cast<uint32_t>(INPUT_GVA & 0xffffffffu));
        put_reg(Pm4::COMPUTE_USER_DATA_0 + 1,
                static_cast<uint32_t>(INPUT_GVA >> 32));
        put_reg(Pm4::COMPUTE_USER_DATA_0 + 2,
                static_cast<uint32_t>(OUTPUT_GVA & 0xffffffffu));
        put_reg(Pm4::COMPUTE_USER_DATA_0 + 3,
                static_cast<uint32_t>(OUTPUT_GVA >> 32));
        put_reg(Pm4::COMPUTE_USER_DATA_0 + 4, N);
        put_reg(Pm4::COMPUTE_USER_DATA_RESOURCE_LO,
                static_cast<uint32_t>(RTABLE_GVA & 0xffffffffu));
        put_reg(Pm4::COMPUTE_USER_DATA_RESOURCE_HI,
                static_cast<uint32_t>(RTABLE_GVA >> 32));
        ring.push_back(Hdr(4, OP_DISPATCH_DIRECT));
        ring.push_back(1); ring.push_back(1); ring.push_back(1); ring.push_back(0);

        VulkanRendererBackend backend;
        backend.Initialize();
        PM4VulkanTranslator translator(backend);
        translator.BindComputeExecutor(&exec, &mem);
        const auto result = translator.TranslateAndExecuteCommandRingChecked(
            ring.data(), ring.size());
        CHECK(result.ok());

        const auto& disp = translator.GetLastComputeDispatch();
        CHECK(disp.attempted);
        CHECK(disp.resources_programmed);
        CHECK(disp.resources_parsed);
        CHECK(disp.resource_error.empty());
        CHECK(disp.resource_buffer_count == 1);
        CHECK(disp.resource_mirror_gva == MIRROR_GVA);
        CHECK(disp.resource_mirror_dwords == MIRROR_DWORDS);
        CHECK(disp.executed_on_gpu == available);

        // THE acceptance check: the dispatched path's final guest state must
        // equal the software-executor reference EXACTLY -- the output SSBO
        // (the lane outputs) and the MUBUF-modified buffer, dword for dword.
        ReferenceRun ref = make_reference();
        CHECK(ref.output == mem.Region(OUTPUT_GVA, N));
        CHECK(ref.mem.Region(BUF_GVA, N) == mem.Region(BUF_GVA, N));
        // And the values are the expected ones ((100 + i) + 50, exact in
        // float32 for this range).
        CHECK(mem.At(OUTPUT_GVA) == AsB(150.0f));
        CHECK(mem.At(OUTPUT_GVA + 4 * 7) == AsB(157.0f));
        CHECK(mem.At(BUF_GVA + 4 * 3) == AsB(153.0f));
        CHECK(backend.GetDispatchedComputeCount() == 1);
        std::cout << "  [ok] output SSBO + MUBUF buffer match the SW reference "
                     "dword-for-dword (served by "
                  << (available ? "the Vulkan device" : "the GCN interpreter")
                  << ")\n";
    }

    // =====================================================================
    // B: draw path (DRAW_INDEX_AUTO) with the vertex-stage resource table.
    // =====================================================================
    std::cout << "[res-dispatch] B: draw dispatch, resource table via VS +8..9\n";
    {
        FlatGuestMemory mem = make_mem();
        PutResourceTable(mem, RTABLE_GVA, table_buffers, MIRROR_GVA,
                         MIRROR_DWORDS);
        std::vector<uint32_t> attr(N);
        for (uint32_t i = 0; i < N; ++i) attr[i] = i;
        mem.PutDwords(ATTR_GVA, attr);

        std::vector<uint32_t> ring;
        auto put_reg = [&](uint32_t off, uint32_t val) {
            ring.push_back(Hdr(2, OP_SET_SH_REG));
            ring.push_back(off);
            ring.push_back(val);
        };
        put_reg(Pm4::SPI_SHADER_PGM_LO_VS,
                static_cast<uint32_t>(SHADER_GVA >> 8));
        put_reg(Pm4::SPI_SHADER_PGM_HI_VS,
                static_cast<uint32_t>(SHADER_GVA >> 32));
        put_reg(Pm4::SPI_SHADER_USER_DATA_VS_0 + 0,
                static_cast<uint32_t>(ATTR_GVA & 0xffffffffu));
        put_reg(Pm4::SPI_SHADER_USER_DATA_VS_0 + 1,
                static_cast<uint32_t>(ATTR_GVA >> 32));
        put_reg(Pm4::SPI_SHADER_USER_DATA_VS_0 + 2,
                static_cast<uint32_t>(OUT2_GVA & 0xffffffffu));
        put_reg(Pm4::SPI_SHADER_USER_DATA_VS_0 + 3,
                static_cast<uint32_t>(OUT2_GVA >> 32));
        put_reg(Pm4::SPI_SHADER_USER_DATA_VS_0 + 4, N);
        put_reg(Pm4::SPI_SHADER_USER_DATA_VS_RESOURCE_LO,
                static_cast<uint32_t>(RTABLE_GVA & 0xffffffffu));
        put_reg(Pm4::SPI_SHADER_USER_DATA_VS_RESOURCE_HI,
                static_cast<uint32_t>(RTABLE_GVA >> 32));
        ring.push_back(Hdr(2, OP_DRAW_INDEX_AUTO));
        ring.push_back(N);
        ring.push_back(0);   // draw initiator

        VulkanRendererBackend backend;
        backend.Initialize();
        PM4VulkanTranslator translator(backend);
        translator.BindComputeExecutor(&exec, &mem);
        const auto result = translator.TranslateAndExecuteCommandRingChecked(
            ring.data(), ring.size());
        CHECK(result.ok());

        const auto& draw = translator.GetLastDrawDispatch();
        CHECK(draw.attempted);
        CHECK(!draw.indexed);
        CHECK(draw.resources_programmed);
        CHECK(draw.resources_parsed);
        CHECK(draw.resource_buffer_count == 1);
        CHECK(draw.executed_on_gpu == available);

        // The vertex stage read the SAME buffer through MUBUF: the
        // transformed vertices equal the reference exactly.
        FlatGuestMemory ref = make_mem();
        std::vector<uint32_t> ref_out;
        GcnSwExecutor sw;
        std::vector<uint32_t> input(N);
        for (uint32_t i = 0; i < N; ++i) input[i] = i;
        CHECK(sw.Run(program.data(), program.size(), N, input, 1, 1, ref_out,
                     &ref, &table_buffers).ok);
        std::vector<uint32_t> got = mem.Region(OUT2_GVA, N);
        CHECK(got == ref_out);
        CHECK(got[0] == AsB(150.0f));
        CHECK(backend.GetDrawCallCount() == 1);
        std::cout << "  [ok] draw-path transformed vertices match the SW "
                     "reference exactly\n";
    }

    // =====================================================================
    // C: fail-closed -- malformed tables drop the resources; the MUBUF
    // program then fails honestly (legacy path, guest memory untouched).
    // =====================================================================
    std::cout << "[res-dispatch] C: malformed tables fail closed\n";
    {
        struct Case {
            const char* name;
            std::vector<uint32_t> table_words;
        };
        std::vector<uint32_t> good_entry = {
            static_cast<uint32_t>(BUF_GVA & 0xffffffffu),
            static_cast<uint32_t>(BUF_GVA >> 32), N, 1};
        const std::vector<Case> cases = {
            {"count zero", {0u, 0u, 0u, 0u}},
            {"count over max",
             {Pm4::GCN_MAX_BUFFER_RESOURCES + 1u, 0u, 0u, 0u}},
            {"zero stride",
             {1u, static_cast<uint32_t>(MIRROR_GVA & 0xffffffffu),
              static_cast<uint32_t>(MIRROR_GVA >> 32), MIRROR_DWORDS,
              good_entry[0], good_entry[1], good_entry[2], 0u}},
            {"mirror size without base", {1u, 0u, 0u, 16u,
                                           good_entry[0], good_entry[1],
                                           good_entry[2], good_entry[3]}},
            {"entry size zero", {1u, 0u, 0u, 0u, good_entry[0],
                                 good_entry[1], 0u, good_entry[3]}},
        };
        for (const auto& c : cases) {
            FlatGuestMemory mem = make_mem();
            mem.PutDwords(RTABLE_GVA, c.table_words);

            std::vector<uint32_t> ring;
            auto put_reg = [&](uint32_t off, uint32_t val) {
                ring.push_back(Hdr(2, OP_SET_SH_REG));
                ring.push_back(off);
                ring.push_back(val);
            };
            put_reg(Pm4::COMPUTE_PGM_LO, static_cast<uint32_t>(SHADER_GVA >> 8));
            put_reg(Pm4::COMPUTE_PGM_HI, static_cast<uint32_t>(SHADER_GVA >> 32));
            put_reg(Pm4::COMPUTE_USER_DATA_0 + 0,
                    static_cast<uint32_t>(INPUT_GVA & 0xffffffffu));
            put_reg(Pm4::COMPUTE_USER_DATA_0 + 1,
                    static_cast<uint32_t>(INPUT_GVA >> 32));
            put_reg(Pm4::COMPUTE_USER_DATA_0 + 2,
                    static_cast<uint32_t>(OUTPUT_GVA & 0xffffffffu));
            put_reg(Pm4::COMPUTE_USER_DATA_0 + 3,
                    static_cast<uint32_t>(OUTPUT_GVA >> 32));
            put_reg(Pm4::COMPUTE_USER_DATA_0 + 4, N);
            put_reg(Pm4::COMPUTE_USER_DATA_RESOURCE_LO,
                    static_cast<uint32_t>(RTABLE_GVA & 0xffffffffu));
            put_reg(Pm4::COMPUTE_USER_DATA_RESOURCE_HI,
                    static_cast<uint32_t>(RTABLE_GVA >> 32));
            ring.push_back(Hdr(4, OP_DISPATCH_DIRECT));
            ring.push_back(1); ring.push_back(1); ring.push_back(1);
            ring.push_back(0);

            VulkanRendererBackend backend;
            backend.Initialize();
            PM4VulkanTranslator translator(backend);
            translator.BindComputeExecutor(&exec, &mem);
            const auto result = translator.TranslateAndExecuteCommandRingChecked(
                ring.data(), ring.size());
            CHECK(result.ok());

            const auto& disp = translator.GetLastComputeDispatch();
            CHECK(disp.resources_programmed);
            CHECK(!disp.resources_parsed);
            CHECK(!disp.resource_error.empty());
            CHECK(!disp.executed_on_gpu);
            // Nothing executed: output stays zero, the buffer is untouched,
            // the legacy backend call still advanced the counter.
            CHECK(mem.At(OUTPUT_GVA) == 0u);
            CHECK(mem.At(BUF_GVA) == AsB(100.0f));
            CHECK(backend.GetDispatchedComputeCount() == 1);
            std::cout << "  [ok] '" << c.name << "' rejected: "
                      << disp.resource_error << "\n";
        }

        // Unreadable table header (beyond the guest region).
        FlatGuestMemory mem = make_mem();
        const uint64_t bad_gva = BASE + 0x200000ull;   // outside the flat map
        std::vector<uint32_t> ring;
        auto put_reg = [&](uint32_t off, uint32_t val) {
            ring.push_back(Hdr(2, OP_SET_SH_REG));
            ring.push_back(off);
            ring.push_back(val);
        };
        put_reg(Pm4::COMPUTE_PGM_LO, static_cast<uint32_t>(SHADER_GVA >> 8));
        put_reg(Pm4::COMPUTE_PGM_HI, static_cast<uint32_t>(SHADER_GVA >> 32));
        put_reg(Pm4::COMPUTE_USER_DATA_0 + 0,
                static_cast<uint32_t>(INPUT_GVA & 0xffffffffu));
        put_reg(Pm4::COMPUTE_USER_DATA_0 + 1,
                static_cast<uint32_t>(INPUT_GVA >> 32));
        put_reg(Pm4::COMPUTE_USER_DATA_0 + 2,
                static_cast<uint32_t>(OUTPUT_GVA & 0xffffffffu));
        put_reg(Pm4::COMPUTE_USER_DATA_0 + 3,
                static_cast<uint32_t>(OUTPUT_GVA >> 32));
        put_reg(Pm4::COMPUTE_USER_DATA_0 + 4, N);
        put_reg(Pm4::COMPUTE_USER_DATA_RESOURCE_LO,
                static_cast<uint32_t>(bad_gva & 0xffffffffu));
        put_reg(Pm4::COMPUTE_USER_DATA_RESOURCE_HI,
                static_cast<uint32_t>(bad_gva >> 32));
        ring.push_back(Hdr(4, OP_DISPATCH_DIRECT));
        ring.push_back(1); ring.push_back(1); ring.push_back(1); ring.push_back(0);
        VulkanRendererBackend backend;
        backend.Initialize();
        PM4VulkanTranslator translator(backend);
        translator.BindComputeExecutor(&exec, &mem);
        CHECK(translator.TranslateAndExecuteCommandRingChecked(
                  ring.data(), ring.size()).ok());
        const auto& disp = translator.GetLastComputeDispatch();
        CHECK(disp.resources_programmed);
        CHECK(!disp.resources_parsed);
        CHECK(mem.At(OUTPUT_GVA) == 0u);
    }

    // =====================================================================
    // D: back-compat -- no resource slots programmed keeps round 18 exactly
    // (a plain program runs; the record carries no resources).
    // =====================================================================
    std::cout << "[res-dispatch] D: no resource table -> round-18 behaviour\n";
    {
        FlatGuestMemory mem = make_mem();
        // v_mul_u32_u24 (integer squares) on the raw input dwords.
        const std::vector<uint32_t> plain = {
            Vop2(GcnOp::V_MUL_U32_U24, 0, 0, V(0)),   // v0 = v0 * v0 (u32)
            Sopp(GcnOp::S_ENDPGM),
        };
        mem.PutDwords(SHADER_GVA, plain);

        std::vector<uint32_t> ring;
        auto put_reg = [&](uint32_t off, uint32_t val) {
            ring.push_back(Hdr(2, OP_SET_SH_REG));
            ring.push_back(off);
            ring.push_back(val);
        };
        put_reg(Pm4::COMPUTE_PGM_LO, static_cast<uint32_t>(SHADER_GVA >> 8));
        put_reg(Pm4::COMPUTE_PGM_HI, static_cast<uint32_t>(SHADER_GVA >> 32));
        put_reg(Pm4::COMPUTE_USER_DATA_0 + 0,
                static_cast<uint32_t>(INPUT_GVA & 0xffffffffu));
        put_reg(Pm4::COMPUTE_USER_DATA_0 + 1,
                static_cast<uint32_t>(INPUT_GVA >> 32));
        put_reg(Pm4::COMPUTE_USER_DATA_0 + 2,
                static_cast<uint32_t>(OUTPUT_GVA & 0xffffffffu));
        put_reg(Pm4::COMPUTE_USER_DATA_0 + 3,
                static_cast<uint32_t>(OUTPUT_GVA >> 32));
        put_reg(Pm4::COMPUTE_USER_DATA_0 + 4, N);
        ring.push_back(Hdr(4, OP_DISPATCH_DIRECT));
        ring.push_back(1); ring.push_back(1); ring.push_back(1); ring.push_back(0);

        VulkanRendererBackend backend;
        backend.Initialize();
        PM4VulkanTranslator translator(backend);
        translator.BindComputeExecutor(&exec, &mem);
        CHECK(translator.TranslateAndExecuteCommandRingChecked(
                  ring.data(), ring.size()).ok());
        const auto& disp = translator.GetLastComputeDispatch();
        CHECK(disp.attempted);
        CHECK(!disp.resources_programmed);
        CHECK(!disp.resources_parsed);
        CHECK(disp.resource_buffer_count == 0);
        CHECK(disp.executed_on_gpu == available);
        // v_mul_u32_u24 on the raw integer lanes: i*i exactly.
        CHECK(mem.At(OUTPUT_GVA) == 0u);                 // 0*0
        CHECK(mem.At(OUTPUT_GVA + 4 * 5) == 25u);        // 5*5
    }

    // =====================================================================
    // E: the push-constant mirror base -- module structure.
    // =====================================================================
    std::cout << "[res-dispatch] E: push-constant mirror-base structure\n";
    {
        ComputeCompilerOptions opt;
        opt.buffers = table_buffers;
        opt.scalar_mirror_base_gva = MIRROR_GVA;
        RDNA2ComputeCompiler cc(opt);
        auto r = cc.Compile(program.data(), program.size());
        CHECK(r.success);
        CHECK(r.used_scalar_mirror);
        CHECK(r.buffer_bindings == 1);
        CHECK(ParsesCleanly(r.spirv));
        // Exactly one PushConstant variable (storage class 9).
        CHECK(CountVariableStorage(r.spirv, 9) == 1);
        // The mirror SSBO sits at binding 2, the MUBUF descriptor at 3.
        CHECK(CountBinding(r.spirv, 2) == 1);
        CHECK(CountBinding(r.spirv, 3) == 1);
        // The push-constant struct carries the Block decoration (2).
        CHECK(CountDecoration(r.spirv, 2) == 1);

        // Without a mirror the module declares no push constants at all and
        // SMEM still fails closed (no base).
        ComputeCompilerOptions no_mirror;
        no_mirror.buffers = table_buffers;
        RDNA2ComputeCompiler cc2(no_mirror);
        const std::vector<uint32_t> smem_only = {
            SmemLoadW0(2, 0), 0x40u, Sopp(GcnOp::S_ENDPGM)};
        auto r2 = cc2.Compile(smem_only.data(), smem_only.size());
        CHECK(!r2.success);   // mirror disabled -> fail closed
    }

    // =====================================================================
    // F: the staging helpers -- exact contents, fail-closed ranges.
    // =====================================================================
    std::cout << "[res-dispatch] F: staging helpers\n";
    {
        FlatGuestMemory mem = make_mem();
        GcnDispatchResources res;
        res.buffers = table_buffers;
        res.scalar_mirror_base_gva = MIRROR_GVA;
        res.scalar_mirror_dwords = MIRROR_DWORDS;
        std::vector<std::vector<uint32_t>> bufs;
        std::vector<uint32_t> mirror;
        CHECK(VulkanComputeExecutor::LoadResourceContents(res, &mem, bufs,
                                                          mirror));
        CHECK(bufs.size() == 1);
        CHECK(bufs[0].size() == N);
        CHECK(bufs[0][0] == AsB(100.0f));
        CHECK(bufs[0][N - 1] == AsB(100.0f + static_cast<float>(N - 1)));
        CHECK(mirror.size() == MIRROR_DWORDS);
        CHECK(mirror[0x40 / 4] == SMEM_CONST);

        // Store-back: modified contents land in guest memory exactly.
        bufs[0][0] = 0x7F7F7F7Fu;
        CHECK(VulkanComputeExecutor::StoreResourceContents(res, &mem, bufs));
        CHECK(mem.At(BUF_GVA) == 0x7F7F7F7Fu);
        CHECK(mem.At(BUF_GVA + 4) == AsB(101.0f));

        // Unreadable buffer -> no partial staging.
        GcnDispatchResources bad = res;
        bad.buffers[0].base_gva = BASE + 0x400000ull;  // outside the map
        std::vector<std::vector<uint32_t>> bufs2;
        std::vector<uint32_t> mirror2;
        CHECK(!VulkanComputeExecutor::LoadResourceContents(bad, &mem, bufs2,
                                                           mirror2));
        CHECK(bufs2.empty());

        // Size mismatch -> the write-back refuses (never a torn buffer).
        std::vector<std::vector<uint32_t>> torn = {{1u, 2u, 3u}};
        CHECK(!VulkanComputeExecutor::StoreResourceContents(res, &mem, torn));
        CHECK(mem.At(BUF_GVA) == 0x7F7F7F7Fu);   // unchanged
    }

    std::cout << "[res-dispatch] " << g_checks << " checks, " << g_failures
              << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
