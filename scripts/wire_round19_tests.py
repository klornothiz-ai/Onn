#!/usr/bin/env python3
"""Wire the round-19 tests into the Makefile (TAB-safe, in-place).

Adds:
  * PM4_RESOURCE_TEST  (tests/pm4_resource_dispatch_test.cpp)  -- phase 1
  * VK_GFX_TEST        (tests/vk_graphics_pipeline_test.cpp)   -- phase 2
as build rules, UNIT_TESTS entries, and `make unit` run lines.
"""
import sys

MK = "Makefile"

def main() -> int:
    with open(MK, "r", encoding="utf-8", newline="") as f:
        text = f.read()

    if "PM4_RESOURCE_TEST" in text:
        print("already wired; nothing to do")
        return 0

    tab = "\t"

    # 1. Variable declarations after PM4_COLOR_TARGET_TEST.
    anchor = "PM4_COLOR_TARGET_TEST := $(TEST_BIN_DIR)/pm4_color_target_test\n"
    assert anchor in text, "color-target variable anchor not found"
    decl = (anchor +
            "PM4_RESOURCE_TEST := $(TEST_BIN_DIR)/pm4_resource_dispatch_test\n"
            "VK_GFX_TEST := $(TEST_BIN_DIR)/vk_graphics_pipeline_test\n")
    text = text.replace(anchor, decl, 1)

    # 2. UNIT_TESTS list entry.
    ut_anchor = "$(PM4_COLOR_TARGET_TEST) $(GCN_SPIRV_FULL_TEST)"
    assert ut_anchor in text, "UNIT_TESTS anchor not found"
    text = text.replace(
        ut_anchor,
        "$(PM4_COLOR_TARGET_TEST) $(PM4_RESOURCE_TEST) $(VK_GFX_TEST) "
        "$(GCN_SPIRV_FULL_TEST)", 1)

    # 3. Build rules (same source set as the color-target suite).
    rule_anchor = ("$(GCN_SPIRV_FULL_TEST): tests/gcn_spirv_full_test.cpp "
                   "src/gpu/gcn_decoder.cpp src/gpu/rdna2_compute_compiler.cpp "
                   "src/gpu/shader_spirv_recompiler.cpp "
                   "src/gpu/vulkan_compute_executor.cpp $(UNIT_HEADERS) | "
                   "$(TEST_BIN_DIR)\n")
    assert rule_anchor in text, "gcn rule anchor not found"
    sources = ("src/gpu/pm4_decoder.cpp src/gpu/pm4_translator.cpp "
               "src/gpu/software_rasterizer.cpp src/gpu/vulkan_backend.cpp "
               "src/gpu/vulkan_compute_executor.cpp src/gpu/gcn_decoder.cpp "
               "src/gpu/rdna2_compute_compiler.cpp "
               "src/gpu/shader_spirv_recompiler.cpp")
    rules = ""
    for var, src in (("PM4_RESOURCE_TEST", "tests/pm4_resource_dispatch_test.cpp"),
                     ("VK_GFX_TEST", "tests/vk_graphics_pipeline_test.cpp")):
        rules += (f"$( {var} ): {src} {sources} $(UNIT_HEADERS) | "
                  f"$(TEST_BIN_DIR)\n".replace("$( ", "$("))
        rules += f"{tab}$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@ $(LDFLAGS)\n\n"
    text = text.replace(rule_anchor, rules + rule_anchor, 1)

    # 4. `make unit` run lines.
    run_anchor = tab + "$(PM4_COLOR_TARGET_TEST)\n"
    assert run_anchor in text, "unit run anchor not found"
    runs = (run_anchor +
            tab + "$(PM4_RESOURCE_TEST)\n" +
            tab + "$(VK_GFX_TEST)\n")
    text = text.replace(run_anchor, runs, 1)

    with open(MK, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    print("wired PM4_RESOURCE_TEST + VK_GFX_TEST (rules, UNIT_TESTS, unit runs)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
