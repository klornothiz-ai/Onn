#pragma once
// ProsperoLayer PS5 emulator - libkernel memory subsystem (Kyty-compatible interface)
#include "common/common.h"
#include <cstdint>
#include <cstddef>

namespace Libs::LibKernel {

namespace Memory {

using MmapCallback = void* (*)(uintptr_t addr, size_t size);
using MmapCallback2 = void* (*)(uintptr_t addr, size_t size, uint32_t type, uint32_t prot);
using MprotectCallback = int (*)(void* addr, size_t size, int prot);
using MunmapCallback = int (*)(void* addr, size_t size);
using MspaceCallback = void* (*)(size_t size, size_t alignment);

int KYTY_SYSV_ABI KernelMmap(void** addr, size_t len, int prot, int flags, int fd, int64_t offset);
int KYTY_SYSV_ABI KernelMunmap(void* addr, size_t len);
int KYTY_SYSV_ABI KernelMprotect(void* addr, size_t len, int prot);
int KYTY_SYSV_ABI KernelQueryMemoryProtection(void* addr, void** start, void** end, int* prot);
int KYTY_SYSV_ABI KernelDirectMemoryQuery(uint64_t offset, int* type, uint64_t* start,
                                          uint64_t* end, int* prot);
void* KYTY_SYSV_ABI KernelGetDirectMemoryOffset(uint64_t offset);
int KYTY_SYSV_ABI KernelReserveDirectMemory(uint64_t start, uint64_t end, int type, uint64_t* out);
int KYTY_SYSV_ABI KernelMapDirectMemory(void* addr, size_t len, int prot, int flags,
                                        uint64_t direct_offset);
int KYTY_SYSV_ABI KernelMapDirectMemory2(void* addr, size_t len, int prot, int flags,
                                         uint64_t direct_offset, int* out_mapped);
int KYTY_SYSV_ABI KernelMapFlexibleMemory(void** addr, size_t len, int prot, int flags,
                                          uint64_t direct_offset);
int KYTY_SYSV_ABI KernelMmapFixed(void* addr, size_t len, int prot, int flags, int fd,
                                  int64_t offset);
int KYTY_SYSV_ABI KernelAllocateDirectMemory(uint64_t start, uint64_t end, uint64_t len,
                                             uint64_t alignment, int type, uint64_t* out);
int KYTY_SYSV_ABI KernelReleaseDirectMemory(uint64_t start, uint64_t len);
uint64_t KYTY_SYSV_ABI KernelGetDirectMemorySize();
uint64_t KYTY_SYSV_ABI KernelAvailableDirectMemorySize();
int KYTY_SYSV_ABI KernelReserveVirtualRange(void** addr, uint64_t len, int flags, uint64_t alignment);
int KYTY_SYSV_ABI KernelSetVirtualRangeName(void* addr, uint64_t len, const char* name);
int KYTY_SYSV_ABI KernelQueryVirtualRangeProtection(void* addr, uint64_t* start, uint64_t* end,
                                                    int* prot);
void* KYTY_SYSV_ABI KernelMspaceMalloc(size_t size);
void* KYTY_SYSV_ABI KernelMspaceCalloc(size_t num, size_t size);
void* KYTY_SYSV_ABI KernelMspaceRealloc(void* ptr, size_t size);
void KYTY_SYSV_ABI KernelMspaceFree(void* ptr);
void* KYTY_SYSV_ABI KernelMspaceMemalign(size_t alignment, size_t size);
int KYTY_SYSV_ABI KernelMspaceMallocStats(void* out_stats, size_t stats_size);

// Extended memory-management surface (Kyty-compatible).
int      KYTY_SYSV_ABI KernelMtypeprotect(void* addr, size_t len, int prot, int type);
int      KYTY_SYSV_ABI KernelVirtualQuery(const void* addr, int* type, void** start, void** end,
                                          int* prot);
uint64_t KYTY_SYSV_ABI KernelConfiguredFlexibleMemorySize();
uint64_t KYTY_SYSV_ABI KernelAvailableFlexibleMemorySize();
int      KYTY_SYSV_ABI KernelMapNamedFlexibleMemory(void** addr, size_t len, int prot, int flags,
                                                    uint64_t direct_offset, const char* name);
int      KYTY_SYSV_ABI KernelMapNamedDirectMemory(void* addr, size_t len, int prot, int flags,
                                                  uint64_t direct_offset, const char* name);
int      KYTY_SYSV_ABI KernelAllocateMainDirectMemory(uint64_t start, uint64_t end, uint64_t len,
                                                      uint64_t alignment, int type, uint64_t* out);
int      KYTY_SYSV_ABI KernelCheckedReleaseDirectMemory(uint64_t start, uint64_t len);
int      KYTY_SYSV_ABI KernelBatchMap(void** addr, size_t len, int prot, int flags,
                                      const uint64_t* direct_offset, size_t count);
int      KYTY_SYSV_ABI KernelBatchMap2(void** addr, size_t len, int prot, int flags,
                                       const uint64_t* direct_offset, size_t count);
int      KYTY_SYSV_ABI KernelMemoryPoolBatch(int op, void* addr, size_t len, uint64_t* out);
int      KYTY_SYSV_ABI KernelMemoryPoolCommit(void* addr, size_t len, int prot);
int      KYTY_SYSV_ABI KernelMemoryPoolDecommit(void* addr, size_t len);
int      KYTY_SYSV_ABI KernelMemoryPoolExpand(void* addr, size_t len);
int      KYTY_SYSV_ABI KernelMemoryPoolReserve(uint64_t start, uint64_t end, uint64_t len,
                                               uint64_t alignment, int type, uint64_t* out);
int      KYTY_SYSV_ABI KernelMemoryPoolGetBlockStats(void* addr, void* out_stats,
                                                     size_t stats_size);
bool     KYTY_SYSV_ABI KernelIsStack(void* addr);
bool     KYTY_SYSV_ABI KernelIsAddressSanitizerEnabled();
uint64_t KYTY_SYSV_ABI KernelGetPageTableStats();
void*    KYTY_SYSV_ABI KernelGetPrtAperture(uint64_t* size);
void     KYTY_SYSV_ABI KernelSetPrtAperture(void* addr, uint64_t size);

void RegisterCallbacks(MmapCallback mmap_cb, MunmapCallback munmap_cb,
                       MprotectCallback mprotect_cb, MspaceCallback mspace_cb);
void RegisterMmapCallback2(MmapCallback2 mmap_cb);

} // namespace Memory

} // namespace Libs::LibKernel
