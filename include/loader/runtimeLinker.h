#pragma once
// ProsperoLayer PS5 emulator - runtime linker interface (Kyty-compatible)
#include "common/common.h"
#include "common/stringUtils.h"
#include "loader/elf.h"
#include "loader/symbolDatabase.h"
#include <cstdint>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace Loader {

// Minimal ELF image view used by KernelGetModuleInfo / unwind helpers.
class ElfImage {
public:
        ElfImage() = default;
        ~ElfImage() = default;

        const Elf64_Ehdr* GetEhdr() const { return m_ehdr; }
        const Elf64_Phdr* GetPhdr() const { return m_phdr; }
        const Elf64_Shdr* GetShdr() const { return m_shdr; }
        uint32_t GetPhnum() const { return m_phnum; }
        uint32_t GetShnum() const { return m_shnum; }
        const char* GetSectionName(uint32_t index) const {
                if (index < m_section_names.size()) {
                        return m_section_names[index].c_str();
                }
                return "";
        }

        // Raw image access.
        const uint8_t* Data() const { return m_data; }
        size_t Size() const { return m_size; }
        // Writable view of the OWNED bytes (used by RelocateProgram to patch
        // the image in place). Only valid after a successful LoadFromMemory.
        uint8_t* MutableData() { return m_owned.data(); }
        void SetRaw(const uint8_t* data, size_t size) { m_data = data; m_size = size; }
        void SetHeaders(const Elf64_Ehdr* ehdr, const Elf64_Phdr* phdr, const Elf64_Shdr* shdr,
                        uint32_t phnum, uint32_t shnum) {
                m_ehdr = ehdr; m_phdr = phdr; m_shdr = shdr; m_phnum = phnum; m_shnum = shnum;
        }
        void SetSectionNames(std::vector<std::string> names) { m_section_names = std::move(names); }

        // Round 9: own the image bytes. Validates the ELF64 x86-64 header,
        // copies the buffer, and points the header views into the owned copy
        // so relocations can patch the image in place. Returns false (with
        // the object left untouched) for a malformed image.
        bool LoadFromMemory(const uint8_t* data, size_t size);

        bool IsValid() const { return m_data != nullptr && m_ehdr != nullptr && m_size != 0; }

private:
        const Elf64_Ehdr*      m_ehdr{nullptr};
        const Elf64_Phdr*      m_phdr{nullptr};
        const Elf64_Shdr*      m_shdr{nullptr};
        const uint8_t*         m_data{nullptr};
        size_t                 m_size{0};
        uint32_t               m_phnum{0};
        uint32_t               m_shnum{0};
        std::vector<std::string> m_section_names;
        std::vector<uint8_t>   m_owned;
};

struct ModuleInfo {
        ModuleInfo() = default;
        ModuleInfo(ModuleInfo&&) noexcept = default;
        ModuleInfo& operator=(ModuleInfo&&) noexcept = default;
        ModuleInfo(const ModuleInfo&) = delete;
        ModuleInfo& operator=(const ModuleInfo&) = delete;

        std::string          name;
        std::filesystem::path file_name;
        uint64_t             base_addr{0};
        uint64_t             base_vaddr{0};
        uint64_t             size{0};
        uint64_t             base_size_aligned{0};
        uint64_t             entry_point{0};
        int                  handle{0};
        uint64_t             unique_id{0};
        bool                 is_loaded{false};
        bool                 is_unloaded{false};  // Round 33: set by unload/unregister
        bool                 dbg_print_reloc{false};

        // Export symbol table (guest .dynsym), used by KernelDlsym.
        struct ExportSymbols {
                std::vector<std::string> names;
                std::vector<uint64_t>    values;
                bool                     is_valid() const { return !names.empty(); }
        };
        std::unique_ptr<ExportSymbols> export_symbols;

        // TLS block base for the main thread (populated by the ELF loader).
        void* tls_base{nullptr};
        size_t tls_size{0};
        size_t tls_align{0};

        // Round 11: TLS template from PT_TLS. tls_vaddr/tls_filesz describe
        // the initialization image inside the ELF; the per-thread allocator
        // (GuestLauncher) copies it after an 8-byte self-pointer header.
        uint64_t tls_vaddr{0};
        size_t   tls_filesz{0};

        // Round 11: DT_INIT / DT_INIT_ARRAY / DT_FINI / DT_FINI_ARRAY parsed
        // from PT_DYNAMIC at load time; executed by the guest launcher.
        struct InitFiniInfo {
                uint64_t init_vaddr{0};         // DT_INIT (runs first)
                uint64_t init_array_vaddr{0};   // DT_INIT_ARRAY
                uint64_t init_array_count{0};   // entries (bytes/8)
                uint64_t fini_vaddr{0};         // DT_FINI (runs last)
                uint64_t fini_array_vaddr{0};   // DT_FINI_ARRAY
                uint64_t fini_array_count{0};

                bool has_init() const {
                        return init_vaddr != 0 || init_array_count != 0;
                }
                bool has_fini() const {
                        return fini_vaddr != 0 || fini_array_count != 0;
                }
        };
        // Round 11: init/fini tables parsed from PT_DYNAMIC (see InitFiniInfo).
        InitFiniInfo init_fini;
        // Guards so init/fini run exactly once per module (ELF spec).
        bool init_arrays_run{false};
        bool fini_arrays_run{false};

        // Round 14: declared dependency graph from PT_DYNAMIC. needed_libraries
        // holds the DT_NEEDED sonames in declaration order; so_name is this
        // module's own DT_SONAME. Relocation import resolution prefers these
        // declared dependencies (direct, then transitive) before falling back
        // to plain load order.
        std::vector<std::string> needed_libraries;
        std::string so_name;

        // Dynamic info (DT_SONAME etc.) exposed to KernelGetModuleInfo.
        struct DynamicInfo {
                const char* so_name{nullptr};
        };
        DynamicInfo* dynamic_info{nullptr};

        // ELF image access (used by KernelGetModuleInfo / unwind helpers).
        std::unique_ptr<ElfImage> elf;

        // Round 9 relocation statistics (populated by RelocateProgram):
        // applied counts every relocation written into the image (own-symbol
        // S+A, B+A and -- round 10 -- cross-module imports alike); imported is
        // the subset whose S came from ANOTHER loaded module's export table.
        size_t relocs_applied{0};
        size_t relocs_skipped{0};
        size_t relocs_imported{0};
};

struct ModuleInfoForUnwind {
        uint64_t    base_addr{0};
        uint64_t    size{0};
        uint64_t    entry_point{0};
        uint64_t    text_addr{0};
        uint64_t    text_size{0};
        std::string name;
        std::filesystem::path file_name;
};

class RuntimeLinker {
public:
        RuntimeLinker() = default;
        ~RuntimeLinker() = default;
        RuntimeLinker(const RuntimeLinker&) = delete;
        RuntimeLinker& operator=(const RuntimeLinker&) = delete;

        static RuntimeLinker& Instance();

        // Module management
        int LoadModule(const std::string& path, uint64_t* out_handle);
        int UnloadModule(uint64_t handle);
        int UnloadProgram(ModuleInfo* program);
        int LoadStartModule(const std::string& path, size_t args, const void* argp, int flags,
                            uint64_t* out_handle, int* out_result);
        int StopUnloadModule(uint64_t handle, size_t args, const void* argp, int flags,
                             int* out_result);
        int StartModule(uint64_t handle, size_t args, const void* argp, int flags,
                        int* out_result);
        int StopModule(uint64_t handle, size_t args, const void* argp, int flags,
                       int* out_result);
        // Kyty-compatible overloads that operate directly on a module.
        int StartModule(ModuleInfo* program, size_t args, const void* argp, int flags);
        int StopModule(ModuleInfo* program, size_t args, const void* argp, int flags);
        // Kyty-compatible: flags may be passed as an int* out-result pointer.
        int StartModule(ModuleInfo* program, size_t args, const void* argp, int* out_result);

        // Program lifecycle (used by libC / libKernel entry points).
        int LoadProgram(const std::string& path, size_t args, const void* argp);
        ModuleInfo* LoadProgram(const std::string& path);
        int RelocateProgram(ModuleInfo* program);
        void* GetProcParam();
        ModuleInfo* FindProgramByFileName(const std::string& name);

        // Application heap API (mspace-backed allocation hooks).
        using AppHeapFunc = void* (*)(size_t size);
        void SetApplicationHeapApi(void* (*alloc)(size_t), void (*free)(void*), void* (*memalign)(size_t, size_t));

        // Thread-local storage support.
        static void* TlsGetAddr(ModuleInfo* program);

        // Symbol resolution
        void* FindSymbol(const std::string& module_name, const std::string& symbol_name);
        int Dlsym(uint64_t handle, const char* name, void** out_addr);
        bool FindProgramByAddr(uint64_t addr, ModuleInfoForUnwind* out);

        // Returns the module whose [base_addr, base_addr+size) contains addr.
        ModuleInfo* FindProgramByAddr(uint64_t addr);
        // Returns the module with the given handle.
        ModuleInfo* FindProgramById(uint64_t handle);

        // Module info
        int GetModuleInfo(uint64_t handle, ModuleInfoForUnwind* info);
        int GetModuleInfoFromAddr(uint64_t addr, ModuleInfoForUnwind* info);

        // Misc
        void StackTrace(uint64_t rbp);
        void SetAtexitCount(int count);
        int  GetAtexitCount() const;

        // Module registration (used by the ELF loader)
        void RegisterModule(ModuleInfo info);
        void UnregisterModule(int handle);
        ModuleInfo* FindModule(const std::string& name);

        // Reads a 64-bit value from a module's loaded image at the given
        // guest virtual address (returns 0 when the address is unmapped).
        static uint64_t ReadFromElf(const ModuleInfo* module, uint64_t vaddr);

        // Round 11: collect the module's init functions in ELF execution order
        // (DT_INIT first, then DT_INIT_ARRAY entries) as absolute GVAs
        // (base_addr + image vaddr). Returns false for a module without an ELF
        // image. Does not execute anything.
        bool GetInitFunctions(const ModuleInfo* program, std::vector<uint64_t>& out) const;
        // Fini functions in ELF execution order (DT_FINI_ARRAY in REVERSE
        // order, then DT_FINI last).
        bool GetFiniFunctions(const ModuleInfo* program, std::vector<uint64_t>& out) const;

        // Round 14: dependency-ordered symbol resolution. Fills `order` with
        // the modules the program's imports should be resolved against:
        // direct DT_NEEDED dependencies (declared order) first, then their
        // transitive dependencies (DFS, cycle-safe), then every other loaded
        // module in load order (the round-10 behaviour as fallback).
        void CollectResolutionOrder(const ModuleInfo* program,
                                    std::vector<const ModuleInfo*>& order) const;

        // Round 14: find a loaded module by DT_SONAME / file name.
        ModuleInfo* FindModuleBySoName(const std::string& name);
        const ModuleInfo* FindModuleBySoName(const std::string& name) const;

        // Application-heap allocator used by KernelApplicationHeapGetMem.
        void* ApplicationHeapMemalign(uint64_t alignment, uint64_t size);
        void  ApplicationHeapFree(void* ptr);

private:
        // Round 11 fix: deque (not vector) so ModuleInfo pointers handed out by
        // FindProgramByFileName / LoadProgram stay valid when later loads append
        // modules -- a vector reallocation left every outstanding pointer
        // dangling (a real latent bug found by the guest-boot test).
        const ModuleInfo* FindModuleBySoNameImpl(const std::string& name) const;

        std::deque<ModuleInfo>  m_modules;
        int                     m_atexit_count{0};
        // Monotonic module-handle allocator: handles are never reused, so a
        // handle stays unique across load/unload cycles (round 9 fix; the old
        // size()+1 scheme handed out colliding handles after an unload).
        int                     m_next_module_id{1};
};

} // namespace Loader
