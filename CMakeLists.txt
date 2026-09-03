cmake_minimum_required(VERSION 3.20)
project(prosperolayer_rdna2_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PROSPERO_BUILD_TESTS "Build the emulator unit tests" ON)

add_library(prospero_core STATIC
  src/common/cond_var.cpp
  src/common/dateTime.cpp
  src/common/emulator_config.cpp
  src/common/file.cpp
  src/cpu/direct_execution.cpp
  src/cpu/fork_process.cpp
  src/cpu/guest_threads.cpp
  src/cpu/jit_executor.cpp
  src/cpu/prospero_syscalls.cpp
  src/cpu/thread_scheduler.cpp
  src/cpu/x86_64_interpreter.cpp
  src/cpu/x86_64_isa_ext.cpp
  src/cpu/x86_64_subset_interpreter.cpp
  src/gpu/gcn_decoder.cpp
  src/gpu/pm4_decoder.cpp
  src/gpu/pm4_translator.cpp
  src/gpu/rdna2_compute_compiler.cpp
  src/gpu/software_rasterizer.cpp
  src/gpu/vulkan_backend.cpp
  src/gpu/vulkan_compute_executor.cpp
  src/gpu/shader_impl.cpp
  src/gpu/shader_spirv_recompiler.cpp
  src/gpu/sync.cpp
  src/gpu/video_out_impl.cpp
  src/kernel/event_flag.cpp
  src/kernel/event_queue.cpp
  src/kernel/file_system.cpp
  src/kernel/kernel_managers.cpp
  src/kernel/memory_extra.cpp
  src/kernel/misc_glue.cpp
  src/kernel/posix_wrappers.cpp
  src/kernel/pthread.cpp
  src/kernel/semaphore.cpp
  src/kernel/sync_on_address.cpp
  src/kernel/time.cpp
  src/loader/elf_loader.cpp
  src/loader/guest_launcher.cpp
  src/loader/game_folder.cpp
  src/loader/runtime_linker.cpp
  src/loader/system_content.cpp
  src/memory/virtual_memory_manager.cpp
)

target_include_directories(prospero_core PUBLIC include libs .)
target_compile_features(prospero_core PUBLIC cxx_std_20)

add_executable(prospero-run src/prospero_cli.cpp)
target_link_libraries(prospero-run PRIVATE prospero_core)
find_package(Threads REQUIRED)

if (MSVC)
  target_compile_options(prospero_core PRIVATE /W4)
else()
  target_compile_options(prospero_core PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(prospero_core PUBLIC Threads::Threads)

endif()

if (PROSPERO_BUILD_TESTS)
  enable_testing()
  function(prospero_test name)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE prospero_core)
    add_test(NAME ${name} COMMAND ${name})
  endfunction()

  prospero_test(cpu_interpreter_test)
  prospero_test(cpu_full_isa_test)
  prospero_test(cpu_avx256_test)
  prospero_test(direct_execution_test)
  prospero_test(jit_executor_test)
  prospero_test(vmm_elf_loader_test)
  prospero_test(pm4_decoder_test)
  prospero_test(pm4_translator_expanded_test)
  prospero_test(rdna2_compute_compiler_test)
  prospero_test(rdna2_spirv_recompiler_test)
  prospero_test(gcn_decoder_test)
  prospero_test(gcn_spirv_full_test)
  prospero_test(gpu_backend_state_test)
  prospero_test(gpu_round20_test)
  prospero_test(software_rasterizer_scissor_test)
  prospero_test(pm4_indirect_draw_test)
  prospero_test(pm4_memory_sync_test)
  prospero_test(gpu_memory_extended_test)
  prospero_test(syscall_io_extended_test)
  prospero_test(guest_boot_test)
  prospero_test(guest_execution_integration_test)
  prospero_test(guest_threads_test)
  prospero_test(elf_execution_integration_test)
  prospero_test(et_dyn_boot_test)
  prospero_test(game_folder_test)
endif()
