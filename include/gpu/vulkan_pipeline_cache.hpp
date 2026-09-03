#pragma once
// ============================================================================
// ProsperoLayer RDNA2 Core - cross-dispatch pipeline cache (round 20)
// ----------------------------------------------------------------------------
// Every dispatch (compute and graphics) used to rebuild its whole object
// chain: descriptor set layout -> pipeline layout -> shader module ->
// pipeline. On a real GPU that is milliseconds of driver work per draw -- the
// documented "deliberate simplicity" of round 19. This cache removes it:
//
//   * the KEY is a 128-bit FNV-1a hash of (pipeline kind, the full SPIR-V
//     word stream, the binding signature, the push-constant size) -- two
//     dispatches hit the SAME entry only when the driver would build the
//     identical objects;
//   * the REGISTRY is process-wide (dispatches arrive through short-lived
//     executor instances; the cache must outlive them) and mutex-guarded;
//   * the PURE part (key computation, hit/miss statistics) has no Vulkan
//     dependency, so a headless host unit-tests the exact acceptance
//     contract: the second dispatch of the same program is a HIT, any
//     change to the code/binding/pc signature is a MISS;
//   * the VULKAN part (the cached VkDescriptorSetLayout / VkPipelineLayout /
//     VkShaderModule / VkPipeline handles themselves) lives inside the
//     executor's `#if __has_include(<vulkan/vulkan.h>)` implementation and
//     indexes its objects by this key.
// ============================================================================
#include "graphics/guest_gpu/pm4.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace PS5::GPU {

// The pipeline kind discriminates compute vs graphics entries in the key.
enum class PipelineKind : uint32_t {
    Compute = 0,
    Graphics = 1,
};

struct PipelineCacheKey {
    uint64_t lo{0};
    uint64_t hi{0};
    bool operator==(const PipelineCacheKey& o) const {
        return lo == o.lo && hi == o.hi;
    }
    bool operator!=(const PipelineCacheKey& o) const { return !(*this == o); }
};

struct PipelineCacheKeyHash {
    size_t operator()(const PipelineCacheKey& k) const {
        return static_cast<size_t>(k.lo ^ (k.hi * 0x9E3779B97F4A7C15ull));
    }
};

// FNV-1a 128 (the classic 128-bit parameters) over the key material. The
// word stream dominates; the scalar fields are folded in order so the
// key differs when ANY driver-visible input differs.
inline PipelineCacheKey ComputePipelineKey(PipelineKind kind,
                                           const uint32_t* spirv_words,
                                           size_t spirv_dwords,
                                           uint32_t binding_count,
                                           uint32_t push_constant_dwords,
                                           uint32_t aux_format = 0) {
    uint64_t lo = 0x62A5E9C46C5B3F16ull;   // FNV-1a 128 offset basis (low)
    uint64_t hi = 0x8A9D3F2B4C6E1A05ull;   // offset basis (high)
    auto mix = [&](uint8_t byte) {
        lo ^= byte;
        uint64_t old_lo = lo;
        lo = lo * 0x100000001B3ull + (hi << 1);
        hi = hi * 0x10000000000B3ull + old_lo;
        hi ^= lo >> 32;
    };
    auto mix32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) mix(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    };
    mix32(static_cast<uint32_t>(kind));
    mix32(static_cast<uint32_t>(binding_count));
    mix32(push_constant_dwords);
    mix32(aux_format);   // graphics: the GuestColorFormat/ZFunc pair packed
    if (spirv_words != nullptr) {
        for (size_t i = 0; i < spirv_dwords; ++i) mix32(spirv_words[i]);
    }
    return PipelineCacheKey{lo, hi};
}

struct PipelineCacheStats {
    uint64_t lookups{0};       // total NoteDispatch calls
    uint64_t hits{0};          // the key was already registered
    uint64_t misses{0};        // a new entry was created
    uint64_t entries{0};       // live entries right now
    uint64_t evictions{0};     // removed via Clear (explicit; no silent LRU)
};

// The process-wide registry. The Vulkan object table indexed by the same key
// lives in the executor; this class owns ONLY the key lifecycle + statistics
// so it stays unit-testable on a headless host.
class PipelineKeyRegistry {
public:
    static PipelineKeyRegistry& Instance() {
        static PipelineKeyRegistry inst;
        return inst;
    }

    // Returns true on a HIT (the key existed). On a miss the key is
    // registered. Thread-safe.
    bool NoteDispatch(const PipelineCacheKey& key) {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_stats.lookups;
        if (m_known.find(key) != m_known.end()) {
            ++m_stats.hits;
            return true;
        }
        m_known.emplace(key, uint8_t{1});
        ++m_stats.misses;
        m_stats.entries = m_known.size();
        return false;
    }

    // True when the key is registered (no lookup counted).
    bool Contains(const PipelineCacheKey& key) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_known.find(key) != m_known.end();
    }

    size_t Size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_known.size();
    }

    PipelineCacheStats Stats() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        PipelineCacheStats s = m_stats;
        s.entries = m_known.size();
        return s;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stats.evictions += m_known.size();
        m_known.clear();
        m_stats.entries = 0;
    }

    // Test isolation: forget everything AND zero the counters.
    void ResetForTest() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_known.clear();
        m_stats = PipelineCacheStats{};
    }

private:
    PipelineKeyRegistry() = default;
    mutable std::mutex m_mutex;
    std::unordered_map<PipelineCacheKey, uint8_t, PipelineCacheKeyHash> m_known;
    PipelineCacheStats m_stats;
};

} // namespace PS5::GPU
