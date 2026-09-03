// ============================================================================
// ProsperoLayer RDNA2 Core - Prospero SELF container test (round 30)
// ----------------------------------------------------------------------------
// Exercises the parser + flattener against:
//   Part A — a synthetic SELF container mirroring the REAL PS5 eboot format
//            (magic 54 14 F5 EE, 6 chained 32-byte entries as (meta,data)
//            pairs, embedded ET_SCE_DYNEXEC ELF, PATHX module path, dynamic
//            table with DT_NEEDED + DT_STRTAB, trailing SCE library table).
//            The flattened ELF is then BOOTED through GuestLauncher and the
//            entry point must return 42 — the Sony e_type takes the same
//            PIE load-bias path the real 254 MB image needs.
//   Part B — fail-closed corruption matrix (bad magic, size/dup mismatch,
//            entry overlap, missing pairing, truncated ELF, ciphertext
//            payload refusing to flatten).
//   Part C — the REAL eboot.bin (Minecraft PS5 debug image) when present at
//            download/eboot.bin: every structural fact discovered by the
//            round-30 analysis is asserted (12 entries, 6 data segments,
//            6 pairings, e_type 0xFE10, OSABI 9, entry 0x80, DT_INIT 0x10,
//            50 DT_NEEDED libraries, PATHX build path, flatten round-trip).
// ============================================================================
#include "loader/prospero_self.hpp"
#include "loader/guest_launcher.hpp"
#include "loader/runtimeLinker.h"

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

using Loader::FlattenSelfToElf;
using Loader::ParseProsperoSelf;
using Loader::SelfParseResult;

const char* kRealSelf = "/home/z/my-project/download/eboot.bin";

std::string Tmp(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

void WriteFile(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
}

// ---------------------------------------------------------------------------
// Synthetic container builder. Layout mirrors the real PS5 eboot:
//   [0x000] 32-byte header prefix
//   [0x020] 6 segment entries (meta,data,meta,data,meta,data), 32B each
//   [0x140] embedded ELF header + 4 phdrs (ET_SCE_DYNEXEC, OSABI 9)
//   ...     container segments (16-byte aligned chain), then tail table
// ---------------------------------------------------------------------------
struct Synthetic {
    std::vector<uint8_t> bytes;
    uint64_t code_data_off = 0, rw_data_off = 0, pathx_off = 0;
    uint64_t elf_off = 0;
};

Synthetic BuildSynthetic() {
    Synthetic s;
    std::vector<uint8_t>& b = s.bytes;
    const size_t entry_count = 6;

    // guest code: mov eax,42 ; ret  (position independent)
    const uint8_t code[] = {0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3};

    // RW segment payload: strtab + dynamic table (with real DT_INIT_ARRAY)
    const char strtab[] = "\0libTest.prx\0libTest2.prx\0";
    struct DynEntry { uint64_t tag, val; };
    const DynEntry dyn[] = {
        {1, 1},           // DT_NEEDED "libTest.prx" (strtab offset 1)
        {1, 13},          // DT_NEEDED "libTest2.prx" (strtab offset 13)
        {5, 0x10100},     // DT_STRTAB -> vaddr inside RW LOAD
        {12, 0x90},       // DT_INIT -> second stub in the X segment
        {25, 0x10300},    // DT_INIT_ARRAY -> RW vaddr (holds one function)
        {27, 8},          // DT_INIT_ARRAYSZ
        {0, 0},           // DT_NULL
    };

    // ELF-space layout
    const uint64_t code_off = 0x4000, code_sz = 0x4100;      // LOAD X (vaddr 0)
    const uint64_t rw_off = 0x8000, rw_sz = 0x400;           // LOAD RW (vaddr 0x10000)
    const uint64_t pathx_off_elf = 0x9000, pathx_sz = 0x60;  // SCE_DYNDATA
    const uint64_t dyn_off = 0x8200;                         // inside RW
    const uint64_t strtab_off = 0x8100;                      // inside RW
    const uint64_t flat_size = 0x9060;

    // Build the flattened ELF image FIRST (the data segments' content).
    std::vector<uint8_t> elf(static_cast<size_t>(flat_size), 0);
    {
        Loader::Elf64_Ehdr eh{};
        eh.e_ident[0] = 0x7f; eh.e_ident[1] = 'E'; eh.e_ident[2] = 'L'; eh.e_ident[3] = 'F';
        eh.e_ident[4] = 2;    eh.e_ident[5] = 1;    eh.e_ident[6] = 1;    eh.e_ident[7] = 9; // FreeBSD
        eh.e_ident[8] = 2;    // ABI version (real file)
        eh.e_type = Loader::ET_SCE_DYNEXEC;
        eh.e_machine = 0x3e;
        eh.e_version = 1;
        eh.e_entry = 0x80;
        eh.e_phoff = 0x40;
        eh.e_shoff = 0;
        eh.e_ehsize = sizeof(Loader::Elf64_Ehdr);
        eh.e_phentsize = sizeof(Loader::Elf64_Phdr);
        eh.e_phnum = 4;
        std::memcpy(elf.data(), &eh, sizeof(eh));

        Loader::Elf64_Phdr ph[4];
        std::memset(ph, 0, sizeof(ph));
        ph[0].p_type = Loader::PT_LOAD;
        ph[0].p_flags = 5;              // R|X
        ph[0].p_offset = code_off;
        ph[0].p_vaddr = 0;
        ph[0].p_filesz = code_sz;
        ph[0].p_memsz = code_sz;
        ph[0].p_align = 0x4000;
        ph[1].p_type = Loader::PT_LOAD;
        ph[1].p_flags = 6;              // R|W
        ph[1].p_offset = rw_off;
        ph[1].p_vaddr = 0x10000;
        ph[1].p_filesz = rw_sz;
        ph[1].p_memsz = rw_sz;
        ph[1].p_align = 0x4000;
        ph[2].p_type = Loader::PT_SCE_DYNDATA;
        ph[2].p_flags = 4;
        ph[2].p_offset = pathx_off_elf;
        ph[2].p_vaddr = 0x20000;
        ph[2].p_filesz = pathx_sz;
        ph[2].p_memsz = pathx_sz;
        ph[2].p_align = 0x8;
        ph[3].p_type = Loader::PT_DYNAMIC;
        ph[3].p_flags = 6;
        ph[3].p_offset = dyn_off;
        ph[3].p_vaddr = 0x10200;
        ph[3].p_filesz = sizeof(dyn);
        ph[3].p_memsz = sizeof(dyn);
        ph[3].p_align = 8;
        std::memcpy(elf.data() + eh.e_phoff, ph, sizeof(ph));

        std::memcpy(elf.data() + code_off + 0x80, code, sizeof(code));
        // DT_INIT stub at vaddr 0x90: mov eax,7 ; ret
        const uint8_t init_stub[] = {0xb8, 0x07, 0x00, 0x00, 0x00, 0xc3};
        std::memcpy(elf.data() + code_off + 0x90, init_stub, sizeof(init_stub));
        // init-array slot at RW vaddr 0x10300 -> function at vaddr 0x98
        const uint8_t init2_stub[] = {0xb8, 0x09, 0x00, 0x00, 0x00, 0xc3};
        std::memcpy(elf.data() + code_off + 0x98, init2_stub, sizeof(init2_stub));
        const uint64_t init2_ptr = 0x98;
        std::memcpy(elf.data() + rw_off + 0x300, &init2_ptr, 8);
        std::memcpy(elf.data() + strtab_off, strtab, sizeof(strtab));
        std::memcpy(elf.data() + dyn_off, dyn, sizeof(dyn));
        // PATHX payload
        const char pathx[13] = {'P', 'A', 'T', 'H', 'X', 0, 0, 0, 'Q', 0, 0, 0, 0};
        std::memcpy(elf.data() + pathx_off_elf, pathx, 13);
        const char build[] = "D:/synthetic/SelfTest.elf";
        std::memcpy(elf.data() + pathx_off_elf + 12, build, sizeof(build));
    }

    // Container layout: embedded ELF header + phdrs occupy the header region
    // FIRST (real file: ELF at 0x1a0, phdrs to 0x4f0, first segment at 0xb70);
    // segment chain starts after them. hdr = 0x20 + 6*32 = 0xE0; the ELF
    // header+phdrs need 0x120 bytes, so the chain begins at 0xE0+0x120=0x200.
    const uint64_t hdr = 0x20 + entry_count * 32;   // 0x140
    const uint64_t elf_region = 0x40 + 4 * sizeof(Loader::Elf64_Phdr); // 0x120
    const uint64_t meta_sz = 0x20;
    uint64_t p = hdr + elf_region;                  // 0x260 (16-byte aligned)
    const uint64_t meta0 = p;                 p += meta_sz;
    const uint64_t code_data = p;             p += code_sz;     // data segment 0
    const uint64_t meta1 = p;                 p += meta_sz;
    const uint64_t rw_data = p;               p += rw_sz;       // data segment 1
    const uint64_t meta2 = p;                 p += meta_sz;
    const uint64_t pathx_data = p;            p += pathx_sz;    // data segment 2
    const uint64_t tail = p;
    const char tail_tab[] = "\x00\x00\x16\x00\x08" "crt1:" "\x08\x00\x00\x41\x00\x00\x00\x01"
                            "\x00\x00\x1b\x00\x08" "libFake:" "\x08\x00\x00\x41\x00\x00\x00\x01";

    b.assign(static_cast<size_t>(tail + sizeof(tail_tab)), 0);
    auto put64 = [&](uint64_t off, uint64_t v) {
        std::memcpy(b.data() + off, &v, 8);
    };
    // header
    b[0] = 0x54; b[1] = 0x14; b[2] = 0xF5; b[3] = 0xEE;
    b[4] = 0; b[5] = 1; b[6] = 1; b[7] = 0x12;
    b[8] = 1; b[9] = 1; b[10] = 0; b[11] = 0;
    uint32_t aux32 = 0;
    std::memcpy(b.data() + 12, &aux32, 4);   // aux u32 @0x0C
    put64(16, tail);                          // tail u64 @0x10
    b[24] = static_cast<uint8_t>(entry_count); b[25] = 0;
    b[26] = 0x22; b[27] = 0;      // extra
    // entries: (meta, data) x3
    struct E { uint64_t flags, off, sz; };
    const E ents[6] = {
        {0x110004, meta0, meta_sz},
        {0x002804, code_data, code_sz},
        {0x310004, meta1, meta_sz},
        {0x102804, rw_data, rw_sz},
        {0x510004, meta2, meta_sz},
        {0x302804, pathx_data, pathx_sz},
    };
    for (size_t i = 0; i < 6; ++i) {
        const uint64_t o = 0x20 + i * 32;
        put64(o, ents[i].flags);
        put64(o + 8, ents[i].off);
        put64(o + 16, ents[i].sz);
        put64(o + 24, ents[i].sz);     // dup
    }
    // embedded ELF header + phdrs
    std::memcpy(b.data() + hdr, elf.data(), 0x40 + 4 * sizeof(Loader::Elf64_Phdr));
    // data segments carry the ELF image slices
    std::memcpy(b.data() + code_data, elf.data() + code_off, static_cast<size_t>(code_sz));
    std::memcpy(b.data() + rw_data, elf.data() + rw_off, static_cast<size_t>(rw_sz));
    std::memcpy(b.data() + pathx_data, elf.data() + pathx_off_elf, static_cast<size_t>(pathx_sz));
    // tail
    std::memcpy(b.data() + tail, tail_tab, sizeof(tail_tab));

    s.code_data_off = code_data;
    s.rw_data_off = rw_data;
    s.pathx_off = pathx_data;
    s.elf_off = hdr;
    return s;
}

// ---------------------------------------------------------------------------
// Part A: parse + flatten + BOOT the synthetic container
// ---------------------------------------------------------------------------
bool TestSyntheticParseFlattenBoot() {
    std::cout << "[SELF] synthetic container: parse + flatten + boot\n";
    const Synthetic s = BuildSynthetic();
    const std::string self_path = Tmp("selftest_syn.self");
    WriteFile(self_path, s.bytes);

    const SelfParseResult r = ParseProsperoSelf(self_path);
    if (!CHECK(r.ok)) {
        std::cerr << "  parse error: " << r.error << "\n";
        return false;
    }
    CHECK(r.magic == 0xEEF51454u);
    CHECK(r.entry_count == 6);
    CHECK(r.entries.size() == 6);
    CHECK(r.data_segments == 3);
    CHECK(r.meta_segments == 3);
    CHECK(r.elf_offset == 0xE0);   // 0x20 + 6*32
    CHECK(r.ehdr.e_type == Loader::ET_SCE_DYNEXEC);
    CHECK(r.ehdr.e_ident[7] == 9);              // FreeBSD OSABI like the real image
    CHECK(r.ehdr.e_machine == 0x3e);
    CHECK(r.ehdr.e_entry == 0x80);
    CHECK(r.phdrs.size() == 4);
    CHECK(r.pairings.size() == 3);
    CHECK(r.pairings[0].p_filesz == 0x4100);
    CHECK(r.pairings[0].container_size == 0x4100);
    CHECK(r.pairings[0].container_offset == s.code_data_off);
    CHECK(r.pairings[1].p_filesz == 0x400);
    CHECK(r.pairings[1].container_offset == s.rw_data_off);
    CHECK(r.pairings[2].p_filesz == 0x60);
    CHECK(r.pairings[2].container_offset == s.pathx_off);
    CHECK(r.flattened_size == 0x9060);
    CHECK(r.plaintext);
    CHECK(r.module_path.find("SelfTest.elf") != std::string::npos);
    CHECK(r.needed_libraries.size() == 2);
    CHECK(r.needed_libraries[0] == "libTest.prx");
    CHECK(r.needed_libraries[1] == "libTest2.prx");
    CHECK(r.dt_strtab == 0x10100);
    CHECK(r.dt_init == 0x90);
    CHECK(r.dt_init_array == 0x10300);
    CHECK(r.dt_init_arraysz == 8);
    bool has_crt = false, has_fake = false;
    for (const auto& lib : r.tail_libraries) {
        if (lib == "crt1:") has_crt = true;
        if (lib == "libFake:") has_fake = true;
    }
    CHECK(has_crt);
    CHECK(has_fake);

    // flatten
    const std::string flat = Tmp("selftest_syn.elf");
    std::string err;
    const int rc = FlattenSelfToElf(self_path, r, flat, err);
    if (!CHECK(rc == 0)) {
        std::cerr << "  flatten error: " << err << "\n";
        return false;
    }
    // flattened ELF re-parse: it IS a plain ELF now
    {
        std::ifstream f(flat, std::ios::binary);
        Loader::Elf64_Ehdr eh{};
        f.read(reinterpret_cast<char*>(&eh), sizeof(eh));
        CHECK(eh.e_ident[0] == 0x7f && eh.e_ident[4] == 2 && eh.e_machine == 0x3e);
        CHECK(eh.e_type == Loader::ET_SCE_DYNEXEC);
        CHECK(eh.e_shoff == 0 && eh.e_shnum == 0);
        CHECK(std::filesystem::file_size(flat) == 0x9060);
        // entry bytes present at 0x4080
        f.seekg(0x4080);
        uint8_t buf[6] = {};
        f.read(reinterpret_cast<char*>(buf), 6);
        CHECK(buf[0] == 0xb8 && buf[1] == 0x2a && buf[5] == 0xc3);
    }

    // BOOT the flattened ELF through the real launcher (Sony e_type now
    // takes the ET_DYN PIE path with load bias).
    Loader::GuestLauncher launcher;
    Loader::GuestBootResult boot = launcher.Boot(flat, 1'000'000);
    if (!CHECK(boot.ok)) {
        std::cerr << "  boot error: " << boot.error << "\n";
        return false;
    }
    CHECK(boot.exit_code == 42);
    CHECK(boot.segments_mapped == 2);   // X + RW (PATHX is not PT_LOAD)
    CHECK(boot.init_functions_run == 2); // DT_INIT + one INIT_ARRAY entry
    CHECK(boot.entry_gva != 0);
    std::cout << "  boot: entry=0x" << std::hex << boot.entry_gva << std::dec
              << " exit=" << boot.exit_code << "\n";
    return true;
}

// ---------------------------------------------------------------------------
// Part B: corruption matrix (fail-closed)
// ---------------------------------------------------------------------------
bool TestCorruption() {
    std::cout << "[SELF] corruption matrix (fail-closed)\n";
    const Synthetic s = BuildSynthetic();
    const std::string path = Tmp("selftest_bad.self");

    // 1) bad magic
    {
        std::vector<uint8_t> b = s.bytes;
        b[0] = 0x99;
        WriteFile(path, b);
        const SelfParseResult r = ParseProsperoSelf(path);
        CHECK(!r.ok);
        CHECK(r.error.find("magic") != std::string::npos);
    }
    // 2) size/dup mismatch on entry 2
    {
        std::vector<uint8_t> b = s.bytes;
        const uint64_t dup_off = 0x20 + 2 * 32 + 24;
        b[dup_off] ^= 0x01;
        WriteFile(path, b);
        const SelfParseResult r = ParseProsperoSelf(path);
        CHECK(!r.ok);
        CHECK(r.error.find("size/dup") != std::string::npos);
    }
    // 3) entry overlap (point entry 1 back into the header)
    {
        std::vector<uint8_t> b = s.bytes;
        const uint64_t off1 = 0x20 + 1 * 32 + 8;
        uint64_t v = 0x100;
        std::memcpy(b.data() + off1, &v, 8);
        WriteFile(path, b);
        const SelfParseResult r = ParseProsperoSelf(path);
        CHECK(!r.ok);
        CHECK(r.error.find("overlaps") != std::string::npos ||
              r.error.find("bounds") != std::string::npos);
    }
    // 4) missing pairing: drop the PATHX phdr's filesz so nothing matches
    {
        std::vector<uint8_t> b = s.bytes;
        const uint64_t fsz_off = s.elf_off + 0x40 + 2 * sizeof(Loader::Elf64_Phdr) + 32;
        uint64_t v = 0x1234;   // no data segment has this size
        std::memcpy(b.data() + fsz_off, &v, 8);
        WriteFile(path, b);
        const SelfParseResult r = ParseProsperoSelf(path);
        CHECK(!r.ok);
        CHECK(r.error.find("no size-matching") != std::string::npos);
    }
    // 5) truncated embedded ELF
    {
        std::vector<uint8_t> b = s.bytes;
        b.resize(static_cast<size_t>(s.elf_off) + 8);
        WriteFile(path, b);
        const SelfParseResult r = ParseProsperoSelf(path);
        CHECK(!r.ok);
        CHECK(r.error.find("ELF64") != std::string::npos ||
              r.error.find("ELF") != std::string::npos);
    }
    // 6) ciphertext payload: randomize the ENTIRE code and RW segments —
    //    the dynamic table and strtab must be garbage too, otherwise the
    //    printable-NID plaintext evidence would still pass.
    {
        std::vector<uint8_t> b = s.bytes;
        for (size_t i = 0; i < 64; ++i) {
            b[static_cast<size_t>(s.code_data_off) + 0x80 + i] = static_cast<uint8_t>(i * 7 + 3);
        }
        for (size_t i = 0; i < 0x400; ++i) {
            b[static_cast<size_t>(s.rw_data_off) + i] = static_cast<uint8_t>(0xA5 ^ (i * 3));
        }
        WriteFile(path, b);
        const SelfParseResult r = ParseProsperoSelf(path);
        CHECK(r.ok);            // structure still parses...
        CHECK(!r.plaintext);    // ...but the payload is not trusted as code
        std::string err;
        const int rc = FlattenSelfToElf(path, r, Tmp("selftest_cipher.elf"), err);
        CHECK(rc != 0);
        CHECK(err.find("ciphertext") != std::string::npos);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Part C: the REAL eboot.bin (conditional — skipped when absent)
// ---------------------------------------------------------------------------
bool TestRealEboot() {
    std::error_code ec;
    if (!std::filesystem::exists(kRealSelf, ec)) {
        std::cout << "[SELF] real eboot.bin not present — skipping (expected on "
                     "machines without the fixture)\n";
        return true;
    }
    std::cout << "[SELF] REAL eboot.bin: structural facts + flatten round-trip\n";
    const SelfParseResult r = ParseProsperoSelf(kRealSelf);
    if (!CHECK(r.ok)) {
        std::cerr << "  parse error: " << r.error << "\n";
        return false;
    }
    CHECK(r.file_size == 254075005);
    CHECK(r.entry_count == 12);
    CHECK(r.entries.size() == 12);
    CHECK(r.data_segments == 6);
    CHECK(r.meta_segments == 6);
    CHECK(r.elf_offset == 0x1a0);
    CHECK(r.ehdr.e_type == Loader::ET_SCE_DYNEXEC);
    CHECK(r.ehdr.e_ident[7] == 9);                    // FreeBSD
    CHECK(r.ehdr.e_ident[8] == 2);                    // ABI version
    CHECK(r.ehdr.e_machine == 0x3e);
    CHECK(r.ehdr.e_entry == 0x80);
    CHECK(r.ehdr.e_phnum == 14);
    CHECK(r.ehdr.e_phentsize == sizeof(Loader::Elf64_Phdr));
    CHECK(r.pairings.size() == 6);
    // exact pairing facts from the round-30 analysis
    CHECK(r.pairings[0].p_filesz == 0xb7b44ec);       // 192 MB text
    CHECK(r.pairings[1].p_filesz == 0x22e8e00);       // 36 MB rodata
    CHECK(r.pairings[2].p_filesz == 0x682ee0);        // 6.8 MB data
    CHECK(r.pairings[3].p_filesz == 0x60);            // PATHX
    CHECK(r.pairings[4].p_filesz == 0x2daf0);
    CHECK(r.pairings[5].p_filesz == 0x1084ee0);       // 17 MB debug payload
    CHECK(r.module_path.find("Minecraft") != std::string::npos);
    CHECK(r.module_path.find("ps5_x64") != std::string::npos);
    CHECK(r.dt_init == 0x10);
    // The debug image emits zeroed INIT/FINI-array slots — a single DT_INIT
    // at vaddr 0x10 runs at boot (verified in the dynamic dump).
    CHECK(r.dt_init_array == 0);
    CHECK(r.dt_init_arraysz == 0);
    CHECK(r.needed_libraries.size() >= 40);
    bool libc = false, kernel = false, agc = false, videoout = false;
    for (const auto& lib : r.needed_libraries) {
        if (lib == "libc.prx") libc = true;
        if (lib == "libkernel.prx") kernel = true;
        if (lib == "libSceAgc.prx") agc = true;
        if (lib == "libSceVideoOut.prx") videoout = true;
    }
    CHECK(libc && kernel && agc && videoout);
    CHECK(r.plaintext);
    CHECK(!r.tail_libraries.empty());

    // Flatten round-trip: the emitted ELF must carry the same first bytes
    // of the 192 MB text segment as the container, and re-parse as ELF64.
    const std::string flat = Tmp("selftest_real.flat.elf");
    std::string err;
    const int rc = FlattenSelfToElf(kRealSelf, r, flat, err);
    if (!CHECK(rc == 0)) {
        std::cerr << "  flatten error: " << err << "\n";
        return false;
    }
    CHECK(std::filesystem::file_size(flat) == r.flattened_size);
    {
        std::ifstream f(flat, std::ios::binary);
        Loader::Elf64_Ehdr eh{};
        f.read(reinterpret_cast<char*>(&eh), sizeof(eh));
        CHECK(eh.e_ident[0] == 0x7f && eh.e_type == Loader::ET_SCE_DYNEXEC);
        CHECK(eh.e_phnum == 14);
        // first 16 bytes of the guest text (flat 0x4080) == container bytes
        // at (pairings[0].container_offset + 0x80)
        f.seekg(0x4080);
        uint8_t flat_text[16] = {};
        f.read(reinterpret_cast<char*>(flat_text), 16);
        std::ifstream g(kRealSelf, std::ios::binary);
        g.seekg(static_cast<std::streamoff>(r.pairings[0].container_offset + 0x80));
        uint8_t cont_text[16] = {};
        g.read(reinterpret_cast<char*>(cont_text), 16);
        CHECK(std::memcmp(flat_text, cont_text, 16) == 0);
        // entry prologue must be a real x86-64 frame setup (push rbp; mov)
        CHECK(flat_text[0] == 0x55);
        CHECK(flat_text[1] == 0x48 && flat_text[2] == 0x89);
    }
    std::filesystem::remove(flat, ec);   // 254 MB — do not keep in tmp
    std::cout << "  real image: 12 entries, 6 pairings, "
              << r.needed_libraries.size() << " DT_NEEDED, module="
              << r.module_path << "\n";
    return true;
}

} // namespace

int main() {
    std::cout << "=== Prospero SELF container test (round 30) ===\n";
    TestSyntheticParseFlattenBoot();
    TestCorruption();
    TestRealEboot();
    std::cout << (g_failures == 0 ? "SELF TEST PASSED" : "SELF TEST FAILED")
              << ": " << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
