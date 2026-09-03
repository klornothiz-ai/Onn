#pragma once
#include "fpu_state.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <unordered_map>
#include <functional>

namespace PS5::CPU {

    enum class ThreadStatus {
        Ready,
        Running,
        Waiting,
        Suspended,
        Terminated
    };

    struct alignas(64) GuestThreadContext {
        uint64_t rax{0}, rbx{0}, rcx{0}, rdx{0};
        uint64_t rsi{0}, rdi{0}, rbp{0}, rsp{0};
        uint64_t r8{0},  r9{0},  r10{0}, r11{0};
        uint64_t r12{0}, r13{0}, r14{0}, r15{0};
        uint64_t rip{0}, rflags{0x202};
        uint64_t fs_base{0};

        FXSaveArea fpu_state;
    };

    struct GuestThread {
        uint32_t tid{0};
        std::string name;
        int priority{256};
        ThreadStatus status{ThreadStatus::Ready};
        
        GuestThreadContext context;
        void* stack_memory{nullptr};
        size_t stack_size{2 * 1024 * 1024};
        
        std::function<void()> entry_func;
        std::shared_ptr<std::thread> host_worker;
    };

    class ThreadScheduler {
    public:
        static ThreadScheduler& Instance();
        ~ThreadScheduler();

        uint32_t CreateGuestThread(const std::string& name, int priority, size_t stack_size, std::function<void()> entry);
        bool StartThread(uint32_t tid);
        bool YieldThread();
        bool SetThreadPriority(uint32_t tid, int new_priority);
        void TerminateCurrentThread();
        GuestThread* GetCurrentThread();

        size_t GetActiveThreadCount() const;

    private:
        ThreadScheduler();

        mutable std::mutex m_sched_mutex;
        std::condition_variable m_sched_cv;
        std::unordered_map<uint32_t, std::shared_ptr<GuestThread>> m_threads;
        uint32_t m_tid_counter{1000};
        thread_local static GuestThread* t_current_thread;
    };

}
