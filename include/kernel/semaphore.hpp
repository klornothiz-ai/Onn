#pragma once
#include <cstdint>
#include <string>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <memory>

namespace PS5::Kernel {

    class Semaphore {
    public:
        Semaphore(const std::string& name, int init_count, int max_count);

        bool Wait(int count, uint32_t timeout_usec = 0);
        bool Signal(int count);
        int GetCount() const;

    private:
        std::string m_name;
        int m_count;
        int m_max_count;
        mutable std::mutex m_mutex;
        std::condition_variable m_cv;
    };

    class SemaphoreManager {
    public:
        static SemaphoreManager& Instance();

        uint32_t Create(const std::string& name, int init_count, int max_count);
        bool Wait(uint32_t handle, int count, uint32_t timeout_usec = 0);
        bool Signal(uint32_t handle, int count);
        bool Delete(uint32_t handle);

    private:
        SemaphoreManager() = default;
        std::mutex m_mgr_mutex;
        std::unordered_map<uint32_t, std::shared_ptr<Semaphore>> m_semaphores;
        uint32_t m_handle_counter{200};
    };

}
