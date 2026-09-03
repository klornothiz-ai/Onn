#!/usr/bin/env python3
"""Wire round-30 SELF parser test into the Makefile (TAB-safe)."""
import re, sys

MK = 'Makefile'
src = open(MK, encoding='utf-8').read()
orig = src

# 1) test variable after SAVEDATA_PERSIST_TEST
anchor = 'SAVEDATA_PERSIST_TEST := $(TEST_BIN_DIR)/savedata_persist_test\n'
if 'SELF_PARSER_TEST :=' not in src:
    src = src.replace(anchor, anchor + 'SELF_PARSER_TEST := $(TEST_BIN_DIR)/self_parser_test\n', 1)

# 2) UNIT_TESTS list
if '$(SELF_PARSER_TEST)' not in src.split('\nunit:')[0]:
    src = src.replace('$(NET_SOCKETS_TEST) $(SAVEDATA_PERSIST_TEST)\n',
                      '$(NET_SOCKETS_TEST) $(SAVEDATA_PERSIST_TEST) $(SELF_PARSER_TEST)\n', 1)

# 3) link rule (ET_DYN_BOOT_TEST deps + prospero_self.cpp), inserted before the
#    round-20 comment block
rule = (
    '# Round 30: Prospero SELF container parser + flattener + real eboot facts.\n'
    '$(SELF_PARSER_TEST): tests/self_parser_test.cpp src/loader/prospero_self.cpp '
    'src/loader/guest_launcher.cpp src/loader/runtime_linker.cpp src/kernel/event_queue.cpp '
    'src/gpu/video_out_impl.cpp src/cpu/prospero_syscalls.cpp src/cpu/fork_process.cpp '
    'src/cpu/guest_threads.cpp src/cpu/thread_scheduler.cpp src/cpu/jit_executor.cpp '
    'src/cpu/direct_execution.cpp src/cpu/x86_64_interpreter.cpp src/cpu/x86_64_x87.cpp '
    'src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_simd_full.cpp src/cpu/x86_64_subset_interpreter.cpp '
    'src/memory/virtual_memory_manager.cpp src/kernel/event_flag.cpp src/kernel/kernel_managers.cpp '
    '$(UNIT_HEADERS) | $(TEST_BIN_DIR)\n'
    '\t$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@ $(LDFLAGS)\n\n'
)
if '$(SELF_PARSER_TEST):' not in src:
    marker = '# Round 20: the DirectExecutionBackend suite'
    src = src.replace(marker, rule + marker, 1)

# 4) recipe body — run after SAVEDATA_PERSIST_TEST line inside unit:
if src.count('\t$(SAVEDATA_PERSIST_TEST)\n') == 1:
    src = src.replace('\t$(SAVEDATA_PERSIST_TEST)\n',
                      '\t$(SAVEDATA_PERSIST_TEST)\n\t$(SELF_PARSER_TEST)\n', 1)

if src != orig:
    open(MK, 'w', encoding='utf-8', newline='').write(src)
    print('Makefile wired for round 30 (self_parser_test)')
else:
    print('no changes (already wired?)')
