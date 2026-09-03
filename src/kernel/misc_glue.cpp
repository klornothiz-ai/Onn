// ProsperoLayer PS5 emulator - misc glue implementations
// PM4 dump, host memory probing, virtual memory helpers and FS glue.
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/hostMemory.h"
#include "common/virtualMemory.h"
#include "kernel/fileSystem.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <string>

namespace Pm4 {

void DumpPm4PacketStream(FILE* f, const uint32_t* cmd_buffer, uint32_t offset, uint32_t num_dw) {
        if (f == nullptr || cmd_buffer == nullptr) {
                return;
        }
        for (uint32_t i = 0; i < num_dw; i++) {
                std::fprintf(f, "%08x: %08x\n", offset + i * 4, cmd_buffer[i]);
        }
}

} // namespace Pm4

namespace Graphics {

bool HostMemoryIsReadable(uint64_t guest_addr) {
        // Guest addresses in the emulated range are backed by host memory;
        // sanity-check the low bits only.
        return guest_addr != 0 && guest_addr < (1ull << 48);
}

} // namespace Graphics

namespace Common::VirtualMemory {

bool AllocFixed(uintptr_t addr, size_t size, Mode mode) {
        (void)mode;
        void* p = mmap(reinterpret_cast<void*>(addr), size,
                       PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        return p != MAP_FAILED;
}

bool Free(uintptr_t addr) {
        return munmap(reinterpret_cast<void*>(addr), 0) == 0;
}

void Free(uintptr_t addr, size_t size) {
        munmap(reinterpret_cast<void*>(addr), size);
}

} // namespace Common::VirtualMemory

namespace Libs::LibKernel {

namespace FileSystem {

// Round 28: the real implementation now lives in file_system.cpp (it resolves
// guest paths through the mount table). This stub was removed.

int KYTY_SYSV_ABI KernelFtruncate(int32_t fd, int64_t length) {
        return ::ftruncate(fd, static_cast<off_t>(length)) == 0 ? 0 : -1;
}

} // namespace FileSystem

} // namespace Libs::LibKernel
