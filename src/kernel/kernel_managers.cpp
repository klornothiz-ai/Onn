// ProsperoLayer PS5 emulator - syscall-facing kernel managers
// Implementation of the PS5::Kernel::EventFlagManager / SemaphoreManager
// used by the syscall dispatcher (prospero_syscalls.cpp).
#include "kernel/event_flag.hpp"
#include "kernel/semaphore.hpp"

#include <chrono>
#include <mutex>

namespace PS5::Kernel {

EventFlag::EventFlag(const std::string& name, uint32_t attr, uint64_t init_pattern)
    : m_name(name), m_attr(attr), m_pattern(init_pattern) {}

uint64_t EventFlag::Set(uint64_t bit_pattern) {
        {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_pattern |= bit_pattern;
        }
        m_cv.notify_all();
        return m_pattern;
}

uint64_t EventFlag::Clear(uint64_t bit_pattern) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pattern &= ~bit_pattern;
        return m_pattern;
}

bool EventFlag::Wait(uint64_t bit_pattern, uint32_t wait_mode, uint64_t* out_bits,
                     uint32_t timeout_usec) {
        std::unique_lock<std::mutex> lock(m_mutex);
        const uint64_t enter_epoch = m_cancel_epoch;
        const auto pred = [this, bit_pattern, wait_mode, enter_epoch]() {
                // A Cancel bumps the epoch and must release the waiter even
                // though the bit condition is not satisfied.
                if (m_cancel_epoch != enter_epoch) {
                        return true;
                }
                const uint64_t matched = m_pattern & bit_pattern;
                if ((wait_mode & SCE_KERNEL_EVF_WAITMODE_OR) != 0) {
                        return matched != 0;
                }
                return matched == bit_pattern;
        };
        bool ok = false;
        if (timeout_usec == 0) {
                m_cv.wait(lock, pred);
                ok = true;
        } else {
                ok = m_cv.wait_for(lock, std::chrono::microseconds(timeout_usec), pred);
        }
        // A cancelled wait returns failure regardless of the bit pattern.
        if (m_cancel_epoch != enter_epoch) {
                return false;
        }
        if (ok) {
                if (out_bits != nullptr) {
                        *out_bits = m_pattern & bit_pattern;
                }
                if ((wait_mode & SCE_KERNEL_EVF_WAITMODE_CLEAR_ALL) != 0) {
                        m_pattern = 0;
                } else if ((wait_mode & SCE_KERNEL_EVF_WAITMODE_CLEAR_PAT) != 0) {
                        m_pattern &= ~bit_pattern;
                }
        }
        return ok;
}

uint64_t EventFlag::Cancel(uint64_t set_pattern) {
        uint64_t previous;
        {
                std::lock_guard<std::mutex> lock(m_mutex);
                previous = m_pattern;
                ++m_cancel_epoch;
                m_pattern = set_pattern;
        }
        m_cv.notify_all();
        return previous;
}

uint64_t EventFlag::GetCurrentPattern() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_pattern;
}

EventFlagManager& EventFlagManager::Instance() {
        static EventFlagManager inst;
        return inst;
}

uint32_t EventFlagManager::Create(const std::string& name, uint32_t attr, uint64_t init_pattern) {
        std::lock_guard<std::mutex> lock(m_mgr_mutex);
        const uint32_t h = ++m_handle_counter;
        m_flags[h] = std::make_shared<EventFlag>(name, attr, init_pattern);
        return h;
}

bool EventFlagManager::Delete(uint32_t handle) {
        std::lock_guard<std::mutex> lock(m_mgr_mutex);
        return m_flags.erase(handle) > 0;
}

bool EventFlagManager::Set(uint32_t handle, uint64_t bits) {
        std::shared_ptr<EventFlag> flag;
        {
                std::lock_guard<std::mutex> lock(m_mgr_mutex);
                const auto it = m_flags.find(handle);
                if (it == m_flags.end()) {
                        return false;
                }
                flag = it->second;
        }
        flag->Set(bits);
        return true;
}

bool EventFlagManager::Clear(uint32_t handle, uint64_t bits) {
        std::shared_ptr<EventFlag> flag;
        {
                std::lock_guard<std::mutex> lock(m_mgr_mutex);
                const auto it = m_flags.find(handle);
                if (it == m_flags.end()) {
                        return false;
                }
                flag = it->second;
        }
        flag->Clear(bits);
        return true;
}

bool EventFlagManager::Wait(uint32_t handle, uint64_t bits, uint32_t mode, uint64_t* out_bits,
                            uint32_t timeout_usec) {
        std::shared_ptr<EventFlag> flag;
        {
                std::lock_guard<std::mutex> lock(m_mgr_mutex);
                const auto it = m_flags.find(handle);
                if (it == m_flags.end()) {
                        return false;
                }
                flag = it->second;
        }
        return flag->Wait(bits, mode, out_bits, timeout_usec);
}

bool EventFlagManager::Cancel(uint32_t handle, uint64_t set_pattern, uint64_t* out_pattern) {
        std::shared_ptr<EventFlag> flag;
        {
                std::lock_guard<std::mutex> lock(m_mgr_mutex);
                const auto it = m_flags.find(handle);
                if (it == m_flags.end()) {
                        return false;
                }
                flag = it->second;
        }
        const uint64_t previous = flag->Cancel(set_pattern);
        if (out_pattern != nullptr) {
                *out_pattern = previous;
        }
        return true;
}

Semaphore::Semaphore(const std::string& name, int init_count, int max_count)
    : m_name(name), m_count(init_count), m_max_count(max_count) {}

bool Semaphore::Wait(int count, uint32_t timeout_usec) {
        std::unique_lock<std::mutex> lock(m_mutex);
        const auto pred = [this, count]() { return m_count >= count; };
        bool ok = false;
        if (timeout_usec == 0) {
                m_cv.wait(lock, pred);
                ok = true;
        } else {
                ok = m_cv.wait_for(lock, std::chrono::microseconds(timeout_usec), pred);
        }
        if (ok) {
                m_count -= count;
        }
        return ok;
}

bool Semaphore::Signal(int count) {
        {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_count + count > m_max_count) {
                        return false;
                }
                m_count += count;
        }
        m_cv.notify_all();
        return true;
}

int Semaphore::GetCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_count;
}

SemaphoreManager& SemaphoreManager::Instance() {
        static SemaphoreManager inst;
        return inst;
}

uint32_t SemaphoreManager::Create(const std::string& name, int init_count, int max_count) {
        std::lock_guard<std::mutex> lock(m_mgr_mutex);
        const uint32_t h = ++m_handle_counter;
        m_semaphores[h] = std::make_shared<Semaphore>(name, init_count, max_count);
        return h;
}

bool SemaphoreManager::Delete(uint32_t handle) {
        std::lock_guard<std::mutex> lock(m_mgr_mutex);
        return m_semaphores.erase(handle) > 0;
}

bool SemaphoreManager::Wait(uint32_t handle, int count, uint32_t timeout_usec) {
        std::shared_ptr<Semaphore> sema;
        {
                std::lock_guard<std::mutex> lock(m_mgr_mutex);
                const auto it = m_semaphores.find(handle);
                if (it == m_semaphores.end()) {
                        return false;
                }
                sema = it->second;
        }
        return sema->Wait(count, timeout_usec);
}

bool SemaphoreManager::Signal(uint32_t handle, int count) {
        std::shared_ptr<Semaphore> sema;
        {
                std::lock_guard<std::mutex> lock(m_mgr_mutex);
                const auto it = m_semaphores.find(handle);
                if (it == m_semaphores.end()) {
                        return false;
                }
                sema = it->second;
        }
        return sema->Signal(count);
}

} // namespace PS5::Kernel
