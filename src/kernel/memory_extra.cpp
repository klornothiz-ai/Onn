// ProsperoLayer PS5 emulator - libkernel memory subsystem extended API
// Host-backed implementations for the direct/flexible memory management
// surface used by the guest libKernel NIDs.
#include "kernel/memory.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <mutex>

namespace Libs::LibKernel {

namespace Memory {

namespace {

std::mutex g_cb_mutex;
MmapCallback   g_mmap_cb{nullptr};
MunmapCallback g_munmap_cb{nullptr};
MprotectCallback g_mprotect_cb{nullptr};
MspaceCallback g_mspace_cb{nullptr};

constexpr uint64_t SCE_KERNEL_GB = 1024ull * 1024ull * 1024ull;

void* DirectHostAddr(uint64_t offset) {
        // Map the direct-memory window onto the host address space 1:1 from
        // the reserved 16 GB region (see InitializeArena).
        static void* base = nullptr;
        if (base == nullptr) {
                base = mmap(nullptr, 16ull * SCE_KERNEL_GB, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
                if (base == MAP_FAILED) {
                        base = nullptr;
                }
        }
        if (base == nullptr) {
                return nullptr;
        }
        return static_cast<char*>(base) + offset;
}

} // namespace

int KYTY_SYSV_ABI KernelMmap(void** addr, size_t len, int prot, int flags, int fd, int64_t offset) {
        (void)flags;
        if (addr == nullptr) {
                return static_cast<int>(0x80020002);
        }
        void* p = mmap(nullptr, len, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
                return -1;
        }
        (void)fd;
        (void)offset;
        *addr = p;
        return 0;
}

int KYTY_SYSV_ABI KernelMunmap(void* addr, size_t len) {
        if (munmap(addr, len) != 0) {
                return -1;
        }
        return 0;
}

uint64_t KYTY_SYSV_ABI KernelGetDirectMemorySize() {
        // Guest-visible direct memory window (PS5: 12 GB usable).
        return 12ull * SCE_KERNEL_GB;
}

uint64_t KYTY_SYSV_ABI KernelAvailableDirectMemorySize() {
        return 12ull * SCE_KERNEL_GB;
}

int KYTY_SYSV_ABI KernelAllocateDirectMemory(uint64_t start, uint64_t end, uint64_t len,
                                             uint64_t alignment, int type, uint64_t* out) {
        (void)end;
        (void)type;
        if (out == nullptr) {
                return static_cast<int>(0x80020002);
        }
        static uint64_t cursor = 0x100000000ull; // 4 GB
        if (alignment == 0) {
                alignment = 0x400000ull; // 4 MB
        }
        cursor = (cursor + alignment - 1) & ~(alignment - 1);
        (void)start;
        *out = cursor;
        cursor += len;
        return 0;
}

int KYTY_SYSV_ABI KernelAllocateMainDirectMemory(uint64_t start, uint64_t end, uint64_t len,
                                                 uint64_t alignment, int type, uint64_t* out) {
        return KernelAllocateDirectMemory(start, end, len, alignment, type, out);
}

int KYTY_SYSV_ABI KernelReleaseDirectMemory(uint64_t start, uint64_t len) {
        (void)start;
        (void)len;
        return 0;
}

int KYTY_SYSV_ABI KernelMapDirectMemory(void* addr, size_t len, int prot, int flags,
                                        uint64_t direct_offset) {
        (void)flags;
        (void)direct_offset;
        if (addr == nullptr) {
                return static_cast<int>(0x80020002);
        }
        void* p = mmap(addr, len, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        return p == MAP_FAILED ? -1 : 0;
}

int KYTY_SYSV_ABI KernelMapDirectMemory2(void* addr, size_t len, int prot, int flags,
                                         uint64_t direct_offset, int* out_mapped) {
        const int rc = KernelMapDirectMemory(addr, len, prot, flags, direct_offset);
        if (out_mapped != nullptr) {
                *out_mapped = rc == 0 ? 1 : 0;
        }
        return rc;
}

int KYTY_SYSV_ABI KernelMapFlexibleMemory(void** addr, size_t len, int prot, int flags,
                                          uint64_t direct_offset) {
        (void)direct_offset;
        return KernelMmap(addr, len, prot, flags, -1, 0);
}

int KYTY_SYSV_ABI KernelMapNamedFlexibleMemory(void** addr, size_t len, int prot, int flags,
                                               uint64_t direct_offset, const char* name) {
        (void)name;
        return KernelMapFlexibleMemory(addr, len, prot, flags, direct_offset);
}

int KYTY_SYSV_ABI KernelMapNamedDirectMemory(void* addr, size_t len, int prot, int flags,
                                             uint64_t direct_offset, const char* name) {
        (void)name;
        return KernelMapDirectMemory(addr, len, prot, flags, direct_offset);
}

int KYTY_SYSV_ABI KernelVirtualQuery(const void* addr, int* type, void** start, void** end,
                                     int* prot) {
        (void)addr;
        if (type != nullptr) {
                *type = 0;
        }
        if (start != nullptr) {
                *start = nullptr;
        }
        if (end != nullptr) {
                *end = nullptr;
        }
        if (prot != nullptr) {
                *prot = 3;
        }
        return 0;
}

bool KYTY_SYSV_ABI KernelIsStack(void* addr) {
        (void)addr;
        return false;
}

int KYTY_SYSV_ABI KernelReserveVirtualRange(void** addr, uint64_t len, int flags,
                                            uint64_t alignment) {
        (void)flags;
        (void)alignment;
        return KernelMmap(addr, len, 0, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

int KYTY_SYSV_ABI KernelSetVirtualRangeName(void* addr, uint64_t len, const char* name) {
        (void)addr;
        (void)len;
        (void)name;
        return 0;
}

void KYTY_SYSV_ABI KernelSetPrtAperture(void* addr, uint64_t size) {
        (void)addr;
        (void)size;
}

uint64_t KYTY_SYSV_ABI KernelGetPageTableStats() {
        return 0;
}

int KYTY_SYSV_ABI KernelMprotect(void* addr, size_t len, int prot) {
        if (mprotect(addr, len, prot) != 0) {
                return -1;
        }
        return 0;
}

int KYTY_SYSV_ABI KernelQueryMemoryProtection(void* addr, void** start, void** end, int* prot) {
        (void)addr;
        if (start != nullptr) {
                *start = nullptr;
        }
        if (end != nullptr) {
                *end = nullptr;
        }
        if (prot != nullptr) {
                *prot = 3; // RW
        }
        return 0;
}

int KYTY_SYSV_ABI KernelDirectMemoryQuery(uint64_t offset, int* type, uint64_t* start,
                                          uint64_t* end, int* prot) {
        (void)offset;
        if (type != nullptr) {
                *type = 0;
        }
        if (start != nullptr) {
                *start = 0;
        }
        if (end != nullptr) {
                *end = KernelGetDirectMemorySize();
        }
        if (prot != nullptr) {
                *prot = 3;
        }
        return 0;
}

int KYTY_SYSV_ABI KernelMtypeprotect(void* addr, size_t len, int prot, int type) {
        (void)type;
        return KernelMprotect(addr, len, prot);
}

uint64_t KYTY_SYSV_ABI KernelConfiguredFlexibleMemorySize() {
        return 12ull * SCE_KERNEL_GB;
}

uint64_t KYTY_SYSV_ABI KernelAvailableFlexibleMemorySize() {
        return 12ull * SCE_KERNEL_GB;
}

int KYTY_SYSV_ABI KernelCheckedReleaseDirectMemory(uint64_t start, uint64_t len) {
        return KernelReleaseDirectMemory(start, len);
}

int KYTY_SYSV_ABI KernelBatchMap(void** addr, size_t len, int prot, int flags,
                                 const uint64_t* direct_offset, size_t count) {
        (void)flags;
        if (addr == nullptr || direct_offset == nullptr || count == 0) {
                return static_cast<int>(0x80020002);
        }
        for (size_t i = 0; i < count; i++) {
                void* host = DirectHostAddr(direct_offset[i]);
                if (host == nullptr) {
                        return -1;
                }
                // Anonymous reservation backing the guest mapping.
                void* p = mmap(nullptr, len, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (p == MAP_FAILED) {
                        return -1;
                }
                (void)host;
                if (i == 0) {
                        *addr = p;
                }
        }
        return 0;
}

int KYTY_SYSV_ABI KernelBatchMap2(void** addr, size_t len, int prot, int flags,
                                  const uint64_t* direct_offset, size_t count) {
        return KernelBatchMap(addr, len, prot, flags, direct_offset, count);
}

int KYTY_SYSV_ABI KernelMemoryPoolBatch(int op, void* addr, size_t len, uint64_t* out) {
        (void)op;
        (void)addr;
        (void)len;
        if (out != nullptr) {
                *out = 0;
        }
        return 0;
}

int KYTY_SYSV_ABI KernelMemoryPoolCommit(void* addr, size_t len, int prot) {
        return KernelMprotect(addr, len, prot);
}

int KYTY_SYSV_ABI KernelMemoryPoolDecommit(void* addr, size_t len) {
        return KernelMprotect(addr, len, PROT_NONE);
}

int KYTY_SYSV_ABI KernelMemoryPoolExpand(void* addr, size_t len) {
        (void)addr;
        (void)len;
        return 0;
}

int KYTY_SYSV_ABI KernelMemoryPoolReserve(uint64_t start, uint64_t end, uint64_t len,
                                          uint64_t alignment, int type, uint64_t* out) {
        (void)end;
        (void)alignment;
        (void)type;
        if (out == nullptr) {
                return static_cast<int>(0x80020002);
        }
        if (start == 0) {
                start = 0x100000000ull; // 4 GB base
        }
        *out = start;
        (void)len;
        return 0;
}

int KYTY_SYSV_ABI KernelMemoryPoolGetBlockStats(void* addr, void* out_stats, size_t stats_size) {
        (void)addr;
        if (out_stats != nullptr && stats_size > 0) {
                std::memset(out_stats, 0, stats_size);
        }
        return 0;
}

bool KYTY_SYSV_ABI KernelIsAddressSanitizerEnabled() {
        return false;
}

void* KYTY_SYSV_ABI KernelGetPrtAperture(uint64_t* size) {
        void* base = DirectHostAddr(0);
        if (size != nullptr) {
                *size = KernelGetDirectMemorySize();
        }
        return base;
}

void RegisterCallbacks(MmapCallback mmap_cb, MunmapCallback munmap_cb,
                       MprotectCallback mprotect_cb, MspaceCallback mspace_cb) {
        std::lock_guard<std::mutex> lock(g_cb_mutex);
        g_mmap_cb     = mmap_cb;
        g_munmap_cb   = munmap_cb;
        g_mprotect_cb = mprotect_cb;
        g_mspace_cb   = mspace_cb;
}

} // namespace Memory

} // namespace Libs::LibKernel
