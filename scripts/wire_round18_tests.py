#!/usr/bin/env python3
"""Round 18 Makefile wiring (tab-safe).

Adds the round-18 test binaries to the Makefile:
  - variable definitions next to the existing *_TEST variables
  - membership in UNIT_TESTS
  - build rules (with the same link sets as their siblings)
  - run lines in the `unit` recipe
Also fixes a latent defect from rounds 15-17: GUEST_THREADS_TEST,
CPU_FULL_ISA_TEST and SYSCALL_DEPTH_TEST were BUILT as unit prerequisites
but never RUN by the `unit` recipe.
"""
import re
import sys

MAKEFILE = "Makefile"

NEW_TESTS = [
    # (var, test source, sources list, run-after anchor)
    ("PM4_COLOR_TARGET_TEST", "tests/pm4_color_target_test.cpp",
     "src/gpu/pm4_decoder.cpp src/gpu/pm4_translator.cpp src/gpu/software_rasterizer.cpp "
     "src/gpu/vulkan_backend.cpp src/gpu/vulkan_compute_executor.cpp src/gpu/gcn_decoder.cpp "
     "src/gpu/rdna2_compute_compiler.cpp src/gpu/shader_spirv_recompiler.cpp",
     "PM4_VGT_FETCH_TEST"),
    ("GCN_SPIRV_FULL_TEST", "tests/gcn_spirv_full_test.cpp",
     "src/gpu/gcn_decoder.cpp src/gpu/rdna2_compute_compiler.cpp "
     "src/gpu/shader_spirv_recompiler.cpp src/gpu/vulkan_compute_executor.cpp",
     "GCN_DECODER_TEST"),
    ("CPU_AVX256_TEST", "tests/cpu_avx256_test.cpp",
     "src/cpu/x86_64_interpreter.cpp src/cpu/x86_64_isa_ext.cpp",
     "CPU_FULL_ISA_TEST"),
    ("ET_DYN_BOOT_TEST", "tests/et_dyn_boot_test.cpp",
     "src/loader/guest_launcher.cpp src/loader/runtime_linker.cpp src/kernel/event_queue.cpp "
     "src/gpu/video_out_impl.cpp src/cpu/prospero_syscalls.cpp src/cpu/guest_threads.cpp "
     "src/cpu/thread_scheduler.cpp src/cpu/jit_executor.cpp src/cpu/x86_64_interpreter.cpp "
     "src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_subset_interpreter.cpp "
     "src/memory/virtual_memory_manager.cpp src/kernel/event_flag.cpp src/kernel/kernel_managers.cpp",
     "GUEST_BOOT_TEST"),
    ("SYSCALL_FORK_TEST", "tests/syscall_fork_test.cpp",
     "src/cpu/prospero_syscalls.cpp src/cpu/fork_process.cpp src/cpu/guest_threads.cpp "
     "src/cpu/thread_scheduler.cpp src/cpu/jit_executor.cpp src/cpu/x86_64_interpreter.cpp "
     "src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_subset_interpreter.cpp "
     "src/memory/virtual_memory_manager.cpp src/kernel/event_flag.cpp "
     "src/kernel/kernel_managers.cpp src/kernel/event_queue.cpp",
     "SYSCALL_DEPTH_TEST"),
]


def main() -> int:
    with open(MAKEFILE, "r", encoding="utf-8") as f:
        text = f.read()

    # 0) latent fix FIRST (its run lines are anchors for the new tests):
    #    run lines for the rounds 15-17 suites that were built but never
    #    executed by `make unit`
    for var in ("GUEST_THREADS_TEST", "CPU_FULL_ISA_TEST", "SYSCALL_DEPTH_TEST"):
        if re.search(rf"^\t\$\({var}\)$", text, flags=re.M):
            print(f"[wire18] {var} already has a run line")
            continue
        m = re.search(r"^\t\$\(SOFTWARE_RASTER_TEST\)$", text, flags=re.M)
        if not m:
            print("[wire18] ERROR: SOFTWARE_RASTER_TEST run line not found")
            return 1
        text = text[: m.end()] + f"\n\t$({var})" + text[m.end():]
        print(f"[wire18] added missing run line for {var}")

    for var, src, sources, run_anchor in NEW_TESTS:
        if var in text:
            print(f"[wire18] {var} already wired; skipping")
            continue

        # 1) variable definition: append after the anchor variable's line
        m = re.search(rf"^{run_anchor} := .*$", text, flags=re.M)
        if not m:
            print(f"[wire18] ERROR: anchor {run_anchor} not found")
            return 1
        text = text[: m.end()] + f"\n{var} := $(TEST_BIN_DIR)/{var.lower()}" + text[m.end():]

        # 2) UNIT_TESTS membership
        m = re.search(r"^UNIT_TESTS := (.*)$", text, flags=re.M)
        if not m:
            print("[wire18] ERROR: UNIT_TESTS not found")
            return 1
        text = text[: m.end(1)] + f" $({var})" + text[m.end(1):]

        # 3) build rule: insert before the `unit:` recipe
        m = re.search(r"^unit: \$\(UNIT_TESTS\)$", text, flags=re.M)
        if not m:
            print("[wire18] ERROR: unit target not found")
            return 1
        rule = (f"$({var}): {src} {sources} $(UNIT_HEADERS) | $(TEST_BIN_DIR)\n"
                f"\t$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@ $(LDFLAGS)\n\n")
        text = text[: m.start()] + rule + text[m.start():]

        # 4) run line in the unit recipe (after the anchor's run line)
        m = re.search(rf"^\t\$\({run_anchor}\)$", text, flags=re.M)
        if not m:
            print(f"[wire18] ERROR: run anchor {run_anchor} not found")
            return 1
        text = text[: m.end()] + f"\n\t$({var})" + text[m.end():]

        print(f"[wire18] wired {var}")

    # 5) latent fix: run lines for the rounds 15-17 suites that were built
    #    but never executed by `make unit`
    for var in ("GUEST_THREADS_TEST", "CPU_FULL_ISA_TEST", "SYSCALL_DEPTH_TEST"):
        if re.search(rf"^\t\$\({var}\)$", text, flags=re.M):
            print(f"[wire18] {var} already has a run line")
            continue
        m = re.search(r"^\t\$\(SOFTWARE_RASTER_TEST\)$", text, flags=re.M)
        if not m:
            print("[wire18] ERROR: SOFTWARE_RASTER_TEST run line not found")
            return 1
        text = text[: m.end()] + f"\n\t$({var})" + text[m.end():]
        print(f"[wire18] added missing run line for {var}")

    with open(MAKEFILE, "w", encoding="utf-8") as f:
        f.write(text)
    print("[wire18] done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
