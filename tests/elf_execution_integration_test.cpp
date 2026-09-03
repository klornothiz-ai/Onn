// ============================================================================
// ProsperoLayer RDNA2 Core - ELF Load + Execute Integration Test
// ----------------------------------------------------------------------------
// Proves the full front-to-back pipeline that a shadPS4-style flow relies on:
// take an already-decrypted x86-64 ELF executable, map its PT_LOAD segments
// into the real 16 GB guest arena via ElfLoader, then start execution at the
// ELF entry point through CPUJitEngine::ExecuteGuestFull (extended interpreter
// + VmmMemoryBus + guest stack + syscall dispatch).
//
// The test synthesises a minimal, valid static ELF64 in memory so it needs no
// external fixture and no toolchain at run time.
// ============================================================================
#include "loader/elf_loader.hpp"
#include "loader/elf_types.hpp"
#include "cpu/jit_executor.hpp"
#include "memory/virtual_memory_manager.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

using PS5::CPU::CPUJitEngine;
using PS5::Loader::ElfLoader;
using PS5::Memory::VirtualMemoryManager;

int g_failures = 0;
int g_checks = 0;
bool Check(bool v, const char* e, int line) {
    ++g_checks;
    if (!v) { ++g_failures; std::cerr << "  [FAIL] line " << line << ": " << e << '\n'; }
    return v;
}
#define CHECK(e) Check((e), #e, __LINE__)

// Build a minimal static ELF64 executable with a single R+X PT_LOAD segment
// containing `code`, entry at the start of that segment.
std::vector<uint8_t> BuildElf(uint64_t vaddr, const std::vector<uint8_t>& code) {
    using namespace PS5::Loader;
    const uint64_t code_off = 0x1000; // place code one page into the file/segment
    std::vector<uint8_t> img(code_off + code.size(), 0);

    Elf64_Ehdr eh{};
    eh.e_ident[0] = 0x7f; eh.e_ident[1] = 'E'; eh.e_ident[2] = 'L'; eh.e_ident[3] = 'F';
    eh.e_ident[4] = 2; // ELFCLASS64
    eh.e_ident[5] = 1; // little-endian
    eh.e_ident[6] = 1; // EV_CURRENT
    eh.e_type = 2;     // ET_EXEC
    eh.e_machine = 0x3e; // x86-64
    eh.e_version = 1;
    eh.e_entry = vaddr + code_off;
    eh.e_phoff = sizeof(Elf64_Ehdr);
    eh.e_shoff = 0;
    eh.e_flags = 0;
    eh.e_ehsize = sizeof(Elf64_Ehdr);
    eh.e_phentsize = sizeof(Elf64_Phdr);
    eh.e_phnum = 1;
    eh.e_shentsize = 0;
    eh.e_shnum = 0;
    eh.e_shstrndx = 0;

    Elf64_Phdr ph{};
    ph.p_type = PT_LOAD;
    ph.p_flags = PF_R | PF_X;
    ph.p_offset = 0;                 // map whole image
    ph.p_vaddr = vaddr;
    ph.p_paddr = vaddr;
    ph.p_filesz = img.size();
    ph.p_memsz = img.size();
    ph.p_align = 0x1000;

    std::memcpy(img.data(), &eh, sizeof(eh));
    std::memcpy(img.data() + eh.e_phoff, &ph, sizeof(ph));
    std::memcpy(img.data() + code_off, code.data(), code.size());
    return img;
}

// A guest program computing 7*6 through a real loop, returning 42 in rax:
//   xor eax, eax        acc = 0
//   mov ecx, 6          i = 6
// top:
//   add eax, 7
//   dec ecx
//   jnz top
//   ret
bool TestLoadAndRunElf() {
    std::cout << "[Integration] load ELF + execute entry point\n";
    const uint64_t vaddr = 0x1000300000ULL;
    std::vector<uint8_t> code = {
        0x31, 0xC0,                   // xor eax, eax
        0xB9, 0x06, 0x00, 0x00, 0x00, // mov ecx, 6
        0x83, 0xC0, 0x07,             // add eax, 7
        0xFF, 0xC9,                   // dec ecx
        0x75, 0xF9,                   // jnz -7
        0xC3,                         // ret
    };
    const std::vector<uint8_t> img = BuildElf(vaddr, code);

    ElfLoader loader;
    CHECK(loader.LoadExecutable(img.data(), img.size()));
    const uint64_t entry = loader.GetEntryPointGva();
    CHECK(entry == vaddr + 0x1000);
    CHECK(!loader.GetSegments().empty());

    const uint64_t result = CPUJitEngine::Instance().ExecuteGuestFull(entry, 0, 0);
    CHECK(result == 42);
    return true;
}

// A second ELF whose entry calls a helper via a real relative call, so the
// call/ret path is exercised through freshly loaded guest code.
//   mov edi, 9
//   call square         ; square(x) = x*x
//   ret
// square:
//   mov eax, edi
//   imul eax, edi
//   ret
bool TestLoadAndRunElfWithCall() {
    std::cout << "[Integration] load ELF with call/ret\n";
    const uint64_t vaddr = 0x1000320000ULL;
    std::vector<uint8_t> code = {
        0xBF, 0x09, 0x00, 0x00, 0x00,       // mov edi, 9
        0xE8, 0x01, 0x00, 0x00, 0x00,       // call +1 (square)
        0xC3,                               // ret
        // square:
        0x89, 0xF8,                         // mov eax, edi
        0x0F, 0xAF, 0xC7,                   // imul eax, edi
        0xC3,                               // ret
    };
    const std::vector<uint8_t> img = BuildElf(vaddr, code);

    ElfLoader loader;
    CHECK(loader.LoadExecutable(img.data(), img.size()));
    const uint64_t result =
        CPUJitEngine::Instance().ExecuteGuestFull(loader.GetEntryPointGva(), 0, 0);
    CHECK(result == 81);
    return true;
}

// A corrupt ELF (bad magic) must be rejected by the loader, never executed.
bool TestRejectsBadElf() {
    std::cout << "[Integration] loader rejects corrupt ELF\n";
    std::vector<uint8_t> junk(256, 0xAB);
    ElfLoader loader;
    CHECK(!loader.LoadExecutable(junk.data(), junk.size()));
    return true;
}

} // namespace

int main() {
    std::cout << "=== ELF Load + Execute Integration Test Suite ===\n";
    VirtualMemoryManager::Instance().InitializeArena();
    TestLoadAndRunElf();
    TestLoadAndRunElfWithCall();
    TestRejectsBadElf();

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    if (g_failures == 0) {
        std::cout << ">> [PASS] ELF load+execute pipeline fully verified.\n";
        return 0;
    }
    std::cerr << ">> [FAIL] " << g_failures << " check(s) failed.\n";
    return 1;
}
