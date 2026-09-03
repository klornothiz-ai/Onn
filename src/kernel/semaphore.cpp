// ProsperoLayer PS5 emulator - libkernel semaphore subsystem implementation
#include "kernel/semaphore.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <unordered_map>

namespace Libs::LibKernel {

namespace Semaphore {

namespace {

struct KernelSemaData {
        std::string                          name;
        int                                  count{0};
        int                                  max_count{0};
        std::mutex                           mutex;
        std::condition_variable              cv;
};

std::mutex                              g_mutex;
std::unordered_map<int32_t, std::unique_ptr<KernelSemaData>> g_semas;
int32_t                                 g_next{1};

KernelSemaData* Find(KernelSema sem) {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_semas.find(sem);
        return it != g_semas.end() ? it->second.get() : nullptr;
}

} // namespace

int KYTY_SYSV_ABI KernelCreateSema(KernelSema* sem, const char* name, uint32_t attr, int init_count,
                                   int max_count, const void* parent) {
        (void)attr;
        (void)parent;
        if (sem == nullptr || max_count <= 0 || init_count < 0 || init_count > max_count) {
                return static_cast<int>(0x80020002);
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        const int32_t id = g_next++;
        auto d = std::make_unique<KernelSemaData>();
        d->name      = name != nullptr ? name : "";
        d->count     = init_count;
        d->max_count = max_count;
        g_semas.emplace(id, std::move(d));
        *sem = id;
        return 0;
}

int KYTY_SYSV_ABI KernelDeleteSema(KernelSema sem) {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_semas.erase(sem) > 0 ? 0 : static_cast<int>(0x80020005);
}

int KYTY_SYSV_ABI KernelWaitSema(KernelSema sem, int count, uint32_t* timeout) {
        auto* d = Find(sem);
        if (d == nullptr || count <= 0) {
                return static_cast<int>(0x80020002);
        }
        std::unique_lock<std::mutex> lock(d->mutex);
        auto pred = [d, count]() { return d->count >= count; };
        bool ok   = false;
        if (timeout != nullptr && *timeout != UINT32_MAX) {
                ok = d->cv.wait_for(lock, std::chrono::microseconds(*timeout), pred);
        } else {
                d->cv.wait(lock, pred);
                ok = true;
        }
        if (ok) {
                d->count -= count;
                return 0;
        }
        return static_cast<int>(0x8002000e); // ETIMEDOUT-ish
}

int KYTY_SYSV_ABI KernelSignalSema(KernelSema sem, int count) {
        auto* d = Find(sem);
        if (d == nullptr || count <= 0) {
                return static_cast<int>(0x80020002);
        }
        {
                std::lock_guard<std::mutex> lock(d->mutex);
                d->count += count;
                if (d->count > d->max_count) {
                        d->count = d->max_count;
                }
        }
        d->cv.notify_all();
        return 0;
}

int KYTY_SYSV_ABI KernelPollSema(KernelSema sem, int count) {
        auto* d = Find(sem);
        if (d == nullptr || count <= 0) {
                return static_cast<int>(0x80020002);
        }
        std::lock_guard<std::mutex> lock(d->mutex);
        if (d->count < count) {
                return static_cast<int>(0x8002000e);
        }
        d->count -= count;
        return 0;
}

int KYTY_SYSV_ABI KernelCancelSema(KernelSema sem, int set_count, uint32_t* old_count) {
        auto* d = Find(sem);
        if (d == nullptr) {
                return static_cast<int>(0x80020002);
        }
        std::lock_guard<std::mutex> lock(d->mutex);
        if (old_count != nullptr) {
                *old_count = static_cast<uint32_t>(d->count);
        }
        d->count = set_count;
        d->cv.notify_all();
        return 0;
}

int KYTY_SYSV_ABI PthreadSemInit(void* sem, int shared, unsigned int value) {
        (void)shared;
        if (sem == nullptr) {
                return -1;
        }
        // Guest semaphore storage is a 4-byte counter; we store the init value
        // directly. Post/Wait operate on the same guest word.
        *static_cast<uint32_t*>(sem) = value;
        return 0;
}

int KYTY_SYSV_ABI PthreadSemDestroy(void* sem) {
        (void)sem;
        return 0;
}

int KYTY_SYSV_ABI PthreadSemWait(void* sem) {
        if (sem == nullptr) {
                return -1;
        }
        auto* v = static_cast<uint32_t*>(sem);
        while (*v == 0) {
                std::this_thread::yield();
        }
        --(*v);
        return 0;
}

int KYTY_SYSV_ABI PthreadSemTrywait(void* sem) {
        if (sem == nullptr) {
                return -1;
        }
        auto* v = static_cast<uint32_t*>(sem);
        if (*v == 0) {
                return -1; // EAGAIN
        }
        --(*v);
        return 0;
}

int KYTY_SYSV_ABI PthreadSemTimedwait(void* sem, const void* abstime) {
        (void)abstime;
        return PthreadSemWait(sem);
}

int KYTY_SYSV_ABI PthreadSemPost(void* sem) {
        if (sem == nullptr) {
                return -1;
        }
        ++(*static_cast<uint32_t*>(sem));
        return 0;
}

int KYTY_SYSV_ABI PthreadSemGetvalue(void* sem, int* value) {
        if (sem == nullptr || value == nullptr) {
                return -1;
        }
        *value = static_cast<int>(*static_cast<uint32_t*>(sem));
        return 0;
}

} // namespace Semaphore

} // namespace Libs::LibKernel
