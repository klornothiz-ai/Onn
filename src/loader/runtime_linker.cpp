// ProsperoLayer PS5 emulator - runtime linker implementation
//
// Round 9 deepening: LoadProgram now owns and parses the ELF image
// (export symbols from .dynsym), RelocateProgram applies real x86-64
// relocations (R_X86_64_RELATIVE / 64 / GLOB_DAT / JUMP_SLOT) to the owned
// image, Dlsym/FindSymbol resolve through the export table, ReadFromElf reads
// from the relocated image, and module handles come from a monotonic
// allocator (unique_id is finally assigned -- KernelLoadModule used to return
// 0 and every subsequent KernelDlsym failed with ESRCH).
#include "loader/runtimeLinker.h"

#include "cpu/hle_trampoline.hpp"
#include "loader/symbolDatabase.h"

#include <functional>
#include "common/singleton.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>

namespace Loader {

namespace {

bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& out) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
                return false;
        }
        const std::streamsize size = file.tellg();
        if (size <= 0 || static_cast<uint64_t>(size) > std::numeric_limits<size_t>::max()) {
                return false;
        }
        out.resize(static_cast<size_t>(size));
        file.seekg(0, std::ios::beg);
        return static_cast<bool>(file.read(reinterpret_cast<char*>(out.data()), size));
}

// Translate a link-time virtual address to a file offset through the PT_LOAD
// segments. Returns false when the address is not covered by any segment.
bool VaddrToFileOffset(const Elf64_Ehdr* ehdr, const Elf64_Phdr* phdr, uint64_t vaddr,
                       uint64_t& out_offset) {
        if (ehdr == nullptr || phdr == nullptr) {
                return false;
        }
        for (uint32_t i = 0; i < ehdr->e_phnum; ++i) {
                const Elf64_Phdr& segment = phdr[i];
                if (segment.p_type != PT_LOAD) {
                        continue;
                }
                if (vaddr >= segment.p_vaddr &&
                    vaddr - segment.p_vaddr < segment.p_filesz) {
                        out_offset = vaddr - segment.p_vaddr + segment.p_offset;
                        return true;
                }
        }
        return false;
}

// Raw .dynsym view: (pointer, symbol count, file offset of the entries).
// Section headers are authoritative when present; otherwise DT_SYMTAB with
// the dynsym->dynstr distance heuristic is used (common linker layout).
// Round 30: real PS5 eboots place .dynstr BEFORE .dynsym (observed on the
// 254 MB Minecraft image: strtab 0xe416840 < symtab 0xe41a6e8), which breaks
// the distance heuristic — DT_HASH carries nchain == symbol count, the
// SysV-standard fallback.
bool LocateDynsym(const ElfImage& image, uint64_t symtab_vaddr, uint64_t strtab_vaddr,
                  uint64_t hash_vaddr, const uint8_t*& out_syms, size_t& out_count) {
        const uint8_t* const data = image.Data();
        const size_t size = image.Size();

        if (image.GetShdr() != nullptr && image.GetShnum() > 0) {
                for (uint32_t i = 0; i < image.GetShnum(); ++i) {
                        const Elf64_Shdr& sh = image.GetShdr()[i];
                        if (sh.sh_type != SHT_DYNSYM || sh.sh_entsize != sizeof(Elf64_Sym)) {
                                continue;
                        }
                        if (sh.sh_offset >= size ||
                            sh.sh_size > size - sh.sh_offset) {
                                continue;
                        }
                        out_syms = data + sh.sh_offset;
                        out_count = static_cast<size_t>(sh.sh_size / sizeof(Elf64_Sym));
                        return true;
                }
        }

        if (symtab_vaddr != 0 && strtab_vaddr > symtab_vaddr) {
                uint64_t symtab_off = 0;
                if (!VaddrToFileOffset(image.GetEhdr(), image.GetPhdr(), symtab_vaddr,
                                       symtab_off)) {
                        return false;
                }
                // The dynamic table does not record the symbol count; the
                // string table almost always directly follows the symbol
                // table in the same PT_LOAD.
                const uint64_t span = strtab_vaddr - symtab_vaddr;
                out_syms = data + symtab_off;
                out_count = static_cast<size_t>(span / sizeof(Elf64_Sym));
                return true;
        }
        if (symtab_vaddr != 0 && hash_vaddr != 0) {
                uint64_t symtab_off = 0;
                uint64_t hash_off = 0;
                if (!VaddrToFileOffset(image.GetEhdr(), image.GetPhdr(), symtab_vaddr,
                                       symtab_off) ||
                    !VaddrToFileOffset(image.GetEhdr(), image.GetPhdr(), hash_vaddr,
                                       hash_off)) {
                        return false;
                }
                if (hash_off + 8 > size) {
                        return false;
                }
                const uint32_t nchain =
                        *reinterpret_cast<const uint32_t*>(data + hash_off + 4);
                const size_t sym_bytes = static_cast<size_t>(nchain) * sizeof(Elf64_Sym);
                if (nchain == 0 || nchain > 0x100000 ||
                    symtab_off + sym_bytes > size) {
                        return false;
                }
                out_syms = data + symtab_off;
                out_count = nchain;
                return true;
        }
        return false;
}

// Locate the dynamic string table for symbol names: section headers are
// authoritative when present; otherwise DT_STRTAB is translated through the
// PT_LOAD segments (DT_STRSZ is not recorded here, so names are bounded by
// the image size -- every name read is bounded with strnlen anyway).
bool LocateDynstr(const ElfImage& image, uint64_t strtab_vaddr,
                  const char*& out_strtab, size_t& out_size) {
        out_strtab = nullptr;
        out_size = 0;
        if (strtab_vaddr == 0) {
                return false;
        }
        if (image.GetShdr() != nullptr) {
                for (uint32_t i = 0; i < image.GetShnum(); ++i) {
                        const Elf64_Shdr& sh = image.GetShdr()[i];
                        if (sh.sh_type == SHT_STRTAB && sh.sh_addr != 0 &&
                            sh.sh_addr == strtab_vaddr) {
                                out_strtab = reinterpret_cast<const char*>(
                                        image.Data() + sh.sh_offset);
                                out_size = static_cast<size_t>(sh.sh_size);
                                return true;
                        }
                }
        }
        uint64_t strtab_off = 0;
        if (VaddrToFileOffset(image.GetEhdr(), image.GetPhdr(), strtab_vaddr,
                              strtab_off)) {
                out_strtab = reinterpret_cast<const char*>(image.Data() + strtab_off);
                out_size = image.Size() - static_cast<size_t>(strtab_off);
                return true;
        }
        return false;
}

} // namespace

RuntimeLinker& RuntimeLinker::Instance() {
        // Single canonical instance: libKernel reaches the linker through
        // Common::Singleton<RuntimeLinker>::Instance(); route the direct
        // accessor to the SAME object so the module list is never split
        // between two singletons.
        return *Common::Singleton<RuntimeLinker>::Instance();
}

// ---------------------------------------------------------------------------
// ElfImage: owned-image loading
// ---------------------------------------------------------------------------
bool ElfImage::LoadFromMemory(const uint8_t* data, size_t size) {
        if (data == nullptr || size < sizeof(Elf64_Ehdr)) {
                return false;
        }
        const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(data);
        if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 ||
            ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3 ||
            ehdr->e_ident[4] != ELFCLASS64 || ehdr->e_machine != EM_X86_64) {
                return false;
        }
        if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0 ||
            ehdr->e_phentsize != sizeof(Elf64_Phdr) ||
            ehdr->e_phoff > size ||
            static_cast<uint64_t>(ehdr->e_phnum) * sizeof(Elf64_Phdr) > size - ehdr->e_phoff) {
                return false;
        }
        if (ehdr->e_shoff != 0) {
                if (ehdr->e_shentsize != sizeof(Elf64_Shdr) || ehdr->e_shoff > size ||
                    static_cast<uint64_t>(ehdr->e_shnum) * sizeof(Elf64_Shdr) >
                        size - ehdr->e_shoff) {
                        return false;
                }
        }

        m_owned.assign(data, data + size);
        m_data = m_owned.data();
        m_size = size;
        m_ehdr = reinterpret_cast<const Elf64_Ehdr*>(m_owned.data());
        m_phdr = reinterpret_cast<const Elf64_Phdr*>(m_owned.data() + ehdr->e_phoff);
        m_phnum = ehdr->e_phnum;
        m_shnum = 0;
        m_shdr = nullptr;
        if (ehdr->e_shoff != 0 && ehdr->e_shstrndx < ehdr->e_shnum) {
                m_shdr = reinterpret_cast<const Elf64_Shdr*>(m_owned.data() + ehdr->e_shoff);
                m_shnum = ehdr->e_shnum;
                // Section names live in the .shstrtab section.
                const Elf64_Shdr& names_section = m_shdr[ehdr->e_shstrndx];
                if (names_section.sh_type == SHT_STRTAB &&
                    names_section.sh_offset < size &&
                    names_section.sh_size <= size - names_section.sh_offset) {
                        const char* const names =
                                reinterpret_cast<const char*>(m_owned.data() +
                                                              names_section.sh_offset);
                        const size_t names_size = static_cast<size_t>(names_section.sh_size);
                        std::vector<std::string> section_names;
                        section_names.reserve(m_shnum);
                        for (uint32_t i = 0; i < m_shnum; ++i) {
                                const uint64_t offset = m_shdr[i].sh_name;
                                if (offset >= names_size) {
                                        section_names.emplace_back();
                                        continue;
                                }
                                const char* name = names + offset;
                                const size_t remaining = names_size - static_cast<size_t>(offset);
                                section_names.emplace_back(name, strnlen(name, remaining));
                        }
                        m_section_names = std::move(section_names);
                }
        }
        return true;
}

// ---------------------------------------------------------------------------
// Module management
// ---------------------------------------------------------------------------
int RuntimeLinker::LoadModule(const std::string& path, uint64_t* out_handle) {
        ModuleInfo info;
        info.name       = path;
        info.is_loaded  = false;
        info.handle     = m_next_module_id++;       // monotonic, never reused
        info.unique_id  = static_cast<uint64_t>(info.handle);
        const uint64_t handle = static_cast<uint64_t>(info.handle);
        m_modules.push_back(std::move(info));
        if (out_handle != nullptr) {
                *out_handle = handle;
        }
        return 0;
}

int RuntimeLinker::UnloadModule(uint64_t handle) {
        auto it = std::find_if(m_modules.begin(), m_modules.end(),
                               [handle](const ModuleInfo& m) { return m.handle == static_cast<int>(handle); });
        if (it == m_modules.end() || it->is_unloaded) {
                return -1;
        }
        // Round 33: mark-as-unloaded instead of erase. std::deque::erase
        // invalidates pointers to elements after the erased position,
        // so any outstanding ModuleInfo* handed out by FindProgramByAddr /
        // FindProgramById / FindModule becomes dangling. Marking keeps all
        // pointers stable while making the module invisible to lookups.
        it->is_unloaded = true;
        return 0;
}

int RuntimeLinker::StartModule(uint64_t handle, size_t args, const void* argp, int flags,
                               int* out_result) {
        (void)handle;
        (void)args;
        (void)argp;
        (void)flags;
        if (out_result != nullptr) {
                *out_result = 0;
        }
        return 0;
}

int RuntimeLinker::StopModule(uint64_t handle, size_t args, const void* argp, int flags,
                              int* out_result) {
        (void)handle;
        (void)args;
        (void)argp;
        (void)flags;
        if (out_result != nullptr) {
                *out_result = 0;
        }
        return 0;
}

// ---------------------------------------------------------------------------
// Program lifecycle (used by libC / libKernel entry points)
// ---------------------------------------------------------------------------
int RuntimeLinker::LoadProgram(const std::string& path, size_t args, const void* argp) {
        (void)args;
        (void)argp;
        ModuleInfo info{};
        info.name      = std::filesystem::path(path).filename().string();
        info.file_name = path;
        info.handle    = m_next_module_id++;        // monotonic, never reused
        info.unique_id = static_cast<uint64_t>(info.handle);

        // Round 9: when the file exists and is a valid ELF64 x86-64 image,
        // own the bytes and parse the export symbol table so Dlsym /
        // RelocateProgram have real data to work with. A missing or non-ELF
        // path still records a name-only module (the legacy behaviour), so
        // KernelGetModuleInfo keeps working for every existing caller.
        std::vector<uint8_t> raw;
        if (ReadWholeFile(path, raw)) {
                auto image = std::make_unique<ElfImage>();
                if (image->LoadFromMemory(raw.data(), raw.size())) {
                        // Round 18: record the PT_LOAD span so
                        // FindProgramByAddr resolves modules loaded from
                        // real ELF images (the field stayed 0 and every
                        // address lookup silently failed -- latent gap).
                        {
                                uint64_t lo = ~0ull, hi = 0;
                                bool any = false;
                                for (uint32_t i = 0; i < image->GetPhnum(); ++i) {
                                        const Elf64_Phdr& ph = image->GetPhdr()[i];
                                        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) {
                                                continue;
                                        }
                                        any = true;
                                        if (ph.p_vaddr < lo) lo = ph.p_vaddr;
                                        if (ph.p_vaddr + ph.p_memsz > hi) {
                                                hi = ph.p_vaddr + ph.p_memsz;
                                        }
                                }
                                if (any) {
                                        info.size = hi - lo;
                                }
                        }
                        // Export symbols: .dynsym entries that are actually
                        // defined in this module (shndx != SHN_UNDEF).
                        uint64_t symtab_vaddr = 0;
                        uint64_t strtab_vaddr = 0;
                        std::vector<uint64_t> needed_offsets;   // round 14: DT_NEEDED
                        uint64_t soname_offset = 0;             // round 14: DT_SONAME
                        const Elf64_Phdr* const phdr = image->GetPhdr();
                        for (uint32_t i = 0; i < image->GetPhnum(); ++i) {
                                if (phdr[i].p_type == PT_TLS) {
                                        // Round 11: TLS template (initial-exec
                                        // model): the per-thread allocator
                                        // copies p_filesz initialized bytes
                                        // from p_offset and zero-fills the
                                        // rest of p_memsz.
                                        info.tls_vaddr = phdr[i].p_vaddr;
                                        info.tls_filesz = static_cast<size_t>(phdr[i].p_filesz);
                                        info.tls_size = static_cast<size_t>(phdr[i].p_memsz);
                                        info.tls_align = static_cast<size_t>(phdr[i].p_align
                                                ? phdr[i].p_align : 16);
                                        continue;
                                }
                                if (phdr[i].p_type != PT_DYNAMIC) {
                                        continue;
                                }
                                const auto* dyn = reinterpret_cast<const Elf64_Dyn*>(
                                        image->Data() + phdr[i].p_offset);
                                const size_t count =
                                        static_cast<size_t>(phdr[i].p_filesz / sizeof(Elf64_Dyn));
                                for (size_t e = 0; e < count && dyn[e].d_tag != DT_NULL; ++e) {
                                        if (dyn[e].d_tag == DT_SYMTAB) {
                                                symtab_vaddr = dyn[e].d_ptr;
                                        } else if (dyn[e].d_tag == DT_STRTAB) {
                                                strtab_vaddr = dyn[e].d_ptr;
                                        } else if (dyn[e].d_tag == DT_NEEDED) {
                                                needed_offsets.push_back(dyn[e].d_val);
                                        } else if (dyn[e].d_tag == DT_SONAME) {
                                                soname_offset = dyn[e].d_val;
                                        } else if (dyn[e].d_tag == DT_INIT) {
                                                info.init_fini.init_vaddr =
                                                        static_cast<uint64_t>(dyn[e].d_ptr);
                                        } else if (dyn[e].d_tag == DT_FINI) {
                                                info.init_fini.fini_vaddr =
                                                        static_cast<uint64_t>(dyn[e].d_ptr);
                                        } else if (dyn[e].d_tag == DT_INIT_ARRAY) {
                                                info.init_fini.init_array_vaddr =
                                                        static_cast<uint64_t>(dyn[e].d_ptr);
                                        } else if (dyn[e].d_tag == DT_INIT_ARRAYSZ) {
                                                info.init_fini.init_array_count =
                                                        static_cast<uint64_t>(dyn[e].d_val) / 8;
                                        } else if (dyn[e].d_tag == DT_FINI_ARRAY) {
                                                info.init_fini.fini_array_vaddr =
                                                        static_cast<uint64_t>(dyn[e].d_ptr);
                                        } else if (dyn[e].d_tag == DT_FINI_ARRAYSZ) {
                                                info.init_fini.fini_array_count =
                                                        static_cast<uint64_t>(dyn[e].d_val) / 8;
                                        }
                                }
                        }

                        const uint8_t* syms = nullptr;
                        size_t sym_count = 0;
                        if (LocateDynsym(*image, symtab_vaddr, strtab_vaddr, 0, syms, sym_count) &&
                            syms != nullptr && sym_count > 0) {
                                // Locate the string table for the symbol names.
                                const char* strtab = nullptr;
                                size_t strtab_size = 0;
                                if (image->GetShdr() != nullptr) {
                                        for (uint32_t i = 0; i < image->GetShnum(); ++i) {
                                                const Elf64_Shdr& sh = image->GetShdr()[i];
                                                if (sh.sh_type == SHT_STRTAB && sh.sh_addr != 0 &&
                                                    sh.sh_addr == strtab_vaddr) {
                                                        strtab = reinterpret_cast<const char*>(
                                                                image->Data() + sh.sh_offset);
                                                        strtab_size = static_cast<size_t>(sh.sh_size);
                                                        break;
                                                }
                                        }
                                }
                                if (strtab == nullptr && strtab_vaddr != 0) {
                                        uint64_t strtab_off = 0;
                                        if (VaddrToFileOffset(image->GetEhdr(), image->GetPhdr(),
                                                              strtab_vaddr, strtab_off)) {
                                                strtab = reinterpret_cast<const char*>(
                                                        image->Data() + strtab_off);
                                                // DT_STRSZ is not recorded here; bound
                                                // names by the image size instead.
                                                strtab_size = image->Size() -
                                                              static_cast<size_t>(strtab_off);
                                        }
                                }
                                if (strtab != nullptr) {
                                        // Round 14: resolve DT_NEEDED names + DT_SONAME
                                        // against the dynamic string table.
                                        const auto read_name = [&](uint64_t offset) -> std::string {
                                                if (offset >= strtab_size) return {};
                                                const char* n = strtab + offset;
                                                const size_t rem =
                                                        strtab_size - static_cast<size_t>(offset);
                                                return std::string(n, strnlen(n, rem));
                                        };
                                        for (const uint64_t off : needed_offsets) {
                                                std::string n = read_name(off);
                                                if (!n.empty()) {
                                                        info.needed_libraries.push_back(std::move(n));
                                                }
                                        }
                                        info.so_name = read_name(soname_offset);
                                        auto exports =
                                                std::make_unique<ModuleInfo::ExportSymbols>();
                                        for (size_t s = 0; s < sym_count; ++s) {
                                                const Elf64_Sym& sym =
                                                        reinterpret_cast<const Elf64_Sym*>(
                                                                syms)[s];
                                                if (sym.st_shndx == SHN_UNDEF ||
                                                    sym.st_name >= strtab_size) {
                                                        continue;
                                                }
                                                const char* name = strtab + sym.st_name;
                                                const size_t remaining =
                                                        strtab_size - static_cast<size_t>(sym.st_name);
                                                const std::string symbol(name,
                                                                         strnlen(name, remaining));
                                                if (symbol.empty()) {
                                                        continue;
                                                }
                                                exports->names.push_back(symbol);
                                                exports->values.push_back(sym.st_value);
                                        }
                                        if (exports->is_valid()) {
                                                info.export_symbols = std::move(exports);
                                        }
                                }
                        }
                        info.elf = std::move(image);
                        info.is_loaded = true;
                }
        }
        m_modules.push_back(std::move(info));
        return 0;
}

ModuleInfo* RuntimeLinker::LoadProgram(const std::string& path) {
        const int rc = LoadProgram(path, 0, nullptr);
        if (rc != 0) {
                return nullptr;
        }
        return FindProgramByFileName(path);
}

// ---------------------------------------------------------------------------
// Relocation
// ---------------------------------------------------------------------------
int RuntimeLinker::RelocateProgram(ModuleInfo* program) {
        if (program == nullptr) {
                return -1;
        }
        program->relocs_applied = 0;
        program->relocs_skipped = 0;
        program->relocs_imported = 0;

        ElfImage* const image = program->elf.get();
        if (image == nullptr || !image->IsValid()) {
                // Name-only module (no ELF image): nothing to relocate. This
                // keeps the legacy no-op behaviour for every pre-round-9
                // caller.
                return 0;
        }

        const uint64_t base = program->base_addr;  // load bias
        const Elf64_Ehdr* const ehdr = image->GetEhdr();
        const Elf64_Phdr* const phdr = image->GetPhdr();
        const size_t image_size = image->Size();

        // Locate PT_DYNAMIC and collect the tags the linker consumes.
        const Elf64_Dyn* dynamic = nullptr;
        size_t dynamic_count = 0;
        for (uint32_t i = 0; i < ehdr->e_phnum; ++i) {
                if (phdr[i].p_type != PT_DYNAMIC) {
                        continue;
                }
                if (phdr[i].p_offset >= image_size ||
                    phdr[i].p_filesz > image_size - phdr[i].p_offset) {
                        continue;
                }
                dynamic = reinterpret_cast<const Elf64_Dyn*>(image->Data() + phdr[i].p_offset);
                dynamic_count = static_cast<size_t>(phdr[i].p_filesz / sizeof(Elf64_Dyn));
                break;
        }
        if (dynamic == nullptr) {
                return 0;  // static image: no relocations
        }

        uint64_t rela_vaddr = 0, rela_size = 0;
        uint64_t rela_ent = sizeof(Elf64_Rela);
        uint64_t jmprel_vaddr = 0, pltrel_size = 0, pltrel = 0;
        uint64_t symtab_vaddr = 0, strtab_vaddr = 0, hash_vaddr = 0;
        for (size_t i = 0; i < dynamic_count && dynamic[i].d_tag != DT_NULL; ++i) {
                switch (dynamic[i].d_tag) {
                case DT_RELA:    rela_vaddr = dynamic[i].d_ptr; break;
                case DT_RELASZ:  rela_size = dynamic[i].d_val; break;
                case DT_RELAENT: rela_ent = dynamic[i].d_val; break;
                case DT_JMPREL:  jmprel_vaddr = dynamic[i].d_ptr; break;
                case DT_PLTRELSZ: pltrel_size = dynamic[i].d_val; break;
                case DT_PLTREL:  pltrel = dynamic[i].d_val; break;
                case DT_SYMTAB:  symtab_vaddr = dynamic[i].d_ptr; break;
                case DT_STRTAB:  strtab_vaddr = dynamic[i].d_ptr; break;
                case DT_HASH:    hash_vaddr = dynamic[i].d_ptr; break;
                default: break;
                }
        }
        if (rela_ent == 0) {
                rela_ent = sizeof(Elf64_Rela);
        }

        // Raw .dynsym view (undefined imports included) for symbol-resolved
        // relocations, plus the string table for the import names.
        const uint8_t* syms = nullptr;
        size_t sym_count = 0;
        LocateDynsym(*image, symtab_vaddr, strtab_vaddr, hash_vaddr, syms, sym_count);
        const char* strtab = nullptr;
        size_t strtab_size = 0;
        LocateDynstr(*image, strtab_vaddr, strtab, strtab_size);

        // Apply one relocation: resolve the value, translate the target
        // vaddr to a file offset, and patch the owned image in place.
        // `imported` is set when an undefined symbol was resolved against
        // ANOTHER loaded module's export table (round 10).
        const auto apply_relocation = [&](const Elf64_Rela& rela) {
                const uint32_t type = ELF64_R_TYPE(rela.r_info);
                const uint32_t sym_index = ELF64_R_SYM(rela.r_info);

                uint64_t value = 0;
                bool write = false;
                bool imported = false;
                if (type == R_X86_64_RELATIVE) {
                        // B + A
                        value = base + static_cast<uint64_t>(rela.r_addend);
                        write = true;
                } else if (type == R_X86_64_64 || type == R_X86_64_GLOB_DAT ||
                           type == R_X86_64_JUMP_SLOT) {
                        // S + A. Defined symbols resolve against the module's
                        // own .dynsym; undefined (imported) symbols are
                        // resolved against the OTHER loaded modules' export
                        // tables in LOAD ORDER (POSIX RTLD_GLOBAL-style search,
                        // round 10) -- the emulator's stand-in for the
                        // dynlib dependency graph. Unresolved imports stay
                        // skipped and counted, never faked.
                        if (syms == nullptr || sym_index >= sym_count) {
                                ++program->relocs_skipped;
                                return;
                        }
                        const Elf64_Sym& symbol =
                                reinterpret_cast<const Elf64_Sym*>(syms)[sym_index];
                        if (symbol.st_shndx != SHN_UNDEF) {
                                value = base + symbol.st_value +
                                        static_cast<uint64_t>(rela.r_addend);
                                write = true;
                        } else {
                                // Imported symbol: read its name, then search
                                // every OTHER loaded module's exports.
                                const char* name = nullptr;
                                if (strtab != nullptr &&
                                    symbol.st_name < strtab_size) {
                                        name = strtab + symbol.st_name;
                                }
                                if (name == nullptr) {
                                        ++program->relocs_skipped;
                                        return;
                                }
                                const size_t remaining =
                                        strtab_size - static_cast<size_t>(symbol.st_name);
                                const std::string symbol_name(
                                        name, strnlen(name, remaining));
                                if (symbol_name.empty()) {
                                        ++program->relocs_skipped;
                                        return;
                                }
                                const ModuleInfo* provider = nullptr;
                                uint64_t provider_value = 0;
                                // Round 14: resolve against the declared
                                // dependency order first (DT_NEEDED direct,
                                // then transitive), then plain load order.
                                std::vector<const ModuleInfo*> order;
                                CollectResolutionOrder(program, order);
                                for (const ModuleInfo* m : order) {
                                        if (m == nullptr || m == program ||
                                            m->export_symbols == nullptr) {
                                                continue;
                                        }
                                        const auto& names = m->export_symbols->names;
                                        for (size_t i = 0; i < names.size(); ++i) {
                                                if (names[i] == symbol_name) {
                                                        provider = m;
                                                        provider_value =
                                                                m->export_symbols->values[i];
                                                        break;
                                                }
                                        }
                                        if (provider != nullptr) {
                                                break;
                                        }
                                }
                                if (provider == nullptr &&
                                    symbol_name.find('#') != std::string::npos) {
                                        // Round 30: PS5 eboots carry versioned
                                        // import names ("bzQExy189ZI#D#E"); the
                                        // HLE database registers the bare NID.
                                        const std::string bare =
                                                symbol_name.substr(0, symbol_name.find('#'));
                                        const ModuleInfo* bare_provider = nullptr;
                                        uint64_t bare_value = 0;
                                        for (const ModuleInfo* m : order) {
                                                if (m == nullptr || m == program ||
                                                    m->export_symbols == nullptr) {
                                                        continue;
                                                }
                                                const auto& names = m->export_symbols->names;
                                                for (size_t i = 0; i < names.size(); ++i) {
                                                        if (names[i] == bare) {
                                                                bare_provider = m;
                                                                bare_value = m->export_symbols->values[i];
                                                                break;
                                                        }
                                                }
                                                if (bare_provider != nullptr) {
                                                        break;
                                                }
                                        }
                                        if (bare_provider != nullptr) {
                                                provider = bare_provider;
                                                provider_value = bare_value;
                                        }
                                }
                                if (provider == nullptr) {
                                        // Round 30: HLE dynamic linking. Real
                                        // PS5 eboots import their libraries
                                        // through PLT/GOT slots; with no
                                        // provider module loaded the slots
                                        // keep link-time resolver addresses
                                        // and the first `call [got]` faults
                                        // (observed on the real 254 MB
                                        // Minecraft image). Resolve the import
                                        // against the HLE SymbolDatabase and
                                        // point the slot at a guest trampoline
                                        // stub: `mov eax,id; syscall; ret` —
                                        // both engines route it to the host
                                        // function with the guest's own ABI
                                        // registers.
                                        const void* hle =
                                                ::Loader::SymbolDatabase::Instance()
                                                    .FindSymbol(symbol_name.c_str());
                                        const void* hle_bare = nullptr;
                                        if (hle == nullptr &&
                                            symbol_name.find('#') != std::string::npos) {
                                                hle_bare = ::Loader::SymbolDatabase::Instance()
                                                    .FindSymbol(symbol_name
                                                        .substr(0, symbol_name.find('#'))
                                                        .c_str());
                                        }
                                        const void* const resolved =
                                                hle != nullptr ? hle : hle_bare;
                                        const uint64_t stub =
                                                resolved != nullptr
                                                        ? PS5::CPU::HleTrampolines::
                                                                  Instance().StubFor(
                                                                      resolved,
                                                                      symbol_name.c_str())
                                                        : 0;
                                        if (stub != 0) {
                                                value = stub;
                                                write = true;
                                                imported = true;
                                        } else {
                                                ++program->relocs_skipped;
                                                return;
                                        }
                                } else {
                                        value = provider->base_addr + provider_value +
                                                static_cast<uint64_t>(rela.r_addend);
                                        write = true;
                                }
                                imported = true;
                        }
                } else {
                        // Unsupported type: recorded, never silently faked.
                        ++program->relocs_skipped;
                        return;
                }

                uint64_t target_offset = 0;
                if (!write ||
                    !VaddrToFileOffset(ehdr, phdr, rela.r_offset, target_offset) ||
                    target_offset + sizeof(uint64_t) > image_size) {
                        ++program->relocs_skipped;
                        return;
                }
                std::memcpy(image->MutableData() + target_offset, &value, sizeof(value));
                ++program->relocs_applied;
                if (imported) {
                        ++program->relocs_imported;
                }
        };

        const auto apply_range = [&](uint64_t table_vaddr, uint64_t table_bytes) {
                if (table_vaddr == 0 || table_bytes == 0) {
                        return;
                }
                uint64_t table_offset = 0;
                if (!VaddrToFileOffset(ehdr, phdr, table_vaddr, table_offset)) {
                        return;
                }
                for (uint64_t off = 0; off + sizeof(Elf64_Rela) <= table_bytes;
                     off += rela_ent) {
                        if (table_offset + off + sizeof(Elf64_Rela) > image_size) {
                                break;
                        }
                        const auto* rela = reinterpret_cast<const Elf64_Rela*>(
                                image->Data() + table_offset + off);
                        apply_relocation(*rela);
                }
        };

        apply_range(rela_vaddr, rela_size);
        // JMPREL entries are Elf64_Rela only when DT_PLTREL says so (x86-64
        // uses RELA; a REL PLT is skipped and counted as unsupported).
        if (pltrel == DT_RELA) {
                apply_range(jmprel_vaddr, pltrel_size);
        } else if (jmprel_vaddr != 0) {
                // Count the skipped REL PLT entries honestly.
                for (uint64_t off = 0; off + sizeof(Elf64_Rela) <= pltrel_size;
                     off += sizeof(Elf64_Rela)) {
                        ++program->relocs_skipped;
                }
        }
        return 0;
}

void* RuntimeLinker::GetProcParam() {
        // The process-parameter struct is owned by the entry-point layer;
        // return a stable non-null placeholder for host-side callers.
        static int proc_param = 0;
        return &proc_param;
}

ModuleInfo* RuntimeLinker::FindProgramByFileName(const std::string& name) {
        for (auto& m : m_modules) {
                if (!m.is_unloaded && (m.name == name || m.file_name.string() == name)) {
                        return &m;
                }
        }
        return nullptr;
}

void RuntimeLinker::SetApplicationHeapApi(void* (*alloc)(size_t), void (*free_fn)(void*),
                                          void* (*memalign)(size_t, size_t)) {
        (void)alloc;
        (void)free_fn;
        (void)memalign;
}

int RuntimeLinker::UnloadProgram(ModuleInfo* program) {
        if (program == nullptr) {
                return 0;
        }
        auto it = std::find_if(m_modules.begin(), m_modules.end(),
                               [program](const ModuleInfo& m) { return &m == program; });
        if (it != m_modules.end()) {
                // Round 33: mark-as-unloaded (see UnloadModule comment).
                it->is_unloaded = true;
        }
        return 0;
}

void* RuntimeLinker::TlsGetAddr(ModuleInfo* program) {
        // TLS blocks are allocated per thread by the ELF loader; expose the
        // program's TLS base (0 until a loader populates it).
        return program != nullptr ? program->tls_base : nullptr;
}

int RuntimeLinker::LoadStartModule(const std::string& path, size_t /*args*/, const void* /*argp*/,
                                   int /*flags*/, uint64_t* out_handle, int* out_result) {
        const int result = LoadModule(path, out_handle);
        if (out_result != nullptr) {
                *out_result = result;
        }
        return result;
}

int RuntimeLinker::StopUnloadModule(uint64_t handle, size_t /*args*/, const void* /*argp*/,
                                    int /*flags*/, int* out_result) {
        UnloadModule(handle);
        if (out_result != nullptr) {
                *out_result = 0;
        }
        return 0;
}

// ---------------------------------------------------------------------------
// Symbol resolution
// ---------------------------------------------------------------------------
void* RuntimeLinker::FindSymbol(const std::string& module_name, const std::string& symbol_name) {
        ModuleInfo* module = FindModule(module_name);
        if (module == nullptr || module->export_symbols == nullptr) {
                return nullptr;
        }
        const ModuleInfo::ExportSymbols& exports = *module->export_symbols;
        for (size_t i = 0; i < exports.names.size(); ++i) {
                if (exports.names[i] == symbol_name) {
                        // Load bias + link-time value (base_addr is 0 until
                        // the loader maps the image).
                        return reinterpret_cast<void*>(module->base_addr + exports.values[i]);
                }
        }
        return nullptr;
}

int RuntimeLinker::Dlsym(uint64_t handle, const char* name, void** out_addr) {
        if (out_addr != nullptr) {
                *out_addr = nullptr;
        }
        if (name == nullptr) {
                return -1;
        }
        ModuleInfo* program = FindProgramById(handle);
        if (program == nullptr || program->export_symbols == nullptr) {
                return -1;
        }
        const ModuleInfo::ExportSymbols& exports = *program->export_symbols;
        for (size_t i = 0; i < exports.names.size(); ++i) {
                if (exports.names[i] == name) {
                        if (out_addr != nullptr) {
                                *out_addr = reinterpret_cast<void*>(program->base_addr +
                                                                    exports.values[i]);
                        }
                        return 0;
                }
        }
        return -1;
}

bool RuntimeLinker::FindProgramByAddr(uint64_t addr, ModuleInfoForUnwind* out) {
        for (const auto& m : m_modules) {
                if (!m.is_unloaded && m.base_addr != 0 && addr >= m.base_addr && addr < m.base_addr + m.size) {
                        if (out != nullptr) {
                                out->base_addr   = m.base_addr;
                                out->size        = m.size;
                                out->entry_point = m.entry_point;
                                out->name        = m.name;
                                out->file_name   = m.name;
                        }
                        return true;
                }
        }
        return false;
}

ModuleInfo* RuntimeLinker::FindProgramByAddr(uint64_t addr) {
        for (auto& m : m_modules) {
                if (!m.is_unloaded && m.base_addr != 0 && addr >= m.base_addr && addr < m.base_addr + m.size) {
                        return &m;
                }
        }
        return nullptr;
}

ModuleInfo* RuntimeLinker::FindProgramById(uint64_t handle) {
        for (auto& m : m_modules) {
                if (!m.is_unloaded && (m.handle == static_cast<int>(handle) || m.unique_id == handle)) {
                        return &m;
                }
        }
        return nullptr;
}

int RuntimeLinker::GetModuleInfo(uint64_t handle, ModuleInfoForUnwind* info) {
        const auto it = std::find_if(m_modules.begin(), m_modules.end(),
                                     [handle](const ModuleInfo& m) { return !m.is_unloaded && m.handle == static_cast<int>(handle); });
        if (it == m_modules.end()) {
                return -1;
        }
        if (info != nullptr) {
                info->base_addr   = it->base_addr;
                info->size        = it->size;
                info->entry_point = it->entry_point;
                info->name        = it->name;
                info->file_name   = it->name;
        }
        return 0;
}

int RuntimeLinker::GetModuleInfoFromAddr(uint64_t addr, ModuleInfoForUnwind* info) {
        return FindProgramByAddr(addr, info) ? 0 : -1;
}

void RuntimeLinker::StackTrace(uint64_t /*rbp*/) {
        // Not implemented: guest unwinding requires DWARF support.
}

void RuntimeLinker::SetAtexitCount(int count) {
        m_atexit_count = count;
}

int RuntimeLinker::GetAtexitCount() const {
        return m_atexit_count;
}

void RuntimeLinker::RegisterModule(ModuleInfo info) {
        // Externally registered modules get a monotonic handle when the
        // caller did not assign one, so every module is addressable through
        // FindProgramById / KernelDlsym.
        if (info.handle == 0) {
                info.handle = m_next_module_id++;
                info.unique_id = static_cast<uint64_t>(info.handle);
        }
        m_modules.push_back(std::move(info));
}

void RuntimeLinker::UnregisterModule(int handle) {
        auto it = std::find_if(m_modules.begin(), m_modules.end(),
                               [handle](const ModuleInfo& m) { return m.handle == handle; });
        if (it != m_modules.end()) {
                // Round 33: mark-as-unloaded (see UnloadModule comment).
                it->is_unloaded = true;
        }
}

ModuleInfo* RuntimeLinker::FindModule(const std::string& name) {
        const auto it = std::find_if(m_modules.begin(), m_modules.end(),
                                     [&name](const ModuleInfo& m) { return !m.is_unloaded && m.name == name; });
        return it != m_modules.end() ? &(*it) : nullptr;
}

// ---------------------------------------------------------------------------
// Image access
// ---------------------------------------------------------------------------
uint64_t RuntimeLinker::ReadFromElf(const ModuleInfo* module, uint64_t vaddr) {
        if (module == nullptr || module->elf == nullptr) {
                return 0;
        }
        const ElfImage& image = *module->elf;
        if (!image.IsValid()) {
                return 0;
        }
        // vaddr is a full guest address: strip the load bias to get the
        // link-time address, then translate to a file offset.
        const uint64_t link_vaddr = vaddr - module->base_addr;
        uint64_t offset = 0;
        if (!VaddrToFileOffset(image.GetEhdr(), image.GetPhdr(), link_vaddr, offset)) {
                return 0;
        }
        if (offset + sizeof(uint64_t) > image.Size()) {
                return 0;
        }
        uint64_t value = 0;
        std::memcpy(&value, image.Data() + offset, sizeof(value));
        return value;
}

void* RuntimeLinker::ApplicationHeapMemalign(uint64_t alignment, uint64_t size) {
        if (alignment < sizeof(void*)) {
                alignment = sizeof(void*);
        }
        size = (size + alignment - 1) & ~(alignment - 1);
        return std::aligned_alloc(static_cast<size_t>(alignment), static_cast<size_t>(size));
}

void RuntimeLinker::ApplicationHeapFree(void* ptr) {
        std::free(ptr);
}

int RuntimeLinker::StartModule(ModuleInfo* program, size_t args, const void* argp, int flags) {
        (void)program;
        (void)args;
        (void)argp;
        (void)flags;
        return 0;
}

int RuntimeLinker::StopModule(ModuleInfo* program, size_t args, const void* argp, int flags) {
        (void)program;
        (void)args;
        (void)argp;
        (void)flags;
        return 0;
}

int RuntimeLinker::StartModule(ModuleInfo* program, size_t args, const void* argp, int* out_result) {
        const int rc = StartModule(program, args, argp, 0);
        if (out_result != nullptr) {
                *out_result = rc;
        }
        return rc;
}

// ---------------------------------------------------------------------------
// Round 11: init/fini function tables (ELF execution order)
// ---------------------------------------------------------------------------
bool RuntimeLinker::GetInitFunctions(const ModuleInfo* program, std::vector<uint64_t>& out) const {
        out.clear();
        if (program == nullptr || program->elf == nullptr || !program->elf->IsValid()) {
                return false;
        }
        const ElfImage* const image = program->elf.get();
        const uint64_t base = program->base_addr;

        // DT_INIT runs first (ELF spec), then DT_INIT_ARRAY entries in order.
        if (program->init_fini.init_vaddr != 0) {
                out.push_back(base + program->init_fini.init_vaddr);
        }
        const uint64_t count = program->init_fini.init_array_count;
        const uint64_t table_vaddr = program->init_fini.init_array_vaddr;
        if (count != 0 && table_vaddr != 0) {
                uint64_t table_off = 0;
                if (!VaddrToFileOffset(image->GetEhdr(), image->GetPhdr(),
                                       table_vaddr, table_off)) {
                        return false;
                }
                // Bound the table read by the image size so a corrupt
                // DT_INIT_ARRAYSZ cannot walk off the buffer.
                const size_t available = (table_off < image->Size())
                        ? static_cast<size_t>(image->Size() - table_off) : 0;
                const uint64_t entries = std::min<uint64_t>(count, available / 8);
                for (uint64_t i = 0; i < entries; ++i) {
                        uint64_t fn = 0;
                        std::memcpy(&fn, image->Data() + table_off + i * 8, 8);
                        if (fn != 0) {
                                out.push_back(base + fn);
                        }
                }
        }
        return true;
}

// ---------------------------------------------------------------------------
// Round 14: dependency graph (DT_NEEDED)
// ---------------------------------------------------------------------------
const ModuleInfo* RuntimeLinker::FindModuleBySoNameImpl(const std::string& name) const {
        if (name.empty()) {
                return nullptr;
        }
        for (const ModuleInfo& m : m_modules) {
                if (!m.is_unloaded && m.so_name == name) {
                        return &m;
                }
                // Fall back to the file name (base name) when the module has
                // no DT_SONAME -- LoadProgram sets name to the file's base.
                if (!m.is_unloaded && (m.name == name ||
                    m.file_name.filename().string() == name)) {
                        return &m;
                }
        }
        return nullptr;
}

ModuleInfo* RuntimeLinker::FindModuleBySoName(const std::string& name) {
        return const_cast<ModuleInfo*>(FindModuleBySoNameImpl(name));
}

const ModuleInfo* RuntimeLinker::FindModuleBySoName(const std::string& name) const {
        return FindModuleBySoNameImpl(name);
}

void RuntimeLinker::CollectResolutionOrder(const ModuleInfo* program,
                                           std::vector<const ModuleInfo*>& order) const {
        order.clear();
        if (program == nullptr) {
                return;
        }
        std::vector<const ModuleInfo*> visited;   // small graphs: linear scan
        const auto is_visited = [&](const ModuleInfo* m) {
                return std::find(visited.begin(), visited.end(), m) != visited.end();
        };
        const auto push = [&](const ModuleInfo* m) {
                if (m != nullptr && m != program && !is_visited(m)) {
                        visited.push_back(m);
                        order.push_back(m);
                }
        };

        // Depth-first over the DECLARED dependencies (direct first).
        std::function<void(const ModuleInfo*, int)> dfs =
                [&](const ModuleInfo* mod, int depth) {
                        if (mod == nullptr || depth > 16) {
                                return;   // cycle / runaway guard
                        }
                        for (const std::string& needed : mod->needed_libraries) {
                                const ModuleInfo* dep = FindModuleBySoName(needed);
                                push(dep);
                                dfs(dep, depth + 1);
                        }
                };
        dfs(program, 0);

        // Then the remaining loaded modules in load order (round-10 fallback).
        for (const ModuleInfo& m : m_modules) {
                if (!m.is_unloaded) {
                        push(&m);
                }
        }
}

bool RuntimeLinker::GetFiniFunctions(const ModuleInfo* program, std::vector<uint64_t>& out) const {
        out.clear();
        if (program == nullptr || program->elf == nullptr || !program->elf->IsValid()) {
                return false;
        }
        const ElfImage* const image = program->elf.get();
        const uint64_t base = program->base_addr;

        // DT_FINI_ARRAY entries run in REVERSE order, then DT_FINI last.
        const uint64_t count = program->init_fini.fini_array_count;
        const uint64_t table_vaddr = program->init_fini.fini_array_vaddr;
        if (count != 0 && table_vaddr != 0) {
                uint64_t table_off = 0;
                if (!VaddrToFileOffset(image->GetEhdr(), image->GetPhdr(),
                                       table_vaddr, table_off)) {
                        return false;
                }
                const size_t available = (table_off < image->Size())
                        ? static_cast<size_t>(image->Size() - table_off) : 0;
                const uint64_t entries = std::min<uint64_t>(count, available / 8);
                for (uint64_t i = entries; i > 0; --i) {
                        uint64_t fn = 0;
                        std::memcpy(&fn, image->Data() + table_off + (i - 1) * 8, 8);
                        if (fn != 0) {
                                out.push_back(base + fn);
                        }
                }
        }
        if (program->init_fini.fini_vaddr != 0) {
                out.push_back(base + program->init_fini.fini_vaddr);
        }
        return true;
}

} // namespace Loader
