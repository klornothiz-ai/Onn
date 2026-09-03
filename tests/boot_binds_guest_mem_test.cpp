// ============================================================================
// ProsperoLayer RDNA2 Core - boot-path bridge binds real VMM guest memory
// ----------------------------------------------------------------------------
// Round 34 station: the previous commit wired the HLE graphics submit path to
// the bridge, but EnableRealCompute(VmmGpuMemory) was never called in a real
// boot -- VmmGpuMemory existed but was dead code and never linked anywhere, so
// the boot-path bridge stayed on the CPU-sim reference path without access to
// the guest's actual address space.
//
// This regression drives the SAME singleton WindowInit() the boot path uses,
// asserts it is bound to a guest-memory accessor (the VMM-backed one), and
// proves that accessor really reads/writes the guest VMM arena by writing
// known words into the arena and reading them back through the bound accessor.
// Device-independent: no Vulkan device is required.
// ============================================================================
#include "graphics/presentation/window.h"
#include "graphics/host_gpu/headless_gpu_bridge.hpp"
#include "memory/virtual_memory_manager.hpp"

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <vector>

namespace {

using ::Graphics::HeadlessGpuBridge;
using ::Graphics::WindowInit;
using PS5::Memory::PageProt;
using PS5::Memory::VirtualMemoryManager;

int g_failures = 0;
bool Check(bool v, const char* e) {
    if (!v) { ++g_failures; std::cerr << "  [FAIL] " << e << '\n'; }
    return v;
}

} // namespace

int main() {
    std::cout << "[boot-binds-guest-mem] round 34: boot-path bridge owns VMM guest memory\n";

    // The boot-path singleton bridge.
    auto& bridge = static_cast<HeadlessGpuBridge&>(WindowInit(640, 360));
    Check(bridge.GuestMemory() != nullptr,
          "boot-path bridge bound a guest-memory accessor (VmmGpuMemory)");
    if (bridge.GuestMemory() == nullptr) {
        std::cout << "FAIL: no accessor bound; " << g_failures << " failures\n";
        return 1;
    }

    // Allocate a real guest page and write known words through the VMM.
    constexpr uint64_t kGva = 0x1000f00000ULL;
    constexpr uint32_t kRw = static_cast<uint32_t>(PageProt::Read) |
                             static_cast<uint32_t>(PageProt::Write);
    constexpr uint32_t kDw = 32;
    auto& vmm = VirtualMemoryManager::Instance();
    if (vmm.AllocateVirtual(kGva, 4096, kRw) != kGva) {
        std::cerr << "  [FAIL] could not allocate guest page\n";
        ++g_failures;
        return 1;
    }
    std::vector<uint32_t> expected(kDw);
    for (uint32_t i = 0; i < kDw; ++i) expected[i] = 0xC0DE0000u + i;
    const bool wrote = vmm.CopyToGuest(kGva, expected.data(), kDw * 4, kRw);

    // Read them back through the accessor the boot path bound.
    std::vector<uint32_t> got(kDw, 0u);
    const bool read = bridge.GuestMemory()->ReadDwords(kGva, got.data(), kDw);
    Check(wrote && read, "wrote via VMM and read via bound accessor");
    bool match = wrote && read;
    for (uint32_t i = 0; i < kDw; ++i) {
        if (got[i] != expected[i]) { match = false; break; }
    }
    Check(match, "bound accessor reads back the exact words written into the VMM arena");

    vmm.FreeVirtual(kGva, 4096);
    const bool pass = g_failures == 0;
    std::cout << (pass ? "PASS" : "FAIL") << ": "
              << "bound=" << (bridge.GuestMemory() != nullptr)
              << " roundtrip=" << (match ? "ok" : "bad") << '\n';
    return pass ? 0 : 1;
}
