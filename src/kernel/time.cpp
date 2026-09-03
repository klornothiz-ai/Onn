// ProsperoLayer PS5 emulator - libkernel time subsystem implementation
#include "kernel/time.h"
#include <chrono>
#include <ctime>
#include <thread>

namespace Libs::LibKernel {

namespace {

constexpr uint64_t USEC_PER_SEC  = 1000000ull;
constexpr uint64_t NSEC_PER_USEC = 1000ull;
constexpr uint64_t NSEC_PER_SEC  = 1000000000ull;

uint64_t NowMicroseconds() {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

} // namespace

int KYTY_SYSV_ABI KernelClockGettime(int clock_id, KernelTimespec* time) {
        (void)clock_id;
        if (time == nullptr) {
                return -1;
        }
        const auto now = NowMicroseconds();
        time->tv_sec  = static_cast<int64_t>(now / USEC_PER_SEC);
        time->tv_nsec = static_cast<int64_t>((now % USEC_PER_SEC) * NSEC_PER_USEC);
        return 0;
}

int KYTY_SYSV_ABI KernelClockGetres(int clock_id, KernelTimespec* res) {
        (void)clock_id;
        if (res == nullptr) {
                return -1;
        }
        res->tv_sec  = 0;
        res->tv_nsec = 1000; // 1 usec resolution
        return 0;
}

int KYTY_SYSV_ABI KernelGettimeofday(KernelTimeval* time) {
        if (time == nullptr) {
                return -1;
        }
        const auto now = NowMicroseconds();
        time->tv_sec  = static_cast<int64_t>(now / USEC_PER_SEC);
        time->tv_usec = static_cast<int64_t>(now % USEC_PER_SEC);
        return 0;
}

int KYTY_SYSV_ABI KernelNanosleep(const KernelTimespec* rqtp, KernelTimespec* rmtp) {
        if (rqtp == nullptr) {
                return -1;
        }
        const auto requested_us =
            rqtp->tv_sec * static_cast<int64_t>(USEC_PER_SEC) + rqtp->tv_nsec / 1000;
        if (requested_us > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(requested_us));
        }
        if (rmtp != nullptr) {
                rmtp->tv_sec  = 0;
                rmtp->tv_nsec = 0;
        }
        return 0;
}

int KYTY_SYSV_ABI KernelUsleep(uint32_t microseconds) {
        if (microseconds > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
        }
        return 0;
}

int KYTY_SYSV_ABI KernelSleep(uint32_t seconds) {
        if (seconds > 0) {
                std::this_thread::sleep_for(std::chrono::seconds(seconds));
        }
        return 0;
}

uint64_t KYTY_SYSV_ABI KernelReadTsc() {
        // Host monotonic nanosecond counter.
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

uint64_t KYTY_SYSV_ABI KernelGetTscFrequency() {
        return 1000000000ull; // 1 GHz virtual TSC
}

int KYTY_SYSV_ABI KernelGettimezone(int* timezone, int* dst, uint32_t* tz_dst_time,
                                    uint32_t* tz_std_time) {
        if (timezone != nullptr) {
                *timezone = 0; // UTC
        }
        if (dst != nullptr) {
                *dst = 0;
        }
        if (tz_dst_time != nullptr) {
                *tz_dst_time = 0;
        }
        if (tz_std_time != nullptr) {
                *tz_std_time = 0;
        }
        return 0;
}

int KYTY_SYSV_ABI KernelConvertUtcToLocaltime(uint64_t utc, uint64_t* local) {
        if (local == nullptr) {
                return -1;
        }
        *local = utc; // UTC == local in default config
        return 0;
}

int KYTY_SYSV_ABI KernelConvertLocaltimeToUtc(uint64_t local, uint64_t* utc) {
        if (utc == nullptr) {
                return -1;
        }
        *utc = local;
        return 0;
}

uint64_t KYTY_SYSV_ABI KernelGetProcessTime() {
        return NowMicroseconds();
}

uint64_t KYTY_SYSV_ABI KernelGetProcessTimeCounterFrequency() {
        return 1000000ull; // 1 MHz virtual counter
}

uint64_t KYTY_SYSV_ABI KernelGetProcessTimeCounter() {
        return NowMicroseconds();
}

} // namespace Libs::LibKernel
