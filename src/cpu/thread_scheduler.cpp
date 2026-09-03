#include "cpu/thread_scheduler.hpp"
#include <iostream>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>

namespace PS5::CPU {

    thread_local GuestThread* ThreadScheduler::t_current_thread = nullptr;

    ThreadScheduler& ThreadScheduler::Instance() {
        static ThreadScheduler inst;
        return inst;
    }

    ThreadScheduler::ThreadScheduler() = default;

    ThreadScheduler::~ThreadScheduler() {
        std::lock_guard<std::mutex> lock(m_sched_mutex);
        for (auto& [tid, th] : m_threads) {
            if (th && th->host_worker && th->host_worker->joinable()) {
                th->status = ThreadStatus::Terminated;
                th->host_worker->join();
            }
            if (th && th->stack_memory && th->stack_memory != MAP_FAILED) {
                munmap(th->stack_memory, th->stack_size);
                th->stack_memory = nullptr;
            }
        }
        m_threads.clear();
    }

    uint32_t ThreadScheduler::CreateGuestThread(const std::string& name, int priority, size_t stack_size, std::function<void()> entry) {
        std::lock_guard<std::mutex> lock(m_sched_mutex);

        uint32_t tid = ++m_tid_counter;
        auto th = std::make_shared<GuestThread>();
        th->tid = tid;
        th->name = name;
        th->priority = priority;
        th->stack_size = (stack_size > 0) ? stack_size : (2 * 1024 * 1024);
        th->entry_func = std::move(entry);
        th->status = ThreadStatus::Ready;

        // Allocate thread stack with bounds checking and failure detection
        th->stack_memory = mmap(nullptr, th->stack_size, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        if (th->stack_memory == MAP_FAILED || th->stack_memory == nullptr) {
            std::cerr << "[Thread Scheduler] Critical: Failed to allocate " << (th->stack_size / 1024) 
                      << " KB stack for thread '" << name << "'\n";
            return 0;
        }

        uintptr_t stack_top = reinterpret_cast<uintptr_t>(th->stack_memory) + th->stack_size;
        th->context.rsp = (stack_top - 128) & ~15ULL;

        FPUStateManager::InitializeGuestFPU(th->context.fpu_state);

        m_threads[tid] = th;
        std::cout << "  [Thread Scheduler] Created PS5 Guest Thread #" << tid 
                  << " ('" << name << "', Priority=" << priority << ", Stack=" << (th->stack_size / 1024) << " KB)\n";

        return tid;
    }

    bool ThreadScheduler::StartThread(uint32_t tid) {
        std::lock_guard<std::mutex> lock(m_sched_mutex);
        auto it = m_threads.find(tid);
        if (it == m_threads.end() || !it->second) return false;

        auto th = it->second;
        if (th->status != ThreadStatus::Ready) return false;

        th->status = ThreadStatus::Running;
        th->host_worker = std::make_shared<std::thread>([th]() {
            // RAII cleanup guard to guarantee t_current_thread is zeroed upon exit
            struct ThreadLocalGuard {
                ThreadLocalGuard(GuestThread* ptr) { ThreadScheduler::t_current_thread = ptr; }
                ~ThreadLocalGuard() { ThreadScheduler::t_current_thread = nullptr; }
            } guard(th.get());

            if (th->entry_func) {
                th->entry_func();
            }
            th->status = ThreadStatus::Terminated;
        });

        return true;
    }

    bool ThreadScheduler::YieldThread() {
        std::this_thread::yield();
        return true;
    }

    bool ThreadScheduler::SetThreadPriority(uint32_t tid, int new_priority) {
        std::lock_guard<std::mutex> lock(m_sched_mutex);
        auto it = m_threads.find(tid);
        if (it == m_threads.end() || !it->second) return false;

        it->second->priority = new_priority;
        return true;
    }

    void ThreadScheduler::TerminateCurrentThread() {
        if (t_current_thread) {
            t_current_thread->status = ThreadStatus::Terminated;
        }
    }

    GuestThread* ThreadScheduler::GetCurrentThread() {
        return t_current_thread;
    }

    size_t ThreadScheduler::GetActiveThreadCount() const {
        std::lock_guard<std::mutex> lock(m_sched_mutex);
        size_t count = 0;
        for (const auto& [tid, th] : m_threads) {
            if (th && th->status == ThreadStatus::Running) count++;
        }
        return count;
    }

}
