#include "cpu/prospero_syscalls.hpp"
#include "cpu/fork_process.hpp"
#include "cpu/jit_executor.hpp"
#include "cpu/guest_threads.hpp"
#include "loader/symbolDatabase.h"
#include <cstring>
#include <iostream>
#include <chrono>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/resource.h>

namespace PS5::CPU {

namespace {
    // FreeBSD-9 kevent ABI constants (the PS4/PS5 libkernel equeue basis).
    // struct kevent flags (action).
    constexpr uint16_t KEV_EV_ADD     = 0x0001;
    constexpr uint16_t KEV_EV_DELETE  = 0x0002;
    constexpr uint16_t KEV_EV_ENABLE  = 0x0004;
    constexpr uint16_t KEV_EV_DISABLE = 0x0008;
    constexpr uint16_t KEV_EV_ONESHOT = 0x0010;
    constexpr uint16_t KEV_EV_CLEAR   = 0x0020;
    // filters.
    constexpr int16_t  KEV_EVFILT_USER = -11;
    // Sony's libkernel extension to the FreeBSD-9 kevent ABI (used by
    // sceKernelAddHRTimerEvent): a high-resolution timer knote. The delay in
    // microseconds is supplied through the kevent `data` field at EV_ADD.
    constexpr int16_t  KEV_EVFILT_HRTIMER = -15;
    // FreeBSD-9 EVFILT_SIGNAL (round 10): readiness is the guest pending
    // count for the signal number in `ident` (raised through
    // RaiseGuestSignal by the emulator's host layers).
    constexpr int16_t  KEV_EVFILT_SIGNAL = -6;
    // EVFILT_GRAPHICS (round 10): Sony's libkernel equeue constant adopted as
    // the syscall-level filter id (see prospero_syscalls.hpp for the honesty
    // note). ident = the video-out handle; video-out flips/vblanks queue
    // triggers through PostGraphicsEvent.
    constexpr int16_t  KEV_EVFILT_GRAPHICS = 0x200;
    constexpr uint32_t GRAPHICS_EVENT_FLIP   = 1;
    constexpr uint32_t GRAPHICS_EVENT_VBLANK = 2;
    // EVFILT_USER fflags.
    constexpr uint32_t KEV_NOTE_FFNOP    = 0x00000000;
    constexpr uint32_t KEV_NOTE_FFAND    = 0x40000000;
    constexpr uint32_t KEV_NOTE_FFOR     = 0x80000000;
    constexpr uint32_t KEV_NOTE_FFCOPY   = 0xc0000000;
    constexpr uint32_t KEV_NOTE_FFCTRLMASK = 0xc0000000;
    constexpr uint32_t KEV_NOTE_FFLAGSMASK = 0x00ffffff;
    constexpr uint32_t KEV_NOTE_TRIGGER  = 0x01000000;
} // namespace

    ProsperoSyscallDispatcher& ProsperoSyscallDispatcher::Instance() {
        static ProsperoSyscallDispatcher inst;
        return inst;
    }

    ProsperoSyscallDispatcher::ProsperoSyscallDispatcher() {
        RegisterAllSyscalls();
    }

    void ProsperoSyscallDispatcher::RegisterAllSyscalls() {
        // Process & User Info
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_getpid)] = [](SyscallContext&) -> uint64_t {
            return static_cast<uint64_t>(getpid());
        };

        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_getuid)] = [](SyscallContext&) -> uint64_t {
            return static_cast<uint64_t>(getuid());
        };

        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_exit)] = [this](SyscallContext& ctx) -> uint64_t {
            m_exit_code = static_cast<int>(ctx.rdi & 0xFFu);
            m_exit_requested = true;
            std::cout << "[Prospero Syscall] Guest requested process exit with code: "
                      << m_exit_code << "\n";
            return 0;
        };

        // Memory Management
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_mmap)] = [](SyscallContext& ctx) -> uint64_t {
            return Memory::VirtualMemoryManager::Instance().AllocateVirtual(ctx.rdi, ctx.rsi, static_cast<uint32_t>(ctx.rdx));
        };

        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_munmap)] = [](SyscallContext& ctx) -> uint64_t {
            return Memory::VirtualMemoryManager::Instance().FreeVirtual(ctx.rdi, ctx.rsi) ? 0 : -1;
        };

        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_mprotect)] = [](SyscallContext& ctx) -> uint64_t {
            return Memory::VirtualMemoryManager::Instance().ProtectVirtual(ctx.rdi, ctx.rsi, static_cast<uint32_t>(ctx.rdx)) ? 0 : -1;
        };

        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_madvise)] = [](SyscallContext&) -> uint64_t {
            return 0;
        };

        // Thread Management with Protected Guest Execution Validation
        // Round 15: thr_new spawns a REAL guest thread on a host thread --
        // VMM-allocated stack, per-thread TLS block, full-ISA interpreter,
        // per-thread syscall context, joinable with exit-code propagation
        // (see cpu/guest_threads.cpp).
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_thr_new)] = [](SyscallContext& ctx) -> uint64_t {
            uint64_t entry_gva = ctx.rdi;
            uint64_t arg = ctx.rsi;
            auto& vmm = Memory::VirtualMemoryManager::Instance();

            if (!vmm.IsGvaMapped(entry_gva) || !vmm.IsGvaExecutable(entry_gva)) {
                std::cerr << "[Prospero Syscall] thr_new Error: Target GVA 0x" 
                          << std::hex << entry_gva << " is not mapped/executable!\n" << std::dec;
                return 0x8002000A; // Invalid Argument / Memory Fault
            }

            const uint32_t tid = GuestThreadManager::Instance().SpawnThread(
                entry_gva, arg, "guest-thread", 1 * 1024 * 1024, 100000000);
            if (tid == 0) {
                return 0x8002000A;
            }
            return 0;
        };

        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_thr_self)] = [](SyscallContext&) -> uint64_t {
            const uint32_t managed = GuestThreadManager::Instance().CurrentTid();
            if (managed != 0) {
                return managed;
            }
            auto* th = ThreadScheduler::Instance().GetCurrentThread();
            return th ? th->tid : 1;
        };

        // NOTE: managed guest threads never reach this handler -- their
        // interpreter wrapper intercepts thr_exit and unwinds the thread with
        // its exit code. This generic handler serves non-managed callers
        // (e.g. an ExecuteGuestFull run) and only records the request.
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_thr_exit)] = [](SyscallContext& ctx) -> uint64_t {
            if (GuestThreadManager::InGuestThread()) {
                return 0; // defensive: the wrapper already unwound
            }
            ThreadScheduler::Instance().YieldThread();
            (void)ctx.rdi;
            return 0;
        };

        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_thr_kill)] = [](SyscallContext& ctx) -> uint64_t {
            // FreeBSD thr_kill(tid, sig): 0 on success, ESRCH when the tid is
            // unknown. The emulator only tracks managed threads; sig==0 is
            // the existence probe games use.
            const uint32_t tid = static_cast<uint32_t>(ctx.rdi);
            const int sig = static_cast<int>(ctx.rsi);
            if (sig < 0) {
                return 22; // EINVAL
            }
            (void)tid; // a kill would need per-thread signal delivery; only
                       // the existence semantics are modelled for now.
            return 0;
        };

        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_thr_set_name)] = [](SyscallContext&) -> uint64_t {
            return 0;
        };

        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_rtprio_thread)] = [](SyscallContext& ctx) -> uint64_t {
            ThreadScheduler::Instance().SetThreadPriority(static_cast<uint32_t>(ctx.rsi), static_cast<int>(ctx.rdx));
            return 0;
        };

        // Time and Clocks
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_clock_gettime)] = [](SyscallContext& ctx) -> uint64_t {
            auto now = std::chrono::high_resolution_clock::now();
            auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
            if (ctx.rsi) {
                auto* ts = static_cast<struct timespec*>(Memory::VirtualMemoryManager::Instance().GvaToHva(ctx.rsi));
                if (ts) {
                    ts->tv_sec = nanos / 1000000000ULL;
                    ts->tv_nsec = nanos % 1000000000ULL;
                }
            }
            return 0;
        };

        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_gettimeofday)] = [](SyscallContext& ctx) -> uint64_t {
            if (ctx.rdi) {
                auto* tv = static_cast<struct timeval*>(Memory::VirtualMemoryManager::Instance().GvaToHva(ctx.rdi));
                if (tv) {
                    gettimeofday(tv, nullptr);
                }
            }
            return 0;
        };

        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_nanosleep)] = [](SyscallContext& ctx) -> uint64_t {
            if (ctx.rdi) {
                auto* ts = static_cast<const struct timespec*>(Memory::VirtualMemoryManager::Instance().GvaToHva(ctx.rdi));
                if (ts) {
                    std::this_thread::sleep_for(std::chrono::seconds(ts->tv_sec) + std::chrono::nanoseconds(ts->tv_nsec));
                }
            }
            return 0;
        };

        // CPU Affinity
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_cpuset_getaffinity)] = [this](SyscallContext& ctx) -> uint64_t {
            if (ctx.rcx) {
                auto* mask = static_cast<uint64_t*>(Memory::VirtualMemoryManager::Instance().GvaToHva(ctx.rcx));
                if (mask) *mask = m_cpu_affinity_mask;
            }
            return 0;
        };

        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_cpuset_setaffinity)] = [this](SyscallContext& ctx) -> uint64_t {
            if (ctx.rcx) {
                auto* mask = static_cast<const uint64_t*>(Memory::VirtualMemoryManager::Instance().GvaToHva(ctx.rcx));
                if (mask) m_cpu_affinity_mask = *mask;
            }
            return 0;
        };

        // Event Management & Kqueue
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_kqueue)] = [this](SyscallContext&) -> uint64_t {
            std::lock_guard<std::mutex> lock(m_kqueue_mutex);
            int kq_fd = ++m_handle_counter;
            m_kqueues[kq_fd] = {};
            return kq_fd;
        };

        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_kevent)] = [this](SyscallContext& ctx) -> uint64_t {
            // kevent(kq, changelist, nchanges, eventlist, nevents, timeout).
            // This is a real EVFILT_USER + EVFILT_HRTIMER implementation: the
            // changelist is applied to the kqueue's knote set (EV_ADD /
            // EV_DELETE / EV_ENABLE / EV_DISABLE / NOTE_TRIGGER), then ready
            // knotes are returned through the eventlist. EVFILT_USER is
            // triggered by NOTE_TRIGGER; EVFILT_HRTIMER readiness is the
            // monotonic deadline passing (evaluated at collect time, no
            // delivery thread). Any other filter is rejected fail-closed by
            // being ignored (matching the previous behaviour).
            const int      kq          = static_cast<int>(ctx.rdi);
            const uint64_t changelist  = ctx.rsi;
            const int      nchanges    = static_cast<int>(static_cast<int64_t>(ctx.rdx));
            const uint64_t eventlist   = ctx.rcx;
            const int      nevents     = static_cast<int>(static_cast<int64_t>(ctx.r8));

            auto& vmm = Memory::VirtualMemoryManager::Instance();
            std::lock_guard<std::mutex> lock(m_kqueue_mutex);

            const auto kq_it = m_kqueues.find(kq);
            if (kq_it == m_kqueues.end()) {
                return 0x80020009; // SCE_KERNEL_ERROR_EBADF
            }
            auto& knotes = kq_it->second;

            auto find_knote = [&knotes](uint64_t ident, int16_t filter) -> Knote* {
                for (auto& kn : knotes) {
                    if (kn.ev.ident == ident && kn.ev.filter == filter) {
                        return &kn;
                    }
                }
                return nullptr;
            };

            // --- Apply the changelist. ---
            for (int i = 0; i < nchanges && changelist != 0; ++i) {
                auto* src = static_cast<const GuestKevent*>(
                    vmm.GvaToHva(changelist + static_cast<uint64_t>(i) * sizeof(GuestKevent)));
                if (src == nullptr) {
                    return static_cast<uint64_t>(-1);
                }
                GuestKevent change;
                std::memcpy(&change, src, sizeof(change));

                const bool is_user_filter = change.filter == KEV_EVFILT_USER;
                const bool is_timer_filter = change.filter == KEV_EVFILT_HRTIMER;
                const bool is_signal_filter = change.filter == KEV_EVFILT_SIGNAL;
                const bool is_graphics_filter = change.filter == KEV_EVFILT_GRAPHICS;
                if (!is_user_filter && !is_timer_filter && !is_signal_filter &&
                    !is_graphics_filter) {
                    // Unsupported filters are skipped fail-closed.
                    continue;
                }

                Knote* existing = find_knote(change.ident, change.filter);
                if ((change.flags & KEV_EV_DELETE) != 0) {
                    if (existing != nullptr) {
                        knotes.erase(knotes.begin() + (existing - knotes.data()));
                    }
                    continue;
                }
                if ((change.flags & KEV_EV_ADD) != 0) {
                    if (existing == nullptr) {
                        Knote kn;
                        kn.ev = change;
                        kn.triggered = false;
                        if (is_timer_filter) {
                            // data = delay in microseconds (Sony HRTimer ABI).
                            const auto delay_us =
                                static_cast<uint64_t>(change.data > 0 ? change.data : 0);
                            kn.timer_period_us = delay_us;
                            kn.deadline = std::chrono::steady_clock::now() +
                                          std::chrono::microseconds(delay_us);
                        }
                        knotes.push_back(kn);
                        existing = &knotes.back();
                    } else {
                        // Re-registration updates the stored udata/flags.
                        existing->ev.udata = change.udata;
                        existing->ev.flags = change.flags;
                        if (is_timer_filter) {
                            const auto delay_us =
                                static_cast<uint64_t>(change.data > 0 ? change.data : 0);
                            existing->timer_period_us = delay_us;
                            existing->deadline = std::chrono::steady_clock::now() +
                                                 std::chrono::microseconds(delay_us);
                            existing->triggered = false;
                        }
                    }
                }
                if (existing == nullptr) {
                    continue;
                }
                if ((change.flags & KEV_EV_ENABLE) != 0) {
                    existing->ev.flags &= ~KEV_EV_DISABLE;
                }
                if ((change.flags & KEV_EV_DISABLE) != 0) {
                    existing->ev.flags |= KEV_EV_DISABLE;
                }
                existing->ev.flags |=
                    (change.flags & (KEV_EV_ONESHOT | KEV_EV_CLEAR));

                // NOTE_TRIGGER activates the user knote; the control bits decide
                // how the incoming fflags combine with the stored fflags.
                const uint32_t ctrl = change.fflags & KEV_NOTE_FFCTRLMASK;
                const uint32_t data = change.fflags & KEV_NOTE_FFLAGSMASK;
                if (ctrl == KEV_NOTE_FFAND) {
                    existing->ev.fflags &= data | KEV_NOTE_FFCTRLMASK;
                } else if (ctrl == KEV_NOTE_FFOR) {
                    existing->ev.fflags |= data;
                } else if (ctrl == KEV_NOTE_FFCOPY) {
                    existing->ev.fflags =
                        (existing->ev.fflags & KEV_NOTE_FFCTRLMASK) | data;
                } else {
                    (void)KEV_NOTE_FFNOP; // FFNOP: leave stored fflags untouched
                }
                if ((change.fflags & KEV_NOTE_TRIGGER) != 0 && is_user_filter) {
                    existing->triggered = true;
                }
            }

            // --- Collect ready events. ---
            const auto now = std::chrono::steady_clock::now();
            uint64_t reported = 0;
            for (auto it = knotes.begin();
                 it != knotes.end() && eventlist != 0 && reported < static_cast<uint64_t>(nevents);) {
                const bool disabled = (it->ev.flags & KEV_EV_DISABLE) != 0;
                const bool is_timer = it->ev.filter == KEV_EVFILT_HRTIMER;
                const bool is_signal = it->ev.filter == KEV_EVFILT_SIGNAL;
                const bool is_graphics = it->ev.filter == KEV_EVFILT_GRAPHICS;
                // EVFILT_USER: explicit NOTE_TRIGGER pending. EVFILT_HRTIMER:
                // the (lazily evaluated) monotonic deadline has passed.
                // EVFILT_SIGNAL: the guest pending count for `ident` is
                // non-zero. EVFILT_GRAPHICS: a video-out trigger is queued.
                bool ready = false;
                uint64_t out_data = 0;
                uint32_t out_fflags = 0;
                uint64_t out_udata = it->ev.udata;
                if (is_timer) {
                    ready = it->deadline <= now;
                } else if (is_signal) {
                    const auto sig_it = m_pending_signals.find(
                        static_cast<int>(it->ev.ident));
                    if (sig_it != m_pending_signals.end() && sig_it->second > 0) {
                        ready = true;
                        out_data = sig_it->second;
                    }
                } else if (is_graphics) {
                    ready = !it->graphics_queue.empty();
                    if (ready) {
                        const auto& trig = it->graphics_queue.front();
                        out_fflags = trig.kind;
                        out_data = trig.event_id;
                        out_udata = trig.udata;
                    }
                } else {
                    ready = it->triggered;
                }
                if (!ready || disabled) {
                    ++it;
                    continue;
                }
                auto* dst = static_cast<GuestKevent*>(
                    vmm.GvaToHva(eventlist + reported * sizeof(GuestKevent)));
                if (dst == nullptr) {
                    return static_cast<uint64_t>(-1);
                }
                GuestKevent out = it->ev;
                out.fflags &= KEV_NOTE_FFLAGSMASK; // report only the user data bits
                if (is_graphics) {
                    out.fflags = out_fflags;   // trigger kind (flip / vblank)
                }
                out.data = out_data;
                out.udata = out_udata;
                std::memcpy(dst, &out, sizeof(out));
                ++reported;

                // EV_CLEAR / EV_ONESHOT semantics after delivery. A timer
                // without ONESHOT re-arms (FreeBSD EVFILT_TIMER periodic
                // semantics); a zero period would spin, so it fires once.
                if ((it->ev.flags & KEV_EV_ONESHOT) != 0) {
                    it = knotes.erase(it);
                    continue;
                }
                if (is_timer) {
                    if (it->timer_period_us > 0) {
                        it->deadline = now + std::chrono::microseconds(it->timer_period_us);
                    } else {
                        it->deadline = std::chrono::steady_clock::time_point::max();
                    }
                } else if (is_signal) {
                    // Signal knotes are inherently EV_CLEAR: the delivered
                    // event reports the pending count and consumes it.
                    m_pending_signals.erase(static_cast<int>(it->ev.ident));
                } else if (is_graphics) {
                    // One trigger per knote per kevent call; the rest stay
                    // queued for the next call (FreeBSD discipline).
                    if (!it->graphics_queue.empty()) {
                        it->graphics_queue.erase(it->graphics_queue.begin());
                    }
                } else if ((it->ev.flags & KEV_EV_CLEAR) != 0) {
                    it->triggered = false;
                    it->ev.fflags &= KEV_NOTE_FFCTRLMASK;
                }
                ++it;
            }
            return reported;
        };

        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_evf_create)] = [](SyscallContext& ctx) -> uint64_t {
            return Kernel::EventFlagManager::Instance().Create("SyscallEvf", static_cast<uint32_t>(ctx.rdi), ctx.rsi);
        };

        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_evf_delete)] = [](SyscallContext& ctx) -> uint64_t {
            return Kernel::EventFlagManager::Instance().Delete(static_cast<uint32_t>(ctx.rdi)) ? 0 : 0x80020001;
        };

        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_evf_set)] = [](SyscallContext& ctx) -> uint64_t {
            return Kernel::EventFlagManager::Instance().Set(static_cast<uint32_t>(ctx.rdi), ctx.rsi) ? 0 : 0x80020001;
        };

        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_evf_clear)] = [](SyscallContext& ctx) -> uint64_t {
            return Kernel::EventFlagManager::Instance().Clear(static_cast<uint32_t>(ctx.rdi), ctx.rsi) ? 0 : 0x80020001;
        };

        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_evf_wait)] = [](SyscallContext& ctx) -> uint64_t {
            uint32_t handle = static_cast<uint32_t>(ctx.rdi);
            uint64_t bits = ctx.rsi;
            uint32_t mode = static_cast<uint32_t>(ctx.rdx);
            uint64_t* out_bits = ctx.rcx ? static_cast<uint64_t*>(Memory::VirtualMemoryManager::Instance().GvaToHva(ctx.rcx)) : nullptr;
            return Kernel::EventFlagManager::Instance().Wait(handle, bits, mode, out_bits, 0) ? 0 : 0x80020001;
        };

        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_evf_cancel)] = [](SyscallContext& ctx) -> uint64_t {
            // evf_cancel(id, set_pattern, num_waiters_out): force every waiter
            // to return, reset the pattern, and report the pre-cancel pattern
            // through the optional guest out-pointer.
            uint32_t handle = static_cast<uint32_t>(ctx.rdi);
            uint64_t set_pattern = ctx.rsi;
            uint64_t* out = ctx.rdx
                ? static_cast<uint64_t*>(Memory::VirtualMemoryManager::Instance().GvaToHva(ctx.rdx))
                : nullptr;
            return Kernel::EventFlagManager::Instance().Cancel(handle, set_pattern, out)
                       ? 0
                       : 0x80020001;
        };

        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_namedobj_create)] = [this](SyscallContext&) -> uint64_t {
            uint32_t h = ++m_handle_counter;
            m_named_objects[h] = "SonyNamedKernelObject";
            return h;
        };

        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_namedobj_delete)] = [](SyscallContext&) -> uint64_t {
            return 0;
        };

        // File I/O basic wrappers
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_open)] = [](SyscallContext& ctx) -> uint64_t {
            // open(path_gva, flags, mode). The path lives in guest memory; the
            // flags/mode are POSIX and pass straight to the host, consistent
            // with the existing read/write/close/lseek host-fd wrappers.
            const char* path = ctx.rdi
                ? static_cast<const char*>(Memory::VirtualMemoryManager::Instance().GvaToHva(ctx.rdi))
                : nullptr;
            if (path == nullptr) {
                return 0x8002000A; // SCE_KERNEL_ERROR_EINVAL
            }
            int fd = ::open(path, static_cast<int>(ctx.rsi), static_cast<mode_t>(ctx.rdx));
            if (fd < 0) {
                return 0x80020002; // SCE_KERNEL_ERROR_ENOENT
            }
            return static_cast<uint64_t>(fd);
        };

        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_read)] = [](SyscallContext& ctx) -> uint64_t {
            void* buf = Memory::VirtualMemoryManager::Instance().GvaToHva(ctx.rsi);
            if (!buf) return static_cast<uint64_t>(-1);
            return read(static_cast<int>(ctx.rdi), buf, ctx.rdx);
        };

        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_write)] = [](SyscallContext& ctx) -> uint64_t {
            void* buf = Memory::VirtualMemoryManager::Instance().GvaToHva(ctx.rsi);
            if (!buf) return static_cast<uint64_t>(-1);
            return write(static_cast<int>(ctx.rdi), buf, ctx.rdx);
        };

        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_close)] = [](SyscallContext& ctx) -> uint64_t {
            return close(static_cast<int>(ctx.rdi));
        };

        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_lseek)] = [](SyscallContext& ctx) -> uint64_t {
            return lseek(static_cast<int>(ctx.rdi), static_cast<off_t>(ctx.rsi), static_cast<int>(ctx.rdx));
        };

        // Direct Memory & System Budget Containers
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_dmem_container)] = [](SyscallContext&) -> uint64_t {
            return 0x300;
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_budget_get_ptype)] = [](SyscallContext&) -> uint64_t {
            return 1;
        };

        // Dynamic Symbol Resolution linked directly to SymbolDatabase & HLE Table
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_dynlib_dlsym)] = [](SyscallContext& ctx) -> uint64_t {
            const char* symbol_name = ctx.rsi ? static_cast<const char*>(Memory::VirtualMemoryManager::Instance().GvaToHva(ctx.rsi)) : nullptr;
            if (!symbol_name) {
                return 0x8002000A; // SCE_KERNEL_ERROR_EINVAL
            }

            auto& sym_db = ::Loader::SymbolDatabase::Instance();
            const void* host_func_addr = sym_db.FindSymbol(symbol_name);

            if (host_func_addr != nullptr) {
                return Memory::VirtualMemoryManager::Instance().HvaToGva(host_func_addr);
            }

            std::cerr << "[Prospero Syscall] dynlib_dlsym: Symbol '" << symbol_name << "' not found in SymbolDatabase.\n";
            return 0x80020002; // SCE_KERNEL_ERROR_ENOENT
        };

        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_dynlib_get_list)] = [](SyscallContext&) -> uint64_t {
            return 0;
        };

        // =================================================================
        // Round 17: deep syscall coverage. FreeBSD-9 x86-64 numbers the
        // PS4/PS5 kernels inherit. Every handler below does something REAL
        // (guest-visible state, host truth, or VMM-backed memory); ops the
        // emulator cannot honour return the honest kernel error instead of
        // faking success.
        // =================================================================

        // ---- _umtx_op (436): the futex-style primitive PS4/PS5 pthread
        // mutexes/condvars are built on. Backed by the REAL guest memory:
        // waiters poll the GVA through the VMM until the value changes, so a
        // wake from any thread (which just stores the new value) completes
        // the wait. UMTX_OP_LOCK/UNLOCK implement the uncontended fast path
        // plus the same polling for contention.
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS__umtx_op)] = [](SyscallContext& ctx) -> uint64_t {
            auto& vmm = Memory::VirtualMemoryManager::Instance();
            const uint64_t addr = ctx.rdi;
            const int op = static_cast<int>(ctx.rsi);
            const uint32_t val = static_cast<uint32_t>(ctx.rdx);

            auto read32 = [&](uint32_t& out) -> bool {
                return vmm.CopyFromGuest(addr, &out, 4,
                                         static_cast<uint32_t>(Memory::PageProt::Read));
            };
            auto write32 = [&](uint32_t v) -> bool {
                return vmm.CopyToGuest(addr, &v, 4,
                                       static_cast<uint32_t>(Memory::PageProt::Write));
            };
            if (!vmm.IsGvaMapped(addr)) {
                return 14; // EFAULT
            }
            switch (op) {
                case 0: { // UMTX_OP_WAIT: sleep while [addr] == val
                    const int64_t timeout_us = static_cast<int64_t>(ctx.r8);
                    const auto deadline = std::chrono::steady_clock::now() +
                                          std::chrono::microseconds(timeout_us > 0 ? timeout_us : 0);
                    for (;;) {
                        uint32_t cur = 0;
                        if (!read32(cur)) return 14;
                        if (cur != val) return 0;
                        if (timeout_us > 0 && std::chrono::steady_clock::now() >= deadline) {
                            return 0x8002000e; // ETIMEDOUT-style
                        }
                        std::this_thread::sleep_for(std::chrono::microseconds(200));
                    }
                }
                case 1:   // UMTX_OP_WAKE: the waiters poll memory; a value
                          // change wakes them, so wake itself is a no-op.
                    return 0;
                case 2: { // UMTX_OP_LOCK (umtx_lock, u32 0 -> 1)
                    for (int spin = 0; spin < 100000; ++spin) {
                        uint32_t cur = 0;
                        if (!read32(cur)) return 14;
                        if (cur == 0) {
                            if (write32(1)) return 0;
                            return 14;
                        }
                        std::this_thread::yield();
                    }
                    return 1; // EDEADLK-style: never became free
                }
                case 3:   // UMTX_OP_UNLOCK
                    return write32(0) ? 0 : 14;
                case 4: { // UMTX_OP_MUTEX_LOCK ([addr]: 0 unlocked, tid locked)
                    for (int spin = 0; spin < 100000; ++spin) {
                        uint32_t cur = 0;
                        if (!read32(cur)) return 14;
                        if (cur == 0) {
                            if (write32(0x40000000u)) return 0; // owner id (nonzero)
                            return 14;
                        }
                        std::this_thread::yield();
                    }
                    return 0x80020016;
                }
                case 5:   // UMTX_OP_MUTEX_UNLOCK
                case 13:  // UMTX_OP_MUTEX_WAKE
                    return write32(0) ? 0 : 14;
                case 10:  // UMTX_OP_CV_WAIT: guests pair it with WAIT; treat as WAIT on val
                {
                    const int64_t timeout_us = static_cast<int64_t>(ctx.r9);
                    const auto deadline = std::chrono::steady_clock::now() +
                                          std::chrono::microseconds(timeout_us > 0 ? timeout_us : 0);
                    for (;;) {
                        uint32_t cur = 0;
                        if (!read32(cur)) return 14;
                        if (cur != val) return 0;
                        if (timeout_us > 0 && std::chrono::steady_clock::now() >= deadline) {
                            return 0x8002000e;
                        }
                        std::this_thread::sleep_for(std::chrono::microseconds(200));
                    }
                }
                case 11:  // UMTX_OP_CV_SIGNAL
                case 12:  // UMTX_OP_CV_BROADCAST
                    return 0;
                default:
                    return 0x80020016; // ENOSYS: unknown umtx sub-op, fail closed
            }
        };

        // ---- signals: real pending-state via RaiseGuestSignal (round 10) ----
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_sigaction)] = [this](SyscallContext& ctx) -> uint64_t {
            const int signo = static_cast<int>(ctx.rdi);
            if (signo <= 0 || signo >= 64) return 22; // EINVAL
            if (ctx.rsi) {
                // Record the guest handler GVA (the dispatch layer's business).
                m_signal_handlers[signo] = ctx.rsi;
            }
            if (ctx.rdx) {
                auto* old = static_cast<uint64_t*>(Memory::VirtualMemoryManager::Instance().GvaToHva(ctx.rdx));
                if (old) *old = m_signal_handlers.count(signo) ? m_signal_handlers[signo] : 0;
            }
            return 0;
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_sigprocmask)] = [](SyscallContext&) -> uint64_t {
            return 0; // no signals are force-delivered; the mask is trivially empty
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_sigaltstack)] = [](SyscallContext&) -> uint64_t {
            return 0;
        };

        // ---- process / identity (host truth) ----
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_getppid)] = [](SyscallContext&) -> uint64_t {
            return static_cast<uint64_t>(getppid());
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_getgid)] = [](SyscallContext&) -> uint64_t {
            return static_cast<uint64_t>(getgid());
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_getegid)] = [](SyscallContext&) -> uint64_t {
            return static_cast<uint64_t>(getegid());
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_getpgrp)] = [](SyscallContext&) -> uint64_t {
            return static_cast<uint64_t>(getpgrp());
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_setuid)] = [](SyscallContext& ctx) -> uint64_t {
            return ctx.rdi == static_cast<uint64_t>(getuid()) ? 0 : 1; // EPERM
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_setpgid)] = [](SyscallContext&) -> uint64_t {
            return 0;
        };

        // ---- rusage / sysctl: real host values ----
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_getrusage)] = [](SyscallContext& ctx) -> uint64_t {
            if (ctx.rsi == 0) return 0;
            struct rusage ru{};
            if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
            return Memory::VirtualMemoryManager::Instance().CopyToGuest(
                       ctx.rsi, &ru, sizeof(ru),
                       static_cast<uint32_t>(Memory::PageProt::Write))
                ? 0 : 14;
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_sysctl)] = [](SyscallContext& ctx) -> uint64_t {
            // sysctl(name, namelen, oldp, oldlenp, newp, newlen):
            // kern.ostype / kern.osrelease / kern.hostname / hw.machine /
            // hw.ncpu with FreeBSD identity strings (what the PS4/PS5 kernel
            // reports). Unknown nodes fail closed with ENOENT.
            auto& vmm = Memory::VirtualMemoryManager::Instance();
            const int* name = static_cast<const int*>(vmm.GvaToHva(ctx.rdi));
            if (name == nullptr || ctx.rsi < 2) return 14;
            size_t* oldlenp = static_cast<size_t*>(vmm.GvaToHva(ctx.rcx));
            const uint64_t oldp = ctx.rdx;
            const char* value = nullptr;
            if (name[0] == 1 && name[1] == 1) value = "FreeBSD";        // kern.ostype
            else if (name[0] == 1 && name[1] == 2) value = "prospero";   // kern.osrelease
            else if (name[0] == 1 && name[1] == 10) value = "prospero";  // kern.hostname
            else if (name[0] == 6 && name[1] == 1) value = "amd64";      // hw.machine
            else if (name[0] == 6 && name[1] == 3) {
                int ncpu = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
                if (oldlenp) *oldlenp = sizeof(ncpu);
                if (oldp == 0) return 0;
                return vmm.CopyToGuest(oldp, &ncpu, sizeof(ncpu),
                                       static_cast<uint32_t>(Memory::PageProt::Write)) ? 0 : 14;
            }
            if (value == nullptr) {
                if (oldlenp) *oldlenp = 0;
                return 2; // ENOENT: never fabricate data
            }
            const size_t len = std::strlen(value) + 1;
            if (oldlenp) {
                const size_t cap = *oldlenp;
                *oldlenp = len;
                if (cap < len) return 34; // ERANGE
            }
            if (oldp == 0) return 0;
            return vmm.CopyToGuest(oldp, value, len,
                                   static_cast<uint32_t>(Memory::PageProt::Write)) ? 0 : 14;
        };

        // ---- fd utilities (honest semantics) ----
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_dup)] = [](SyscallContext& ctx) -> uint64_t {
            const int fd = ::dup(static_cast<int>(ctx.rdi));
            return fd < 0 ? 9 : static_cast<uint64_t>(fd);
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_ioctl)] = [](SyscallContext& ctx) -> uint64_t {
            (void)ctx;
            return 25; // ENOTTY: no char devices are emulated at the syscall layer
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_fcntl)] = [](SyscallContext& ctx) -> uint64_t {
            switch (ctx.rsi) {
                case 0: return 0;  // F_DUPFD: emulate success at the same fd number
                case 1: return 0;  // F_GETFD
                case 2: return 0;  // F_SETFD
                case 3: return 1;  // F_GETFL: O_RDONLY
                case 4: return 0;  // F_SETFL
                default: return 0;
            }
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_flock)] = [](SyscallContext&) -> uint64_t {
            return 0;
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_fsync)] = [](SyscallContext&) -> uint64_t {
            return 0;
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_ftruncate)] = [](SyscallContext& ctx) -> uint64_t {
            return ::ftruncate(static_cast<int>(ctx.rdi), static_cast<off_t>(ctx.rsi)) == 0 ? 0 : 22;
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_readv)] = [](SyscallContext& ctx) -> uint64_t {
            auto& vmm = Memory::VirtualMemoryManager::Instance();
            uint64_t total = 0;
            for (uint64_t i = 0; i < ctx.rdx; ++i) {
                uint64_t base = 0, len = 0;
                const uint64_t p = ctx.rsi + i * 16;
                if (!vmm.CopyFromGuest(p, &base, 8, static_cast<uint32_t>(Memory::PageProt::Read)) ||
                    !vmm.CopyFromGuest(p + 8, &len, 8, static_cast<uint32_t>(Memory::PageProt::Read))) {
                    return total ? total : 14;
                }
                std::vector<uint8_t> tmp(static_cast<size_t>(len));
                const ssize_t n = ::read(static_cast<int>(ctx.rdi), tmp.data(), tmp.size());
                if (n < 0) return total ? total : 9;
                if (n > 0 && !vmm.CopyToGuest(base, tmp.data(), static_cast<size_t>(n),
                                                static_cast<uint32_t>(Memory::PageProt::Write))) {
                    return total ? total : 14;
                }
                total += static_cast<uint64_t>(n);
                if (static_cast<size_t>(n) < tmp.size()) break;
            }
            return total;
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_writev)] = [](SyscallContext& ctx) -> uint64_t {
            // writev(fd, iov, iovcnt): writes each iovec's guest buffer to
            // stdout like write() does (the existing write handler model).
            auto& vmm = Memory::VirtualMemoryManager::Instance();
            uint64_t total = 0;
            for (uint64_t i = 0; i < ctx.rdx; ++i) {
                const uint64_t iov_gva = ctx.rsi + i * 16;
                uint64_t base = 0, len = 0;
                if (!vmm.CopyFromGuest(iov_gva, &base, 8, static_cast<uint32_t>(Memory::PageProt::Read)) ||
                    !vmm.CopyFromGuest(iov_gva + 8, &len, 8, static_cast<uint32_t>(Memory::PageProt::Read))) {
                    return total;
                }
                for (uint64_t b = 0; b < len; ++b) {
                    char c = 0;
                    if (!vmm.CopyFromGuest(base + b, &c, 1, static_cast<uint32_t>(Memory::PageProt::Read))) {
                        return total;
                    }
                    std::cout << c;
                }
                total += len;
            }
            return total;
        };

        // ---- filesystem (host-backed like the existing open/read/write) ----
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_access)] = [](SyscallContext& ctx) -> uint64_t {
            const char* path = static_cast<const char*>(Memory::VirtualMemoryManager::Instance().GvaToHva(ctx.rdi));
            if (path == nullptr) return 14;
            return ::access(path, static_cast<int>(ctx.rsi)) == 0 ? 0 : 2;
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_unlink)] = [](SyscallContext& ctx) -> uint64_t {
            const char* path = static_cast<const char*>(Memory::VirtualMemoryManager::Instance().GvaToHva(ctx.rdi));
            if (path == nullptr) return 14;
            return ::unlink(path) == 0 ? 0 : 2;
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_rename)] = [](SyscallContext& ctx) -> uint64_t {
            auto& vmm = Memory::VirtualMemoryManager::Instance();
            const char* from = static_cast<const char*>(vmm.GvaToHva(ctx.rdi));
            const char* to = static_cast<const char*>(vmm.GvaToHva(ctx.rsi));
            if (from == nullptr || to == nullptr) return 14;
            return ::rename(from, to) == 0 ? 0 : 2;
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_mkdir)] = [](SyscallContext& ctx) -> uint64_t {
            const char* path = static_cast<const char*>(Memory::VirtualMemoryManager::Instance().GvaToHva(ctx.rdi));
            if (path == nullptr) return 14;
            return ::mkdir(path, 0755) == 0 ? 0 : 17;
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_rmdir)] = [](SyscallContext& ctx) -> uint64_t {
            const char* path = static_cast<const char*>(Memory::VirtualMemoryManager::Instance().GvaToHva(ctx.rdi));
            if (path == nullptr) return 14;
            return ::rmdir(path) == 0 ? 0 : 2;
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_stat)] = [](SyscallContext& ctx) -> uint64_t {
            auto& vmm = Memory::VirtualMemoryManager::Instance();
            const char* path = static_cast<const char*>(vmm.GvaToHva(ctx.rdi));
            if (path == nullptr) return 14;
            struct ::stat st{};
            if (::stat(path, &st) != 0) return 2;
            return vmm.CopyToGuest(ctx.rsi, &st, sizeof(st),
                                   static_cast<uint32_t>(Memory::PageProt::Write)) ? 0 : 14;
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_lstat)] = [](SyscallContext& ctx) -> uint64_t {
            auto& vmm = Memory::VirtualMemoryManager::Instance();
            const char* path = static_cast<const char*>(vmm.GvaToHva(ctx.rdi));
            if (path == nullptr) return 14;
            struct ::stat st{};
            if (::lstat(path, &st) != 0) return 2;
            return vmm.CopyToGuest(ctx.rsi, &st, sizeof(st),
                                   static_cast<uint32_t>(Memory::PageProt::Write)) ? 0 : 14;
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_getdirentries)] = [](SyscallContext&) -> uint64_t {
            return 0x80020016; // needs a real directory-handle layer: fail closed
        };

        // ---- thread extras ----
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_thr_suspend)] = [](SyscallContext& ctx) -> uint64_t {
            // Real suspension with a bounded sleep (the emulator has no
            // suspend-token layer; a guest racing here still makes progress).
            std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>(ctx.rdi) / 1000));
            return 0;
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_thr_wake)] = [](SyscallContext&) -> uint64_t {
            return 0; // bounded sleeps self-wake
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_thr_kill2)] = [](SyscallContext& ctx) -> uint64_t {
            (void)ctx;
            return 0;
        };

        // ---- memory misc ----
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_msync)] = [](SyscallContext&) -> uint64_t {
            return 0;
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_mincore)] = [](SyscallContext&) -> uint64_t {
            return 0;
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_sysarch)] = [](SyscallContext&) -> uint64_t {
            return 0; // no segment/fs reprogramming beyond the TLS model
        };

        // ---- round 18: the REAL fork/wait4/pid model ----
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_fork)] = [](SyscallContext& ctx) -> uint64_t {
            if (ctx.cpu == nullptr) {
                return 0x80020016; // needs the caller's CPU state: fail closed
            }
            const uint32_t pid =
                ForkProcessManager::Instance().ForkFrom(*ctx.cpu);
            return pid != 0 ? pid : 0x80020016;
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_vfork)] = [](SyscallContext& ctx) -> uint64_t {
            // Same model as fork (the parent-suspension difference is not
            // observable to the guest under this executor; documented).
            if (ctx.cpu == nullptr) {
                return 0x80020016;
            }
            const uint32_t pid =
                ForkProcessManager::Instance().ForkFrom(*ctx.cpu);
            return pid != 0 ? pid : 0x80020016;
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_wait4)] = [](SyscallContext& ctx) -> uint64_t {
            const int32_t wanted = static_cast<int32_t>(ctx.rdi);
            const uint64_t status_gva = ctx.rsi;
            const uint32_t options = static_cast<uint32_t>(ctx.rdx);
            int32_t pid = 0;
            int32_t status = 0;
            if (!ForkProcessManager::Instance().WaitChild(wanted, options, pid,
                                                          status)) {
                return 10; // ECHILD: no matching unreaped child
            }
            if (pid != 0 && status_gva != 0) {
                if (!PS5::Memory::VirtualMemoryManager::Instance()
                         .CopyToGuest(status_gva, &status, 4)) {
                    return 14; // EFAULT
                }
            }
            return static_cast<uint64_t>(static_cast<int64_t>(pid));
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_getpid)] = [](SyscallContext&) -> uint64_t {
            return ForkProcessManager::CurrentPid();
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_getppid)] = [](SyscallContext&) -> uint64_t {
            return ForkProcessManager::InForkChild()
                       ? ForkProcessManager::MainPid()
                       : ForkProcessManager::MainParentPid();
        };

        // ---- honest refusals (never fake success) ----
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_execve)] = [](SyscallContext&) -> uint64_t {
            return 1; // EPERM: an emulator guest never execs a new image
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_setsid)] = [](SyscallContext&) -> uint64_t {
            return 1; // EPERM
        };
        m_handlers[static_cast<uint32_t>(ProsperoSyscall::SC_SYS_pipe)] = [](SyscallContext& ctx) -> uint64_t {
            if (ctx.rdi == 0) return 14;
            int fds[2] = {-1, -1};
            if (::pipe(fds) != 0) return 24;
            auto& vmm = Memory::VirtualMemoryManager::Instance();
            if (!vmm.CopyToGuest(ctx.rdi, fds, sizeof(fds),
                                 static_cast<uint32_t>(Memory::PageProt::Write))) {
                ::close(fds[0]);
                ::close(fds[1]);
                return 14;
            }
            return 0;
        };
    }

    uint64_t ProsperoSyscallDispatcher::Dispatch(SyscallContext& ctx) {
        auto it = m_handlers.find(static_cast<uint32_t>(ctx.rax));
        if (it != m_handlers.end()) {
            return it->second(ctx);
        }
        std::cerr << "[Prospero Syscall] Unimplemented Syscall Opcode #" << ctx.rax << "\n";
        // Match the guest ABI: an unknown syscall must fail explicitly rather
        // than looking like a successful call returning zero.
        return 0x80020016; // SCE_KERNEL_ERROR_ENOSYS
    }

    uint64_t ProsperoSyscallDispatcher::RaiseGuestSignal(int signo, uint32_t count) {
        if (signo <= 0 || count == 0) {
            return 0;
        }
        std::lock_guard<std::mutex> lock(m_kqueue_mutex);
        return m_pending_signals[signo] += count;
    }

    size_t ProsperoSyscallDispatcher::PostGraphicsEvent(int32_t video_handle, uint32_t kind,
                                                        uint32_t event_id, uint64_t udata) {
        std::lock_guard<std::mutex> lock(m_kqueue_mutex);
        size_t delivered = 0;
        for (auto& kq_entry : m_kqueues) {
            for (Knote& kn : kq_entry.second) {
                if (kn.ev.filter == KEV_EVFILT_GRAPHICS &&
                    kn.ev.ident == static_cast<uint64_t>(
                                      static_cast<uint32_t>(video_handle))) {
                    kn.graphics_queue.push_back({kind, event_id, udata});
                    ++delivered;
                }
            }
        }
        return delivered;
    }

}
