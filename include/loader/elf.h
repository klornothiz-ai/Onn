#pragma once
// ProsperoLayer PS5 emulator - ELF loader interface (Kyty-compatible)
#include "common/common.h"
#include "loader/elf_types.hpp"
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace Loader {

// ELF program-header constants (Kyty-compatible, in the Loader namespace).
constexpr uint32_t PT_LOAD          = 1;
constexpr uint32_t PT_DYNAMIC       = 2;
constexpr uint32_t PT_INTERP        = 3;
constexpr uint32_t PT_TLS           = 7;
constexpr uint32_t PT_GNU_EH_FRAME  = 0x6474e550;
constexpr uint32_t PT_GNU_STACK     = 0x6474e551;
constexpr uint32_t PT_OS_RELRO      = 0x60000010;
constexpr uint32_t PT_OS_COMMONPAGE = 0x60000011;

// ELF section-header constants.
constexpr uint32_t SHT_PROGBITS = 1;
constexpr uint32_t SHT_SYMTAB  = 2;
constexpr uint32_t SHT_STRTAB  = 3;
constexpr uint32_t SHT_RELA    = 4;
constexpr uint32_t SHT_DYNAMIC = 6;
constexpr uint32_t SHT_DYNSYM  = 11;

// ELF header identification.
constexpr uint8_t  ELFMAG0 = 0x7f;
constexpr uint8_t  ELFMAG1 = 'E';
constexpr uint8_t  ELFMAG2 = 'L';
constexpr uint8_t  ELFMAG3 = 'F';
constexpr uint8_t  ELFCLASS64 = 2;
constexpr uint16_t EM_X86_64  = 0x3e;
constexpr uint16_t ET_EXEC = 2;
constexpr uint16_t ET_DYN  = 3;

// x86-64 relocation types the linker applies.
constexpr uint32_t R_X86_64_64        = 1;
constexpr uint32_t R_X86_64_GLOB_DAT  = 6;
constexpr uint32_t R_X86_64_JUMP_SLOT = 7;
constexpr uint32_t R_X86_64_RELATIVE  = 8;

// Symbol binding / type helpers (Elf64_Sym.st_info).
constexpr uint32_t ELF64_ST_BIND(uint8_t info) { return info >> 4; }
constexpr uint32_t ELF64_ST_TYPE(uint8_t info) { return info & 0x0f; }
constexpr uint32_t STB_GLOBAL = 1;
constexpr uint32_t STB_WEAK   = 2;
constexpr uint32_t STT_NOTYPE  = 0;
constexpr uint32_t STT_OBJECT  = 1;
constexpr uint32_t STT_FUNC    = 2;
constexpr uint16_t SHN_UNDEF   = 0;

// ELF word types (Kyty-compatible).
using Elf64_Word  = uint32_t;
using Elf64_Addr  = uint64_t;
using Elf64_Off   = uint64_t;
using Elf64_Half  = uint16_t;
using Elf64_Xword = uint64_t;
using Elf64_Sxword = int64_t;

// Dynamic-array tags (Elf64_Dyn.d_tag). Only the tags the runtime linker
// consumes are listed; unknown tags are skipped by the parser.
constexpr Elf64_Sxword DT_NULL     = 0;
constexpr Elf64_Sxword DT_NEEDED   = 1;
constexpr Elf64_Sxword DT_PLTRELSZ = 2;
constexpr Elf64_Sxword DT_HASH     = 4;
constexpr Elf64_Sxword DT_STRTAB   = 5;
constexpr Elf64_Sxword DT_SYMTAB   = 6;
constexpr Elf64_Sxword DT_RELA     = 7;
constexpr Elf64_Sxword DT_RELASZ   = 8;
constexpr Elf64_Sxword DT_RELAENT  = 9;
constexpr Elf64_Sxword DT_STRSZ    = 10;
constexpr Elf64_Sxword DT_SYMENT   = 11;
constexpr Elf64_Sxword DT_INIT     = 12;  // round 11: module init function
constexpr Elf64_Sxword DT_FINI     = 13;  // round 11: module fini function
constexpr Elf64_Sxword DT_SONAME   = 14;
constexpr Elf64_Sxword DT_PLTREL   = 20;
constexpr Elf64_Sxword DT_JMPREL   = 23;
constexpr Elf64_Sxword DT_INIT_ARRAY    = 25;  // round 11: init function table
constexpr Elf64_Sxword DT_FINI_ARRAY    = 26;  // round 11: fini function table
constexpr Elf64_Sxword DT_INIT_ARRAYSZ  = 27;  // bytes (8 per entry)
constexpr Elf64_Sxword DT_FINI_ARRAYSZ  = 28;  // bytes (8 per entry)
constexpr Elf64_Sxword DT_REL      = 17;   // PLTREL value: DT_REL relocations

// ELF symbol / dynamic / relocation entries.
struct Elf64_Sym {
        Elf64_Word      st_name;
        uint8_t         st_info;
        uint8_t         st_other;
        Elf64_Half      st_shndx;
        Elf64_Addr      st_value;
        Elf64_Xword     st_size;
};

struct Elf64_Dyn {
        Elf64_Sxword    d_tag;
        union {
                Elf64_Xword d_val;
                Elf64_Addr  d_ptr;
        };
};

struct Elf64_Rela {
        Elf64_Addr      r_offset;
        Elf64_Xword     r_info;
        Elf64_Sxword    r_addend;
};

// Relocation info helpers (Elf64_Rela.r_info).
constexpr uint32_t ELF64_R_SYM(Elf64_Xword info) { return static_cast<uint32_t>(info >> 32); }
constexpr uint32_t ELF64_R_TYPE(Elf64_Xword info) { return static_cast<uint32_t>(info & 0xffffffff); }

struct Elf64_Ehdr {
        uint8_t  e_ident[16];
        uint16_t e_type;
        uint16_t e_machine;
        uint32_t e_version;
        uint64_t e_entry;
        uint64_t e_phoff;
        uint64_t e_shoff;
        uint32_t e_flags;
        uint16_t e_ehsize;
        uint16_t e_phentsize;
        uint16_t e_phnum;
        uint16_t e_shentsize;
        uint16_t e_shnum;
        uint16_t e_shstrndx;
};

struct Elf64_Phdr {
        uint32_t p_type;
        uint32_t p_flags;
        uint64_t p_offset;
        uint64_t p_vaddr;
        uint64_t p_paddr;
        uint64_t p_filesz;
        uint64_t p_memsz;
        uint64_t p_align;
};

struct Elf64_Shdr {
        uint32_t sh_name;
        uint32_t sh_type;
        uint64_t sh_flags;
        uint64_t sh_addr;
        uint64_t sh_offset;
        uint64_t sh_size;
        uint32_t sh_link;
        uint32_t sh_info;
        uint64_t sh_addralign;
        uint64_t sh_entsize;
};

// NOTE: Loader::ModuleInfo and Loader::ModuleInfoForUnwind are defined in
// loader/runtimeLinker.h (the authoritative, full-featured version).
// elf.h intentionally does not re-define them to avoid ODR conflicts.

} // namespace Loader
