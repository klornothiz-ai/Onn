#pragma once
#include <cstdint>
#include <string>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <memory>

namespace PS5::Kernel {

    constexpr uint32_t SCE_KERNEL_EVF_WAITMODE_AND       = 0x01;
    constexpr uint32_t SCE_KERNEL_EVF_WAITMODE_OR        = 0x02;
    constexpr uint32_t SCE_KERNEL_EVF_WAITMODE_CLEAR_ALL = 0x10;
    constexpr uint32_t SCE_KERNEL_EVF_WAITMODE_CLEAR_PAT = 0x20;

    class EventFlag {
    public:
        EventFlag(const std::string& name, uint32_t attr, uint64_t init_pattern);

        uint64_t Set(uint64_t bit_pattern);
        uint64_t Clear(uint64_t bit_pattern);
        bool Wait(uint64_t bit_pattern, uint32_t wait_mode, uint64_t* out_bits, uint32_t timeout_usec = 0);
        // Force all current waiters to return false, then set the pattern to
        // set_pattern. Returns the pattern observed just before the reset.
        uint64_t Cancel(uint64_t set_pattern);
        uint64_t GetCurrentPattern() const;

    private:
        std::string m_name;
        uint32_t m_attr;
        uint64_t m_pattern;
        mutable std::mutex m_mutex;
        std::condition_variable m_cv;
        uint64_t m_cancel_epoch{0};

        bool CheckCondition(uint64_t pattern, uint32_t mode, uint64_t& matched_bits);
    };

    class EventFlagManager {
    public:
        static EventFlagManager& Instance();

        uint32_t Create(const std::string& name, uint32_t attr, uint64_t init_pattern);
        bool Set(uint32_t handle, uint64_t bits);
        bool Clear(uint32_t handle, uint64_t bits);
        bool Wait(uint32_t handle, uint64_t bits, uint32_t mode, uint64_t* out_bits, uint32_t timeout_usec = 0);
        // Cancel: force all current waiters to return, reset the pattern to
        // set_pattern, and report the pre-cancel pattern through out_pattern.
        bool Cancel(uint32_t handle, uint64_t set_pattern, uint64_t* out_pattern);
        bool Delete(uint32_t handle);

    private:
        EventFlagManager() = default;
        std::mutex m_mgr_mutex;
        std::unordered_map<uint32_t, std::shared_ptr<EventFlag>> m_flags;
        uint32_t m_handle_counter{100};
    };

}
