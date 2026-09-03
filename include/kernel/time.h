#pragma once
// ProsperoLayer PS5 emulator - libkernel time subsystem (Kyty-compatible interface)
#include "common/common.h"
#include <cstdint>

namespace Libs::LibKernel {

struct KernelTimespec {
        int64_t tv_sec;
        int64_t tv_nsec;
};

struct KernelTimeval {
        int64_t tv_sec;
        int64_t tv_usec;
};

int KYTY_SYSV_ABI KernelClockGettime(int clock_id, KernelTimespec* time);
int KYTY_SYSV_ABI KernelClockGetres(int clock_id, KernelTimespec* res);
int KYTY_SYSV_ABI KernelGettimeofday(KernelTimeval* time);
int KYTY_SYSV_ABI KernelNanosleep(const KernelTimespec* rqtp, KernelTimespec* rmtp);
int KYTY_SYSV_ABI KernelUsleep(uint32_t microseconds);
int KYTY_SYSV_ABI KernelSleep(uint32_t seconds);
int KYTY_SYSV_ABI KernelGettimezone(int* timezone, int* dst, uint32_t* tz_dst_time,
                                    uint32_t* tz_std_time);
int KYTY_SYSV_ABI KernelConvertUtcToLocaltime(uint64_t utc, uint64_t* local);
int KYTY_SYSV_ABI KernelConvertLocaltimeToUtc(uint64_t local, uint64_t* utc);
uint64_t KYTY_SYSV_ABI KernelGetProcessTime();
uint64_t KYTY_SYSV_ABI KernelGetProcessTimeCounterFrequency();
uint64_t KYTY_SYSV_ABI KernelGetProcessTimeCounter();
uint64_t KYTY_SYSV_ABI KernelGetTscFrequency();
uint64_t KYTY_SYSV_ABI KernelReadTsc();

} // namespace Libs::LibKernel
