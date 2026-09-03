// ============================================================================
// ProsperoLayer RDNA2 Core - runtime linker deepening test (round 9)
// ----------------------------------------------------------------------------
// Builds a synthetic ELF64 x86-64 shared object IN MEMORY (program headers,
// .dynsym/.dynstr, .rela.dyn with an R_X86_64_RELATIVE entry, .rela.plt with
// an R_X86_64_JUMP_SLOT entry, a PT_DYNAMIC table and section headers),
// writes it to a temp file and drives it through RuntimeLinker:
//
//   * LoadProgram owns the image and parses the export table,
//   * Dlsym / FindSymbol resolve the exported symbol (with load bias),
//   * RelocateProgram applies RELATIVE and JUMP_SLOT relocations to the
//     owned image in place (S + A / B + A semantics),
//   * ReadFromElf reads the PATCHED values back,
//   * module handles are monotonic: never reused across unload/reload (the
//     old size()+1 scheme collided; unique_id was never assigned at all, so
//     KernelLoadModule returned 0 and KernelDlsym always failed with ESRCH),
//   * RuntimeLinker::Instance() and Common::Singleton<RuntimeLinker> now
//     return the SAME object (the module list can no longer be split),
//   * non-ELF / missing paths keep the legacy name-only behaviour.
//
// Round 10 additions: cross-module symbol resolution. A synthetic PROVIDER
// pair (both exporting `dup_func`, plus `api_func` on the first) and an
// IMPORTER whose .rela.dyn/.rela.plt reference undefined symbols prove that
// RelocateProgram resolves imports against the OTHER loaded modules' export
// tables in LOAD ORDER (first provider wins), counts them separately
// (relocs_imported), leaves unresolved imports honestly skipped, and keeps
// Dlsym module-scoped.
// ============================================================================
#include "loader/runtimeLinker.h"
#include "common/singleton.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace {

int g_failures = 0, g_checks = 0;
bool Check(bool v, const char* e, int line) {
    ++g_checks;
    if (!v) { ++g_failures; std::cerr << "  [FAIL] line " << line << ": " << e << '\n'; }
    return v;
}
#define CHECK(e) Check((e), #e, __LINE__)

using Loader::Elf64_Ehdr;
using Loader::Elf64_Phdr;
using Loader::Elf64_Shdr;
using Loader::Elf64_Sym;
using Loader::Elf64_Dyn;
using Loader::Elf64_Rela;

// --- synthetic module layout (file offset == link-time vaddr) ---------------
constexpr uint64_t kOffEhdr    = 0x0000;   // 64 bytes
constexpr uint64_t kOffPhdrs   = 0x0040;   // 2 * 56
constexpr uint64_t kOffDynsym  = 0x0100;   // 2 entries
constexpr uint64_t kOffDynstr  = 0x0140;
constexpr uint64_t kOffRelaDyn = 0x0160;   // 1 entry
constexpr uint64_t kOffRelaPlt = 0x0180;   // 1 entry
constexpr uint64_t kOffShstrtab= 0x0200;
constexpr uint64_t kOffDynamic = 0x0240;   // dynamic array
constexpr uint64_t kOffData    = 0x3000;   // relocated targets
constexpr uint64_t kSymExportValue = 0x2100;
constexpr uint64_t kImageSize  = 0x4000;

std::vector<uint8_t> BuildSyntheticModule() {
    std::vector<uint8_t> image(kImageSize, 0);
    // (fields are written directly through typed pointers below)

    // --- Ehdr ---------------------------------------------------------------
    auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(image.data());
    ehdr->e_ident[0] = 0x7f; ehdr->e_ident[1] = 'E';
    ehdr->e_ident[2] = 'L';  ehdr->e_ident[3] = 'F';
    ehdr->e_ident[4] = 2;    // ELFCLASS64
    ehdr->e_type = Loader::ET_DYN;
    ehdr->e_machine = Loader::EM_X86_64;
    ehdr->e_version = 1;
    ehdr->e_phoff = kOffEhdr + 64;
    ehdr->e_shoff = kImageSize - 7 * sizeof(Elf64_Shdr);  // 7 sections at the end
    ehdr->e_ehsize = sizeof(Elf64_Ehdr);
    ehdr->e_phentsize = sizeof(Elf64_Phdr);
    ehdr->e_phnum = 2;
    ehdr->e_shentsize = sizeof(Elf64_Shdr);
    ehdr->e_shnum = 7;
    ehdr->e_shstrndx = 6;  // .shstrtab index (section 6)

    // --- Phdrs --------------------------------------------------------------
    auto* phdr = reinterpret_cast<Elf64_Phdr*>(image.data() + kOffPhdrs);
    phdr[0].p_type = Loader::PT_LOAD;
    phdr[0].p_flags = 5;  // R+X
    phdr[0].p_offset = 0;
    phdr[0].p_vaddr = 0;
    phdr[0].p_filesz = kImageSize;
    phdr[0].p_memsz = kImageSize;
    phdr[0].p_align = 0x1000;
    phdr[1].p_type = Loader::PT_DYNAMIC;
    phdr[1].p_flags = 6;  // R+W
    phdr[1].p_offset = kOffDynamic;
    phdr[1].p_vaddr = kOffDynamic;
    phdr[1].p_filesz = 12 * sizeof(Elf64_Dyn);
    phdr[1].p_memsz = 12 * sizeof(Elf64_Dyn);
    phdr[1].p_align = 8;

    // --- .dynsym: null entry + exported_func --------------------------------
    auto* dynsym = reinterpret_cast<Elf64_Sym*>(image.data() + kOffDynsym);
    dynsym[1].st_name = 1;                 // "exported_func" (offset 1 in .dynstr)
    dynsym[1].st_info = (Loader::STB_GLOBAL << 4) | Loader::STT_FUNC;
    dynsym[1].st_shndx = 1;                // defined in section 1 (.dynsym is 1)
    dynsym[1].st_value = kSymExportValue;

    // --- .dynstr -------------------------------------------------------------
    std::memcpy(image.data() + kOffDynstr, "\0exported_func\0", 16);

    // --- .rela.dyn: R_X86_64_RELATIVE @ 0x3000, addend +0x2100 ---------------
    auto* rela_dyn = reinterpret_cast<Elf64_Rela*>(image.data() + kOffRelaDyn);
    rela_dyn[0].r_offset = kOffData;
    rela_dyn[0].r_info =
        (static_cast<uint64_t>(0) << 32) | Loader::R_X86_64_RELATIVE;
    rela_dyn[0].r_addend = static_cast<int64_t>(kSymExportValue);

    // --- .rela.plt: JUMP_SLOT @ 0x3010 against exported_func (sym 1) ---------
    auto* rela_plt = reinterpret_cast<Elf64_Rela*>(image.data() + kOffRelaPlt);
    rela_plt[0].r_offset = kOffData + 0x10;
    rela_plt[0].r_info =
        (static_cast<uint64_t>(1) << 32) | Loader::R_X86_64_JUMP_SLOT;
    rela_plt[0].r_addend = -8;

    // --- .shstrtab -----------------------------------------------------------
    {
        const char names[] = "\0.dynsym\0.dynstr\0.rela.dyn\0.rela.plt\0.shstrtab\0.dynstuff\0";
        std::memcpy(image.data() + kOffShstrtab, names, sizeof(names));
    }

    // --- PT_DYNAMIC array ----------------------------------------------------
    {
        auto* dyn = reinterpret_cast<Elf64_Dyn*>(image.data() + kOffDynamic);
        size_t i = 0;
        auto tag = [&](int64_t t, uint64_t v) {
            dyn[i].d_tag = t; dyn[i].d_ptr = v; ++i;
        };
        tag(Loader::DT_SYMTAB, kOffDynsym);
        tag(Loader::DT_STRTAB, kOffDynstr);
        tag(Loader::DT_STRSZ, 16);
        tag(Loader::DT_SYMENT, sizeof(Elf64_Sym));
        tag(Loader::DT_RELA, kOffRelaDyn);
        tag(Loader::DT_RELASZ, sizeof(Elf64_Rela));
        tag(Loader::DT_RELAENT, sizeof(Elf64_Rela));
        tag(Loader::DT_JMPREL, kOffRelaPlt);
        tag(Loader::DT_PLTRELSZ, sizeof(Elf64_Rela));
        tag(Loader::DT_PLTREL, static_cast<uint64_t>(Loader::DT_RELA));
        tag(Loader::DT_NULL, 0);
        // (12th slot stays zeroed: d_tag == DT_NULL terminates the walk.)
    }

    // --- Shdrs ----------------------------------------------------------------
    auto* shdr = reinterpret_cast<Elf64_Shdr*>(image.data() + ehdr->e_shoff);
    size_t used = 0;
    auto section = [&](uint32_t name, uint32_t type, uint64_t off, uint64_t size,
                       uint32_t link, uint64_t entsize) {
        Elf64_Shdr& sh = shdr[used];
        sh.sh_name = name; sh.sh_type = type; sh.sh_flags = 0;
        sh.sh_addr = off; sh.sh_offset = off; sh.sh_size = size;
        sh.sh_link = link; sh.sh_info = 0; sh.sh_addralign = 8; sh.sh_entsize = entsize;
        ++used;
    };
    section(0, 0, 0, 0, 0, 0);                                   // [0] NULL
    // offsets inside .shstrtab:
    //   1 ".dynsym"  9 ".dynstr" 17 ".rela.dyn" 28 ".rela.plt" 39 ".shstrtab" 50 ".dynstuff"
    section(1,  Loader::SHT_DYNSYM, kOffDynsym, 2 * sizeof(Elf64_Sym), 2, sizeof(Elf64_Sym));  // [1]
    section(9,  Loader::SHT_STRTAB, kOffDynstr, 16, 0, 0);                                    // [2]
    section(17, Loader::SHT_RELA,   kOffRelaDyn, sizeof(Elf64_Rela), 1, sizeof(Elf64_Rela));  // [3]
    section(28, Loader::SHT_RELA,   kOffRelaPlt, sizeof(Elf64_Rela), 1, sizeof(Elf64_Rela));  // [4]
    section(39, Loader::SHT_STRTAB, kOffShstrtab, 64, 0, 0);                                  // [5] .shstrtab
    section(50, Loader::SHT_PROGBITS, kOffData, 0x20, 0, 0);                                  // [6] data
    // NOTE: e_shstrndx points at section 6 above; fix it to the .shstrtab one.
    ehdr->e_shstrndx = 5;
    (void)kOffEhdr;
    return image;
}

bool WriteFile(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) return false;
    file.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(file);
}

// --- round 10: provider / importer module builders ---------------------------
// Shared compact layout (file offset == link-time vaddr).
constexpr uint64_t kPPhdrs    = 0x0040;   // 2 * 56
constexpr uint64_t kPDynsym   = 0x0100;
constexpr uint64_t kPDynstr   = 0x0160;
constexpr uint64_t kPShstrtab = 0x0220;
constexpr uint64_t kPDynamic  = 0x0260;
constexpr uint64_t kPData     = 0x3000;
constexpr uint64_t kPImageSize = 0x4000;

constexpr uint64_t kApiValue  = 0x2100;   // provider A: api_func
constexpr uint64_t kDupAValue = 0x2200;   // provider A: dup_func (loaded first)
constexpr uint64_t kDupBValue = 0x2A00;   // provider B: dup_func (loaded second)

void PutEhdrPhdrs(std::vector<uint8_t>& image, size_t shnum, size_t shstrndx,
                  size_t dyn_bytes) {
    auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(image.data());
    ehdr->e_ident[0] = 0x7f; ehdr->e_ident[1] = 'E';
    ehdr->e_ident[2] = 'L';  ehdr->e_ident[3] = 'F';
    ehdr->e_ident[4] = 2;    // ELFCLASS64
    ehdr->e_type = Loader::ET_DYN;
    ehdr->e_machine = Loader::EM_X86_64;
    ehdr->e_version = 1;
    ehdr->e_phoff = kPPhdrs;
    ehdr->e_shoff = kPImageSize - shnum * sizeof(Elf64_Shdr);
    ehdr->e_ehsize = sizeof(Elf64_Ehdr);
    ehdr->e_phentsize = sizeof(Elf64_Phdr);
    ehdr->e_phnum = 2;
    ehdr->e_shentsize = sizeof(Elf64_Shdr);
    ehdr->e_shnum = shnum;
    ehdr->e_shstrndx = shstrndx;

    auto* phdr = reinterpret_cast<Elf64_Phdr*>(image.data() + kPPhdrs);
    phdr[0].p_type = Loader::PT_LOAD;
    phdr[0].p_flags = 5;  // R+X
    phdr[0].p_offset = 0;
    phdr[0].p_vaddr = 0;
    phdr[0].p_filesz = kPImageSize;
    phdr[0].p_memsz = kPImageSize;
    phdr[0].p_align = 0x1000;
    phdr[1].p_type = Loader::PT_DYNAMIC;
    phdr[1].p_flags = 6;  // R+W
    phdr[1].p_offset = kPDynamic;
    phdr[1].p_vaddr = kPDynamic;
    phdr[1].p_filesz = dyn_bytes;
    phdr[1].p_memsz = dyn_bytes;
    phdr[1].p_align = 8;
}

// Provider module: exports the given (name, value) pairs, no relocations.
// `names` / `values` must be parallel arrays.
std::vector<uint8_t> BuildProviderModule(const std::vector<const char*>& names,
                                         const std::vector<uint64_t>& values,
                                         const char* soname = nullptr) {
    std::vector<uint8_t> image(kPImageSize, 0);
    const size_t nsyms = names.size() + 1;   // + null entry

    // .dynstr: "\0" + each name + "\0" [+ soname]
    std::string dynstr = std::string("\0", 1);
    std::vector<uint64_t> name_off;
    for (const char* n : names) {
        name_off.push_back(dynstr.size());
        dynstr += n;
        dynstr += '\0';
    }
    uint64_t soname_off = 0;
    if (soname != nullptr) {
        soname_off = dynstr.size();
        dynstr += soname;
        dynstr += '\0';
    }
    std::memcpy(image.data() + kPDynstr, dynstr.data(), dynstr.size());

    auto* dynsym = reinterpret_cast<Elf64_Sym*>(image.data() + kPDynsym);
    for (size_t i = 0; i < names.size(); ++i) {
        dynsym[i + 1].st_name = static_cast<uint32_t>(name_off[i]);
        dynsym[i + 1].st_info = (Loader::STB_GLOBAL << 4) | Loader::STT_FUNC;
        dynsym[i + 1].st_shndx = 1;   // defined in .dynsym (non-UNDEF)
        dynsym[i + 1].st_value = values[i];
    }

    // PT_DYNAMIC: SYMTAB, STRTAB, STRSZ, SYMENT, NULL.
    {
        auto* dyn = reinterpret_cast<Elf64_Dyn*>(image.data() + kPDynamic);
        size_t i = 0;
        auto tag = [&](int64_t t, uint64_t v) { dyn[i].d_tag = t; dyn[i].d_ptr = v; ++i; };
        tag(Loader::DT_SYMTAB, kPDynsym);
        tag(Loader::DT_STRTAB, kPDynstr);
        tag(Loader::DT_STRSZ, dynstr.size());
        tag(Loader::DT_SYMENT, sizeof(Elf64_Sym));
        if (soname != nullptr) {
            tag(Loader::DT_SONAME, soname_off);
        }
        tag(Loader::DT_NULL, 0);
        // Sections: [0] NULL, [1] .dynsym, [2] .dynstr, [3] .shstrtab, [4] .data
        PutEhdrPhdrs(image, /*shnum=*/5, /*shstrndx=*/3, (i + 1) * sizeof(Elf64_Dyn));
    }

    // .shstrtab + 5 sections: NULL, .dynsym, .dynstr, .shstrtab, .data
    {
        const char snames[] = "\0.dynsym\0.dynstr\0.shstrtab\0.data\0";
        std::memcpy(image.data() + kPShstrtab, snames, sizeof(snames));
        auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(image.data());
        auto* shdr = reinterpret_cast<Elf64_Shdr*>(image.data() + ehdr->e_shoff);
        auto section = [&](uint32_t name, uint32_t type, uint64_t off, uint64_t size,
                           uint32_t link, uint64_t entsize, size_t idx) {
            shdr[idx].sh_name = name; shdr[idx].sh_type = type; shdr[idx].sh_flags = 0;
            shdr[idx].sh_addr = off; shdr[idx].sh_offset = off; shdr[idx].sh_size = size;
            shdr[idx].sh_link = link; shdr[idx].sh_info = 0;
            shdr[idx].sh_addralign = 8; shdr[idx].sh_entsize = entsize;
        };
        section(0, 0, 0, 0, 0, 0, 0);
        section(1, Loader::SHT_DYNSYM, kPDynsym, nsyms * sizeof(Elf64_Sym), 2,
                sizeof(Elf64_Sym), 1);
        section(9, Loader::SHT_STRTAB, kPDynstr, dynstr.size(), 0, 0, 2);
        section(17, Loader::SHT_STRTAB, kPShstrtab, sizeof(snames), 0, 0, 3);
        section(27, Loader::SHT_PROGBITS, kPData, 0x20, 0, 0, 4);
    }
    return image;
}

// Importer module: imports api_func / dup_func / ghost_func (all SHN_UNDEF),
// with relocations:
//   .rela.dyn: R_X86_64_64       @ 0x3000 vs api_func   (resolvable import)
//              R_X86_64_GLOB_DAT @ 0x3008 vs ghost_func (unresolvable import)
//              R_X86_64_RELATIVE @ 0x3010, addend 0x2100 (own)
//   .rela.plt: R_X86_64_JUMP_SLOT@ 0x3020 vs dup_func, addend -0x10 (import)
std::vector<uint8_t> BuildImporterModule(const std::vector<const char*>& needed = {},
                                        const char* soname = nullptr) {
    std::vector<uint8_t> image(kPImageSize, 0);
    constexpr uint64_t kRelaDyn = 0x0190;   // 3 entries
    constexpr uint64_t kRelaPlt = 0x01E0;   // 1 entry

    // .dynstr: "\0api_func\0dup_func\0ghost_func\0" [+ needed names]
    // NOTE: explicit length -- a plain "\0..." literal would truncate at the
    // first null byte with the char* std::string constructor.
    std::string dynstr = std::string("\0api_func\0dup_func\0ghost_func\0", 30);
    const uint32_t off_api  = 1;
    const uint32_t off_dup  = 10;
    const uint32_t off_ghost = 19;
    std::vector<uint64_t> needed_off;
    for (const char* n : needed) {
        needed_off.push_back(dynstr.size());
        dynstr += n;
        dynstr += '\0';
    }
    uint64_t soname_off = 0;
    if (soname != nullptr) {
        soname_off = dynstr.size();
        dynstr += soname;
        dynstr += '\0';
    }
    std::memcpy(image.data() + kPDynstr, dynstr.data(), dynstr.size());

    auto* dynsym = reinterpret_cast<Elf64_Sym*>(image.data() + kPDynsym);
    dynsym[1].st_name = off_api;
    dynsym[1].st_info = (Loader::STB_GLOBAL << 4) | Loader::STT_FUNC;
    dynsym[1].st_shndx = Loader::SHN_UNDEF;    // IMPORT
    dynsym[2].st_name = off_dup;
    dynsym[2].st_info = (Loader::STB_GLOBAL << 4) | Loader::STT_FUNC;
    dynsym[2].st_shndx = Loader::SHN_UNDEF;    // IMPORT
    dynsym[3].st_name = off_ghost;
    dynsym[3].st_info = (Loader::STB_GLOBAL << 4) | Loader::STT_FUNC;
    dynsym[3].st_shndx = Loader::SHN_UNDEF;    // IMPORT (never defined anywhere)

    auto* rela_dyn = reinterpret_cast<Elf64_Rela*>(image.data() + kRelaDyn);
    rela_dyn[0].r_offset = kPData;
    rela_dyn[0].r_info = (1ull << 32) | Loader::R_X86_64_64;
    rela_dyn[0].r_addend = 0;
    rela_dyn[1].r_offset = kPData + 8;
    rela_dyn[1].r_info = (3ull << 32) | Loader::R_X86_64_GLOB_DAT;
    rela_dyn[1].r_addend = 0;
    rela_dyn[2].r_offset = kPData + 0x10;
    rela_dyn[2].r_info = Loader::R_X86_64_RELATIVE;
    rela_dyn[2].r_addend = static_cast<int64_t>(kApiValue);

    auto* rela_plt = reinterpret_cast<Elf64_Rela*>(image.data() + kRelaPlt);
    rela_plt[0].r_offset = kPData + 0x20;
    rela_plt[0].r_info = (2ull << 32) | Loader::R_X86_64_JUMP_SLOT;
    rela_plt[0].r_addend = -0x10;

    // PT_DYNAMIC.
    {
        auto* dyn = reinterpret_cast<Elf64_Dyn*>(image.data() + kPDynamic);
        size_t i = 0;
        auto tag = [&](int64_t t, uint64_t v) { dyn[i].d_tag = t; dyn[i].d_ptr = v; ++i; };
        tag(Loader::DT_SYMTAB, kPDynsym);
        tag(Loader::DT_STRTAB, kPDynstr);
        tag(Loader::DT_STRSZ, dynstr.size());
        tag(Loader::DT_SYMENT, sizeof(Elf64_Sym));
        for (size_t n = 0; n < needed_off.size(); ++n) {
            tag(Loader::DT_NEEDED, needed_off[n]);
        }
        if (soname != nullptr) {
            tag(Loader::DT_SONAME, soname_off);
        }
        tag(Loader::DT_RELA, kRelaDyn);
        tag(Loader::DT_RELASZ, 3 * sizeof(Elf64_Rela));
        tag(Loader::DT_RELAENT, sizeof(Elf64_Rela));
        tag(Loader::DT_JMPREL, kRelaPlt);
        tag(Loader::DT_PLTRELSZ, sizeof(Elf64_Rela));
        tag(Loader::DT_PLTREL, static_cast<uint64_t>(Loader::DT_RELA));
        tag(Loader::DT_NULL, 0);
        PutEhdrPhdrs(image, /*shnum=*/7, /*shstrndx=*/5, (i + 1) * sizeof(Elf64_Dyn));
    }

    // .shstrtab + 7 sections: NULL, .dynsym, .dynstr, .rela.dyn, .rela.plt,
    // .shstrtab, .data
    {
        const char snames[] = "\0.dynsym\0.dynstr\0.rela.dyn\0.rela.plt\0.shstrtab\0.data\0";
        std::memcpy(image.data() + kPShstrtab, snames, sizeof(snames));
        auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(image.data());
        auto* shdr = reinterpret_cast<Elf64_Shdr*>(image.data() + ehdr->e_shoff);
        auto section = [&](uint32_t name, uint32_t type, uint64_t off, uint64_t size,
                           uint32_t link, uint64_t entsize, size_t idx) {
            shdr[idx].sh_name = name; shdr[idx].sh_type = type; shdr[idx].sh_flags = 0;
            shdr[idx].sh_addr = off; shdr[idx].sh_offset = off; shdr[idx].sh_size = size;
            shdr[idx].sh_link = link; shdr[idx].sh_info = 0;
            shdr[idx].sh_addralign = 8; shdr[idx].sh_entsize = entsize;
        };
        section(0, 0, 0, 0, 0, 0, 0);
        section(1, Loader::SHT_DYNSYM, kPDynsym, 4 * sizeof(Elf64_Sym), 2,
                sizeof(Elf64_Sym), 1);
        section(9, Loader::SHT_STRTAB, kPDynstr, dynstr.size(), 0, 0, 2);
        section(17, Loader::SHT_RELA, kRelaDyn, 3 * sizeof(Elf64_Rela), 1,
                sizeof(Elf64_Rela), 3);
        section(27, Loader::SHT_RELA, kRelaPlt, sizeof(Elf64_Rela), 1,
                sizeof(Elf64_Rela), 4);
        section(37, Loader::SHT_STRTAB, kPShstrtab, sizeof(snames), 0, 0, 5);
        section(47, Loader::SHT_PROGBITS, kPData, 0x40, 0, 0, 6);
    }
    return image;
}

} // namespace

int main() {
    std::cout << "=== Runtime Linker Deepening Test (round 9) ===\n";

    // The two singleton accessors must return the SAME object (round 9 fix:
    // libKernel used Common::Singleton<RuntimeLinker> while runtime_linker.cpp
    // served its own static -- two module lists).
    CHECK(&Loader::RuntimeLinker::Instance() ==
          Common::Singleton<Loader::RuntimeLinker>::Instance());
    std::cout << "  [ok] RuntimeLinker::Instance() == Common::Singleton access\n";

    const auto dir = std::filesystem::temp_directory_path() / "prospero-rtlinker-test";
    std::filesystem::create_directories(dir);
    const auto mod_path = dir / "synthetic_mod.so";
    CHECK(WriteFile(mod_path, BuildSyntheticModule()));

    auto& rt = Loader::RuntimeLinker::Instance();

    // ==========================================================================
    // 1. LoadProgram: image ownership + export parsing + handle assignment.
    // ==========================================================================
    auto* program = rt.LoadProgram(mod_path.string());
    CHECK(program != nullptr);
    CHECK(program->unique_id != 0);              // was NEVER assigned before round 9
    CHECK(program->handle == static_cast<int>(program->unique_id));
    CHECK(program->elf != nullptr);
    CHECK(program->elf->IsValid());
    CHECK(program->is_loaded);
    CHECK(program->export_symbols != nullptr);
    const uint64_t handle = program->unique_id;
    std::cout << "  [ok] LoadProgram: image owned, exports parsed, handle = " << handle << "\n";

    // ==========================================================================
    // 2. Dlsym / FindSymbol resolve through the export table (load bias 0).
    // ==========================================================================
    {
        void* addr = nullptr;
        CHECK(rt.Dlsym(handle, "exported_func", &addr) == 0);
        CHECK(addr == reinterpret_cast<void*>(kSymExportValue));
        CHECK(rt.Dlsym(handle, "no_such_symbol", &addr) == -1);
        CHECK(addr == nullptr);
        CHECK(rt.Dlsym(handle + 999, "exported_func", &addr) == -1);

        void* via_name = rt.FindSymbol("synthetic_mod.so", "exported_func");
        CHECK(via_name == reinterpret_cast<void*>(kSymExportValue));
        CHECK(rt.FindSymbol("missing_module", "exported_func") == nullptr);
        std::cout << "  [ok] Dlsym / FindSymbol resolve 'exported_func' at 0x"
                  << std::hex << kSymExportValue << std::dec << "\n";
    }

    // ==========================================================================
    // 3. RelocateProgram: RELATIVE + JUMP_SLOT applied to the owned image.
    // ==========================================================================
    constexpr uint64_t kBase = 0x100000;
    program->base_addr = kBase;   // load bias, as a loader would set it
    CHECK(rt.RelocateProgram(program) == 0);
    CHECK(program->relocs_applied == 2);
    CHECK(program->relocs_skipped == 0);

    // RELATIVE @ +0x3000: base + addend.
    CHECK(Loader::RuntimeLinker::ReadFromElf(program, kBase + kOffData) ==
          kBase + kSymExportValue);
    // JUMP_SLOT @ +0x3010: base + st_value + addend(-8).
    CHECK(Loader::RuntimeLinker::ReadFromElf(program, kBase + kOffData + 0x10) ==
          kBase + kSymExportValue - 8);
    // Address outside every PT_LOAD -> 0 (fail closed).
    CHECK(Loader::RuntimeLinker::ReadFromElf(program, kBase + 0x5000) == 0);
    std::cout << "  [ok] RelocateProgram: RELATIVE -> 0x" << std::hex << (kBase + kSymExportValue)
              << ", JUMP_SLOT -> 0x" << (kBase + kSymExportValue - 8) << std::dec << "\n";

    // Dlsym now includes the load bias.
    {
        void* addr = nullptr;
        CHECK(rt.Dlsym(handle, "exported_func", &addr) == 0);
        CHECK(addr == reinterpret_cast<void*>(kBase + kSymExportValue));
    }

    // ==========================================================================
    // 4. Handle uniqueness across unload/reload (regression: the old
    //    size()+1 allocator handed out colliding handles).
    // ==========================================================================
    {
        CHECK(rt.UnloadProgram(program) == 0);
        auto* reloaded = rt.LoadProgram(mod_path.string());
        CHECK(reloaded != nullptr);
        CHECK(reloaded->unique_id != handle);    // never reused
        CHECK(reloaded->handle == static_cast<int>(reloaded->unique_id));

        // LoadModule (the libKernel path) also allocates monotonically.
        uint64_t h2 = 0;
        CHECK(rt.LoadModule("whatever.prx", &h2) == 0);
        CHECK(h2 != reloaded->unique_id);
        CHECK(h2 != handle);
        CHECK(rt.UnloadModule(h2) == 0);
        CHECK(rt.UnloadModule(h2) == -1);        // double unload fails closed
        std::cout << "  [ok] handles stay unique across unload/reload ("
                  << handle << " -> " << reloaded->unique_id << ")\n";
    }

    // ==========================================================================
    // 5. Legacy behaviour preserved: missing / non-ELF paths still record a
    //    name-only module (no image, no exports, RelocateProgram is a no-op).
    // ==========================================================================
    {
        const auto nonelf_path = dir / "not_an_elf.bin";
        CHECK(WriteFile(nonelf_path, {'h', 'e', 'l', 'l', 'o'}));
        auto* nonelf = rt.LoadProgram(nonelf_path.string());
        CHECK(nonelf != nullptr);
        CHECK(nonelf->elf == nullptr);
        CHECK(nonelf->export_symbols == nullptr);
        CHECK(rt.RelocateProgram(nonelf) == 0);
        CHECK(nonelf->relocs_applied == 0);
        void* addr = reinterpret_cast<void*>(0x1);
        CHECK(rt.Dlsym(nonelf->unique_id, "exported_func", &addr) == -1);
        CHECK(addr == nullptr);

        auto* missing = rt.LoadProgram((dir / "does_not_exist.prx").string());
        CHECK(missing != nullptr);               // still recorded (legacy contract)
        CHECK(missing->elf == nullptr);
        CHECK(rt.RelocateProgram(nullptr) == -1);
        std::cout << "  [ok] non-ELF / missing paths keep the legacy name-only behaviour\n";
    }

    // ==========================================================================
    // 6. GetModuleInfo / FindProgramById round-trip on the reloaded module.
    //    (NOTE: ModuleInfo pointers are owned by the linker's module vector;
    //    re-find by name after any further loads, like libKernel does.)
    // ==========================================================================
    {
        Loader::ModuleInfo* m = rt.FindProgramByFileName(mod_path.string());
        CHECK(m != nullptr);
        CHECK(m->unique_id != handle);   // the reloaded instance, not the first
        CHECK(m->name == "synthetic_mod.so");
        Loader::ModuleInfoForUnwind info{};
        CHECK(rt.GetModuleInfo(m->unique_id, &info) == 0);
        CHECK(info.name == "synthetic_mod.so");
        CHECK(rt.GetModuleInfo(0xDEAD, &info) == -1);
        std::cout << "  [ok] module info round-trip\n";
    }

    // ==========================================================================
    // 7. Cross-module symbol resolution (round 10): RelocateProgram resolves
    //    undefined imports against the OTHER loaded modules' export tables in
    //    load order. Pointer discipline: load everything FIRST, then re-find
    //    each module (LoadProgram push_back can reallocate the vector).
    // ==========================================================================
    {
        CHECK(WriteFile(dir / "provider_a.so",
                        BuildProviderModule({"api_func", "dup_func"},
                                            {kApiValue, kDupAValue})));
        CHECK(WriteFile(dir / "provider_b.so",
                        BuildProviderModule({"dup_func"}, {kDupBValue})));
        CHECK(WriteFile(dir / "importer.so", BuildImporterModule()));

        CHECK(rt.LoadProgram((dir / "provider_a.so").string()) != nullptr);
        CHECK(rt.LoadProgram((dir / "provider_b.so").string()) != nullptr);
        CHECK(rt.LoadProgram((dir / "importer.so").string()) != nullptr);

        constexpr uint64_t kBaseA = 0x100000;
        constexpr uint64_t kBaseB = 0x180000;
        constexpr uint64_t kBaseI = 0x200000;

        auto* pa = rt.FindProgramByFileName((dir / "provider_a.so").string());
        auto* pb = rt.FindProgramByFileName((dir / "provider_b.so").string());
        auto* im = rt.FindProgramByFileName((dir / "importer.so").string());
        CHECK(pa != nullptr && pb != nullptr && im != nullptr);
        CHECK(pa->export_symbols != nullptr && pb->export_symbols != nullptr);
        // The importer's own .dynsym has only UNDEF entries -> no exports.
        CHECK(im->export_symbols == nullptr);
        pa->base_addr = kBaseA;
        pb->base_addr = kBaseB;
        im->base_addr = kBaseI;

        // Providers relocate cleanly (no relocation tables at all).
        CHECK(rt.RelocateProgram(pa) == 0);
        CHECK(pa->relocs_applied == 0 && pa->relocs_skipped == 0 &&
              pa->relocs_imported == 0);

        // The importer: api_func + dup_func resolve cross-module, ghost_func
        // stays honestly skipped, RELATIVE applies locally.
        CHECK(rt.RelocateProgram(im) == 0);
        CHECK(im->relocs_applied == 3);     // api import + dup import + RELATIVE
        CHECK(im->relocs_imported == 2);
        CHECK(im->relocs_skipped == 1);     // ghost_func (defined nowhere)

        // api_func: only provider A exports it -> A's bias + value.
        CHECK(Loader::RuntimeLinker::ReadFromElf(im, kBaseI + kPData) ==
              kBaseA + kApiValue);
        // dup_func: BOTH providers export it -> the FIRST loaded wins (A's
        // 0x2200, not B's 0x2A00).
        CHECK(Loader::RuntimeLinker::ReadFromElf(im, kBaseI + kPData + 0x20) ==
              kBaseA + kDupAValue - 0x10);
        // Own RELATIVE: the importer's own bias.
        CHECK(Loader::RuntimeLinker::ReadFromElf(im, kBaseI + kPData + 0x10) ==
              kBaseI + kApiValue);
        // The unresolved ghost_func slot keeps its original (zero) bytes.
        CHECK(Loader::RuntimeLinker::ReadFromElf(im, kBaseI + kPData + 8) == 0);

        // Dlsym stays module-scoped (documented): the importer cannot resolve
        // its imports through Dlsym, and provider B never sees A's api_func.
        void* addr = reinterpret_cast<void*>(0x1);
        CHECK(rt.Dlsym(im->unique_id, "api_func", &addr) == -1);
        CHECK(addr == nullptr);
        CHECK(rt.Dlsym(pa->unique_id, "api_func", &addr) == 0);
        CHECK(addr == reinterpret_cast<void*>(kBaseA + kApiValue));
        CHECK(rt.Dlsym(pb->unique_id, "api_func", &addr) == -1);

        std::cout << "  [ok] cross-module: api_func -> provider A, dup_func -> "
                     "FIRST provider, ghost_func skipped, own RELATIVE applied\n";
    }

    // ==========================================================================
    // 12. DT_NEEDED dependency graph (round 14): imports resolve against the
    //     DECLARED dependencies first (direct, then transitive), falling back
    //     to load order. SONAMEs are parsed and recorded.
    // ==========================================================================
    {
        constexpr uint64_t kBaseA = 0x100000;
        constexpr uint64_t kBaseB = 0x180000;
        constexpr uint64_t kBaseC = 0x200000;
        // Providers with SONAMEs; A loads first (its dup_func would win under
        // the round-10 load-order rule).
        CHECK(WriteFile(dir / "soname_a.so",
                        BuildProviderModule({"dup_func"}, {kDupAValue}, "libA.so")));
        CHECK(WriteFile(dir / "soname_b.so",
                        BuildProviderModule({"dup_func"}, {kDupBValue}, "libB.so")));
        // Mid module: SONAME libM.so, needs libB, imports dup_func.
        CHECK(WriteFile(dir / "soname_m.so",
                        BuildImporterModule({"libB.so"}, "libM.so")));
        // Importer 1: needs ONLY libB -> dup_func must come from B.
        CHECK(WriteFile(dir / "needed_importer.so",
                        BuildImporterModule({"libB.so"})));
        // Importer 2: needs ONLY libM (which needs libB) -> dup_func resolves
        //     transitively through M's dependency chain.
        CHECK(WriteFile(dir / "mid_importer.so",
                        BuildImporterModule({"libM.so"})));

        Loader::RuntimeLinker rt;
        // Providers first (A then B = load order that must LOSE).
        CHECK(rt.LoadProgram((dir / "soname_a.so").string(), 0, nullptr) == 0);
        CHECK(rt.LoadProgram((dir / "soname_b.so").string(), 0, nullptr) == 0);
        CHECK(rt.LoadProgram((dir / "needed_importer.so").string(), 0, nullptr) == 0);

        auto* mod_a = rt.FindProgramByFileName((dir / "soname_a.so").string());
        auto* mod_b = rt.FindProgramByFileName((dir / "soname_b.so").string());
        auto* mod_i = rt.FindProgramByFileName((dir / "needed_importer.so").string());
        CHECK(mod_a != nullptr && mod_b != nullptr && mod_i != nullptr);
        CHECK(mod_a->so_name == "libA.so");
        CHECK(mod_b->so_name == "libB.so");
        CHECK(mod_i->needed_libraries.size() == 1);
        CHECK(mod_i->needed_libraries[0] == "libB.so");

        mod_b->base_addr = kBaseB;   // distinct bases so values are observable
        mod_i->base_addr = kBaseC;

        // The import of dup_func must resolve against B (dependency order),
        // NOT against A (load order).
        CHECK(rt.RelocateProgram(mod_i) == 0);
        // Only dup_func is exported by the providers here; api_func and
        // ghost_func are honestly skipped (nobody exports them).
        CHECK(mod_i->relocs_applied == 2);   // dup import + RELATIVE
        CHECK(mod_i->relocs_imported == 1);
        CHECK(mod_i->relocs_skipped == 2);
        const uint64_t got_dup =
            Loader::RuntimeLinker::ReadFromElf(mod_i, kBaseC + kPData + 0x20);
        CHECK(got_dup == kBaseB + kDupBValue - 0x10);

        // Transitive: A (load order first) + B + M(needs B) + importer(needs
        // M only). The importer's resolution order is [M, B (via M), A, B].
        // dup_func is exported by A and B ONLY -> B wins through the chain,
        // proving the dependency walk is transitive.
        Loader::RuntimeLinker rt2;
        CHECK(rt2.LoadProgram((dir / "soname_a.so").string(), 0, nullptr) == 0);
        CHECK(rt2.LoadProgram((dir / "soname_b.so").string(), 0, nullptr) == 0);
        CHECK(rt2.LoadProgram((dir / "soname_m.so").string(), 0, nullptr) == 0);
        CHECK(rt2.LoadProgram((dir / "mid_importer.so").string(), 0, nullptr) == 0);
        auto* m_a = rt2.FindProgramByFileName((dir / "soname_a.so").string());
        auto* m_b = rt2.FindProgramByFileName((dir / "soname_b.so").string());
        auto* m_m = rt2.FindProgramByFileName((dir / "soname_m.so").string());
        auto* m_i = rt2.FindProgramByFileName((dir / "mid_importer.so").string());
        CHECK(m_a != nullptr && m_b != nullptr && m_m != nullptr && m_i != nullptr);
        CHECK(m_m->so_name == "libM.so");
        CHECK(m_m->needed_libraries.size() == 1);
        CHECK(m_m->needed_libraries[0] == "libB.so");
        CHECK(m_i->needed_libraries.size() == 1);
        CHECK(m_i->needed_libraries[0] == "libM.so");
        m_a->base_addr = kBaseA;
        m_b->base_addr = kBaseB;
        m_i->base_addr = kBaseC;
        CHECK(rt2.RelocateProgram(m_i) == 0);
        const uint64_t got_dup2 =
            Loader::RuntimeLinker::ReadFromElf(m_i, kBaseC + kPData + 0x20);
        CHECK(got_dup2 == kBaseB + kDupBValue - 0x10);

        std::cout << "  [ok] DT_NEEDED: direct dependency beats load order; "
                     "SONAMEs recorded; needed list parsed\n";
    }

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    if (g_failures == 0) {
        std::cout << ">> [PASS] RuntimeLinker loads, relocates and resolves a real "
                     "ELF64 image; handles are unique; imports resolve across "
                     "modules in load order.\n";
    }
    return g_failures == 0 ? 0 : 1;
}
