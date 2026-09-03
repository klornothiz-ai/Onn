// prospero_self.hpp — Prospero (PS5) SELF container parser + ELF flattener.
//
// Round 30: the emulator received a REAL PS5 eboot.bin (Minecraft, debug
// image, 254 MB) and this module is the honest, fail-closed bridge between
// the Sony container format and the runtime linker's ELF path.
//
// Format facts (reverse-engineered from the real binary, verified by
// tests/self_parser_test.cpp — synthetic containers AND the real file):
//
//   +0x00  u32  magic     0x54 0x14 0xF5 0xEE  (LE 0xEEF51454) — the PS5
//         SELF magic. Distinct from the PS4-family "\x00SCE" containers.
//   +0x04  u8[4] version  (real file: 00 01 01 12)
//   +0x08  u16  mode      (real file: 0x0101)
//   +0x0A  u8   endian    (0 = LE)
//   +0x0B  u8   attr
//   +0x0C  u64  aux       0x06100560 in the real file (recorded, meaning
//         not yet established — it points at live code inside LOAD[0])
//   +0x14  u64  tail      file offset of the trailing SCE library table
//         (real file: filesize - 749)
//   +0x18  u16  entries   number of 32-byte segment entries (real: 12)
//   +0x1A  u16  extra     0x0022 in the real file (recorded, unknown)
//   +0x20  entry[entries]: 32 bytes each
//            +0x00 u64 flags   (flags & 0xFF00)==0x2800 -> DATA segment
//                              (flags & 0xFF00)==0x0000 -> metadata segment
//            +0x08 u64 offset  file offset (16-byte aligned chaining)
//            +0x10 u64 size
//            +0x18 u64 size2   must equal size (integrity invariant)
//   +0x20+entries*32         embedded ELF64 header (real file: 0x1a0)
//
// The embedded ELF is a genuine ELF64/LE/x86-64 image with OSABI=9
// (FreeBSD), e_type=0xFE10 (ET_SCE_DYNEXEC, PIE-style) and 0x4000 (16 KiB
// PS5 page) alignment on the load segments. Its PT_LOAD program headers
// pair 1:1, IN ORDER, with the container's DATA segments — every pairing
// is validated by exact filesz equality before anything is trusted.
//
// This project does NOT decrypt: if a container's data segments turn out
// to be ciphertext (retail image), ParseProsperoSelf still reports the
// structure but FlattenSelfToElf refuses to emit a "decrypted" ELF.
// The debug/devkit images this was built against are plaintext, which the
// parser detects by ELF-phantom checks (x86-64 prologues at the entry
// point, valid dynamic tags).
//
// No decryption keys, no DRM circumvention — structural analysis only.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "loader/elf.h"

namespace Loader {

// Sony extended ELF types (orbis/prospero family).
constexpr uint16_t ET_SCE_EXEC     = 0xfe04;
constexpr uint16_t ET_SCE_DYNEXEC  = 0xfe10;
constexpr uint16_t ET_SCE_DYNAMIC  = 0xfe18;

// Sony extended program header types seen in real PS5 eboots.
constexpr uint32_t PT_SCE_DYNDATA  = 0x61000001;  // module path (PATHX)
constexpr uint32_t PT_SCE_COMMENT  = 0x6fffff00;
constexpr uint32_t PT_SCE_VERSIONS = 0x6fffff01;

constexpr uint32_t PROSPERO_SELF_MAGIC = 0xEEF51454;  // LE 54 14 F5 EE

struct SelfSegmentEntry {
    uint64_t flags{0};
    uint64_t offset{0};
    uint64_t size{0};
    uint64_t size_dup{0};
    bool is_data{false};   // (flags & 0xFF00) == 0x2800
};

// One validated PT_LOAD <-> container data-segment pairing.
struct SelfLoadPairing {
    uint32_t phdr_index{0};
    uint64_t p_offset{0};        // ELF image offset (flat output position)
    uint64_t p_vaddr{0};
    uint64_t p_filesz{0};
    uint64_t p_memsz{0};
    uint32_t p_flags{0};
    uint32_t entry_index{0};     // container entry backing this phdr
    uint64_t container_offset{0};
    uint64_t container_size{0};
};

struct SelfParseResult {
    bool ok{false};
    std::string error;

    // Container header.
    uint32_t magic{0};
    uint8_t version[4] = {0, 0, 0, 0};
    uint16_t mode{0};
    uint8_t endian{0};
    uint8_t attr{0};
    uint64_t aux{0};
    uint64_t tail_offset{0};
    uint16_t entry_count{0};
    uint16_t extra{0};
    uint64_t file_size{0};

    std::vector<SelfSegmentEntry> entries;
    size_t data_segments{0};
    size_t meta_segments{0};

    // Embedded ELF (parsed copy, validated).
    uint64_t elf_offset{0};      // container offset of the ELF header
    Elf64_Ehdr ehdr{};
    std::vector<Elf64_Phdr> phdrs;
    std::vector<SelfLoadPairing> pairings;   // PT_LOAD + PT_SCE_DYNDATA
    uint64_t flattened_size{0};  // max(p_offset+p_filesz) over all phdrs

    // PATHX (build path) when a PT_SCE_DYNDATA segment exists.
    std::string module_path;

    // Dynamic facts (best effort; parsed through the pairing map).
    std::vector<std::string> needed_libraries;
    uint64_t dt_init{0};
    uint64_t dt_init_array{0};
    uint64_t dt_init_arraysz{0};
    uint64_t dt_strtab{0};

    // Trailing SCE library table (structure facts only).
    std::vector<std::string> tail_libraries;
    uint64_t tail_bytes{0};

    // Plaintext verdict: entry-point bytes decode as x86-64 code and the
    // dynamic table parses (retail ciphertext fails both).
    bool plaintext{false};
};

// Parses a Prospero SELF container. Reads the whole file once (the 254 MB
// real image is handled through a windowed reader — only the header, entry
// table, embedded ELF header/phdrs, PATHX, dynamic tags and the tail table
// are touched, never the 200 MB payload).
SelfParseResult ParseProsperoSelf(const std::string& path);

// Emits a bootable flat ELF (p_offset-relative layout) from a parsed
// container. The output is a genuine ET_SCE_DYNEXEC ELF that
// GuestLauncher::Boot loads like any homebrew image. Fails closed when the
// parse is not ok, the pairing is incomplete, or the payload is ciphertext.
// Returns 0 on success, nonzero + *error_out on failure.
int FlattenSelfToElf(const std::string& self_path, const SelfParseResult& parsed,
                     const std::string& out_path, std::string& error_out);

// Fast classification helper shared with the boot classifier: true when the
// first 4 bytes are the Prospero SELF magic.
bool HasProsperoSelfMagic(const uint8_t head[4]);

} // namespace Loader
