// ============================================================================
// ProsperoLayer RDNA2 Core - Guest program launcher (round 11)
// ============================================================================
#include "loader/guest_launcher.hpp"

#include "loader/prospero_self.hpp"

#include "cpu/jit_executor.hpp"
#include "cpu/guest_threads.hpp"
#include "memory/virtual_memory_manager.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace Loader {
namespace {

using PS5::Memory::PageProt;
using PS5::Memory::VirtualMemoryManager;

constexpr uint32_t kRwProt = static_cast<uint32_t>(PageProt::Read) |
                             static_cast<uint32_t>(PageProt::Write);

uint32_t FlagsToProt(uint32_t p_flags) {
    uint32_t prot = 0;
    if ((p_flags & 0x4) != 0) prot |= static_cast<uint32_t>(PageProt::Read);   // PF_R
    if ((p_flags & 0x2) != 0) prot |= static_cast<uint32_t>(PageProt::Write);  // PF_W
    if ((p_flags & 0x1) != 0) {                                               // PF_X
        // The interpreter fetches through the checked read path.
        prot |= static_cast<uint32_t>(PageProt::Read);
        prot |= static_cast<uint32_t>(PageProt::Exec);
    }
    if (prot == 0) prot = static_cast<uint32_t>(PageProt::Read);
    return prot;
}

} // namespace

uint64_t GuestLauncher::RunGuestFunction(uint64_t entry_gva, uint64_t arg0, uint64_t arg1,
                                         uint64_t fs_base, size_t instruction_limit) {
    return PS5::CPU::CPUJitEngine::Instance().ExecuteGuestFull(entry_gva, arg0, arg1,
                                                               instruction_limit, fs_base);
}

uint64_t GuestLauncher::AllocateThreadTls(const ModuleInfo& program) {
    auto& vmm = VirtualMemoryManager::Instance();

    const size_t image_bytes = program.tls_size;  // PT_TLS p_memsz
    // Layout: [self ptr (8)] [TLS image, zero-filled to p_memsz] [red zone pad]
    const size_t block_size = std::max<size_t>(0x1000, 8 + image_bytes);
    const uint64_t block = vmm.AllocateVirtual(0, block_size, kRwProt);
    if (block == 0) {
        return 0;
    }
    if (!vmm.ZeroGuest(block, block_size)) {
        return 0;
    }
    // fs:[0] reads back the block GVA (thread self-pointer).
    if (!vmm.CopyToGuest(block, &block, 8, kRwProt)) {
        return 0;
    }

    // Copy the PT_TLS initialization image after the self pointer.
    if (program.tls_filesz != 0 && program.elf != nullptr && program.elf->IsValid()) {
        const ElfImage* const image = program.elf.get();
        const Elf64_Phdr* const phdr = image->GetPhdr();
        for (uint32_t i = 0; i < image->GetPhnum(); ++i) {
            if (phdr[i].p_type != PT_TLS) {
                continue;
            }
            if (phdr[i].p_offset >= image->Size() ||
                phdr[i].p_filesz > image->Size() - phdr[i].p_offset) {
                break;
            }
            if (!vmm.CopyToGuest(block + 8, image->Data() + phdr[i].p_offset,
                                 static_cast<size_t>(phdr[i].p_filesz), kRwProt)) {
                return 0;
            }
            break;
        }
    }
    return block;
}

GuestBootResult GuestLauncher::Boot(const std::string& elf_path, size_t instruction_limit) {
    GuestBootResult result;

    // 1) Own + analyze the image through the runtime linker.
    ModuleInfo* const program = RuntimeLinker::Instance().LoadProgram(elf_path);
    if (program == nullptr) {
        result.error = "LoadProgram failed";
        return result;
    }
    if (program->elf == nullptr || !program->elf->IsValid()) {
        result.error = "not a valid ELF64 x86-64 image";
        return result;
    }
    const Elf64_Ehdr* const ehdr = program->elf->GetEhdr();
    const ElfImage* const image = program->elf.get();
    const Elf64_Phdr* const phdr = image->GetPhdr();
    auto& vmm = VirtualMemoryManager::Instance();

    // Round 18: ET_DYN (position-independent) images get a REAL load bias.
    // Round 30: Sony e_types (ET_SCE_DYNEXEC 0xFE10 / ET_SCE_EXEC 0xFE04 /
    // ET_SCE_DYNAMIC 0xFE18) behave as PIE exactly like ET_DYN — real PS5
    // eboots are ET_SCE_DYNEXEC with R_X86_64_RELATIVE tables sized for a
    // relocated base (the 254 MB Minecraft image carries ~877k of them).
    const bool pie_style = ehdr->e_type == ET_DYN ||
                           ehdr->e_type == ET_SCE_DYNEXEC ||
                           ehdr->e_type == ET_SCE_EXEC ||
                           ehdr->e_type == ET_SCE_DYNAMIC;
    if (pie_style) {
    // The loader picks a free region for the whole image span (probe-allocate
    // + free so the segment mappings can claim it piecemeal), records it in
    // ModuleInfo::base_addr -- so RelocateProgram applies every relocation
    // relative to the bias -- and the segment mapping + entry point below use
    // the same base. ET_EXEC keeps base_addr == 0 (link-time addresses).
    uint64_t lo = ~0ull;
        uint64_t hi = 0;
        bool any_load = false;
        for (uint32_t i = 0; i < image->GetPhnum(); ++i) {
            if (phdr[i].p_type != PT_LOAD || phdr[i].p_memsz == 0) {
                continue;
            }
            any_load = true;
            lo = std::min<uint64_t>(lo, phdr[i].p_vaddr);
            hi = std::max<uint64_t>(hi, phdr[i].p_vaddr + phdr[i].p_memsz);
        }
        if (!any_load) {
            result.error = "ET_DYN image has no PT_LOAD segments";
            return result;
        }
        const uint64_t span =
            ((hi - lo) + 0xFFFull) & ~0xFFFull;    // 4K-aligned span
        const uint64_t probe = vmm.AllocateVirtual(0, static_cast<size_t>(span),
                                                   kRwProt);
        if (probe == 0) {
            result.error = "ET_DYN load-bias allocation failed";
            return result;
        }
        if (!vmm.FreeVirtual(probe, static_cast<size_t>(span))) {
            result.error = "ET_DYN probe release failed";
            return result;
        }
        program->base_addr = probe;
    } else if (ehdr->e_type != ET_EXEC) {
        result.error = "unsupported ELF type (ET_EXEC / ET_DYN only)";
        return result;
    }

    // 2) Apply relocations to the owned image (so the bytes we map are final).
    if (RuntimeLinker::Instance().RelocateProgram(program) != 0) {
        result.error = "RelocateProgram failed";
        return result;
    }
    result.relocs_applied = program->relocs_applied;
    result.relocs_imported = program->relocs_imported;
    result.relocs_skipped = program->relocs_skipped;

    // 3) Map the PATCHED PT_LOADs into the guest VMM.
    std::vector<uint64_t> mapped_bases;
    std::vector<uint64_t> mapped_sizes;
    std::vector<uint64_t> exec_ranges_lo;
    std::vector<uint64_t> exec_ranges_hi;
    for (uint32_t i = 0; i < image->GetPhnum(); ++i) {
        if (phdr[i].p_type != PT_LOAD || phdr[i].p_memsz == 0) {
            continue;
        }
        // Round 30: debug-style PT_LOADs with zero permissions (Sony images
        // append the DWARF payload as an unpermitted load — the real
        // Minecraft eboot has one at an unaligned vaddr) carry symbols for
        // the linker but are never mapped into the guest VMM.
        if (phdr[i].p_flags == 0) {
            continue;
        }
        const uint64_t gva = program->base_addr + phdr[i].p_vaddr;
        const size_t memsz = static_cast<size_t>(phdr[i].p_memsz);
        const uint32_t final_prot = FlagsToProt(phdr[i].p_flags);
        const uint64_t base = vmm.AllocateVirtual(gva, memsz, kRwProt);
        if (base == 0) {
            result.error = "segment mapping collision at GVA 0x" +
                           std::to_string(gva);
            return result;
        }
        bool ok = true;
        if (phdr[i].p_filesz != 0) {
            ok = phdr[i].p_offset <= image->Size() &&
                 phdr[i].p_filesz <= image->Size() - phdr[i].p_offset &&
                 vmm.CopyToGuest(base, image->Data() + phdr[i].p_offset,
                                 static_cast<size_t>(phdr[i].p_filesz), kRwProt);
        }
        if (ok && phdr[i].p_memsz > phdr[i].p_filesz) {
            ok = vmm.ZeroGuest(base + phdr[i].p_filesz,
                               static_cast<size_t>(phdr[i].p_memsz - phdr[i].p_filesz));
        }
        if (ok) {
            ok = vmm.ProtectVirtual(base, memsz, final_prot);
        }
        if (!ok) {
            result.error = "segment copy/protect failed";
            vmm.FreeVirtual(base, memsz);
            return result;
        }
        mapped_bases.push_back(base);
        mapped_sizes.push_back(memsz);
        result.segments_mapped++;
        if ((final_prot & static_cast<uint32_t>(PageProt::Exec)) != 0) {
            exec_ranges_lo.push_back(base);
            exec_ranges_hi.push_back(base + memsz);
        }
    }
    if (mapped_bases.empty()) {
        result.error = "no PT_LOAD segments";
        return result;
    }

    // 4) Entry point must live in an executable segment.
    const uint64_t entry = program->base_addr + ehdr->e_entry;
    bool entry_ok = false;
    for (size_t i = 0; i < exec_ranges_lo.size(); ++i) {
        if (entry >= exec_ranges_lo[i] && entry < exec_ranges_hi[i]) {
            entry_ok = true;
            break;
        }
    }
    if (!entry_ok) {
        result.error = "entry point not inside an executable segment";
        return result;
    }
    result.entry_gva = entry;

    // 5) Main-thread TLS block (FS base).
    const uint64_t tls_base = AllocateThreadTls(*program);
    if (program->tls_size != 0 && tls_base == 0) {
        result.error = "TLS allocation failed";
        return result;
    }
    result.tls_base_gva = tls_base;

    // Round 15: guest threads spawned through thr_new allocate their own TLS
    // block from this module's template (per-thread copy, self-ptr at fs:[0]).
    // The pointer is stable: RuntimeLinker owns modules in a deque.
    {
        ModuleInfo* const tls_program = program;
        PS5::CPU::GuestThreadManager::Instance().SetTlsAllocator(
            [tls_program]() -> uint64_t {
                return GuestLauncher().AllocateThreadTls(*tls_program);
            });
    }

    // 6) Run DT_INIT + DT_INIT_ARRAY in ELF order, exactly once.
    std::vector<uint64_t> init_fns;
    if (RuntimeLinker::Instance().GetInitFunctions(program, init_fns)) {
        for (const uint64_t fn : init_fns) {
            RunGuestFunction(fn, 0, 0, tls_base, instruction_limit);
            ++result.init_functions_run;
        }
    }
    program->init_arrays_run = true;

    // 7) Jump to the ELF entry point (rdi = 0, rsi = 0).
    // Round 30: real PS5 eboots dereference the entry arguments immediately
    // (the Minecraft entry starts with `mov r14d,[rdi]; mov rbx,rsi; lea
    // r15,[rdi+8]` — the module-args block). A NULL rdi faults on the first
    // instruction, so the launcher passes a zeroed 4K args page: [rdi]=0
    // reads as "argc 0 / no auxv", rsi points at a NULL-terminated empty
    // argv. Synthetic ET_EXEC images that ignore rdi/rsi are unaffected.
    const uint64_t args_gva = vmm.AllocateVirtual(0, 0x1000, kRwProt);
    if (args_gva != 0) {
        vmm.ZeroGuest(args_gva, 0x1000);
    }
    const uint64_t arg0 = args_gva != 0 ? args_gva : 0;
    const uint64_t arg1 = args_gva != 0 ? args_gva + 0x800 : 0;
    result.exit_code = RunGuestFunction(entry, arg0, arg1, tls_base, instruction_limit);
    result.ok = true;
    return result;
}

size_t GuestLauncher::Shutdown(ModuleInfo* program, uint64_t fs_base,
                               size_t instruction_limit) {
    if (program == nullptr || program->fini_arrays_run) {
        return 0;
    }
    program->fini_arrays_run = true;
    std::vector<uint64_t> fini_fns;
    if (!RuntimeLinker::Instance().GetFiniFunctions(program, fini_fns)) {
        return 0;
    }
    size_t run = 0;
    for (const uint64_t fn : fini_fns) {
        RunGuestFunction(fn, 0, 0, fs_base, instruction_limit);
        ++run;
    }
    return run;
}

} // namespace Loader
