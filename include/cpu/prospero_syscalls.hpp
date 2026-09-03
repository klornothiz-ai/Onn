#pragma once
#include "cpu/x86_64_interpreter.hpp"  // CpuState (SyscallContext::cpu)
#include "memory/virtual_memory_manager.hpp"
#include "cpu/thread_scheduler.hpp"
#include "kernel/event_flag.hpp"
#include "kernel/semaphore.hpp"
#include <cstdint>
#include <unordered_map>
#include <functional>
#include <vector>
#include <mutex>
#include <chrono>

namespace PS5::CPU {

    enum class ProsperoSyscall : uint32_t {
        SC_SYS_exit                = 1,
        SC_SYS_fork                = 2,
        SC_SYS_read                = 3,
        SC_SYS_write               = 4,
        SC_SYS_open                = 5,
        SC_SYS_close               = 6,
        SC_SYS_wait4               = 7,
        SC_SYS_getpid              = 20,
        SC_SYS_getuid              = 24,
        SC_SYS_munmap              = 73,
        SC_SYS_mprotect            = 74,
        SC_SYS_madvise             = 75,
        SC_SYS_socket              = 97,
        SC_SYS_connect             = 98,
        SC_SYS_bind                = 104,
        SC_SYS_gettimeofday        = 116,
        SC_SYS_clock_gettime       = 232,
        SC_SYS_nanosleep           = 240,
        SC_SYS_kqueue              = 362,
        SC_SYS_kevent              = 363,
        SC_SYS_thr_exit            = 431,
        SC_SYS_thr_new             = 432,
        SC_SYS_thr_self            = 433,
        SC_SYS_thr_kill            = 434,
        SC_SYS_thr_set_name        = 464,
        SC_SYS_rtprio_thread       = 466,
        SC_SYS_mmap                = 477,
        SC_SYS_lseek               = 478,
        SC_SYS_cpuset_getaffinity  = 487,
        SC_SYS_cpuset_setaffinity  = 488,
        SC_SYS_namedobj_create     = 532,
        SC_SYS_namedobj_delete     = 533,
        SC_SYS_evf_create          = 538,
        SC_SYS_evf_delete          = 539,
        SC_SYS_evf_open            = 540,
        SC_SYS_evf_set             = 541,
        SC_SYS_evf_clear           = 542,
        SC_SYS_evf_wait            = 543,
        SC_SYS_evf_cancel          = 544,
        SC_SYS_dmem_container      = 585,
        SC_SYS_dynlib_dlsym        = 591,
        SC_SYS_dynlib_get_list     = 592,
        SC_SYS_dynlib_get_info     = 593,
        SC_SYS_budget_get_ptype    = 610,
        // ---- Round 17: FreeBSD-9 x86-64 numbering (inherited by the
        // PS4/PS5 kernels) + the Sony extension _umtx_op. Only syscalls the
        // emulator can back with REAL behaviour are registered; the rest keep
        // failing closed with ENOSYS through Dispatch(). ----
        SC_SYS_setuid              = 23,
        SC_SYS_getppid             = 39,
        SC_SYS_dup                 = 41,
        SC_SYS_pipe                = 42,
        SC_SYS_getegid             = 43,
        SC_SYS_sigaction           = 46,
        SC_SYS_getgid              = 47,
        SC_SYS_sigaltstack         = 51,
        SC_SYS_ioctl               = 52,
        SC_SYS_access              = 33,
        SC_SYS_unlink              = 10,
        SC_SYS_rename              = 128,
        SC_SYS_mkdir               = 136,
        SC_SYS_rmdir               = 137,
        SC_SYS_setpgid             = 82,
        SC_SYS_getpgrp             = 81,
        SC_SYS_fcntl               = 92,
        SC_SYS_readv               = 120,
        SC_SYS_writev              = 121,
        SC_SYS_fsync               = 95,
        SC_SYS_flock               = 131,
        SC_SYS_stat                = 188,
        SC_SYS_fstat               = 189,
        SC_SYS_lstat               = 190,
        SC_SYS_getdirentries       = 196,
        SC_SYS_sysarch             = 165,
        SC_SYS_getrusage           = 117,
        SC_SYS_sysctl              = 200,
        SC_SYS_sigprocmask         = 340,
        SC_SYS_execve              = 59,
        SC_SYS_vfork               = 66,
        SC_SYS_msync               = 61,
        SC_SYS_mincore             = 78,
        SC_SYS_setsid              = 147,
        SC_SYS_ftruncate           = 480,
        SC_SYS__umtx_op            = 436,
        SC_SYS_thr_suspend         = 442,
        SC_SYS_thr_wake            = 443,
        SC_SYS_thr_kill2           = 481
    };

    struct SyscallContext {
        uint64_t rax{0};
        uint64_t rdi{0};
        uint64_t rsi{0};
        uint64_t rdx{0};
        uint64_t rcx{0};
        uint64_t r8{0};
        uint64_t r9{0};
        // Round 18: the CALLING thread's register state (set by the syscall
        // call sites). fork() needs it to snapshot/resume the caller; null
        // keeps handlers working exactly as before.
        CpuState* cpu{nullptr};
    };

    class ProsperoSyscallDispatcher {
    public:
        static ProsperoSyscallDispatcher& Instance();

        uint64_t Dispatch(SyscallContext& ctx);
        size_t GetSyscallCount() const { return m_handlers.size(); }
        bool ExitRequested() const { return m_exit_requested; }
        int ExitCode() const { return m_exit_code; }
        void ClearExitRequest() { m_exit_requested = false; m_exit_code = 0; }

        // FreeBSD-9 struct kevent (the PS4/PS5 libkernel equeue basis), 64-bit
        // layout, 32 bytes. Exposed so tests can build changelists directly.
        struct GuestKevent {
            uint64_t ident{0};   // identifier for this event
            int16_t  filter{0};  // filter for event (e.g. EVFILT_USER)
            uint16_t flags{0};   // action flags (EV_ADD / EV_DELETE / ...)
            uint32_t fflags{0};  // filter flag value (e.g. NOTE_TRIGGER)
            int64_t  data{0};    // filter data value
            uint64_t udata{0};   // opaque user data identifier
        };
        static_assert(sizeof(GuestKevent) == 32, "guest kevent must be 32 bytes");

        // kevent filters (FreeBSD-9 ABI + the Sony extensions this model
        // implements). EVFILT_SIGNAL is FreeBSD's -6. EVFILT_GRAPHICS uses
        // Sony's libkernel equeue constant (0x200) as the syscall-level filter
        // id too -- Sony's numeric syscall-level value is not publicly
        // documented, so the emulator adopts the libkernel constant as its
        // convention and documents it here.
        static constexpr int16_t KEV_EVFILT_USER     = -11;
        static constexpr int16_t KEV_EVFILT_HRTIMER  = -15;
        static constexpr int16_t KEV_EVFILT_SIGNAL   = -6;
        static constexpr int16_t KEV_EVFILT_GRAPHICS = 0x200;

        // EVFILT_GRAPHICS trigger kinds (reported in the delivered fflags).
        static constexpr uint32_t GRAPHICS_EVENT_FLIP    = 1;
        static constexpr uint32_t GRAPHICS_EVENT_VBLANK  = 2;

        // Round 10: models the kernel's pending-signal state for EVFILT_SIGNAL
        // knotes. The emulator's host layers raise guest signals through this
        // entry point; kevent delivery reports and clears the pending count
        // (FreeBSD signal knotes are inherently EV_CLEAR). Returns the new
        // pending count for the signal.
        uint64_t RaiseGuestSignal(int signo, uint32_t count = 1);

        // Round 10: video-out bridge. Posts a graphics event (flip / vblank)
        // for the given video-out handle to EVERY kqueue holding an
        // EVFILT_GRAPHICS knote whose ident matches the handle. Returns how
        // many knotes the event was queued to (0 = nobody listens).
        size_t PostGraphicsEvent(int32_t video_handle, uint32_t kind,
                                 uint32_t event_id, uint64_t udata);

    private:
        ProsperoSyscallDispatcher();
        std::unordered_map<uint32_t, std::function<uint64_t(SyscallContext&)>> m_handlers;

        // A registered knote inside a kqueue: the event as registered plus
        // whether an EVFILT_USER trigger is currently pending. Timer knotes
        // (EVFILT_HRTIMER) instead carry a deadline evaluated lazily against
        // the monotonic clock when events are collected (round 9).
        // EVFILT_GRAPHICS knotes (round 10) carry a FIFO of pending video-out
        // triggers (one delivered per kevent call, FreeBSD knote discipline).
        struct GraphicsTrigger {
            uint32_t kind{0};      // GRAPHICS_EVENT_FLIP / _VBLANK
            uint32_t event_id{0};
            uint64_t udata{0};
        };
        struct Knote {
            GuestKevent ev{};
            bool        triggered{false};
            // EVFILT_HRTIMER: one-shot or periodic deadline (period = ev.data
            // microseconds; 0 for non-timer knotes).
            std::chrono::steady_clock::time_point deadline{};
            uint64_t timer_period_us{0};
            // EVFILT_GRAPHICS: pending video-out triggers, oldest first.
            std::vector<GraphicsTrigger> graphics_queue;
        };
        // kq fd -> registered knotes, keyed by (ident, filter).
        std::unordered_map<int, std::vector<Knote>> m_kqueues;
        // Guest pending-signal counts (EVFILT_SIGNAL readiness state).
        std::unordered_map<int, uint64_t> m_pending_signals;
        std::unordered_map<uint32_t, std::string> m_named_objects;
        // Round 17: guest sigaction handler GVAs, keyed by signal number.
        std::unordered_map<int, uint64_t> m_signal_handlers;
        uint64_t m_cpu_affinity_mask{0xFF};
        std::mutex m_kqueue_mutex;
        uint32_t m_handle_counter{1000};
        bool m_exit_requested{false};
        int m_exit_code{0};

        void RegisterAllSyscalls();
    };

}
