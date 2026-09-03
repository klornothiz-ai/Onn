// ProsperoLayer PS5 emulator - libkernel sync-on-address subsystem implementation
#include "kernel/syncOnAddress.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Libs::LibKernel {

namespace SyncOnAddress {

namespace {

struct Waiter {
        std::atomic<bool>  signaled{false};
        std::atomic<bool>  armed{true};
};

std::mutex                           g_mutex;
std::unordered_map<const void*, std::vector<Waiter*>> g_waiters;

} // namespace

template <typename T>
int WaitImpl(volatile T* address, T expected, int64_t timeout_micros,
             void (*dispatch_signal)(void)) {
        if (address == nullptr) return static_cast<int>(0x80020006); // EINVAL-ish
        Waiter w;
        const void* key = reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(address));
        {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_waiters[key].push_back(&w);
        }
        const bool finite = timeout_micros > 0;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::microseconds(finite ? timeout_micros : 0);
        int result = 0;
        while (w.armed.load(std::memory_order_acquire)) {
                if (*address != expected) {
                        w.signaled.store(true, std::memory_order_release);
                        w.armed.store(false, std::memory_order_release);
                        result = 0;
                        break;
                }
                if (finite && std::chrono::steady_clock::now() >= deadline) {
                        w.armed.store(false, std::memory_order_release);
                        result = static_cast<int>(0x8002000e); // ETIMEDOUT-ish
                        break;
                }
                if (dispatch_signal != nullptr) dispatch_signal();
                std::this_thread::yield();
        }
        {
                std::lock_guard<std::mutex> lock(g_mutex);
                auto it = g_waiters.find(key);
                if (it != g_waiters.end()) {
                        auto& vec = it->second;
                        vec.erase(std::remove(vec.begin(), vec.end(), &w), vec.end());
                        if (vec.empty()) g_waiters.erase(it);
                }
        }
        return result;
}

int KYTY_SYSV_ABI Wait32(volatile uint32_t* address, uint32_t expected, int64_t timeout_micros,
                         void (*dispatch_signal)(void)) {
        return WaitImpl<uint32_t>(address, expected, timeout_micros, dispatch_signal);
}

int KYTY_SYSV_ABI Wait64(volatile uint64_t* address, uint64_t expected, int64_t timeout_micros,
                         void (*dispatch_signal)(void)) {
        return WaitImpl<uint64_t>(address, expected, timeout_micros, dispatch_signal);
}

int KYTY_SYSV_ABI Wake(volatile void* address, int32_t count) {
        (void)count;
        const void* key = reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(address));
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_waiters.find(key);
        if (it == g_waiters.end()) {
                return 0;
        }
        for (auto* w : it->second) {
                w->armed.store(false);
                w->signaled.store(true);
        }
        return static_cast<int>(it->second.size());
}

} // namespace SyncOnAddress

} // namespace Libs::LibKernel
