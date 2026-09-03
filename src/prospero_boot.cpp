// prospero_boot.cpp — implementation of the real game boot path.
// See include/prospero_boot.hpp for the contract and history.
#include "prospero_boot.hpp"

#include "cpu/direct_execution.hpp"
#include "cpu/jit_executor.hpp"
#include "libs/libs.h"
#include "loader/game_folder.hpp"
#include "loader/guest_launcher.hpp"
#include "loader/prospero_self.hpp"
#include "loader/runtimeLinker.h"
#include "loader/symbolDatabase.h"

#include <dlfcn.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace PS5::Boot {

namespace {

// HLE registration runs exactly once per process (the SymbolDatabase dedupes
// entries but its counter would grow on every re-registration, and the guest
// never needs the same NID twice).
std::once_flag g_hle_once;
size_t g_hle_symbols = 0;

void RegisterHleOnce() {
    std::call_once(g_hle_once, [] {
        Libs::InitAll(&::Loader::SymbolDatabase::Instance());
        g_hle_symbols = ::Loader::SymbolDatabase::Instance().GetCount();
    });
}

enum class HeaderKind {
    Unreadable,
    NotElf,
    Elf32,
    ElfWrongArch,
    Elf64X8664,
    SelfContainer,        // Sony PS4-family SELF ("\x00SCE") container
    SelfContainerProspero // PS5/Prospero SELF (magic 54 14 F5 EE)
};

HeaderKind ClassifyHeader(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return HeaderKind::Unreadable;
    }
    uint8_t head[64] = {};
    file.read(reinterpret_cast<char*>(head), sizeof(head));
    if (file.gcount() < 16) {
        return HeaderKind::Unreadable;
    }
    // Sony PS4-family SELF containers: magic 0x00454353 ("SCE\0" in little-endian order).
    if (head[0] == 0x00 && head[1] == 'S' && head[2] == 'C' && head[3] == 'E') {
        return HeaderKind::SelfContainer;
    }
    // Prospero (PS5) SELF containers: magic 54 14 F5 EE — the format the
    // real eboot.bin this round was built against uses.
    if (::Loader::HasProsperoSelfMagic(head)) {
        return HeaderKind::SelfContainerProspero;
    }
    if (head[0] != 0x7f || head[1] != 'E' || head[2] != 'L' || head[3] != 'F') {
        return HeaderKind::NotElf;
    }
    if (head[4] != 2) {
        return HeaderKind::Elf32;
    }
    // e_machine at offset 18 (little-endian).
    const uint16_t machine = static_cast<uint16_t>(head[18]) |
                             (static_cast<uint16_t>(head[19]) << 8);
    if (machine != 0x3e) {
        return HeaderKind::ElfWrongArch;
    }
    return HeaderKind::Elf64X8664;
}

} // namespace

BootReport RunGame(const std::string& path, const BootOptions& options) {
    BootReport report;

    // ------------------------------------------------------------------
    // 1) Locate the executable: a game directory is scanned the same way
    //    prospero-run always did; a direct file path is used as-is.
    // ------------------------------------------------------------------
    std::error_code ec;
    const std::filesystem::path input(path);
    std::filesystem::path executable;

    if (std::filesystem::is_directory(input, ec)) {
        Loader::GameFolder game;
        std::string scan_error;
        if (!Loader::GameFolderScanner::Scan(input, game, scan_error)) {
            report.error = scan_error;
            return report;
        }
        report.scanned = true;
        report.game_root = game.root.string();
        report.executable_path = game.executable.string();
        report.resources = game.resources.size();
        executable = game.executable;
    } else if (std::filesystem::is_regular_file(input, ec)) {
        report.scanned = true;
        report.game_root = input.parent_path().string();
        report.executable_path = input.string();
        executable = input;
    } else {
        report.error = "path is neither a game directory nor a file";
        return report;
    }

    // ------------------------------------------------------------------
    // 2) Honest pre-classification of the image. The loader below fails
    //    closed anyway; this only produces a precise human reason.
    // ------------------------------------------------------------------
    switch (ClassifyHeader(executable.string())) {
        case HeaderKind::Unreadable:
            report.error = "executable unreadable or smaller than an ELF header";
            return report;
        case HeaderKind::NotElf:
            report.error = "executable is not an ELF image";
            return report;
        case HeaderKind::Elf32:
            report.error = "32-bit ELF images are not supported (PS5 is ELF64)";
            return report;
        case HeaderKind::ElfWrongArch:
            report.error = "ELF image is not x86-64";
            return report;
        case HeaderKind::SelfContainer:
            report.error =
                "SELF container (encrypted retail image): this project does "
                "not decrypt; provide a decrypted/homebrew ELF";
            return report;
        case HeaderKind::SelfContainerProspero: {
            // Round 30: parse the REAL PS5 container, validate every field
            // (fail-closed), and — when the payload is plaintext debug/
            // devkit data — emit a bootable flat ELF for the normal path.
            const ::Loader::SelfParseResult self =
                ::Loader::ParseProsperoSelf(executable.string());
            if (!self.ok) {
                report.error = "Prospero SELF: " + self.error;
                return report;
            }
            report.self_parsed = true;
            report.self_plaintext = self.plaintext;
            report.self_entries = self.entries.size();
            report.self_data_segments = self.data_segments;
            report.self_pairings = self.pairings.size();
            report.self_needed_libraries = self.needed_libraries.size();
            report.self_module_path = self.module_path;
            report.self_embedded_type =
                self.ehdr.e_type == ::Loader::ET_SCE_DYNEXEC ? "ET_SCE_DYNEXEC"
                : self.ehdr.e_type == ::Loader::ET_SCE_EXEC   ? "ET_SCE_EXEC"
                : self.ehdr.e_type == ::Loader::ET_SCE_DYNAMIC ? "ET_SCE_DYNAMIC"
                : self.ehdr.e_type == ::Loader::ET_DYN         ? "ET_DYN"
                : "ET_EXEC";
            if (!self.plaintext) {
                report.error =
                    "Prospero SELF parsed (structure OK) but the payload is "
                    "ciphertext: retail image — this project does not decrypt";
                return report;
            }
            const std::string flat = executable.string() + ".flat.elf";
            std::string flat_err;
            if (::Loader::FlattenSelfToElf(executable.string(), self, flat,
                                           flat_err) != 0) {
                report.error = "Prospero SELF flatten failed: " + flat_err;
                return report;
            }
            report.flattened_elf_path = flat;
            executable = flat;   // boot the flattened ELF through the real path
            break;
        }
        case HeaderKind::Elf64X8664:
            break;
    }

    // ------------------------------------------------------------------
    // 3) Register the whole HLE surface (1600+ NIDs) exactly once.
    // ------------------------------------------------------------------
    RegisterHleOnce();
    report.hle_registered = true;
    report.hle_symbols_registered = g_hle_symbols;

    // ------------------------------------------------------------------
    // 3.5) GPU pre-warm: load the Vulkan loader NOW, on the main thread.
    // Once the direct-execution seccomp guard arms on a trampoline thread,
    // file-backed mmap (which dlopen needs) is denied THERE by design; a
    // dlopen of an already-loaded library only refcounts, so pre-loading
    // keeps the GPU path reachable under native execution.
    // ------------------------------------------------------------------
    for (const char* name : {"libvulkan.so.1", "libvulkan.so"}) {
        if (dlopen(name, RTLD_NOW | RTLD_LOCAL) != nullptr) {
            break;
        }
    }

    // ------------------------------------------------------------------
    // 4) Pick the execution engine. Direct = native guest execution on the
    //    host CPU (round 20). The backend must be enabled explicitly (arena +
    //    identity mapping + code-write notifications + signal handlers); if
    //    that fails we record why and run the interpreter — same
    //    guest-visible contract either way (fail-closed).
    // ------------------------------------------------------------------
    auto& engine = CPU::CPUJitEngine::Instance();
    if (options.prefer_direct_execution) {
        if (!CPU::DirectExecutionBackend::Instance().Enable()) {
            report.direct_outcome = "backend enable failed (arena/identity-map)";
            engine.SetExecutionBackend(CPU::GuestExecutionBackend::Interpreter);
        } else {
            engine.SetExecutionBackend(CPU::GuestExecutionBackend::Direct);
        }
    } else {
        engine.SetExecutionBackend(CPU::GuestExecutionBackend::Interpreter);
    }
    const uint64_t syscalls_before = engine.GetInterceptedSyscallCount();

    // ------------------------------------------------------------------
    // 5) Boot through the REAL pipeline: RuntimeLinker::LoadProgram (own
    //    image, .dynsym/.dynstr, relocations, DT_NEEDED resolution) ->
    //    segment mapping -> TLS block -> DT_INIT/INIT_ARRAY -> entry point,
    //    with live guest syscalls (the dispatcher + HLE NIDs above).
    // ------------------------------------------------------------------
    ::Loader::GuestLauncher launcher;
    const ::Loader::GuestBootResult boot =
        launcher.Boot(executable.string(), options.instruction_limit);

    report.entry_gva = boot.entry_gva;
    report.init_functions_run = boot.init_functions_run;
    report.fini_functions_run = boot.fini_functions_run;
    report.segments_mapped = boot.segments_mapped;
    report.tls_base_gva = boot.tls_base_gva;
    report.relocs_applied = boot.relocs_applied;
    report.relocs_imported = boot.relocs_imported;
    report.relocs_skipped = boot.relocs_skipped;

    if (!boot.ok) {
        report.error = boot.error.empty() ? "guest boot failed" : boot.error;
        return report;
    }
    report.booted = true;
    report.executed = true;
    report.exit_code = boot.exit_code;

    // ------------------------------------------------------------------
    // 6) Execution facts: which engine ran, syscall traffic.
    // ------------------------------------------------------------------
    report.syscalls_intercepted =
        engine.GetInterceptedSyscallCount() - syscalls_before;
    if (options.prefer_direct_execution) {
        if (report.direct_outcome.empty()) {
            // Backend enabled and selected: which path actually ran?
            const auto& outcome = engine.LastDirectOutcome();
            const bool native =
                outcome.reason == CPU::DirectStopReason::Returned ||
                outcome.reason == CPU::DirectStopReason::ThreadExit;
            report.execution_engine = native ? "native-direct" : "interpreter";
            if (!native) {
                report.direct_outcome = CPU::ToString(outcome.reason);
            }
        } else {
            report.execution_engine = "interpreter";
        }
    } else {
        report.execution_engine = "interpreter";
    }
    return report;
}

std::string FormatSummary(const BootReport& r) {
    if (!r.scanned) {
        return "not started: " + r.error;
    }
    if (!r.booted) {
        std::string s = "boot failed: " + r.error;
        if (r.self_parsed) {
            s += " [SELF parsed: " + std::to_string(r.self_entries) +
                 " entries, " + std::to_string(r.self_pairings) + " pairings, " +
                 std::to_string(r.self_needed_libraries) + " imports]";
        }
        return s;
    }
    std::string s = "booted " + r.executable_path;
    if (r.self_parsed) {
        s += " [SELF→ELF: " + std::to_string(r.self_entries) + " entries, " +
             std::to_string(r.self_pairings) + " PT_LOAD pairings, " +
             std::to_string(r.self_needed_libraries) + " DT_NEEDED, " +
             r.self_embedded_type + "]";
    }
    if (r.execution_engine == "native-direct") {
        s += " [native-direct]";
    } else if (!r.direct_outcome.empty()) {
        s += " [interpreter, direct declined: " + r.direct_outcome + "]";
    } else {
        s += " [interpreter]";
    }
    s += ", exit=" + std::to_string(r.exit_code);
    s += ", syscalls=" + std::to_string(r.syscalls_intercepted);
    return s;
}

} // namespace PS5::Boot
