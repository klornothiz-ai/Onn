#pragma once
// ProsperoLayer PS5 emulator - libkernel semaphore subsystem (Kyty-compatible interface)
#include "common/common.h"
#include <cstdint>

namespace Libs::LibKernel {

namespace Semaphore {

using KernelSema = int32_t;

constexpr uint32_t SCE_KERNEL_SEMA_ATTR_THREAD = 0x01u;

int KYTY_SYSV_ABI KernelCreateSema(KernelSema* sem, const char* name, uint32_t attr, int init_count,
                                   int max_count, const void* parent);
int KYTY_SYSV_ABI KernelDeleteSema(KernelSema sem);
int KYTY_SYSV_ABI KernelWaitSema(KernelSema sem, int count, uint32_t* timeout);
int KYTY_SYSV_ABI KernelSignalSema(KernelSema sem, int count);
int KYTY_SYSV_ABI KernelPollSema(KernelSema sem, int count);
int KYTY_SYSV_ABI KernelCancelSema(KernelSema sem, int set_count, uint32_t* old_count);

// Guest-side semaphore helpers (Kyty-compatible).
int KYTY_SYSV_ABI PthreadSemInit(void* sem, int shared, unsigned int value);
int KYTY_SYSV_ABI PthreadSemDestroy(void* sem);
int KYTY_SYSV_ABI PthreadSemWait(void* sem);
int KYTY_SYSV_ABI PthreadSemTrywait(void* sem);
int KYTY_SYSV_ABI PthreadSemTimedwait(void* sem, const void* abstime);
int KYTY_SYSV_ABI PthreadSemPost(void* sem);
int KYTY_SYSV_ABI PthreadSemGetvalue(void* sem, int* value);

} // namespace Semaphore

} // namespace Libs::LibKernel
