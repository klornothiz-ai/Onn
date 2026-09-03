#pragma once
#include "common/common.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace Kyty::Core {
    using Mutex = std::mutex;
    using CondVar = std::condition_variable;
    using Thread = std::thread;
}

namespace Common {

// Kyty-style mutex wrapper: provides both Lock()/Unlock() (Kyty API) and
// lock()/unlock() (std API) so both calling conventions compile.
class Mutex {
public:
        Mutex() = default;
        ~Mutex() = default;
        Mutex(const Mutex&) = delete;
        Mutex& operator=(const Mutex&) = delete;

        void Lock() { m_mutex.lock(); }
        void Unlock() { m_mutex.unlock(); }
        void lock() { m_mutex.lock(); }
        void unlock() { m_mutex.unlock(); }
        bool try_lock() { return m_mutex.try_lock(); }

private:
        std::mutex m_mutex;
};

class LockGuard {
public:
        explicit LockGuard(Mutex& m): m_mutex(m) { m_mutex.Lock(); }
        ~LockGuard() { m_mutex.Unlock(); }
        LockGuard(const LockGuard&) = delete;
        LockGuard& operator=(const LockGuard&) = delete;

private:
        Mutex& m_mutex;
};

class Thread {
public:
        Thread() = default;

        template <typename Fn, typename... Args>
        explicit Thread(Fn&& fn, Args&&... args)
            : m_thread(std::forward<Fn>(fn), std::forward<Args>(args)...) {}

        static int GetThreadIdUnique() { return 1; }
        static void SleepMicro(uint64_t microseconds) {
                std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
        }
        static void SleepMilli(uint64_t milliseconds) {
                std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
        }
        void Detach() { m_thread.detach(); }
        void Join() { m_thread.join(); }

private:
        std::thread m_thread;
};

} // namespace Common
