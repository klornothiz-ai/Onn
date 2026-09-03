#pragma once
// ============================================================================
// ProsperoLayer RDNA2 Core - Guest program launcher (round 11)
// ----------------------------------------------------------------------------
// The end-to-end boot orchestrator a shadPS4-style flow needs:
//
//   ELF file
//     -> RuntimeLinker::LoadProgram        (own image, parse exports/TLS/init)
//     -> RuntimeLinker::RelocateProgram    (apply relocations in the image)
//     -> GuestLauncher                     (map the PATCHED segments into VMM,
//                                           allocate the main-thread TLS block,
//                                           run DT_INIT + DT_INIT_ARRAY,
//                                           jump to the ELF entry point)
//     -> exit code (RAX at the entry `ret`)
//
// TLS model (initial-exec): each thread's block is laid out as
//     [0..8)   self pointer (fs:[0] reads back the block GVA)
//     [8..)    the module's PT_TLS initialization image, zero-filled to p_memsz
// The block GVA is what ExecuteGuestFull loads into FS.base.
//
// Limitations (documented): ET_EXEC images with fixed vaddrs only (ET_DYN load
// bias is not applied yet); init/fini run on the caller's thread.
// ============================================================================

#include "loader/runtimeLinker.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Loader {

struct GuestBootResult {
    bool ok{false};
    std::string error;
    uint64_t exit_code{0};
    uint64_t entry_gva{0};
    uint64_t tls_base_gva{0};
    size_t init_functions_run{0};
    size_t fini_functions_run{0};
    // Segment mapping statistics.
    size_t segments_mapped{0};
    // Round 30: dynamic linking facts (how many relocations applied,
    // imports resolved to HLE trampolines, and honestly skipped ones).
    size_t relocs_applied{0};
    size_t relocs_imported{0};
    size_t relocs_skipped{0};
};

class GuestLauncher {
public:
    // Full boot flow for an already-decrypted x86-64 ET_EXEC image. The
    // program is registered in RuntimeLinker, its relocations are applied to
    // the owned image, the PATCHED PT_LOADs are mapped into the guest VMM,
    // the main-thread TLS block is allocated, DT_INIT/DT_INIT_ARRAY run in
    // ELF order, and finally the ELF entry point executes (rdi=0, rsi=0).
    GuestBootResult Boot(const std::string& elf_path, size_t instruction_limit = 1000000);

    // Allocates a per-thread TLS block for the module's PT_TLS template.
    // Returns the GVA to load into FS.base (0 on failure).
    uint64_t AllocateThreadTls(const ModuleInfo& program);

    // Runs one guest function (SysV: rdi=arg0, rsi=arg1) on the current
    // thread with the given FS base. Returns RAX at the entry `ret`
    // (0 on fault/limit).
    static uint64_t RunGuestFunction(uint64_t entry_gva, uint64_t arg0, uint64_t arg1,
                                     uint64_t fs_base, size_t instruction_limit = 1000000);

    // Runs the module's fini functions in ELF order (DT_FINI_ARRAY reverse,
    // then DT_FINI) exactly once. Returns how many ran.
    size_t Shutdown(ModuleInfo* program, uint64_t fs_base, size_t instruction_limit = 1000000);
};

} // namespace Loader
