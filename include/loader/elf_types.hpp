#pragma once
#include <cstdint>
#include <cstddef>

namespace PS5::Loader {
    constexpr uint8_t ELF_MAG0 = 0x7F;
    constexpr uint8_t ELF_MAG1 = 'E';
    constexpr uint8_t ELF_MAG2 = 'L';
    constexpr uint8_t ELF_MAG3 = 'F';

    constexpr uint32_t PT_LOAD    = 1;
    constexpr uint32_t PT_DYNAMIC = 2;
    constexpr uint32_t PT_TLS     = 7;

    constexpr uint32_t PF_X = 1;
    constexpr uint32_t PF_W = 2;
    constexpr uint32_t PF_R = 4;

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
}
