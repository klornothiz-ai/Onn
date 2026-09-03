// ============================================================================
// ProsperoLayer RDNA2 Core - Virtual Memory Manager Expanded Test Suite
// ============================================================================
// Description: Broadens the dependency-free coverage of the guest virtual
//              memory manager: allocation, GVA<->HVA mapping, overlapping
//              rejection, permission-protected copies, protection changes,
//              and free/realloc. Uses the same lightweight custom harness as
//              the other dependency-free suites.
// ============================================================================

#include "memory/virtual_memory_manager.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace {

using PS5::Memory::PageProt;
using PS5::Memory::VirtualMemoryManager;

constexpr uint32_t kRW = static_cast<uint32_t>(PageProt::Read) |
                         static_cast<uint32_t>(PageProt::Write);
constexpr uint32_t kRWX = static_cast<uint32_t>(PageProt::Read) |
                          static_cast<uint32_t>(PageProt::Write) |
                          static_cast<uint32_t>(PageProt::Exec);
constexpr uint32_t kRO = static_cast<uint32_t>(PageProt::Read);

bool Check(bool value, const char* expression, int line) {
    if (!value) {
        std::cerr << "check failed at line " << line << ": " << expression << '\n';
    }
    return value;
}

#define CHECK(expression) \
    do { \
        if (!Check((expression), #expression, __LINE__)) return false; \
    } while (false)

// Allocate a region and confirm GVA/HVA round-trips and payload survives.
bool AllocateMapAndWrite() {
    constexpr uint64_t kGva = 0x1100000000ULL;
    auto& vmm = VirtualMemoryManager::Instance();
    CHECK(vmm.AllocateVirtual(kGva, 4096, kRW) == kGva);

    void* hva = vmm.GvaToHva(kGva);
    CHECK(hva != nullptr);
    CHECK(vmm.HvaToGva(hva) == kGva);

    const std::array<char, 16> payload = {"GVA-HVA-copy"};
    CHECK(vmm.CopyToGuest(kGva, payload.data(), payload.size()));
    char buffer[16] = {0};
    CHECK(vmm.CopyFromGuest(kGva, buffer, payload.size()));
    CHECK(std::memcmp(buffer, payload.data(), payload.size()) == 0);

    CHECK(vmm.IsGvaMapped(kGva));
    CHECK(vmm.IsGvaReadable(kGva));
    CHECK(vmm.IsGvaWritable(kGva));
    CHECK(!vmm.IsGvaExecutable(kGva));
    CHECK(vmm.FreeVirtual(kGva, 4096));
    return true;
}

// Two allocations must not overlap; an in-range request must be rejected.
bool OverlappingAllocationIsRejected() {
    auto& vmm = VirtualMemoryManager::Instance();
    constexpr uint64_t kBase = 0x1100100000ULL;
    CHECK(vmm.AllocateVirtual(kBase, 64 * 1024 * 1024, kRW) == kBase);
    // 64 MB from kBase reaches kBase + 0x04000000. Any request inside that
    // window must be refused by the VMM collision check.
    const uint64_t overlap_gva = kBase + 0x02000000ULL;
    CHECK(vmm.AllocateVirtual(overlap_gva, 4096, kRW) == 0);
    // A request just past the tail of the first region succeeds.
    const uint64_t beyond_gva = kBase + 0x04001000ULL;
    CHECK(vmm.AllocateVirtual(beyond_gva, 4096, kRW) == beyond_gva);
    CHECK(vmm.FreeVirtual(beyond_gva, 4096));
    CHECK(vmm.FreeVirtual(kBase, 64 * 1024 * 1024));
    return true;
}

// Unmapped guest addresses must never yield a host pointer or copy.
bool UnmappedRangeIsRejected() {
    auto& vmm = VirtualMemoryManager::Instance();
    constexpr uint64_t kUnmapped = 0x1100010000ULL;
    CHECK(!vmm.IsGvaMapped(kUnmapped));
    CHECK(vmm.GvaToHva(kUnmapped) == nullptr);
    uint8_t sink = 0;
    CHECK(!vmm.CopyFromGuest(kUnmapped, &sink, 1));
    CHECK(!vmm.CopyToGuest(kUnmapped, &sink, 1));
    return true;
}

// Permission protection must gate copies and exec queries.
bool ProtectionGatesAccess() {
    auto& vmm = VirtualMemoryManager::Instance();
    constexpr uint64_t kGva = 0x1100020000ULL;
    CHECK(vmm.AllocateVirtual(kGva, 4096, kRO));

    CHECK(vmm.IsGvaReadable(kGva));
    CHECK(!vmm.IsGvaWritable(kGva));
    // A write requires writable protection and must be refused while RO.
    const char data = 'x';
    CHECK(!vmm.CopyToGuest(kGva, &data, 1));

    CHECK(vmm.ProtectVirtual(kGva, 4096, kRWX));
    CHECK(vmm.IsGvaWritable(kGva));
    CHECK(vmm.IsGvaExecutable(kGva));
    CHECK(vmm.CopyToGuest(kGva, &data, 1));

    // A read-only copy request (CopyToGuest) is denied for an exec-only flip.
    CHECK(vmm.ProtectVirtual(kGva, 4096, kRO));
    CHECK(!vmm.IsGvaExecutable(kGva));
    CHECK(vmm.FreeVirtual(kGva, 4096));
    return true;
}

// Protect on a mismatched or unmapped region must fail cleanly.
bool ProtectMismatchIsRejected() {
    auto& vmm = VirtualMemoryManager::Instance();
    constexpr uint64_t kGva = 0x1100030000ULL;
    CHECK(vmm.AllocateVirtual(kGva, 8192, kRW));
    // Wrong size for the whole entry -> rejected, protections unchanged.
    CHECK(!vmm.ProtectVirtual(kGva, 4096, kRO));
    CHECK(vmm.IsGvaWritable(kGva));
    // Unmapped address -> rejected.
    CHECK(!vmm.ProtectVirtual(0x1200000000ULL, 4096, kRO));
    CHECK(vmm.FreeVirtual(kGva, 8192));
    return true;
}

struct TestCase {
    const char* name;
    bool (*function)();
};

} // namespace

int main() {
    const TestCase tests[] = {
        {"AllocateMapAndWrite", AllocateMapAndWrite},
        {"OverlappingAllocationIsRejected", OverlappingAllocationIsRejected},
        {"UnmappedRangeIsRejected", UnmappedRangeIsRejected},
        {"ProtectionGatesAccess", ProtectionGatesAccess},
        {"ProtectMismatchIsRejected", ProtectMismatchIsRejected},
    };

    size_t passed = 0;
    for (const auto& test : tests) {
        const bool success = test.function();
        std::cout << (success ? "[PASS] " : "[FAIL] ") << test.name << '\n';
        passed += success ? 1 : 0;
    }
    std::cout << passed << '/' << std::size(tests) << " tests passed\n";
    return passed == std::size(tests) ? 0 : 1;
}