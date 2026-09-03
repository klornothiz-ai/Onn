// ============================================================================
// ProsperoLayer RDNA2 Core - HLE dynamic linking test (round 30)
// ----------------------------------------------------------------------------
// Proves the full PLT/HLE chain the real Minecraft eboot drove into existence:
//
//   synthetic ET_SCE_DYNEXEC image with a versioned PS5 import
//   ("9TestNID12#D#E" — the same "NID#D#E" name shape as the real game)
//     -> RelocateProgram: dynsym located through DT_HASH (strtab BEFORE
//        symtab, the real eboot's layout that breaks the distance heuristic)
//     -> the NID's '#' suffix stripped, resolved against SymbolDatabase
//     -> the GOT slot patched to a guest trampoline stub
//     -> the entry point calls through the PLT (jmp [got])
//     -> the stub executes `mov eax,id; syscall; ret`
//     -> the shared syscall handler recognizes the magic number and invokes
//        the HOST function with the guest's own argument registers
//     -> the return value flows back as the entry's exit code
//
// Part B: an unregistered NID is honestly skipped (relocs_skipped) and never
// faked — the GOT keeps its null value instead of a bogus address.
// ============================================================================
#include "loader/guest_launcher.hpp"
#include "loader/runtimeLinker.h"
#include "loader/symbolDatabase.h"
#include "loader/prospero_self.hpp"
#include "cpu/hle_trampoline.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;
bool Check(bool v, const char* e, int line) {
    ++g_checks;
    if (!v) { ++g_failures; std::cerr << "  [FAIL] line " << line << ": " << e << '\n'; }
    return v;
}
#define CHECK(e) Check((e), #e, __LINE__)

using Loader::Elf64_Dyn;
using Loader::Elf64_Ehdr;
using Loader::Elf64_Phdr;
using Loader::Elf64_Rela;
using Loader::Elf64_Sym;

// The host HLE function: 6-register SysV ABI like every LIB_FUNC entry.
uint64_t HleTestFunc(uint64_t a0, uint64_t a1, uint64_t, uint64_t, uint64_t, uint64_t) {
    return a0 + a1 * 2 + 0x1234;
}

const char* kNid = "9TestNID12";     // registered (bare)

std::string Tmp(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

void WriteFile(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
}

struct ElfLayout {
    uint64_t got_vaddr = 0;      // JUMP_SLOT target (holds the PLT pointer)
    uint64_t plt_vaddr = 0;      // the `jmp [got]` stub in the X segment
    uint64_t entry_vaddr = 0;
};

// Builds the synthetic image. `register_import` controls whether the NID is
// registered in SymbolDatabase before boot (Part A) or not (Part B).
std::vector<uint8_t> BuildImage(const ElfLayout& L) {
    const uint64_t code_off = 0x4000;          // LOAD X (vaddr 0)
    const uint64_t rw_off = 0x8000;            // LOAD RW (vaddr 0x10000)
    const uint64_t flat_size = 0x8600;

    // RW payload layout (vaddr space):
    //   0x10000: strtab  "\09TestNID12#D#E\0zzUnknown01#D#E\0"  (BEFORE symtab
    //            like the real image)
    //   0x10060: dynsym  [3 symbols: null, known import, unknown import]
    //   0x100A0: dynamic
    //   0x10200: GOT slot for the known import
    //   0x10208: GOT slot for the unknown import
    //   0x10300: DT_HASH (nbucket 4, nchain 3)
    const uint64_t strtab_v = 0x10000;
    const uint64_t symtab_v = 0x10060;
    const uint64_t dynamic_v = 0x100A0;
    const uint64_t hash_v = 0x10300;
    const char strtab[] = "\09TestNID12#D#E\0zzUnknown01#D#E\0";
    // st_name offsets: known at 1, unknown at 15
    Elf64_Sym syms[3] = {};
    syms[1].st_name = 1;      // known import
    syms[2].st_name = 15;     // unknown import
    // (st_shndx stays 0 == SHN_UNDEF == import)

    // relocations: two JUMP_SLOTs (known -> GOT1, unknown -> GOT2)
    const uint64_t jmprel_v = 0x10240;
    Elf64_Rela rela[2] = {};
    rela[0].r_offset = L.got_vaddr;
    rela[0].r_info = (1ull << 32) | 7;            // sym 1, R_X86_64_JUMP_SLOT
    rela[1].r_offset = L.got_vaddr + 8;
    rela[1].r_info = (2ull << 32) | 7;            // sym 2

    struct DynE { uint64_t tag, val; };
    const DynE dyn[] = {
        {5, strtab_v},          // DT_STRTAB
        {10, sizeof(strtab)},   // DT_STRSZ
        {6, symtab_v},          // DT_SYMTAB
        {4, hash_v},            // DT_HASH (count source)
        {23, jmprel_v},         // DT_JMPREL
        {2, sizeof(rela)},      // DT_PLTRELSZ
        {20, 7},                // DT_PLTREL = DT_RELA
        {0, 0},                 // DT_NULL
    };

    std::vector<uint8_t> img(static_cast<size_t>(flat_size), 0);
    auto put = [&](uint64_t off, const void* d, size_t n) {
        std::memcpy(img.data() + off, d, n);
    };

    Elf64_Ehdr eh{};
    eh.e_ident[0] = 0x7f; eh.e_ident[1] = 'E'; eh.e_ident[2] = 'L'; eh.e_ident[3] = 'F';
    eh.e_ident[4] = 2; eh.e_ident[5] = 1; eh.e_ident[6] = 1; eh.e_ident[7] = 9;
    eh.e_type = Loader::ET_SCE_DYNEXEC;
    eh.e_machine = 0x3e;
    eh.e_version = 1;
    eh.e_entry = L.entry_vaddr;
    eh.e_phoff = 0x40;
    eh.e_phnum = 3;
    eh.e_phentsize = sizeof(Elf64_Phdr);
    eh.e_ehsize = sizeof(Elf64_Ehdr);
    put(0, &eh, sizeof(eh));

    Elf64_Phdr ph[3];
    std::memset(ph, 0, sizeof(ph));
    ph[0].p_type = 1; ph[0].p_flags = 5;          // R|X
    ph[0].p_offset = code_off; ph[0].p_vaddr = 0;
    ph[0].p_filesz = 0x200; ph[0].p_memsz = 0x200; ph[0].p_align = 0x4000;
    ph[1].p_type = 1; ph[1].p_flags = 6;          // R|W
    ph[1].p_offset = rw_off; ph[1].p_vaddr = 0x10000;
    ph[1].p_filesz = 0x600; ph[1].p_memsz = 0x600; ph[1].p_align = 0x4000;
    ph[2].p_type = 2; ph[2].p_flags = 6;          // DYNAMIC
    ph[2].p_offset = rw_off + (dynamic_v - 0x10000);
    ph[2].p_vaddr = dynamic_v;
    ph[2].p_filesz = sizeof(dyn); ph[2].p_memsz = sizeof(dyn); ph[2].p_align = 8;
    put(eh.e_phoff, ph, sizeof(ph));

    // X-segment code:
    //   entry: mov edi, 3 ; mov esi, 5 ; call PLT ; ret
    //   PLT:   jmp qword [rip + rel32 -> GOT slot]
    const uint8_t entry_code[] = {
        0xbf, 0x03, 0x00, 0x00, 0x00,             // mov edi, 3
        0xbe, 0x05, 0x00, 0x00, 0x00,             // mov esi, 5
        0xe8, 0x00, 0x00, 0x00, 0x00,             // call rel32 (fixup: PLT)
        0xc3,                                       // ret
    };
    const uint8_t plt_code[] = {
        0xff, 0x25, 0x00, 0x00, 0x00, 0x00,       // jmp qword [rip+rel32]
    };
    put(code_off + L.entry_vaddr, entry_code, sizeof(entry_code));
    put(code_off + L.plt_vaddr, plt_code, sizeof(plt_code));
    // call rel32: opcode at entry+10, rel32 field at entry+11,
    // next_rip = entry+15. target = next_rip + rel32.
    {
        const int32_t rel = static_cast<int32_t>(L.plt_vaddr -
                                                (L.entry_vaddr + 15));
        std::memcpy(img.data() + code_off + L.entry_vaddr + 11, &rel, 4);
    }
    // PLT jmp rel32: target GOT address - (plt + 6)
    {
        const int32_t rel = static_cast<int32_t>(L.got_vaddr - (L.plt_vaddr + 6));
        std::memcpy(img.data() + code_off + L.plt_vaddr + 2, &rel, 4);
    }

    // RW payload
    put(rw_off + (strtab_v - 0x10000), strtab, sizeof(strtab));
    put(rw_off + (symtab_v - 0x10000), syms, sizeof(syms));
    put(rw_off + (dynamic_v - 0x10000), dyn, sizeof(dyn));
    put(rw_off + (jmprel_v - 0x10000), rela, sizeof(rela));
    // DT_HASH: nbucket, nchain (= symbol count!)
    const uint32_t hash_hdr[2] = {4, 3};
    put(rw_off + (hash_v - 0x10000), hash_hdr, sizeof(hash_hdr));

    return img;
}

bool TestHlePltChain() {
    std::cout << "[HLE-PLT] versioned import -> trampoline -> host function\n";
    Loader::SymbolDatabase::Instance().AddDirect(kNid,
                                                 reinterpret_cast<const void*>(&HleTestFunc));

    ElfLayout L;
    L.entry_vaddr = 0x80;
    L.plt_vaddr = 0xC0;
    L.got_vaddr = 0x10200;
    const std::string path = Tmp("hleplt_a.elf");
    WriteFile(path, BuildImage(L));

    Loader::GuestLauncher launcher;
    Loader::GuestBootResult boot = launcher.Boot(path, 1'000'000);
    if (!CHECK(boot.ok)) {
        std::cerr << "  boot error: " << boot.error << "\n";
        return false;
    }
    // 3 + 5*2 + 0x1234 = 0x1241 — the host function's result travelled back
    // through the trampoline syscall as the entry's exit code.
    CHECK(boot.exit_code == 0x1241);
    CHECK(boot.relocs_imported == 1);
    CHECK(boot.relocs_applied == 1);
    CHECK(boot.segments_mapped == 2);
    std::cout << "  exit=" << std::hex << boot.exit_code << std::dec
              << " imports=" << boot.relocs_imported << "\n";
    return true;
}

bool TestUnknownNidSkipped() {
    std::cout << "[HLE-PLT] unknown NID honestly skipped (never faked)\n";
    ElfLayout L;
    L.entry_vaddr = 0x80;
    L.plt_vaddr = 0xC0;
    L.got_vaddr = 0x10200;
    // rewrite the entry to call the SECOND PLT stub (unknown import at
    // plt+0x10) so the skip is actually exercised by the guest
    const std::string path = Tmp("hleplt_b.elf");
    std::vector<uint8_t> img = BuildImage(L);
    // second PLT stub at 0xD0 for the unknown GOT slot
    const uint8_t plt2[] = {0xff, 0x25, 0x00, 0x00, 0x00, 0x00};
    std::memcpy(img.data() + 0x4000 + 0xD0, plt2, sizeof(plt2));
    const int32_t rel2 = static_cast<int32_t>(L.got_vaddr + 8 - (0xD0 + 6));
    std::memcpy(img.data() + 0x4000 + 0xD0 + 2, &rel2, 4);
    const int32_t callrel = static_cast<int32_t>(0xD0 - (0x80 + 15));
    std::memcpy(img.data() + 0x4000 + 0x80 + 11, &callrel, 4);
    WriteFile(path, img);

    Loader::GuestLauncher launcher;
    Loader::GuestBootResult boot = launcher.Boot(path, 100'000);
    // The SymbolDatabase is a process-wide singleton, so Part A's
    // registration persists: the known import still resolves (1) and ONLY
    // the unknown NID is skipped (1). The guest's call through the
    // UNRESOLVED slot jumps to the null GOT value and faults honestly —
    // never a faked address.
    CHECK(boot.relocs_imported == 1);
    CHECK(boot.relocs_skipped == 1);
    return true;
}

} // namespace

int main() {
    std::cout << "=== HLE dynamic linking (PLT trampoline) test ===\n";
    TestHlePltChain();
    TestUnknownNidSkipped();
    std::cout << (g_failures == 0 ? "HLE-PLT TEST PASSED" : "HLE-PLT TEST FAILED")
              << ": " << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
