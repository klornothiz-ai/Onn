// prospero_self.cpp — Prospero (PS5) SELF container parser implementation.
//
// Round 30. Everything here is derived from the real 254 MB eboot.bin the
// project received (Minecraft PS5, debug image): magic, chained 32-byte
// segment entries, embedded ET_SCE_DYNEXEC ELF, PT_LOAD <-> data-segment
// size-exact pairing, PATHX build path, dynamic table through the pairing
// map, trailing SCE library table. See prospero_self.hpp for the layout.
//
// Design rules:
//   * WINDOWED reads only — the 192 MB text segment is never loaded into
//     RAM by the parser. FlattenSelfToElf streams 1 MiB chunks.
//   * fail-closed: every field is validated before it is trusted, and the
//     first inconsistency produces a precise error string.
//   * no decryption: this module reads structure, never keys.
#include "loader/prospero_self.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace Loader {

namespace {
std::string Hex32(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "%x", v);
    return std::string(b);
}
std::string Hex64(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof(b), "%llx", static_cast<unsigned long long>(v));
    return std::string(b);
}
} // namespace

namespace {

constexpr uint64_t kMaxEntries = 256;          // sanity bound (real: 12)
constexpr uint64_t kEntrySize = 32;
constexpr uint64_t kHeaderPrefix = 0x20;       // magic..extra before entries
constexpr uint64_t kMaxTailBytes = 64 * 1024;  // tail table read bound
constexpr uint64_t kStreamChunk = 1 << 20;     // 1 MiB copy granularity

bool ReadAt(std::ifstream& f, uint64_t off, void* dst, size_t n) {
    if (n == 0) {
        return true;
    }
    f.seekg(static_cast<std::streamoff>(off), std::ios::beg);
    if (!f.good()) {
        return false;
    }
    f.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(n));
    return static_cast<size_t>(f.gcount()) == n;
}

uint64_t Rd64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | p[i];
    }
    return v;
}
uint32_t Rd32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[3]) << 24) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[1]) << 8) | static_cast<uint32_t>(p[0]);
}
uint16_t Rd16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[1]) << 8) | p[0]);
}

// x86-64 prologue heuristic for the plaintext verdict: the byte at the
// entry point must be a plausible first instruction of a function frame
// (push family, REX prefix, or sub rsp).
bool LooksLikeCodeStart(const uint8_t* p) {
    const uint8_t b0 = p[0];
    const bool push = (b0 >= 0x50 && b0 <= 0x57) || b0 == 0x41 || b0 == 0x55;
    const bool rex = (b0 >= 0x48 && b0 <= 0x4f);
    const bool sub_rsp = b0 == 0x83 || b0 == 0x81;
    const bool mov_imm = (b0 >= 0xb8 && b0 <= 0xbf);   // mov e?x, imm32
    const bool xor_eax = b0 == 0x31 || b0 == 0x33;
    if (!(push || rex || sub_rsp || mov_imm || xor_eax)) {
        return false;
    }
    // mov-imm and xor prologues are complete first instructions already.
    if (mov_imm) {
        return true;   // the imm32 bytes are unconstrained
    }
    // second byte: modrm/reg pattern that follows a push/rex is almost
    // always 0x89 (mov r,r), 0x8b, 0x83, 0x81, 0xe8 (call), or another
    // REX/push prefix — random ciphertext fails this ~93% of the time.
    const uint8_t b1 = p[1];
    switch (b1) {
        case 0x89: case 0x8b: case 0x83: case 0x81: case 0xe8: case 0xe9:
        case 0x85: case 0x8d: case 0x3d: case 0x05:
            return true;
        default:
            return (b1 >= 0x48 && b1 <= 0x5f) || (b1 >= 0x40 && b1 <= 0x57);
    }
}

bool IsLikelyPrintableName(const std::string& s) {
    if (s.size() < 3) {
        return false;
    }
    size_t printable = 0;
    for (const char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '.' || c == ':') {
            ++printable;
        }
    }
    return printable >= s.size() - 1;
}

// Extract unique "name:"-style tokens from the trailing SCE library table.
std::vector<std::string> ScanTailLibraries(const uint8_t* data, size_t n) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t c = data[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '.' || c == ':') {
            cur.push_back(static_cast<char>(c));
        } else {
            if (!cur.empty() && cur.back() == ':' && cur.size() >= 4 &&
                cur.size() <= 64 &&
                std::find(out.begin(), out.end(), cur) == out.end()) {
                out.push_back(cur);
            }
            cur.clear();
        }
    }
    if (!cur.empty() && cur.back() == ':' && std::find(out.begin(), out.end(), cur) == out.end()) {
        out.push_back(cur);
    }
    return out;
}

} // namespace

bool HasProsperoSelfMagic(const uint8_t head[4]) {
    return head[0] == 0x54 && head[1] == 0x14 && head[2] == 0xF5 && head[3] == 0xEE;
}

SelfParseResult ParseProsperoSelf(const std::string& path) {
    SelfParseResult r;

    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        r.error = "SELF: cannot open file";
        return r;
    }

    struct stat st{};
    if (stat(path.c_str(), &st) != 0 || st.st_size <= 0) {
        r.error = "SELF: cannot stat file";
        return r;
    }
    r.file_size = static_cast<uint64_t>(st.st_size);

    // ---- container header -------------------------------------------------
    uint8_t head[32] = {};
    if (!ReadAt(f, 0, head, sizeof(head))) {
        r.error = "SELF: header unreadable";
        return r;
    }
    r.magic = Rd32(head);
    if (!HasProsperoSelfMagic(head)) {
        r.error = "SELF: bad magic (expected 54 14 F5 EE)";
        return r;
    }
    std::memcpy(r.version, head + 4, 4);
    r.mode = Rd16(head + 8);
    r.endian = head[10];
    r.attr = head[11];
    r.aux = Rd32(head + 12);              // u32 @0x0C (real: 0x06100560)
    r.tail_offset = Rd64(head + 16);      // u64 @0x10 (real: filesize-749)
    r.entry_count = Rd16(head + 24);      // u16 @0x18 (real: 12)
    r.extra = Rd16(head + 26);            // u16 @0x1A (real: 0x22)

    if (r.entry_count == 0 || r.entry_count > kMaxEntries) {
        r.error = "SELF: implausible entry count " + std::to_string(r.entry_count);
        return r;
    }
    if (r.tail_offset > r.file_size) {
        r.error = "SELF: tail offset beyond EOF";
        return r;
    }

    // ---- segment entry table ----------------------------------------------
    r.entries.resize(r.entry_count);
    std::vector<uint8_t> table(static_cast<size_t>(kEntrySize * r.entry_count));
    if (!ReadAt(f, kHeaderPrefix, table.data(), table.size())) {
        r.error = "SELF: entry table unreadable";
        return r;
    }
    for (uint32_t i = 0; i < r.entry_count; ++i) {
        const uint8_t* e = table.data() + static_cast<size_t>(i * kEntrySize);
        SelfSegmentEntry& ent = r.entries[i];
        ent.flags = Rd64(e);
        ent.offset = Rd64(e + 8);
        ent.size = Rd64(e + 16);
        ent.size_dup = Rd64(e + 24);
        ent.is_data = (ent.flags & 0xFF00) == 0x2800;

        if (ent.size != ent.size_dup) {
            r.error = "SELF: entry " + std::to_string(i) + " size/dup mismatch";
            return r;
        }
        if (ent.offset % 16 != 0) {
            r.error = "SELF: entry " + std::to_string(i) + " offset not 16-byte aligned";
            return r;
        }
        if (ent.size > r.file_size || ent.offset > r.file_size - ent.size) {
            r.error = "SELF: entry " + std::to_string(i) + " exceeds file bounds";
            return r;
        }
        if (ent.is_data) {
            ++r.data_segments;
        } else {
            ++r.meta_segments;
        }
        // Chaining: each entry starts at/after the previous one ends, with
        // a bounded alignment gap (real file: 0 or 4 bytes).
        if (i > 0) {
            const SelfSegmentEntry& prev = r.entries[i - 1];
            const uint64_t prev_end = prev.offset + prev.size;
            if (ent.offset < prev_end) {
                r.error = "SELF: entry " + std::to_string(i) + " overlaps entry " +
                          std::to_string(i - 1);
                return r;
            }
            if (ent.offset - prev_end > 0x1000) {
                r.error = "SELF: entry " + std::to_string(i) + " chain gap too large";
                return r;
            }
        }
    }

    // ---- embedded ELF header + program headers ----------------------------
    r.elf_offset = kHeaderPrefix + kEntrySize * r.entry_count;
    if (r.elf_offset % 16 != 0) {
        r.error = "SELF: embedded ELF at unaligned offset";
        return r;
    }
    Elf64_Ehdr eh{};
    if (!ReadAt(f, r.elf_offset, &eh, sizeof(eh))) {
        r.error = "SELF: embedded ELF header unreadable";
        return r;
    }
    if (eh.e_ident[0] != 0x7f || eh.e_ident[1] != 'E' || eh.e_ident[2] != 'L' ||
        eh.e_ident[3] != 'F' || eh.e_ident[4] != 2 || eh.e_ident[5] != 1) {
        r.error = "SELF: embedded image is not an ELF64 LE executable";
        return r;
    }
    if (eh.e_machine != EM_X86_64) {
        r.error = "SELF: embedded ELF is not x86-64";
        return r;
    }
    if (eh.e_type != ET_EXEC && eh.e_type != ET_DYN &&
        eh.e_type != ET_SCE_EXEC && eh.e_type != ET_SCE_DYNEXEC &&
        eh.e_type != ET_SCE_DYNAMIC) {
        r.error = "SELF: unsupported embedded e_type 0x" + Hex32(eh.e_type);
        return r;
    }
    if (eh.e_phentsize != sizeof(Elf64_Phdr) || eh.e_phnum == 0 || eh.e_phnum > 256) {
        r.error = "SELF: implausible phdr table";
        return r;
    }
    r.ehdr = eh;
    r.phdrs.resize(eh.e_phnum);
    if (!ReadAt(f, r.elf_offset + eh.e_phoff, r.phdrs.data(),
                sizeof(Elf64_Phdr) * eh.e_phnum)) {
        r.error = "SELF: phdr table unreadable";
        return r;
    }
    // The embedded ELF header + phdr table live in the header region BEFORE
    // the first container segment (real file: ELF at 0x1a0, first entry at
    // 0xb70). A first entry overlapping the phdr table means corruption.
    const uint64_t phdr_end = r.elf_offset + eh.e_phoff +
                              sizeof(Elf64_Phdr) * eh.e_phnum;
    if (!r.entries.empty() && r.entries.front().offset < phdr_end) {
        r.error = "SELF: first segment entry overlaps the embedded ELF phdrs";
        return r;
    }

    // ---- PT_LOAD / PT_SCE_DYNDATA <-> data-segment pairing (size exact) --
    std::vector<bool> used(r.entry_count, false);
    uint64_t flat_max = eh.e_phoff + sizeof(Elf64_Phdr) * eh.e_phnum;
    for (uint32_t p = 0; p < eh.e_phnum; ++p) {
        const Elf64_Phdr& ph = r.phdrs[p];
        flat_max = std::max<uint64_t>(flat_max, ph.p_offset + ph.p_filesz);
        if (ph.p_type != PT_LOAD && ph.p_type != PT_SCE_DYNDATA) {
            continue;
        }
        if (ph.p_filesz == 0) {
            continue;  // BSS-only loads need no container backing
        }
        int matched = -1;
        for (uint32_t e = 0; e < r.entry_count; ++e) {
            if (used[e] || !r.entries[e].is_data) {
                continue;
            }
            if (r.entries[e].size == ph.p_filesz) {
                matched = static_cast<int>(e);
                break;
            }
        }
        if (matched < 0) {
            r.error = "SELF: phdr " + std::to_string(p) + " (filesz 0x" +
                      Hex64(ph.p_filesz) +
                      ") has no size-matching data segment";
            return r;
        }
        used[static_cast<size_t>(matched)] = true;
        SelfLoadPairing pair;
        pair.phdr_index = p;
        pair.p_offset = ph.p_offset;
        pair.p_vaddr = ph.p_vaddr;
        pair.p_filesz = ph.p_filesz;
        pair.p_memsz = ph.p_memsz;
        pair.p_flags = ph.p_flags;
        pair.entry_index = static_cast<uint32_t>(matched);
        pair.container_offset = r.entries[static_cast<size_t>(matched)].offset;
        pair.container_size = r.entries[static_cast<size_t>(matched)].size;
        r.pairings.push_back(pair);
    }
    r.flattened_size = flat_max;

    // ---- PT_SCE_DYNDATA (PATHX build path) ---------------------------------
    for (const SelfLoadPairing& pair : r.pairings) {
        if (r.phdrs[pair.phdr_index].p_type != PT_SCE_DYNDATA) {
            continue;
        }
        if (pair.p_filesz > 4096) {
            continue;
        }
        std::vector<uint8_t> px(static_cast<size_t>(pair.p_filesz));
        if (ReadAt(f, pair.container_offset, px.data(), px.size())) {
            const char* magic = "PATHX";
            if (px.size() > 16 && std::memcmp(px.data(), magic, 5) == 0) {
                const size_t begin = 12;  // PATHX + hdr fields precede the string
                size_t end = begin;
                while (end < px.size() && px[end] != 0) {
                    ++end;
                }
                if (end > begin) {
                    r.module_path.assign(reinterpret_cast<const char*>(px.data() + begin),
                                         end - begin);
                }
            }
        }
    }

    // ---- dynamic facts through the pairing map -----------------------------
    auto elfOffToContainer = [&](uint64_t elf_off) -> uint64_t {
        for (const SelfLoadPairing& pair : r.pairings) {
            if (elf_off >= pair.p_offset && elf_off < pair.p_offset + pair.p_filesz) {
                return pair.container_offset + (elf_off - pair.p_offset);
            }
        }
        return 0;
    };
    auto vaddrToContainer = [&](uint64_t vaddr) -> uint64_t {
        for (const SelfLoadPairing& pair : r.pairings) {
            if (vaddr >= pair.p_vaddr && vaddr < pair.p_vaddr + pair.p_filesz) {
                return pair.container_offset + (vaddr - pair.p_vaddr);
            }
        }
        return 0;
    };

    for (const Elf64_Phdr& ph : r.phdrs) {
        if (ph.p_type != PT_DYNAMIC || ph.p_filesz == 0 || ph.p_filesz > (1 << 20)) {
            continue;
        }
        const uint64_t dyn_cont = elfOffToContainer(ph.p_offset);
        if (dyn_cont == 0) {
            continue;  // dynamic not inside a paired segment: skip facts
        }
        std::vector<uint8_t> dyn(static_cast<size_t>(ph.p_filesz));
        if (!ReadAt(f, dyn_cont, dyn.data(), dyn.size())) {
            break;
        }
        std::vector<uint64_t> needed_offs;
        for (size_t o = 0; o + 16 <= dyn.size(); o += 16) {
            const uint64_t tag = Rd64(dyn.data() + o);
            const uint64_t val = Rd64(dyn.data() + o + 8);
            if (tag == DT_NULL) {
                break;
            }
            if (tag == DT_NEEDED) {
                needed_offs.push_back(val);
            } else if (tag == DT_STRTAB) {
                r.dt_strtab = val;
            } else if (tag == DT_INIT) {
                r.dt_init = val;
            } else if (tag == DT_INIT_ARRAY) {
                r.dt_init_array = val;
            } else if (tag == DT_INIT_ARRAYSZ) {
                r.dt_init_arraysz = val;
            }
        }
        if (r.dt_strtab != 0 && !needed_offs.empty()) {
            const uint64_t strtab_cont = vaddrToContainer(r.dt_strtab);
            if (strtab_cont != 0) {
                std::vector<uint8_t> strtab(64 * 1024);
                const size_t got = std::min<uint64_t>(strtab.size(), r.file_size - strtab_cont);
                strtab.resize(got);
                if (ReadAt(f, strtab_cont, strtab.data(), got)) {
                    for (const uint64_t no : needed_offs) {
                        if (no >= strtab.size()) {
                            continue;
                        }
                        const char* p = reinterpret_cast<const char*>(strtab.data()) + no;
                        const size_t len = strnlen(p, strtab.size() - no);
                        if (len >= 3) {
                            std::string name(p, len);
                            if (IsLikelyPrintableName(name)) {
                                r.needed_libraries.push_back(name);
                            }
                        }
                    }
                }
            }
        }
        break;  // first PT_DYNAMIC only
    }

    // ---- trailing SCE library table ----------------------------------------
    if (r.tail_offset < r.file_size) {
        r.tail_bytes = std::min<uint64_t>(r.file_size - r.tail_offset, kMaxTailBytes);
        std::vector<uint8_t> tail(static_cast<size_t>(r.tail_bytes));
        if (ReadAt(f, r.tail_offset, tail.data(), tail.size())) {
            r.tail_libraries = ScanTailLibraries(tail.data(), tail.size());
        }
    }

    // ---- plaintext verdict --------------------------------------------------
    {
        uint8_t probe[8] = {};
        const uint64_t entry_cont = vaddrToContainer(r.ehdr.e_entry);
        if (entry_cont != 0 && ReadAt(f, entry_cont, probe, sizeof(probe))) {
            r.plaintext = LooksLikeCodeStart(probe);
        }
        // Strengthen: ciphertext randomizes the dynamic tags; a parsed
        // DT_STRTAB with printable DT_NEEDED names is strong plaintext
        // evidence (retail images fail the printable-name filter).
        if (!r.needed_libraries.empty()) {
            r.plaintext = true;
        }
        if (entry_cont == 0) {
            r.error = "SELF: entry point not covered by any paired segment";
            return r;
        }
    }

    r.ok = true;
    return r;
}

int FlattenSelfToElf(const std::string& self_path, const SelfParseResult& parsed,
                     const std::string& out_path, std::string& error_out) {
    if (!parsed.ok) {
        error_out = "SELF: refusing to flatten an unvalidated container";
        return 1;
    }
    if (!parsed.plaintext) {
        error_out = "SELF: payload is ciphertext (retail image); this project "
                    "does not decrypt — provide a debug/devkit image";
        return 2;
    }
    if (parsed.pairings.empty()) {
        error_out = "SELF: no PT_LOAD pairing — nothing to emit";
        return 3;
    }

    std::ifstream in(self_path, std::ios::binary);
    if (!in.is_open()) {
        error_out = "SELF: cannot reopen container";
        return 4;
    }

    FILE* out = std::fopen(out_path.c_str(), "wb");
    if (out == nullptr) {
        error_out = "SELF: cannot create output ELF at " + out_path;
        return 5;
    }

    // Final size up-front (sparse-friendly); then write header, phdrs, and
    // stream every paired segment to its p_offset position.
    if (ftruncate(fileno(out), static_cast<off_t>(parsed.flattened_size)) != 0) {
        error_out = "SELF: ftruncate failed for flat ELF";
        std::fclose(out);
        return 6;
    }

    auto writeAt = [&](uint64_t off, const void* data, size_t n) -> bool {
        return std::fseek(out, static_cast<long>(off), SEEK_SET) == 0 &&
               std::fwrite(data, 1, n, out) == n;
    };

    // Header copy with section headers stripped (the container does not
    // carry them; e_shoff in the embedded header is stale).
    Elf64_Ehdr eh = parsed.ehdr;
    eh.e_shoff = 0;
    eh.e_shnum = 0;
    eh.e_shstrndx = 0;
    if (!writeAt(0, &eh, sizeof(eh))) {
        error_out = "SELF: ELF header write failed";
        std::fclose(out);
        return 7;
    }
    if (!writeAt(eh.e_phoff, parsed.phdrs.data(),
                 sizeof(Elf64_Phdr) * parsed.phdrs.size())) {
        error_out = "SELF: phdr write failed";
        std::fclose(out);
        return 8;
    }

    std::vector<uint8_t> chunk;
    chunk.resize(static_cast<size_t>(kStreamChunk));
    for (const SelfLoadPairing& pair : parsed.pairings) {
        uint64_t remaining = pair.container_size;
        uint64_t src = pair.container_offset;
        uint64_t dst = pair.p_offset;
        while (remaining > 0) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(remaining, kStreamChunk));
            in.seekg(static_cast<std::streamoff>(src), std::ios::beg);
            if (!in.good()) {
                error_out = "SELF: container read failed during flatten";
                std::fclose(out);
                return 9;
            }
            in.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(n));
            if (static_cast<size_t>(in.gcount()) != n) {
                error_out = "SELF: short container read during flatten";
                std::fclose(out);
                return 10;
            }
            if (std::fseek(out, static_cast<long>(dst), SEEK_SET) != 0 ||
                std::fwrite(chunk.data(), 1, n, out) != n) {
                error_out = "SELF: flat write failed at offset " + std::to_string(dst);
                std::fclose(out);
                return 11;
            }
            src += n;
            dst += n;
            remaining -= n;
        }
    }

    std::fflush(out);
    std::fclose(out);
    return 0;
}

} // namespace Loader
