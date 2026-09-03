// hle_trampoline.cpp — see hle_trampoline.hpp for the design.
#include "cpu/hle_trampoline.hpp"

#include "memory/virtual_memory_manager.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace PS5::CPU {

using Memory::PageProt;
using Memory::VirtualMemoryManager;

namespace {
std::mutex g_trampoline_mutex;
// Registered by DirectExecutionBackend::Enable (null in interpreter-only
// processes — stubs then run their real `syscall`, which the interpreter
// decodes and routes through the shared handler).
HleTrampolines::StubPreDiscoverHook g_pre_discover = nullptr;

// The stub region stays Read+Write+Exec for its whole life: stubs are
// emitted lazily (one per newly-resolved import), so a W^X flip after the
// first stub would make every later CopyToGuest fail (observed live: the
// real eboot resolved exactly ONE import, the first).
constexpr uint32_t kRwxAllocProt =
    static_cast<uint32_t>(PageProt::Read) | static_cast<uint32_t>(PageProt::Write) |
    static_cast<uint32_t>(PageProt::Exec);

// The host HLE functions are registered as `void*` and invoked through the
// 6-integer-register SysV ABI the guest already set up. Every HLE function
// in this project is KYTY_SYSV_ABI with integer/pointer parameters, so the
// call below is ABI-identical to the guest's own PLT call.
using HleFn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

} // namespace

void HleTrampolines::SetStubPreDiscoverHook(StubPreDiscoverHook hook) {
    std::lock_guard<std::mutex> lock(g_trampoline_mutex);
    g_pre_discover = hook;
}

HleTrampolines& HleTrampolines::Instance() {
    static HleTrampolines instance;
    return instance;
}

bool HleTrampolines::EnsureRegion() {
    if (m_region_base != 0) {
        return true;
    }
    auto& vmm = VirtualMemoryManager::Instance();
    const size_t region_bytes = static_cast<size_t>(kHleStubSize) * kHleMaxStubs;
    // Fixed placement near the TOP of the 16 GB arena (15.75 GB in). The
    // image bias probe prefers low addresses; a first-fit trampoline region
    // would steal the freed probe window and the PT_LOAD mapping below
    // would collide with it (observed on the real Minecraft boot).
    const uint64_t preferred =
        vmm.GetArenaBaseGva() + 0x3F0000000ull - region_bytes;
    uint64_t base = vmm.AllocateVirtual(preferred, region_bytes, kRwxAllocProt);
    if (base == 0) {
        base = vmm.AllocateVirtual(0, region_bytes, kRwxAllocProt);
    }
    if (base == 0) {
        return false;
    }
    m_region_base = base;
    return true;
}

uint64_t HleTrampolines::EmitStub(uint32_t id) {
    // stub: mov eax, id32 ; syscall ; ret ; pad
    uint8_t stub[static_cast<size_t>(kHleStubSize)] = {};
    stub[0] = 0xb8;
    const uint32_t nr = static_cast<uint32_t>(kHleSyscallBase + id);
    std::memcpy(stub + 1, &nr, 4);
    stub[5] = 0x0f;
    stub[6] = 0x05;   // syscall = 0F 05 (0F 34 is sysenter/reserved)
    stub[7] = 0xc3;

    auto& vmm = VirtualMemoryManager::Instance();
    const uint64_t gva = m_region_base + static_cast<uint64_t>(id) * kHleStubSize;
    if (!vmm.CopyToGuest(gva, stub, sizeof(stub), kRwxAllocProt)) {
        return 0;
    }
    // The stub is reached through an INDIRECT jmp [got] — the direct
    // backend only arms entry traps for direct branch targets, so it would
    // execute `mov eax; syscall` NATIVELY and the magic number would hit
    // the seccomp guard. When the hook is armed, the fresh stub block is
    // pre-discovered: its syscall site becomes an interception trap and is
    // serviced at native speed.
    if (g_pre_discover != nullptr) {
        g_pre_discover(gva);
    }
    return gva;
}

uint64_t HleTrampolines::StubFor(const void* host_func, const char* name) {
    if (host_func == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_trampoline_mutex);

    const auto found = m_by_func.find(host_func);
    if (found != m_by_func.end()) {
        return m_region_base + static_cast<uint64_t>(found->second) * kHleStubSize;
    }
    if (m_funcs.size() >= kHleMaxStubs) {
        return 0;
    }
    if (!EnsureRegion()) {
        return 0;
    }

    const uint32_t id = static_cast<uint32_t>(m_funcs.size());
    const uint64_t stub_gva = EmitStub(id);
    if (stub_gva == 0) {
        return 0;
    }
    m_funcs.push_back(host_func);
    m_by_func[host_func] = id;
    m_names.resize(m_funcs.size());
    if (name != nullptr) {
        m_names[id] = name;
    }
    return stub_gva;
}

uint64_t HleTrampolines::Dispatch(uint64_t syscall_nr, uint64_t a0, uint64_t a1,
                                  uint64_t a2, uint64_t a3, uint64_t a4,
                                  uint64_t a5) const {
    if (!IsHleCall(syscall_nr)) {
        return 0;
    }
    const uint64_t id = syscall_nr - kHleSyscallBase;
    if (id >= m_funcs.size()) {
        return 0;
    }
    const auto fn = reinterpret_cast<HleFn>(m_funcs[static_cast<size_t>(id)]);
    {
        static std::atomic<int> trace_count{0};
        if (trace_count.fetch_add(1) < 64) {
            std::fprintf(stderr, "[hle-call] #%llu %s(0x%llx, 0x%llx, 0x%llx)\n",
                         (unsigned long long)id,
                         id < m_names.size() && !m_names[static_cast<size_t>(id)].empty()
                             ? m_names[static_cast<size_t>(id)].c_str() : "?",
                         (unsigned long long)a0, (unsigned long long)a1,
                         (unsigned long long)a2);
        }
    }
    return fn(a0, a1, a2, a3, a4, a5);
}

} // namespace PS5::CPU
