// prospero_boot.hpp — the REAL game boot path, shared by `prospero-run`
// (the shipped entry point) and the integrated engine self-test.
//
// This module exists because, before round 29, `prospero-run` stopped after
// reading the ELF header and printed "CPU/ABI/GPU execution is not enabled"
// while the engine it was linked against already had every piece needed to
// actually boot code: the VMM arena, the runtime linker (ELF load, DT_NEEDED
// resolution, relocations), the HLE symbol database (1600+ registered NIDs),
// the syscall dispatcher and both execution engines (native DirectExecution
// with interpreter fallback).
//
// The flow below is end-to-end and honest:
//   1. locate the executable (GameFolderScanner for a directory, or a direct
//      ELF file path),
//   2. validate/parse it with the real ElfLoader (encrypted or malformed
//      images fail CLOSED with a precise reason),
//   3. register the whole HLE surface via Libs::InitAll,
//   4. boot through the real GuestLauncher: segment mapping, relocations,
//      TLS allocation, DT_INIT/INIT_ARRAY, then the ELF entry point runs on
//      the CPUJitEngine (DirectExecutionBackend first when enabled, the
//      interpreter as the fail-closed fallback) with LIVE guest syscalls,
//   5. report every observable fact (exit code, syscall count, engine used,
//      direct-execution decline reason when applicable).
//
// Nothing in this path is faked: if the guest faults, the report says so.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace PS5::Boot {

struct BootOptions {
    // Instruction budget for the entry point execution. Generous default:
    // real games run far longer, the limit only bounds a runaway guest.
    size_t instruction_limit = 500'000'000;
    // Prefer the native DirectExecutionBackend (round 20). When false, or
    // when the backend declines, execution falls back to the interpreter —
    // same guest-visible contract either way.
    bool prefer_direct_execution = true;
};

struct BootReport {
    // Stage progression (all false => nothing ran).
    bool scanned{false};       // game folder / ELF located
    bool loaded{false};        // ElfLoader parsed it (segments + entry)
    bool hle_registered{false};// HLE NIDs registered in SymbolDatabase
    bool booted{false};        // GuestLauncher::Boot succeeded
    bool executed{false};      // entry point actually ran to completion

    std::string error;         // first failure reason (empty on success)

    // ELF / folder facts.
    std::string game_root;
    std::string executable_path;
    uint64_t entry_gva{0};
    size_t segments{0};
    size_t resources{0};

    // Boot facts (from GuestBootResult).
    uint64_t exit_code{0};
    size_t init_functions_run{0};
    size_t fini_functions_run{0};
    size_t segments_mapped{0};
    uint64_t tls_base_gva{0};

    // Execution facts.
    uint64_t syscalls_intercepted{0};
    std::string execution_engine;   // "native-direct" | "interpreter"
    std::string direct_outcome;     // decline reason when native was tried

    // HLE facts.
    size_t hle_symbols_registered{0};

    // Round 30: Prospero SELF container facts (real PS5 eboot.bin).
    bool self_parsed{false};         // container header+entries+ELF validated
    bool self_plaintext{false};      // data segments are live code, not ciphertext
    size_t self_entries{0};          // segment entries in the container
    size_t self_data_segments{0};    // PT_LOAD-backed data segments
    size_t self_pairings{0};         // PT_LOAD <-> data-segment pairs
    size_t self_needed_libraries{0}; // DT_NEEDED imports resolved from the image
    size_t relocs_applied{0};        // round 30: dynamic linking facts
    size_t relocs_imported{0};       // imports resolved to HLE trampolines
    size_t relocs_skipped{0};        // honestly skipped relocations
    std::string self_module_path;    // PATHX build path (devkit provenance)
    std::string self_embedded_type;  // e.g. "ET_SCE_DYNEXEC"
    std::string flattened_elf_path;  // the bootable ELF emitted from the SELF
};

// Boots the game at `path` (a game directory or a direct ELF file).
BootReport RunGame(const std::string& path, const BootOptions& options = {});

// Human-readable one-line summary (used by prospero-run and the self-test).
std::string FormatSummary(const BootReport& r);

} // namespace PS5::Boot
