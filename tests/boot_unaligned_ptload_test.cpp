// ============================================================================
// ProsperoLayer RDNA2 Core - Unaligned PT_LOAD boot regression test
// ----------------------------------------------------------------------------
// Round 34: GuestLauncher::Boot mapped each PT_LOAD at its raw p_vaddr, so a
// real compiler/linker PIE with a non-page-aligned RW segment (e.g. .data at
// 0x2f20) failed AllocateVirtual's page-alignment requirement and aborted the
// whole boot with "segment mapping collision". This test builds a minimal
// ET_DYN image whose RW PT_LOAD vaddr is deliberately unaligned and asserts the
// boot maps every PT_LOAD (fail-closed on the old behaviour).
// ============================================================================
#include "loader/guest_launcher.hpp"
#include "loader/runtimeLinker.h"
#include "loader/elf_types.hpp"
#include "memory/virtual_memory_manager.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

using Loader::GuestLauncher;
using PS5::Memory::VirtualMemoryManager;
int g_failures = 0;
int g_checks = 0;
bool Check(bool v, const char* e, int line) {
    ++g_checks;
    if (!v) { ++g_failures; std::cerr << "  [FAIL] line " << line << ": " << e << '\n'; }
    return v;
}
#define CHECK(e) Check((e), #e, __LINE__)

// Minimal ET_DYN image: R+X code segment at 0x1000 (entry `ret` at 0x1010) and
// an R+W data segment whose vaddr is NOT page aligned (0x2f20).
std::vector<uint8_t> BuildElf() {
    using namespace Loader;
    constexpr uint64_t kCodeOff = 0x1000;
    constexpr uint64_t kDataOff = 0x2000;
    constexpr uint64_t kDataVaddr = 0x2f20;   // deliberately unaligned
    constexpr uint64_t kDataSize = 0xf0;

    std::vector<uint8_t> img(kDataOff + kDataSize, 0);
    img[kCodeOff + 0x10] = 0xC3;              // ret at vaddr 0x1010

    Elf64_Ehdr eh{};
    eh.e_ident[0] = 0x7f; eh.e_ident[1] = 'E'; eh.e_ident[2] = 'L'; eh.e_ident[3] = 'F';
    eh.e_ident[4] = 2; eh.e_ident[5] = 1; eh.e_ident[6] = 1;
    eh.e_type = 3;                            // ET_DYN
    eh.e_machine = 0x3e;                      // x86-64
    eh.e_version = 1;
    eh.e_entry = 0x1010;
    eh.e_phoff = sizeof(Elf64_Ehdr);
    eh.e_ehsize = sizeof(Elf64_Ehdr);
    eh.e_phentsize = sizeof(Elf64_Phdr);
    eh.e_phnum = 2;
    std::memcpy(img.data(), &eh, sizeof(eh));

    auto* p0 = reinterpret_cast<Elf64_Phdr*>(img.data() + sizeof(Elf64_Ehdr));
    p0->p_type = PT_LOAD; p0->p_flags = 0x5;  // R+X
    p0->p_offset = kCodeOff; p0->p_vaddr = 0x1000;
    p0->p_filesz = 0x20; p0->p_memsz = 0x20; p0->p_align = 0x1000;

    auto* p1 = p0 + 1;
    p1->p_type = PT_LOAD; p1->p_flags = 0x6;  // R+W
    p1->p_offset = kDataOff; p1->p_vaddr = kDataVaddr;
    p1->p_filesz = kDataSize; p1->p_memsz = kDataSize; p1->p_align = 0x1000;

    return img;
}

bool WriteTempFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    FILE* f = fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    const size_t n = fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
    return n == bytes.size();
}

} // namespace

int main() {
    std::cout << "[boot_unaligned] round 34: unaligned PT_LOAD maps + boots\n";

    const std::vector<uint8_t> elf = BuildElf();
    const std::string path = "/tmp/boot_unaligned_ptload.elf";
    CHECK(WriteTempFile(path, elf));
    if (g_failures != 0) { std::cout << g_checks << " checks, " << g_failures << " failures\n"; return 1; }

    GuestLauncher launcher;
    const auto boot = launcher.Boot(path, 1000000);

    // Both PT_LOADs must map; the unaligned RW one must not abort the boot.
    CHECK(boot.ok);
    CHECK(boot.segments_mapped == 2);
    CHECK(boot.error.empty());

    std::cout << "  boot ok=" << boot.ok
              << " segments=" << boot.segments_mapped
              << " entry=0x" << std::hex << boot.entry_gva << std::dec << '\n';

    const bool pass = g_failures == 0;
    std::cout << (pass ? "PASS" : "FAIL") << ": " << g_checks << " checks, "
              << g_failures << " failures\n";
    return pass ? 0 : 1;
}
