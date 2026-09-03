// ProsperoLayer PS5 emulator - libkernel event flag subsystem implementation
#include "kernel/eventFlag.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <unordered_map>

namespace Libs::LibKernel {

namespace EventFlag {

namespace {

struct KernelEventFlagData {
        std::string                    name;
        uint32_t                       attr{0};
        uint64_t                       pattern{0};
        std::mutex                     mutex;
        std::condition_variable        cv;
};

std::mutex                                g_mutex;
std::unordered_map<int32_t, std::unique_ptr<KernelEventFlagData>> g_flags;
int32_t                                   g_next{1};

KernelEventFlagData* Find(KernelEventFlag efa) {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_flags.find(efa);
        return it != g_flags.end() ? it->second.get() : nullptr;
}

uint64_t CheckCondition(const KernelEventFlagData& d, uint64_t bits, uint32_t wait_mode) {
        const uint64_t matched = d.pattern & bits;
        if ((wait_mode & SCE_KERNEL_EVF_WAITMODE_OR) != 0) {
                return matched != 0 ? matched : 0;
        }
        // AND mode: all requested bits must be set.
        return (matched == bits) ? matched : 0;
}

} // namespace

int KYTY_SYSV_ABI KernelCreateEventFlag(KernelEventFlag* efa, const char* name, uint32_t attr,
                                        uint64_t init_pattern, const void* parent) {
        (void)parent;
        if (efa == nullptr) {
                return static_cast<int>(0x80020002);
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        const int32_t id = g_next++;
        auto d = std::make_unique<KernelEventFlagData>();
        d->name    = name != nullptr ? name : "";
        d->attr    = attr;
        d->pattern = init_pattern;
        g_flags.emplace(id, std::move(d));
        *efa = id;
        return 0;
}

int KYTY_SYSV_ABI KernelDeleteEventFlag(KernelEventFlag efa) {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_flags.erase(efa) > 0 ? 0 : static_cast<int>(0x80020005);
}

int KYTY_SYSV_ABI KernelSetEventFlag(KernelEventFlag efa, uint64_t bits) {
        auto* d = Find(efa);
        if (d == nullptr) {
                return static_cast<int>(0x80020005);
        }
        {
                std::lock_guard<std::mutex> lock(d->mutex);
                d->pattern |= bits;
        }
        d->cv.notify_all();
        return 0;
}

int KYTY_SYSV_ABI KernelClearEventFlag(KernelEventFlag efa, uint64_t bits) {
        auto* d = Find(efa);
        if (d == nullptr) {
                return static_cast<int>(0x80020005);
        }
        std::lock_guard<std::mutex> lock(d->mutex);
        d->pattern &= ~bits;
        return 0;
}

int KYTY_SYSV_ABI KernelPollEventFlag(KernelEventFlag efa, uint64_t bits, uint32_t wait_mode,
                                      uint64_t* out_bits) {
        auto* d = Find(efa);
        if (d == nullptr) {
                return static_cast<int>(0x80020005);
        }
        std::lock_guard<std::mutex> lock(d->mutex);
        const uint64_t matched = CheckCondition(*d, bits, wait_mode);
        if (matched == 0 && (wait_mode & SCE_KERNEL_EVF_WAITMODE_OR) == 0) {
                // AND mode with partial match.
                if ((d->pattern & bits) != 0) {
                        return static_cast<int>(0x8002000f); // EBUSY-ish
                }
        }
        if (matched == 0) {
                return static_cast<int>(0x8002000e); // ETIMEDOUT-ish
        }
        if (out_bits != nullptr) {
                *out_bits = matched;
        }
        if ((wait_mode & SCE_KERNEL_EVF_WAITMODE_CLEAR_ALL) != 0) {
                d->pattern = 0;
        } else if ((wait_mode & SCE_KERNEL_EVF_WAITMODE_CLEAR_PAT) != 0) {
                d->pattern &= ~bits;
        }
        return 0;
}

int KYTY_SYSV_ABI KernelWaitEventFlag(KernelEventFlag efa, uint64_t bits, uint32_t wait_mode,
                                      uint64_t* out_bits, uint32_t* timeout) {
        auto* d = Find(efa);
        if (d == nullptr) {
                return static_cast<int>(0x80020005);
        }
        std::unique_lock<std::mutex> lock(d->mutex);
        auto pred = [d, bits, wait_mode]() {
                return CheckCondition(*d, bits, wait_mode) != 0;
        };
        bool ok = false;
        if (timeout != nullptr && *timeout != UINT32_MAX) {
                ok = d->cv.wait_for(lock, std::chrono::microseconds(*timeout), pred);
        } else {
                d->cv.wait(lock, pred);
                ok = true;
        }
        if (!ok) {
                return static_cast<int>(0x8002000e);
        }
        const uint64_t matched = CheckCondition(*d, bits, wait_mode);
        if (out_bits != nullptr) {
                *out_bits = matched;
        }
        if ((wait_mode & SCE_KERNEL_EVF_WAITMODE_CLEAR_ALL) != 0) {
                d->pattern = 0;
        } else if ((wait_mode & SCE_KERNEL_EVF_WAITMODE_CLEAR_PAT) != 0) {
                d->pattern &= ~bits;
        }
        return 0;
}

int KYTY_SYSV_ABI KernelCancelEventFlag(KernelEventFlag efa, uint64_t* out_bits,
                                        int32_t num_cancel, uint32_t* canceled_patterns) {
        (void)num_cancel;
        (void)canceled_patterns;
        auto* d = Find(efa);
        if (d == nullptr) {
                return static_cast<int>(0x80020005);
        }
        {
                std::lock_guard<std::mutex> lock(d->mutex);
                if (out_bits != nullptr) {
                        *out_bits = d->pattern;
                }
        }
        d->cv.notify_all();
        return 0;
}

} // namespace EventFlag

} // namespace Libs::LibKernel
