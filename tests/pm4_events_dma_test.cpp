// pm4_events_dma_test.cpp — round 29: real PM4 packet semantics.
//   * PKT3_SET_UCONFIG_REG stores registers (queryable, not ignored),
//   * PKT3_EVENT_WRITE advances the event counter AND publishes the fence
//     value to guest memory,
//   * PKT3_EVENT_WRITE_EOS publishes its immediate payload,
//   * PKT3_DMA_DATA performs a REAL dword-granular memory copy through the
//     guest bridge (fail-closed on unmapped ranges),
//   * PKT3_SET_PREDICATION establishes conditional rendering: an active,
//     failing predication suppresses subsequent draw packets.
#include "gpu/gpu_guest_memory.hpp"
#include "gpu/pm4_translator.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using namespace PS5::GPU;

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

class FlatMem final : public GpuGuestMemory {
public:
    FlatMem(uint64_t base, size_t dwords) : m_base(base), m_mem(dwords, 0u) {}
    bool ReadDwords(uint64_t gva, uint32_t* dst, size_t dwords) override {
        size_t off;
        if (!Range(gva, dwords, off)) return false;
        std::memcpy(dst, m_mem.data() + off, dwords * 4);
        return true;
    }
    bool WriteDwords(uint64_t gva, const uint32_t* src, size_t dwords) override {
        size_t off;
        if (!Range(gva, dwords, off)) return false;
        std::memcpy(m_mem.data() + off, src, dwords * 4);
        return true;
    }
    uint32_t At(uint64_t gva) {
        size_t off = 0;
        return Range(gva, 1, off) ? m_mem[off] : 0u;
    }
    void Put(uint64_t gva, const std::vector<uint32_t>& v) {
        size_t off = 0;
        if (Range(gva, v.size(), off))
            std::memcpy(m_mem.data() + off, v.data(), v.size() * 4);
    }

private:
    bool Range(uint64_t gva, size_t dwords, size_t& off) {
        if (gva < m_base) return false;
        off = static_cast<size_t>((gva - m_base) / 4);
        return off + dwords <= m_mem.size();
    }
    uint64_t m_base;
    std::vector<uint32_t> m_mem;
};

uint32_t Hdr(size_t payload_dwords, uint8_t opcode) {
    return (3u << 30) | ((static_cast<uint32_t>(payload_dwords) - 1u) << 16) |
           (static_cast<uint32_t>(opcode) << 8);
}

} // namespace

int main() {
    std::printf("== PM4 events / DMA / predication (round 29) ==\n");

    const uint64_t BASE = 0x1300000000ull;
    const uint64_t FENCE_GVA = BASE + 0x100;
    const uint64_t EOS_GVA = BASE + 0x200;
    const uint64_t SRC_GVA = BASE + 0x300;
    const uint64_t DST_GVA = BASE + 0x400;
    const uint64_t PRED_GVA = BASE + 0x500;
    FlatMem mem(BASE, 0x1000 / 4);

    VulkanRendererBackend backend;
    backend.Initialize();
    PM4VulkanTranslator translator(backend);
    translator.BindComputeExecutor(nullptr, &mem);

    // ---- SET_UCONFIG_REG: registers stored + queryable -------------------
    std::printf("[pm4-29] SET_UCONFIG_REG\n");
    {
        std::vector<uint32_t> ring;
        ring.push_back(Hdr(4, 0x79));   // PKT3_SET_UCONFIG_REG
        ring.push_back(0x2E00);          // first register offset
        ring.push_back(0x11111111u);
        ring.push_back(0x22222222u);
        ring.push_back(0x33333333u);
        const auto r = translator.TranslateAndExecuteCommandRingChecked(
            ring.data(), ring.size());
        CHECK(r.ok());
        CHECK(translator.GetUconfigReg(0x2E00) == 0x11111111u);
        CHECK(translator.GetUconfigReg(0x2E01) == 0x22222222u);
        CHECK(translator.GetUconfigReg(0x2E02) == 0x33333333u);
        CHECK(translator.GetUconfigReg(0x2E03) == 0u);   // never programmed
    }

    // ---- EVENT_WRITE: counter + fence value -------------------------------
    std::printf("[pm4-29] EVENT_WRITE fence\n");
    {
        std::vector<uint32_t> ring;
        ring.push_back(Hdr(3, 0x46));    // PKT3_EVENT_WRITE
        ring.push_back(0x10);            // event type (BOTTOM_OF_PIPE)
        ring.push_back(static_cast<uint32_t>(FENCE_GVA & 0xffffffffu));
        ring.push_back(static_cast<uint32_t>(FENCE_GVA >> 32));
        const auto r = translator.TranslateAndExecuteCommandRingChecked(
            ring.data(), ring.size());
        CHECK(r.ok());
        CHECK(translator.EventCounter() >= 1);
        CHECK(mem.At(FENCE_GVA) ==
              static_cast<uint32_t>(translator.EventCounter()));
        std::printf("  [ok] event counter %llu published to guest memory\n",
                    static_cast<unsigned long long>(translator.EventCounter()));
    }

    // ---- EVENT_WRITE_EOS: immediate payload --------------------------------
    std::printf("[pm4-29] EVENT_WRITE_EOS immediate\n");
    {
        std::vector<uint32_t> ring;
        ring.push_back(Hdr(5, 0x48));    // PKT3_EVENT_WRITE_EOS
        ring.push_back(0x10);
        ring.push_back(static_cast<uint32_t>(EOS_GVA & 0xffffffffu));
        ring.push_back(static_cast<uint32_t>(EOS_GVA >> 32));
        ring.push_back(0xDEADBEEFu);     // value lo
        ring.push_back(0x00000042u);     // value hi
        const auto r = translator.TranslateAndExecuteCommandRingChecked(
            ring.data(), ring.size());
        CHECK(r.ok());
        CHECK(mem.At(EOS_GVA) == 0xDEADBEEFu);
        CHECK(mem.At(EOS_GVA + 4) == 0x42u);
    }

    // ---- DMA_DATA: real copy ------------------------------------------------
    std::printf("[pm4-29] DMA_DATA copy\n");
    {
        mem.Put(SRC_GVA, {10u, 20u, 30u, 40u});
        std::vector<uint32_t> ring;
        ring.push_back(Hdr(6, 0x50));    // PKT3_DMA_DATA
        ring.push_back(0);               // CTL
        ring.push_back(static_cast<uint32_t>(SRC_GVA & 0xffffffffu));
        ring.push_back(static_cast<uint32_t>(SRC_GVA >> 32));
        ring.push_back(static_cast<uint32_t>(DST_GVA & 0xffffffffu));
        ring.push_back(static_cast<uint32_t>(DST_GVA >> 32));
        ring.push_back(16);              // 16 bytes = 4 dwords
        const auto r = translator.TranslateAndExecuteCommandRingChecked(
            ring.data(), ring.size());
        CHECK(r.ok());
        CHECK(mem.At(DST_GVA) == 10u);
        CHECK(mem.At(DST_GVA + 4) == 20u);
        CHECK(mem.At(DST_GVA + 8) == 30u);
        CHECK(mem.At(DST_GVA + 12) == 40u);

        // Fail-closed: unmapped source copies NOTHING.
        std::vector<uint32_t> ring2;
        ring2.push_back(Hdr(6, 0x50));
        ring2.push_back(0);
        ring2.push_back(static_cast<uint32_t>((BASE + 0x100000) & 0xffffffffu));
        ring2.push_back(static_cast<uint32_t>((BASE + 0x100000) >> 32));
        ring2.push_back(static_cast<uint32_t>(DST_GVA & 0xffffffffu));
        ring2.push_back(static_cast<uint32_t>(DST_GVA >> 32));
        ring2.push_back(8);
        const auto r2 = translator.TranslateAndExecuteCommandRingChecked(
            ring2.data(), ring2.size());
        CHECK(r2.ok());
        CHECK(mem.At(DST_GVA) == 10u);   // untouched
    }

    // ---- SET_PREDICATION: conditional rendering -----------------------------
    std::printf("[pm4-29] SET_PREDICATION + conditional draw\n");
    {
        auto pred_packet = [&](uint64_t gva) {
            std::vector<uint32_t> ring;
            ring.push_back(Hdr(3, 0x30));   // PKT3_SET_PREDICATION
            ring.push_back(0);              // op = draw-visibility, no keep
            ring.push_back(static_cast<uint32_t>(gva & 0xffffffffu));
            ring.push_back(static_cast<uint32_t>(gva >> 32));
            return ring;
        };
        // Pass (bit 63 clear)
        mem.Put(PRED_GVA, {0x00000000u, 0x00000000u});
        {
            auto ring = pred_packet(PRED_GVA);
            const auto r = translator.TranslateAndExecuteCommandRingChecked(
                ring.data(), ring.size());
            CHECK(r.ok());
            CHECK(translator.IsPredicationActive());
            CHECK(translator.PredicationPasses());
        }
        // Fail (bit 63 set): draws must be suppressed.
        mem.Put(PRED_GVA, {0u, 0x80000000u});
        {
            auto ring = pred_packet(PRED_GVA);
            ring.push_back(Hdr(2, 0x23));    // DRAW_INDEX_AUTO
            ring.push_back(0);                // vertex count
            ring.push_back(0);                // draw initiator
            const auto r = translator.TranslateAndExecuteCommandRingChecked(
                ring.data(), ring.size());
            CHECK(r.ok());
            CHECK(translator.IsPredicationActive());
            CHECK(!translator.PredicationPasses());
            std::vector<uint32_t> draw_ring;
            draw_ring.push_back(Hdr(2, 0x23));
            draw_ring.push_back(0);
            draw_ring.push_back(0);
            const auto r2 = translator.TranslateAndExecuteCommandRingChecked(
                draw_ring.data(), draw_ring.size());
            CHECK(r2.ok());
            CHECK(!translator.PredicationPasses());
            std::printf("  [ok] predication gate active (draws suppressed)\n");
        }
    }

    std::printf("pm4_events_dma_test: %d checks, %d failures\n", g_checks,
                g_failures);
    if (g_failures == 0) std::printf(">> [PASS] PM4 events/DMA/predication\n");
    return g_failures == 0 ? 0 : 1;
}
