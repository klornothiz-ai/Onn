CXX ?= g++
UNAME_S := $(shell uname -s 2>/dev/null)
IS_WINDOWS := $(if $(findstring MINGW,$(UNAME_S)),1,$(if $(findstring MSYS,$(UNAME_S)),1,$(if $(findstring CYGWIN,$(UNAME_S)),1,)))
THREAD_FLAGS := $(if $(IS_WINDOWS),,-pthread)
PLATFORM_LIBS := $(if $(IS_WINDOWS),,-ldl)
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS := $(shell pkg-config --libs sdl2 2>/dev/null)
HAVE_SDL := $(if $(SDL_LIBS),1,)
HAVE_FFMPEG := $(if $(wildcard /usr/include/libavcodec/avcodec.h),1,)
HAVE_JSON := $(if $(wildcard /usr/include/nlohmann/json.hpp),1,)
HAVE_FMT := $(if $(wildcard /usr/include/fmt/format.h),1,)
# Real Vulkan compute path (item #1). When the loader dev headers are present we
# link libvulkan directly for the compute executor; otherwise the executor
# compiles a self-contained stub (via __has_include) that reports Unavailable.
HAVE_VULKAN := $(if $(wildcard /usr/include/vulkan/vulkan.h),1,)
VK_LIBS := $(if $(HAVE_VULKAN),-lvulkan,)
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic $(THREAD_FLAGS) -Iinclude -Ilibs -I. $(SDL_CFLAGS)
LDFLAGS ?= $(THREAD_FLAGS) $(PLATFORM_LIBS) $(SDL_LIBS) $(VK_LIBS) $(if $(HAVE_FFMPEG),-lavcodec -lavformat -lavutil -lswresample -lswscale,) $(if $(HAVE_FMT),-lfmt,)
TEST_CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror $(THREAD_FLAGS) -Iinclude -Ilibs -I.

# Full prototype build. It requires the optional SDL2, FFmpeg, fmt, and nlohmann-json packages.
CORE_SRCS := $(wildcard src/common/*.cpp) \
	$(wildcard src/cpu/*.cpp) \
	$(wildcard src/gpu/*.cpp) \
	$(wildcard src/kernel/*.cpp) \
	$(wildcard src/loader/*.cpp) \
	$(wildcard src/memory/*.cpp) \
	$(wildcard src/audio/*.cpp)
HLE_SRCS := $(wildcard libs/*.cpp) $(wildcard libs/ajm/*.cpp)
HLE_SRCS := $(filter-out libs/controller_stub.cpp libs/dialog_kernel_stub.cpp,$(HLE_SRCS))
ifneq ($(HAVE_SDL),1)
HLE_SRCS := $(filter-out libs/audio.cpp libs/libAudio2.cpp libs/libNet.cpp libs/libAudio.cpp,$(HLE_SRCS))
# Without SDL2 the real audio/net/platform backends are excluded, so provide
# safe no-op registrations for the HLE symbols libs.cpp still references.
HLE_SRCS += libs/optional_stubs_sdl.cpp
# Real SDL-free AudioOut surface backed by HeadlessAudioSink (round 8, item
# #2): the guest sceAudioOut* entry points stay resolvable and observable on
# a headless host instead of disappearing with the SDL backend.
HLE_SRCS += libs/audio_headless.cpp
endif
ifneq ($(HAVE_FFMPEG),1)
HLE_SRCS := $(filter-out libs/libVideoDec2.cpp,$(HLE_SRCS))
# Without FFmpeg, provide the avpriv_vga16_font table (normally in libavutil)
# and a no-op registration for the video-decoder HLE lib.
HLE_SRCS += libs/optional_stubs_ffmpeg.cpp
endif
ifneq ($(HAVE_JSON),1)
HLE_SRCS := $(filter-out libs/libJson2.cpp,$(HLE_SRCS))
endif
ifneq ($(HAVE_FFMPEG),1)
# Keep the core emulator build usable without optional multimedia headers.
HLE_SRCS := $(filter-out libs/ajm.cpp libs/avPlayer.cpp libs/videoDec2Decoder.cpp libs/ajm/%.cpp,$(HLE_SRCS))
endif
ifeq ($(HAVE_SDL),1)
CORE_SRCS += src/gpu/vulkan_backend.cpp
endif
SRCS := $(CORE_SRCS) $(HLE_SRCS)
OBJS := $(SRCS:.cpp=.o)
CORE_OBJS := $(CORE_SRCS:.cpp=.o)
# Round 29: prospero-run boots games through the REAL pipeline
# (scan -> HLE InitAll -> GuestLauncher boot with live syscalls), so it links
# the full engine exactly like the integrated self-test target.
RUNNER_OBJS := $(OBJS)
TARGET := ps5_native_vulkan_emulator
RUNNER := prospero-run

UNIT_HEADERS := $(shell find include -type f)
TEST_BIN_DIR := build/tests
CPU_TEST := $(TEST_BIN_DIR)/cpu_interpreter_test
JIT_TEST := $(TEST_BIN_DIR)/jit_executor_test
EXIT_PROP_TEST := $(TEST_BIN_DIR)/exit_code_propagation_test
JIT_CHAINING_TEST := $(TEST_BIN_DIR)/jit_chaining_test
VMM_ELF_TEST := $(TEST_BIN_DIR)/vmm_elf_loader_test
PM4_TEST := $(TEST_BIN_DIR)/pm4_decoder_test
SPIRV_TEST := $(TEST_BIN_DIR)/rdna2_spirv_recompiler_test
GPU_TEST := $(TEST_BIN_DIR)/gpu_backend_state_test
FOLDER_TEST := $(TEST_BIN_DIR)/game_folder_test
BOOT_UNALIGNED_TEST := $(TEST_BIN_DIR)/boot_unaligned_ptload_test
VMM_EXPANDED_TEST := $(TEST_BIN_DIR)/vmm_expanded_test
SYSCALL_TEST := $(TEST_BIN_DIR)/syscall_dispatcher_test
SYSCALL_EXP_TEST := $(TEST_BIN_DIR)/syscall_kevent_expanded_test
PM4_TRANSLATOR_TEST := $(TEST_BIN_DIR)/pm4_translator_expanded_test
X86_INTERP_TEST := $(TEST_BIN_DIR)/x86_64_interpreter_test
GUEST_EXEC_TEST := $(TEST_BIN_DIR)/guest_execution_integration_test
ELF_EXEC_TEST := $(TEST_BIN_DIR)/elf_execution_integration_test
COMPUTE_CC_TEST := $(TEST_BIN_DIR)/rdna2_compute_compiler_test
VK_COMPUTE_TEST := $(TEST_BIN_DIR)/vulkan_compute_executor_test
PM4_REAL_COMPUTE_TEST := $(TEST_BIN_DIR)/pm4_real_compute_integration_test
HLE_REAL_COMPUTE_TEST := $(TEST_BIN_DIR)/hle_real_compute_submit_test
HLE_LIBPAD_TEST := $(TEST_BIN_DIR)/hle_libpad_input_test
AUDIO_SINK_TEST := $(TEST_BIN_DIR)/headless_audio_sink_test
HLE_AUDIO_OUT_TEST := $(TEST_BIN_DIR)/hle_audio_headless_test
HLE_GFX_TEST := $(TEST_BIN_DIR)/hle_graphics_submit_test
HLE_KERNEL_TEST := $(TEST_BIN_DIR)/hle_libkernel_test
PM4_DRAW_TEST := $(TEST_BIN_DIR)/pm4_draw_realpath_test
RT_LINKER_TEST := $(TEST_BIN_DIR)/runtime_linker_test
KERNEL_EQ_TEST := $(TEST_BIN_DIR)/kernel_event_queue_test
GUEST_BOOT_TEST := $(TEST_BIN_DIR)/guest_boot_test
ET_DYN_BOOT_TEST := $(TEST_BIN_DIR)/et_dyn_boot_test
GUEST_THREADS_TEST := $(TEST_BIN_DIR)/guest_threads_test
CPU_FULL_ISA_TEST := $(TEST_BIN_DIR)/cpu_full_isa_test
CPU_AVX256_TEST := $(TEST_BIN_DIR)/cpu_avx256_test
CPU_X87_TEST := $(TEST_BIN_DIR)/cpu_x87_test
SYSCALL_DEPTH_TEST := $(TEST_BIN_DIR)/syscall_depth_test
SYSCALL_FORK_TEST := $(TEST_BIN_DIR)/syscall_fork_test
DIRECT_EXEC_TEST := build/tests/direct_execution_test
SECCOMP_GUARD_TEST := $(TEST_BIN_DIR)/seccomp_guard_test
GPU_WAVE29_TEST := $(TEST_BIN_DIR)/gpu_wave29_test
PM4_EVENTS_TEST := $(TEST_BIN_DIR)/pm4_events_dma_test
HLE_ENTROPY_TEST := $(TEST_BIN_DIR)/hle_entropy_test
GPU_ROUND20_TEST := build/tests/gpu_round20_test
GPU_MEM_EXT_TEST := $(TEST_BIN_DIR)/gpu_memory_extended_test
SYSCALL_IO_EXT_TEST := $(TEST_BIN_DIR)/syscall_io_extended_test
CPU_SIMD_DIFF_TEST := $(TEST_BIN_DIR)/cpu_simd_diff_test
GPU_IMAGE_FLAT_TEST := $(TEST_BIN_DIR)/gpu_image_flat_test
GPU_MIMG_TEST := $(TEST_BIN_DIR)/gpu_mimg_test
NET_SOCKETS_TEST := $(TEST_BIN_DIR)/net_sockets_test
SAVEDATA_PERSIST_TEST := $(TEST_BIN_DIR)/savedata_persist_test
SELF_PARSER_TEST := $(TEST_BIN_DIR)/self_parser_test
HLE_PLT_TEST := $(TEST_BIN_DIR)/hle_plt_test


GCN_DECODER_TEST := $(TEST_BIN_DIR)/gcn_decoder_test
GCN_SPIRV_FULL_TEST := $(TEST_BIN_DIR)/gcn_spirv_full_test
SOFTWARE_RASTER_TEST := $(TEST_BIN_DIR)/software_rasterizer_test
SOFTWARE_RASTER_TEX_TEST := $(TEST_BIN_DIR)/software_rasterizer_texture_test
SOFTWARE_RASTER_BLEND_TEST := $(TEST_BIN_DIR)/software_rasterizer_blend_test
LOCK_PREFIX_TEST := $(TEST_BIN_DIR)/lock_prefix_test
PM4_VGT_FETCH_TEST := $(TEST_BIN_DIR)/pm4_vgt_fetch_test
PM4_COLOR_TARGET_TEST := $(TEST_BIN_DIR)/pm4_color_target_test
PM4_RESOURCE_TEST := $(TEST_BIN_DIR)/pm4_resource_dispatch_test
VK_GFX_TEST := $(TEST_BIN_DIR)/vk_graphics_pipeline_test
UNIT_TESTS := $(CPU_TEST) $(JIT_TEST) $(JIT_CHAINING_TEST) $(X86_INTERP_TEST) $(GUEST_EXEC_TEST) $(ELF_EXEC_TEST) $(VMM_ELF_TEST) $(VMM_EXPANDED_TEST) $(SYSCALL_TEST) $(SYSCALL_EXP_TEST) $(HLE_KERNEL_TEST) $(PM4_TEST) $(PM4_TRANSLATOR_TEST) $(HLE_GFX_TEST) $(SPIRV_TEST) $(COMPUTE_CC_TEST) $(VK_COMPUTE_TEST) $(PM4_REAL_COMPUTE_TEST) $(HLE_REAL_COMPUTE_TEST) $(HLE_LIBPAD_TEST) $(AUDIO_SINK_TEST) $(HLE_AUDIO_OUT_TEST) $(FOLDER_TEST) $(PM4_DRAW_TEST) $(PM4_VGT_FETCH_TEST) $(RT_LINKER_TEST) $(KERNEL_EQ_TEST) $(GUEST_BOOT_TEST) $(GCN_DECODER_TEST) $(SOFTWARE_RASTER_TEST) $(SOFTWARE_RASTER_TEX_TEST) $(SOFTWARE_RASTER_BLEND_TEST) $(GUEST_THREADS_TEST) $(CPU_FULL_ISA_TEST) $(SYSCALL_DEPTH_TEST) $(PM4_COLOR_TARGET_TEST) $(PM4_RESOURCE_TEST) $(VK_GFX_TEST) $(GCN_SPIRV_FULL_TEST) $(CPU_AVX256_TEST) $(ET_DYN_BOOT_TEST) $(SYSCALL_FORK_TEST) $(DIRECT_EXEC_TEST) $(SECCOMP_GUARD_TEST) $(GPU_ROUND20_TEST) $(GPU_MEM_EXT_TEST) $(SYSCALL_IO_EXT_TEST) $(CPU_SIMD_DIFF_TEST) $(CPU_X87_TEST) $(GPU_IMAGE_FLAT_TEST) $(GPU_MIMG_TEST) $(GPU_WAVE29_TEST) $(PM4_EVENTS_TEST) $(HLE_ENTROPY_TEST) $(NET_SOCKETS_TEST) $(SAVEDATA_PERSIST_TEST) $(SELF_PARSER_TEST) $(HLE_PLT_TEST) $(LOCK_PREFIX_TEST) $(BOOT_UNALIGNED_TEST) $(EXIT_PROP_TEST)

all: $(TARGET) $(RUNNER)

$(RUNNER): src/prospero_cli.cpp src/prospero_boot.cpp $(RUNNER_OBJS)
	$(CXX) $(CXXFLAGS) $(filter %.cpp %.o,$^) $(LDFLAGS) -o $@

$(TARGET): $(OBJS) src/prospero_boot.o tests/main.o
	$(CXX) $(CXXFLAGS) $(filter %.o,$^) $(LDFLAGS) -o $@

$(TEST_BIN_DIR):
	mkdir -p $@

$(CPU_TEST): tests/cpu_interpreter_test.cpp src/cpu/x86_64_subset_interpreter.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@

# The JIT engine's full-execution path pulls in the extended interpreter and the
# syscall dispatcher (plus its scheduler/event-flag/kernel deps), so the JIT
# unit test links them alongside the subset interpreter it still caches blocks
# with.
$(JIT_TEST): tests/jit_executor_test.cpp src/cpu/jit_executor.cpp src/cpu/hle_trampoline.cpp src/cpu/direct_execution.cpp src/cpu/x86_64_interpreter.cpp src/cpu/x86_64_x87.cpp src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_simd_full.cpp src/cpu/x86_64_subset_interpreter.cpp src/cpu/prospero_syscalls.cpp src/cpu/fork_process.cpp src/cpu/guest_threads.cpp src/cpu/thread_scheduler.cpp src/memory/virtual_memory_manager.cpp src/kernel/event_flag.cpp src/kernel/kernel_managers.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)

# Round 34 regression: a guest exit() syscall must stop the run and return the
# guest's process-exit code (42) instead of running past it and returning 0.
$(EXIT_PROP_TEST): tests/exit_code_propagation_test.cpp src/cpu/jit_executor.cpp src/cpu/hle_trampoline.cpp src/cpu/direct_execution.cpp src/cpu/x86_64_interpreter.cpp src/cpu/x86_64_x87.cpp src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_simd_full.cpp src/cpu/x86_64_subset_interpreter.cpp src/cpu/prospero_syscalls.cpp src/cpu/fork_process.cpp src/cpu/guest_threads.cpp src/cpu/thread_scheduler.cpp src/memory/virtual_memory_manager.cpp src/kernel/event_flag.cpp src/kernel/kernel_managers.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@

# JIT block-chaining self-test: same link set as the JIT executor test.
$(JIT_CHAINING_TEST): tests/jit_chaining_test.cpp src/cpu/jit_executor.cpp src/cpu/hle_trampoline.cpp src/cpu/direct_execution.cpp src/cpu/x86_64_interpreter.cpp src/cpu/x86_64_x87.cpp src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_simd_full.cpp src/cpu/x86_64_subset_interpreter.cpp src/cpu/prospero_syscalls.cpp src/cpu/fork_process.cpp src/cpu/guest_threads.cpp src/cpu/thread_scheduler.cpp src/memory/virtual_memory_manager.cpp src/kernel/event_flag.cpp src/kernel/kernel_managers.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@

$(VMM_ELF_TEST): tests/vmm_elf_loader_test.cpp src/memory/virtual_memory_manager.cpp src/loader/elf_loader.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@

# The PM4 unit test supplies a backend stub, so no Vulkan implementation is linked.
$(PM4_TEST): tests/pm4_decoder_test.cpp src/gpu/pm4_decoder.cpp src/gpu/pm4_translator.cpp src/gpu/software_rasterizer.cpp src/gpu/vulkan_compute_executor.cpp src/gpu/gcn_decoder.cpp src/gpu/rdna2_compute_compiler.cpp src/gpu/shader_spirv_recompiler.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) $(VK_LIBS) -o $@

$(SPIRV_TEST): tests/rdna2_spirv_recompiler_test.cpp src/gpu/shader_spirv_recompiler.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@

# The compute compiler emits a full SSBO-backed compute kernel. It dumps the
# generated SPIR-V to build/tests/*.spv; when spirv-cross is installed on the
# host, `make spirv-validate` round-trips those to GLSL as an external check.
$(COMPUTE_CC_TEST): tests/rdna2_compute_compiler_test.cpp src/gpu/rdna2_compute_compiler.cpp src/gpu/gcn_decoder.cpp src/gpu/shader_spirv_recompiler.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@

$(FOLDER_TEST): tests/game_folder_test.cpp src/loader/game_folder.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@

# HLE libPad input (item #2): drives the real controller state machine through
# its guest-facing Pad* entry points with injected host events. No SDL / no
# device -- pure state-machine + kernel time. Links controller + kernel/time.
$(HLE_LIBPAD_TEST): tests/hle_libpad_input_test.cpp libs/controller.cpp src/kernel/time.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@

# Headless audio sink (item #2): SDL-free PCM decode + accounting for the
# AudioOut path. Self-contained; links only the sink.
$(AUDIO_SINK_TEST): tests/headless_audio_sink_test.cpp src/audio/headless_audio_sink.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@

# Headless AudioOut wiring (round 8, item #2): the guest-facing AudioOut
# entry points over the real HeadlessAudioSink, plus the NID registration
# the guest dlsym path uses. No SDL / no audio device required.
$(HLE_AUDIO_OUT_TEST): tests/hle_audio_headless_test.cpp libs/audio_headless.cpp src/audio/headless_audio_sink.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@

# Real Vulkan compute executor (item #1): RDNA2 -> SPIR-V -> real VkBuffers
# (SSBOs) + descriptor sets + compute pipeline -> vkCmdDispatch -> results read
# back from GPU memory. Links libvulkan when its dev headers are present
# (llvmpipe/lavapipe here, a physical GPU on the user's machine); otherwise the
# executor compiles its Unavailable stub and the test stays green headless.
$(VK_COMPUTE_TEST): tests/vulkan_compute_executor_test.cpp src/gpu/vulkan_compute_executor.cpp src/gpu/gcn_decoder.cpp src/gpu/rdna2_compute_compiler.cpp src/gpu/shader_spirv_recompiler.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) $(VK_LIBS) -o $@

# End-to-end: a PM4 DISPATCH ring drives the real VulkanComputeExecutor through
# the translator, reading shader+SSBOs from a guest-memory implementer and
# writing results back -- the item #1 integration into the submit path. Links
# the real backend (vulkan_backend.cpp -> dl/SDL) plus the executor + compiler.
$(PM4_REAL_COMPUTE_TEST): tests/pm4_real_compute_integration_test.cpp src/gpu/pm4_decoder.cpp src/gpu/pm4_translator.cpp src/gpu/software_rasterizer.cpp src/gpu/vulkan_backend.cpp src/gpu/vulkan_compute_executor.cpp src/gpu/gcn_decoder.cpp src/gpu/rdna2_compute_compiler.cpp src/gpu/shader_spirv_recompiler.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(SDL_CFLAGS) $(filter %.cpp,$^) $(PLATFORM_LIBS) $(SDL_LIBS) $(VK_LIBS) -o $@

# Final item #1 wiring: the HLE-style SubmitCompute path (HeadlessGpuBridge) with
# EnableRealCompute drives the real executor. Links the bridge, backend,
# translator, executor and compiler.
$(HLE_REAL_COMPUTE_TEST): tests/hle_real_compute_submit_test.cpp src/gpu/headless_gpu_bridge.cpp src/gpu/pm4_decoder.cpp src/gpu/pm4_translator.cpp src/gpu/software_rasterizer.cpp src/gpu/vulkan_backend.cpp src/gpu/vulkan_compute_executor.cpp src/gpu/gcn_decoder.cpp src/gpu/rdna2_compute_compiler.cpp src/gpu/shader_spirv_recompiler.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(SDL_CFLAGS) $(filter %.cpp,$^) $(PLATFORM_LIBS) $(SDL_LIBS) $(VK_LIBS) -o $@

# HLE libKernel integration: drives the shipped semaphore / sync-on-address /
# time primitives directly through their guest-facing entry points, including
# real cross-thread producer/consumer and futex-style wakeups. Links only the
# three kernel units under test -- no Vulkan, VMM or optional backend.
$(HLE_KERNEL_TEST): tests/hle_libkernel_test.cpp src/kernel/semaphore.cpp src/kernel/sync_on_address.cpp src/kernel/time.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@

# HLE graphics-driver submit path: drives the headless GPU bridge (the object
# the HLE driver holds as g_renderer) with real PM4 draw/compute rings through
# the real PM4 translator and the real software-fallback Vulkan backend. The
# backend dlopen()s the Vulkan loader when present and degrades to software
# otherwise, so it needs the platform dl library but no SDL/display.
$(HLE_GFX_TEST): tests/hle_graphics_submit_test.cpp src/gpu/headless_gpu_bridge.cpp src/gpu/pm4_decoder.cpp src/gpu/pm4_translator.cpp src/gpu/software_rasterizer.cpp src/gpu/vulkan_backend.cpp src/gpu/shader_spirv_recompiler.cpp src/gpu/vulkan_compute_executor.cpp src/gpu/gcn_decoder.cpp src/gpu/rdna2_compute_compiler.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(SDL_CFLAGS) $(filter %.cpp,$^) $(PLATFORM_LIBS) $(SDL_LIBS) $(VK_LIBS) -o $@

$(PM4_DRAW_TEST): tests/pm4_draw_realpath_test.cpp src/gpu/pm4_decoder.cpp src/gpu/pm4_translator.cpp src/gpu/software_rasterizer.cpp src/gpu/vulkan_backend.cpp src/gpu/vulkan_compute_executor.cpp src/gpu/gcn_decoder.cpp src/gpu/rdna2_compute_compiler.cpp src/gpu/shader_spirv_recompiler.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@
$(PM4_VGT_FETCH_TEST): tests/pm4_vgt_fetch_test.cpp src/gpu/pm4_decoder.cpp src/gpu/pm4_translator.cpp src/gpu/software_rasterizer.cpp src/gpu/vulkan_backend.cpp src/gpu/vulkan_compute_executor.cpp src/gpu/gcn_decoder.cpp src/gpu/rdna2_compute_compiler.cpp src/gpu/shader_spirv_recompiler.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@

$(RT_LINKER_TEST): tests/runtime_linker_test.cpp src/loader/runtime_linker.cpp src/cpu/hle_trampoline.cpp src/memory/virtual_memory_manager.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@

$(SOFTWARE_RASTER_TEST): tests/software_rasterizer_test.cpp src/gpu/software_rasterizer.cpp src/gpu/pm4_decoder.cpp src/gpu/pm4_translator.cpp src/gpu/vulkan_compute_executor.cpp src/gpu/gcn_decoder.cpp src/gpu/rdna2_compute_compiler.cpp src/gpu/shader_spirv_recompiler.cpp src/gpu/vulkan_backend.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@ $(LDFLAGS)

$(SOFTWARE_RASTER_TEX_TEST): tests/software_rasterizer_texture_test.cpp src/gpu/software_rasterizer.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@

$(SOFTWARE_RASTER_BLEND_TEST): tests/software_rasterizer_blend_test.cpp src/gpu/software_rasterizer.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@

$(LOCK_PREFIX_TEST): tests/lock_prefix_test.cpp src/cpu/x86_64_interpreter.cpp src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_x87.cpp src/cpu/x86_64_simd_full.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@

$(GCN_DECODER_TEST): tests/gcn_decoder_test.cpp src/gpu/gcn_decoder.cpp src/gpu/vulkan_compute_executor.cpp src/gpu/rdna2_compute_compiler.cpp src/gpu/shader_spirv_recompiler.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@ $(LDFLAGS)

$(GUEST_BOOT_TEST): tests/guest_boot_test.cpp src/loader/guest_launcher.cpp src/loader/runtime_linker.cpp src/cpu/hle_trampoline.cpp src/kernel/event_queue.cpp src/gpu/video_out_impl.cpp src/cpu/prospero_syscalls.cpp src/cpu/fork_process.cpp src/cpu/guest_threads.cpp src/cpu/thread_scheduler.cpp src/cpu/jit_executor.cpp src/cpu/direct_execution.cpp src/cpu/x86_64_interpreter.cpp src/cpu/x86_64_x87.cpp src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_simd_full.cpp src/cpu/x86_64_subset_interpreter.cpp src/memory/virtual_memory_manager.cpp src/kernel/event_flag.cpp src/kernel/kernel_managers.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)

# Round 34 regression: unaligned PT_LOAD in GuestLauncher::Boot must page-align
# and extend the guest mapping instead of failing the whole boot.
$(BOOT_UNALIGNED_TEST): tests/boot_unaligned_ptload_test.cpp src/loader/guest_launcher.cpp src/loader/runtime_linker.cpp src/cpu/hle_trampoline.cpp src/kernel/event_queue.cpp src/gpu/video_out_impl.cpp src/cpu/prospero_syscalls.cpp src/cpu/fork_process.cpp src/cpu/guest_threads.cpp src/cpu/thread_scheduler.cpp src/cpu/jit_executor.cpp src/cpu/direct_execution.cpp src/cpu/x86_64_interpreter.cpp src/cpu/x86_64_x87.cpp src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_simd_full.cpp src/cpu/x86_64_subset_interpreter.cpp src/memory/virtual_memory_manager.cpp src/kernel/event_flag.cpp src/kernel/kernel_managers.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@

$(GUEST_THREADS_TEST): tests/guest_threads_test.cpp src/cpu/guest_threads.cpp src/cpu/prospero_syscalls.cpp src/cpu/fork_process.cpp src/cpu/thread_scheduler.cpp src/cpu/jit_executor.cpp src/cpu/hle_trampoline.cpp src/cpu/direct_execution.cpp src/cpu/x86_64_interpreter.cpp src/cpu/x86_64_x87.cpp src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_simd_full.cpp src/cpu/x86_64_subset_interpreter.cpp src/memory/virtual_memory_manager.cpp src/kernel/event_flag.cpp src/kernel/kernel_managers.cpp src/kernel/event_queue.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(CXXFLAGS) -Iinclude -Ilibs -o $@ $< src/cpu/guest_threads.cpp src/cpu/prospero_syscalls.cpp src/cpu/fork_process.cpp src/cpu/thread_scheduler.cpp src/cpu/jit_executor.cpp src/cpu/hle_trampoline.cpp src/cpu/direct_execution.cpp src/cpu/x86_64_interpreter.cpp src/cpu/x86_64_x87.cpp src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_simd_full.cpp src/cpu/x86_64_subset_interpreter.cpp src/memory/virtual_memory_manager.cpp src/kernel/event_flag.cpp src/kernel/kernel_managers.cpp src/kernel/event_queue.cpp $(LDFLAGS)

$(CPU_FULL_ISA_TEST): tests/cpu_full_isa_test.cpp src/cpu/x86_64_interpreter.cpp src/cpu/x86_64_x87.cpp src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_simd_full.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(CXXFLAGS) -Iinclude -Ilibs -o $@ $< src/cpu/x86_64_interpreter.cpp src/cpu/x86_64_x87.cpp src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_simd_full.cpp $(LDFLAGS)

$(SYSCALL_DEPTH_TEST): tests/syscall_depth_test.cpp src/cpu/prospero_syscalls.cpp src/cpu/fork_process.cpp src/cpu/guest_threads.cpp src/cpu/thread_scheduler.cpp src/memory/virtual_memory_manager.cpp src/kernel/event_flag.cpp src/kernel/kernel_managers.cpp src/kernel/event_queue.cpp src/cpu/x86_64_interpreter.cpp src/cpu/x86_64_x87.cpp src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_simd_full.cpp src/cpu/jit_executor.cpp src/cpu/hle_trampoline.cpp src/cpu/direct_execution.cpp src/cpu/x86_64_subset_interpreter.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(CXXFLAGS) -Iinclude -Ilibs -o $@ $< src/cpu/prospero_syscalls.cpp src/cpu/fork_process.cpp src/cpu/guest_threads.cpp src/cpu/thread_scheduler.cpp src/memory/virtual_memory_manager.cpp src/kernel/event_flag.cpp src/kernel/kernel_managers.cpp src/kernel/event_queue.cpp src/cpu/x86_64_interpreter.cpp src/cpu/x86_64_x87.cpp src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_simd_full.cpp src/cpu/jit_executor.cpp src/cpu/hle_trampoline.cpp src/cpu/direct_execution.cpp src/cpu/x86_64_subset_interpreter.cpp $(LDFLAGS)

$(KERNEL_EQ_TEST): tests/kernel_event_queue_test.cpp src/kernel/event_queue.cpp src/gpu/video_out_impl.cpp src/cpu/prospero_syscalls.cpp src/cpu/fork_process.cpp src/cpu/guest_threads.cpp src/cpu/thread_scheduler.cpp src/cpu/jit_executor.cpp src/cpu/hle_trampoline.cpp src/cpu/direct_execution.cpp src/cpu/x86_64_interpreter.cpp src/cpu/x86_64_x87.cpp src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_simd_full.cpp src/cpu/x86_64_subset_interpreter.cpp src/memory/virtual_memory_manager.cpp src/kernel/event_flag.cpp src/kernel/kernel_managers.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@

$(VMM_EXPANDED_TEST): tests/vmm_expanded_test.cpp src/memory/virtual_memory_manager.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@

# The syscall dispatcher links the real thread scheduler, JIT engine, VMM and
# event-flag manager it dispatches into; the tests never enter the guest-thread
# execution path, so no Vulkan or optional backend is required.
$(SYSCALL_TEST): tests/syscall_dispatcher_test.cpp src/cpu/prospero_syscalls.cpp src/cpu/fork_process.cpp src/cpu/guest_threads.cpp src/cpu/thread_scheduler.cpp src/cpu/jit_executor.cpp src/cpu/hle_trampoline.cpp src/cpu/direct_execution.cpp src/cpu/x86_64_interpreter.cpp src/cpu/x86_64_x87.cpp src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_simd_full.cpp src/cpu/x86_64_subset_interpreter.cpp src/memory/virtual_memory_manager.cpp src/kernel/event_flag.cpp src/kernel/kernel_managers.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@

# Expanded syscall coverage (item #4): real kevent EVFILT_USER, evf_cancel and
# open. Same link set as the dispatcher test -- register-only, VMM-backed.
$(SYSCALL_EXP_TEST): tests/syscall_kevent_expanded_test.cpp src/cpu/prospero_syscalls.cpp src/cpu/fork_process.cpp src/cpu/guest_threads.cpp src/cpu/thread_scheduler.cpp src/cpu/jit_executor.cpp src/cpu/hle_trampoline.cpp src/cpu/direct_execution.cpp src/cpu/x86_64_interpreter.cpp src/cpu/x86_64_x87.cpp src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_simd_full.cpp src/cpu/x86_64_subset_interpreter.cpp src/memory/virtual_memory_manager.cpp src/kernel/event_flag.cpp src/kernel/kernel_managers.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@

# Like pm4_decoder_test, the translator test supplies its own backend stub, so
# no Vulkan implementation is linked.
$(PM4_TRANSLATOR_TEST): tests/pm4_translator_expanded_test.cpp src/gpu/pm4_decoder.cpp src/gpu/pm4_translator.cpp src/gpu/software_rasterizer.cpp src/gpu/vulkan_compute_executor.cpp src/gpu/gcn_decoder.cpp src/gpu/rdna2_compute_compiler.cpp src/gpu/shader_spirv_recompiler.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) $(VK_LIBS) -o $@

# The extended x86-64 interpreter is a self-contained execution core tested
# over a flat in-process memory bus; it links no other engine subsystem.
$(X86_INTERP_TEST): tests/x86_64_interpreter_test.cpp src/cpu/x86_64_interpreter.cpp src/cpu/x86_64_x87.cpp src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_simd_full.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@

# End-to-end guest execution over the live VMM arena: links the JIT engine, the
# extended interpreter, the subset interpreter (still referenced by the JIT),
# the VMM, and the syscall dispatcher with its scheduler/event-flag/kernel deps.
$(GUEST_EXEC_TEST): tests/guest_execution_integration_test.cpp src/cpu/jit_executor.cpp src/cpu/hle_trampoline.cpp src/cpu/direct_execution.cpp src/cpu/x86_64_interpreter.cpp src/cpu/x86_64_x87.cpp src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_simd_full.cpp src/cpu/x86_64_subset_interpreter.cpp src/cpu/prospero_syscalls.cpp src/cpu/fork_process.cpp src/cpu/guest_threads.cpp src/cpu/thread_scheduler.cpp src/memory/virtual_memory_manager.cpp src/kernel/event_flag.cpp src/kernel/kernel_managers.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@

# Front-to-back: ElfLoader maps a decrypted static ELF64 into the guest arena,
# then ExecuteGuestFull runs it from the ELF entry point (shadPS4-style flow).
$(ELF_EXEC_TEST): tests/elf_execution_integration_test.cpp src/loader/elf_loader.cpp src/cpu/jit_executor.cpp src/cpu/hle_trampoline.cpp src/cpu/direct_execution.cpp src/cpu/x86_64_interpreter.cpp src/cpu/x86_64_x87.cpp src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_simd_full.cpp src/cpu/x86_64_subset_interpreter.cpp src/cpu/prospero_syscalls.cpp src/cpu/fork_process.cpp src/cpu/guest_threads.cpp src/cpu/thread_scheduler.cpp src/memory/virtual_memory_manager.cpp src/kernel/event_flag.cpp src/kernel/kernel_managers.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@

ifneq ($(HAVE_SDL),1)
$(GPU_TEST):
	@echo "GPU backend test skipped: SDL2 development headers are unavailable"
else
$(GPU_TEST): tests/gpu_backend_state_test.cpp src/gpu/vulkan_backend.cpp src/gpu/shader_spirv_recompiler.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(SDL_CFLAGS) $(filter %.cpp,$^) $(PLATFORM_LIBS) $(SDL_LIBS) -o $@
endif

$(PM4_COLOR_TARGET_TEST): tests/pm4_color_target_test.cpp src/gpu/pm4_decoder.cpp src/gpu/pm4_translator.cpp src/gpu/software_rasterizer.cpp src/gpu/vulkan_backend.cpp src/gpu/vulkan_compute_executor.cpp src/gpu/gcn_decoder.cpp src/gpu/rdna2_compute_compiler.cpp src/gpu/shader_spirv_recompiler.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@ $(LDFLAGS)

$(PM4_RESOURCE_TEST): tests/pm4_resource_dispatch_test.cpp src/gpu/pm4_decoder.cpp src/gpu/pm4_translator.cpp src/gpu/software_rasterizer.cpp src/gpu/vulkan_backend.cpp src/gpu/vulkan_compute_executor.cpp src/gpu/gcn_decoder.cpp src/gpu/rdna2_compute_compiler.cpp src/gpu/shader_spirv_recompiler.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@ $(LDFLAGS)

$(VK_GFX_TEST): tests/vk_graphics_pipeline_test.cpp src/gpu/pm4_decoder.cpp src/gpu/pm4_translator.cpp src/gpu/software_rasterizer.cpp src/gpu/vulkan_backend.cpp src/gpu/vulkan_compute_executor.cpp src/gpu/gcn_decoder.cpp src/gpu/rdna2_compute_compiler.cpp src/gpu/shader_spirv_recompiler.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@ $(LDFLAGS)

$(GCN_SPIRV_FULL_TEST): tests/gcn_spirv_full_test.cpp src/gpu/gcn_decoder.cpp src/gpu/rdna2_compute_compiler.cpp src/gpu/shader_spirv_recompiler.cpp src/gpu/vulkan_compute_executor.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@ $(LDFLAGS)

$(CPU_AVX256_TEST): tests/cpu_avx256_test.cpp src/cpu/x86_64_interpreter.cpp src/cpu/x86_64_x87.cpp src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_simd_full.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@ $(LDFLAGS)

# Round 29: the complete x87 engine (opcodes D8..DF: loads/stores, arithmetic,
# compares + condition codes, transcendentals, integer/BCD conversions,
# environment save/restore). Host long double IS the 80-bit x87 format, so
# the interpreter model matches real x87 bit-for-bit by construction.
$(CPU_X87_TEST): tests/cpu_x87_test.cpp src/cpu/x86_64_interpreter.cpp src/cpu/x86_64_x87.cpp src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_simd_full.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@ $(LDFLAGS)

# Round 28: differential SIMD verification -- every form in
# tests/simd_forms_table.inc executes NATIVELY on the host (wrappers in
# tests/simd_native_forms.S) and through the interpreter; register files,
# upper YMM halves, written memory and RFLAGS must match bit-exactly.
$(TEST_BIN_DIR)/simd_native_forms.o: tests/simd_native_forms.S | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) -c $< -o $@

$(CPU_SIMD_DIFF_TEST): tests/cpu_simd_diff_test.cpp src/cpu/x86_64_interpreter.cpp src/cpu/x86_64_x87.cpp src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_simd_full.cpp $(TEST_BIN_DIR)/simd_native_forms.o $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) $(TEST_BIN_DIR)/simd_native_forms.o -o $@ $(LDFLAGS)

# Round 28: MIMG/EXP/FLAT decode + software image model + barrier recognition
$(GPU_IMAGE_FLAT_TEST): tests/gpu_image_flat_test.cpp src/gpu/gcn_decoder.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@ $(LDFLAGS)

# Round 29: VOP3P (v_mad_mix_f32), VINTRP interpolation against the EXP
# param exports, DS integer/float reductions and the wavefront lane ops
# (ds_swizzle_b32 / ds_bpermute_b32 with simultaneous-shuffle semantics).
$(GPU_WAVE29_TEST): tests/gpu_wave29_test.cpp src/gpu/gcn_decoder.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@ $(LDFLAGS)

# Round 28: full MIMG coverage -- SPIR-V compiler image lowering (sample/
# fetch/gather/store/atomics), the extended resource-table ABI, and dispatch
# parity against a direct GcnSwExecutor reference (hardware path when a
# Vulkan device exists, software otherwise).
$(GPU_MIMG_TEST): tests/gpu_mimg_test.cpp src/gpu/gcn_decoder.cpp src/gpu/rdna2_compute_compiler.cpp src/gpu/vulkan_compute_executor.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@ $(VK_LIBS) $(LDFLAGS)

# Round 29: real PM4 packet semantics -- SET_UCONFIG_REG storage,
# EVENT_WRITE fence publication, EVENT_WRITE_EOS immediates, DMA_DATA memory
# copies through the guest bridge, and SET_PREDICATION conditional rendering.
$(PM4_EVENTS_TEST): tests/pm4_events_dma_test.cpp src/gpu/pm4_decoder.cpp src/gpu/pm4_translator.cpp src/gpu/software_rasterizer.cpp src/gpu/vulkan_backend.cpp src/gpu/vulkan_compute_executor.cpp src/gpu/gcn_decoder.cpp src/gpu/rdna2_compute_compiler.cpp src/gpu/shader_spirv_recompiler.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@ $(LDFLAGS)

# Round 29: libSceRandom real kernel entropy (getrandom(2) replaces the
# fixed-seed LCG) + the fsync NID going through the real host descriptor.
$(HLE_ENTROPY_TEST): tests/hle_entropy_test.cpp libs/libNet.cpp libs/network.cpp libs/ps_errno.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@ $(LDFLAGS)

# Round 28: the real POSIX socket backend (loopback TCP lifecycle through the
# guest-facing libSceNet wrappers + FreeBSD errno translation + NIDs).
$(NET_SOCKETS_TEST): tests/net_sockets_test.cpp libs/libNet.cpp libs/network.cpp libs/ps_errno.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@ $(LDFLAGS)

# Round 28: save-data persistence (PARAM.bin + icon0.png through the mounted
# save directory, surviving umount/remount cycles).
$(SAVEDATA_PERSIST_TEST): tests/savedata_persist_test.cpp libs/libSaveData.cpp src/loader/system_content.cpp src/kernel/file_system.cpp libs/ps_errno.cpp src/common/file.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@ $(LDFLAGS)

$(ET_DYN_BOOT_TEST): tests/et_dyn_boot_test.cpp src/loader/guest_launcher.cpp src/loader/runtime_linker.cpp src/cpu/hle_trampoline.cpp src/kernel/event_queue.cpp src/gpu/video_out_impl.cpp src/cpu/prospero_syscalls.cpp src/cpu/fork_process.cpp src/cpu/guest_threads.cpp src/cpu/thread_scheduler.cpp src/cpu/jit_executor.cpp src/cpu/direct_execution.cpp src/cpu/x86_64_interpreter.cpp src/cpu/x86_64_x87.cpp src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_simd_full.cpp src/cpu/x86_64_subset_interpreter.cpp src/memory/virtual_memory_manager.cpp src/kernel/event_flag.cpp src/kernel/kernel_managers.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@ $(LDFLAGS)

$(SYSCALL_FORK_TEST): tests/syscall_fork_test.cpp src/cpu/prospero_syscalls.cpp src/cpu/fork_process.cpp src/cpu/guest_threads.cpp src/cpu/thread_scheduler.cpp src/cpu/jit_executor.cpp src/cpu/hle_trampoline.cpp src/cpu/direct_execution.cpp src/cpu/x86_64_interpreter.cpp src/cpu/x86_64_x87.cpp src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_simd_full.cpp src/cpu/x86_64_subset_interpreter.cpp src/memory/virtual_memory_manager.cpp src/kernel/event_flag.cpp src/kernel/kernel_managers.cpp src/kernel/event_queue.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@ $(LDFLAGS)

# Round 30: HLE dynamic linking — PLT/GOT imports through guest trampolines.
$(HLE_PLT_TEST): tests/hle_plt_test.cpp src/cpu/hle_trampoline.cpp src/loader/guest_launcher.cpp src/loader/runtime_linker.cpp src/kernel/event_queue.cpp src/gpu/video_out_impl.cpp src/cpu/prospero_syscalls.cpp src/cpu/fork_process.cpp src/cpu/guest_threads.cpp src/cpu/thread_scheduler.cpp src/cpu/jit_executor.cpp src/cpu/direct_execution.cpp src/cpu/x86_64_interpreter.cpp src/cpu/x86_64_x87.cpp src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_simd_full.cpp src/cpu/x86_64_subset_interpreter.cpp src/memory/virtual_memory_manager.cpp src/kernel/event_flag.cpp src/kernel/kernel_managers.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@ $(LDFLAGS)

# Round 30: Prospero SELF container parser + flattener + real eboot facts.
$(SELF_PARSER_TEST): tests/self_parser_test.cpp src/loader/prospero_self.cpp src/loader/guest_launcher.cpp src/loader/runtime_linker.cpp src/cpu/hle_trampoline.cpp src/kernel/event_queue.cpp src/gpu/video_out_impl.cpp src/cpu/prospero_syscalls.cpp src/cpu/fork_process.cpp src/cpu/guest_threads.cpp src/cpu/thread_scheduler.cpp src/cpu/jit_executor.cpp src/cpu/direct_execution.cpp src/cpu/x86_64_interpreter.cpp src/cpu/x86_64_x87.cpp src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_simd_full.cpp src/cpu/x86_64_subset_interpreter.cpp src/memory/virtual_memory_manager.cpp src/kernel/event_flag.cpp src/kernel/kernel_managers.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@ $(LDFLAGS)

# Round 20: the DirectExecutionBackend suite -- guest code runs NATIVELY on
# the host CPU (trampoline + signal exits); syscall/cpuid/rdtsc/SSE4a/BMI/movbe/
# tzcnt sites are ud2-intercepted and replayed through the interpreter core.
$(DIRECT_EXEC_TEST): tests/direct_execution_test.cpp src/cpu/direct_execution.cpp src/cpu/hle_trampoline.cpp src/cpu/x86_64_interpreter.cpp src/cpu/x86_64_x87.cpp src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_simd_full.cpp src/cpu/x86_64_subset_interpreter.cpp src/memory/virtual_memory_manager.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@

# Round 29: PROOF that the deny-by-default seccomp allowlist enforces on a
# thread that entered native guest execution (open/ptrace/fork/clone-as-
# process/unlisted numbers all return EPERM; the allowed surface keeps
# working).
$(SECCOMP_GUARD_TEST): tests/seccomp_guard_test.cpp src/cpu/direct_execution.cpp src/cpu/hle_trampoline.cpp src/cpu/x86_64_interpreter.cpp src/cpu/x86_64_x87.cpp src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_simd_full.cpp src/cpu/x86_64_subset_interpreter.cpp src/memory/virtual_memory_manager.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@

# Round 20: vertex control flow + the extended CB format set + the
# cross-dispatch pipeline cache (headless: the SPIR-V structure, the software
# rasterizer encodes, the translator gate, and the pure cache registry).
$(GPU_ROUND20_TEST): tests/gpu_round20_test.cpp src/gpu/pm4_decoder.cpp src/gpu/pm4_translator.cpp src/gpu/software_rasterizer.cpp src/gpu/vulkan_backend.cpp src/gpu/vulkan_compute_executor.cpp src/gpu/gcn_decoder.cpp src/gpu/rdna2_compute_compiler.cpp src/gpu/shader_spirv_recompiler.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) $(VK_LIBS) -o $@

# Round 26: S_BUFFER_LOAD_* through the software GCN executor and the compute
# compiler, including the two var_buf/register-liveness fixes this round made
# (S_BUFFER_LOAD_* now allocates its buffer-descriptor SPIR-V variables even
# without a MUBUF instruction present, and its destination SGPRs are marked
# live by the same pre-scan that plain S_LOAD_* uses).
$(GPU_MEM_EXT_TEST): tests/gpu_memory_extended_test.cpp src/gpu/gcn_decoder.cpp src/gpu/rdna2_compute_compiler.cpp src/gpu/shader_spirv_recompiler.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@

# Round 26: dup/readv/pipe/ftruncate as host-backed syscalls, verified
# through the VMM.
$(SYSCALL_IO_EXT_TEST): tests/syscall_io_extended_test.cpp src/cpu/prospero_syscalls.cpp src/cpu/fork_process.cpp src/cpu/guest_threads.cpp src/cpu/thread_scheduler.cpp src/cpu/jit_executor.cpp src/cpu/hle_trampoline.cpp src/cpu/direct_execution.cpp src/cpu/x86_64_interpreter.cpp src/cpu/x86_64_x87.cpp src/cpu/x86_64_isa_ext.cpp src/cpu/x86_64_simd_full.cpp src/cpu/x86_64_subset_interpreter.cpp src/memory/virtual_memory_manager.cpp src/kernel/event_flag.cpp src/kernel/kernel_managers.cpp $(UNIT_HEADERS) | $(TEST_BIN_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(filter %.cpp,$^) -o $@

unit: $(UNIT_TESTS)
	$(CPU_TEST)
	$(JIT_TEST)
	$(EXIT_PROP_TEST)
	$(X86_INTERP_TEST)
	$(GUEST_EXEC_TEST)
	$(ELF_EXEC_TEST)
	$(VMM_ELF_TEST)
	$(VMM_EXPANDED_TEST)
	$(SYSCALL_TEST)
	$(SYSCALL_EXP_TEST)
	$(HLE_KERNEL_TEST)
	$(PM4_TEST)
	$(PM4_TRANSLATOR_TEST)
	$(HLE_GFX_TEST)
	$(SPIRV_TEST)
	$(COMPUTE_CC_TEST)
	$(VK_COMPUTE_TEST)
	$(PM4_REAL_COMPUTE_TEST)
	$(HLE_REAL_COMPUTE_TEST)
	$(HLE_LIBPAD_TEST)
	$(AUDIO_SINK_TEST)
	$(HLE_AUDIO_OUT_TEST)
	$(FOLDER_TEST)
	$(PM4_DRAW_TEST)
	$(PM4_VGT_FETCH_TEST)
	$(PM4_COLOR_TARGET_TEST)
	$(PM4_RESOURCE_TEST)
	$(VK_GFX_TEST)
	$(RT_LINKER_TEST)
	$(KERNEL_EQ_TEST)
	$(GUEST_BOOT_TEST)
	$(BOOT_UNALIGNED_TEST)
	$(ET_DYN_BOOT_TEST)
	$(GCN_DECODER_TEST)
	$(GCN_SPIRV_FULL_TEST)
	$(SOFTWARE_RASTER_TEST)
	$(SYSCALL_DEPTH_TEST)
	$(SYSCALL_FORK_TEST)
	$(DIRECT_EXEC_TEST)
	$(SECCOMP_GUARD_TEST)
	$(GPU_ROUND20_TEST)
	$(GPU_MEM_EXT_TEST)
	$(SYSCALL_IO_EXT_TEST)
	$(CPU_FULL_ISA_TEST)
	$(GPU_MIMG_TEST)
	$(GPU_WAVE29_TEST)
	$(PM4_EVENTS_TEST)
	$(HLE_ENTROPY_TEST)
	$(NET_SOCKETS_TEST)
	$(SAVEDATA_PERSIST_TEST)
	$(SELF_PARSER_TEST)
	$(HLE_PLT_TEST)
	$(SELF_PARSER_TEST)
	$(CPU_AVX256_TEST)
	$(CPU_X87_TEST)
	$(GUEST_THREADS_TEST)
	$(CPU_SIMD_DIFF_TEST)
	$(GPU_IMAGE_FLAT_TEST)

test: unit

# Optional external validation: decompile every emitted .spv with spirv-cross.
# Skips cleanly if spirv-cross is not installed.
spirv-validate: $(COMPUTE_CC_TEST)
	$(COMPUTE_CC_TEST)
	@if command -v spirv-cross >/dev/null 2>&1; then \
		for f in build/tests/*.spv; do \
			echo "== spirv-cross $$f =="; spirv-cross $$f || exit 1; \
		done; \
		echo "[PASS] all emitted SPIR-V modules round-tripped through spirv-cross"; \
	else echo "spirv-cross not installed; skipping external SPIR-V validation"; fi

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

libs/%.o: libs/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

tests/%.o: tests/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) tests/*.o *.o $(TARGET)
	rm -rf build

.PHONY: all clean test unit
