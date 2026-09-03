// prospero_cli.cpp — the `prospero-run` entry point.
//
// Round 29 rewrite. Until this round the binary stopped after reading the
// ELF header and printed "CPU/ABI/GPU execution is not enabled" even though
// every layer it needed was already linked into it. Now it boots the game
// through the real pipeline (see prospero_boot.hpp):
//
//   prospero-run <game-directory | elf-file> [--max-instructions N]
//               [--interpreter] [--quiet]
//
//   --max-instructions N   entry-point instruction budget (default 500M)
//   --interpreter          skip the native DirectExecutionBackend (debug aid)
//   --quiet                summary line only
#include "prospero_boot.hpp"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

namespace {

void PrintUsage() {
    std::cerr << "usage: prospero-run <game-directory | elf-file>\n"
                 "                 [--max-instructions N] [--interpreter] [--quiet]\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage();
        return 2;
    }

    PS5::Boot::BootOptions options;
    std::string path;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--max-instructions" && i + 1 < argc) {
            const long long n = std::atoll(argv[++i]);
            if (n > 0) {
                options.instruction_limit = static_cast<size_t>(n);
            }
        } else if (arg == "--interpreter") {
            options.prefer_direct_execution = false;
        } else if (arg == "--quiet") {
            // handled below via flag on options (no-op here)
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage();
            return 0;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "unknown option: " << arg << "\n";
            PrintUsage();
            return 2;
        } else if (path.empty()) {
            path = arg;
        } else {
            std::cerr << "unexpected extra argument: " << arg << "\n";
            PrintUsage();
            return 2;
        }
    }
    if (path.empty()) {
        PrintUsage();
        return 2;
    }
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--quiet") == 0) quiet = true;
    }

    const PS5::Boot::BootReport report = PS5::Boot::RunGame(path, options);

    if (!quiet) {
        std::cout << "game root:        "
                  << (report.game_root.empty() ? "-" : report.game_root) << "\n"
                  << "executable:       "
                  << (report.executable_path.empty() ? "-" : report.executable_path) << "\n"
                  << "resources:        " << report.resources << "\n"
                  << "entry point:      0x" << std::hex << report.entry_gva
                  << std::dec << "\n"
                  << "HLE NIDs:         " << report.hle_symbols_registered << "\n"
                  << "segments mapped:  " << report.segments_mapped << "\n"
                  << "relocs applied:   " << report.relocs_applied << "\n"
                  << "relocs imported:  " << report.relocs_imported << "\n"
                  << "relocs skipped:   " << report.relocs_skipped << "\n"
                  << "init functions:   " << report.init_functions_run << "\n"
                  << "TLS base:         0x" << std::hex << report.tls_base_gva
                  << std::dec << "\n"
                  << "engine:           "
                  << (report.execution_engine.empty() ? "-" : report.execution_engine);
        if (!report.direct_outcome.empty()) {
            std::cout << " (direct declined: " << report.direct_outcome << ")";
        }
        std::cout << "\n"
                  << "guest syscalls:   " << report.syscalls_intercepted << "\n"
                  << "exit code:        " << report.exit_code << "\n";
    }

    if (!report.error.empty()) {
        std::cerr << "error: " << report.error << "\n";
        return 1;
    }
    std::cout << PS5::Boot::FormatSummary(report) << "\n";
    // Propagate the guest's exit code into the runner's process exit so the
    // CLI honestly reflects what the guest requested (e.g. a homebrew payload
    // that calls exit(42) yields a 42 exit status).
    if (report.booted) {
        return static_cast<int>(report.exit_code);
    }
    return 0;
}
