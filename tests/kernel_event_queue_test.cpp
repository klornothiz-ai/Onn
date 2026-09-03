// ============================================================================
// ProsperoLayer RDNA2 Core - kernel event-queue deepening test (round 9)
// ----------------------------------------------------------------------------
// Part A drives the libkernel Equeue (src/kernel/event_queue.cpp) directly:
//   * user-event lifecycle (add -> trigger -> wait -> udata/data round-trip),
//   * unknown-trigger -> ENOENT, empty wait -> ETIMEDOUT (with out_num = 0),
//   * HR-timer delivery through the real delivery thread,
//   * timer cancellation (KernelDeleteHRTimerEvent suppresses delivery),
//   * the round 9 use-after-free regression: deleting an equeue with an
//     in-flight HR-timer thread now BLOCKS until the thread retires (the old
//     code freed the impl under the sleeping thread -- undefined behaviour).
//
// Part B drives the raw syscall dispatcher's kevent with EVFILT_HRTIMER
// (Sony's -15 extension to the FreeBSD-9 ABI, newly modelled in round 9):
//   * a timer knote arms from the kevent `data` microseconds,
//   * not ready before the deadline, delivered after it,
//   * EV_ONESHOT removes the knote after delivery,
//   * a timer without ONESHOT re-arms (periodic semantics),
//   * EV_DELETE cancels a pending timer,
//   * unsupported filters stay skipped fail-closed.
//
// Part C (round 10) drives the remaining event filters and the video-out
// wiring:
//   * EVFILT_SIGNAL (FreeBSD -6): knote readiness is the guest pending-signal
//     count (raised through RaiseGuestSignal); delivery reports the count and
//     consumes it (signal knotes are inherently EV_CLEAR); ONESHOT removes.
//   * EVFILT_GRAPHICS (0x200): knote ident = video-out handle; video-out
//     flips/vblanks queue triggers through PostGraphicsEvent; one trigger per
//     knote per kevent call; EV_DELETE cancels.
//   * Video-out integration: registration no longer fires events (round 10
//     fidelity fix); VideoOutSubmitFlip delivers the flip event to a libkernel
//     Equeue AND to a syscall kqueue with an EVFILT_GRAPHICS knote;
//     VideoOutGetVblankStatus fires the vblank events; Delete* removes the
//     registration.
// ============================================================================
#include "kernel/eventQueue.h"
#include "cpu/prospero_syscalls.hpp"
#include "graphics/presentation/videoOut.h"
#include "memory/virtual_memory_manager.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>

namespace {

int g_failures = 0, g_checks = 0;
bool Check(bool v, const char* e, int line) {
    ++g_checks;
    if (!v) { ++g_failures; std::cerr << "  [FAIL] line " << line << ": " << e << '\n'; }
    return v;
}
#define CHECK(e) Check((e), #e, __LINE__)

using Libs::LibKernel::EventQueue::KernelCreateEqueue;
using Libs::LibKernel::EventQueue::KernelDeleteEqueue;
using Libs::LibKernel::EventQueue::KernelWaitEqueue;
using Libs::LibKernel::EventQueue::KernelAddUserEvent;
using Libs::LibKernel::EventQueue::KernelTriggerUserEvent;
using Libs::LibKernel::EventQueue::KernelDeleteUserEvent;
using Libs::LibKernel::EventQueue::KernelAddHRTimerEvent;
using Libs::LibKernel::EventQueue::KernelDeleteHRTimerEvent;
using Libs::LibKernel::EventQueue::KernelEvent;
using Libs::LibKernel::EventQueue::KernelEqueue;

constexpr int kEINVAL = 22;
constexpr int kENOENT  = 2;
constexpr int kETIMEDOUT = 35;

using Clock = std::chrono::steady_clock;
double MsSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

// ---------------------------------------------------------------------------
// Part A: libkernel Equeue
// ---------------------------------------------------------------------------
void PartALibkernelEqueue() {
    std::cout << "--- Part A: libkernel Equeue ---\n";

    // A1. create / validation
    KernelEqueue eq = -1;
    CHECK(KernelCreateEqueue(&eq, "test_eq", 0) == 0);
    CHECK(eq != -1);
    CHECK(KernelCreateEqueue(nullptr, "bad", 0) == kEINVAL);

    // A2. user-event lifecycle: add (pre-queues a registration event),
    //     trigger, wait, round-trip udata + data.
    {
        int32_t id = 0;
        CHECK(KernelAddUserEvent(eq, &id, 0xCAFEULL) == 0);
        CHECK(id != 0);

        // Drain the registration event first.
        KernelEvent ev[2]{};
        int32_t n = 0;
        uint32_t timeout = 200000;  // microseconds
        CHECK(KernelWaitEqueue(eq, ev, 2, &n, &timeout) == 0);
        CHECK(n == 1);
        CHECK(ev[0].udata == 0xCAFEULL);
        CHECK(ev[0].event_id == id);

        CHECK(KernelTriggerUserEvent(eq, id, 7) == 0);
        timeout = 200000;
        CHECK(KernelWaitEqueue(eq, ev, 2, &n, &timeout) == 0);
        CHECK(n == 1);
        CHECK(ev[0].event_id == id);
        CHECK(ev[0].data == 7);
        CHECK(ev[0].udata == 0xCAFEULL);

        // Unknown id fails closed.
        CHECK(KernelTriggerUserEvent(eq, id + 1000, 1) == kENOENT);
        CHECK(KernelDeleteUserEvent(eq, id) == 0);
        std::cout << "  [ok] user event: add/trigger/wait round-trip + ENOENT\n";
    }

    // A3. empty wait -> ETIMEDOUT with out_num = 0.
    {
        KernelEvent ev[1]{};
        int32_t n = -1;
        uint32_t timeout = 60000;  // 60 ms
        const auto t0 = Clock::now();
        CHECK(KernelWaitEqueue(eq, ev, 1, &n, &timeout) == kETIMEDOUT);
        CHECK(n == 0);
        CHECK(MsSince(t0) >= 55.0);
        CHECK(KernelWaitEqueue(eq, nullptr, 1, &n, &timeout) == kEINVAL);
        std::cout << "  [ok] empty wait times out (ETIMEDOUT, out_num = 0)\n";
    }

    // A4. HR-timer delivery through the real delivery thread.
    {
        int32_t id = 0;
        CHECK(KernelAddHRTimerEvent(eq, &id, 0xBEEFULL, 0, 100000) == 0);  // 100 ms
        KernelEvent ev[1]{};
        int32_t n = 0;
        uint32_t timeout = 1000000;  // 1 s
        const auto t0 = Clock::now();
        CHECK(KernelWaitEqueue(eq, ev, 1, &n, &timeout) == 0);
        CHECK(n == 1);
        CHECK(ev[0].event_id == id);
        CHECK(ev[0].udata == 0xBEEFULL);
        CHECK(ev[0].filter == Libs::LibKernel::EventQueue::KERNEL_EVFILT_HRTIMER);
        CHECK(MsSince(t0) >= 90.0);   // the deadline was actually honoured
        std::cout << "  [ok] HR timer fires after the programmed delay\n";
    }

    // A5. cancelling the timer suppresses delivery.
    {
        int32_t id = 0;
        CHECK(KernelAddHRTimerEvent(eq, &id, 0, 0, 150000) == 0);  // 150 ms
        CHECK(KernelDeleteHRTimerEvent(eq, id) == 0);
        KernelEvent ev[1]{};
        int32_t n = -1;
        uint32_t timeout = 300000;
        CHECK(KernelWaitEqueue(eq, ev, 1, &n, &timeout) == kETIMEDOUT);
        CHECK(n == 0);
        std::cout << "  [ok] deleted timer is not delivered\n";
    }

    // A6. UAF regression: delete an equeue with an in-flight timer thread.
    //     The fixed deleter BLOCKS until the delivery thread retires; the old
    //     code returned immediately and freed the impl under the sleeper.
    {
        KernelEqueue victim = -1;
        CHECK(KernelCreateEqueue(&victim, "victim", 0) == 0);
        int32_t id = 0;
        CHECK(KernelAddHRTimerEvent(victim, &id, 0, 0, 350000) == 0);  // 350 ms
        const auto t0 = Clock::now();
        CHECK(KernelDeleteEqueue(victim) == 0);
        const double took_ms = MsSince(t0);
        CHECK(took_ms >= 300.0);   // the deleter actually waited
        std::cout << "  [ok] delete-with-pending-timer waits for the delivery thread ("
                  << static_cast<int>(took_ms) << " ms)\n";
    }

    // A7. the process is still healthy after the freed-impl window: a fresh
    //     equeue works end to end.
    {
        KernelEqueue eq2 = -1;
        CHECK(KernelCreateEqueue(&eq2, "after", 0) == 0);
        int32_t id = 0;
        CHECK(KernelAddUserEvent(eq2, &id, 0x1234) == 0);
        CHECK(KernelTriggerUserEvent(eq2, id, 42) == 0);
        KernelEvent ev[2]{};
        int32_t n = 0;
        uint32_t timeout = 200000;
        CHECK(KernelWaitEqueue(eq2, ev, 2, &n, &timeout) == 0);
        CHECK(n == 2);  // registration + trigger
        CHECK(KernelDeleteEqueue(eq2) == 0);
        std::cout << "  [ok] equeue lifecycle healthy after the UAF regression run\n";
    }

    CHECK(KernelDeleteEqueue(eq) == 0);
}

// ---------------------------------------------------------------------------
// Part B: syscall kevent with EVFILT_HRTIMER
// ---------------------------------------------------------------------------
void PartBKeventHrtimer() {
    std::cout << "--- Part B: syscall kevent EVFILT_HRTIMER ---\n";

    using PS5::CPU::ProsperoSyscall;
    using PS5::CPU::ProsperoSyscallDispatcher;
    using PS5::CPU::SyscallContext;
    using GuestKevent = ProsperoSyscallDispatcher::GuestKevent;
    auto& vmm = PS5::Memory::VirtualMemoryManager::Instance();

    constexpr int16_t  EVFILT_HRTIMER = -15;
    constexpr int16_t  EVFILT_UNSUPPORTED = -1;   // EVFILT_READ: not modelled
    constexpr uint16_t EV_ADD     = 0x0001;
    constexpr uint16_t EV_DELETE  = 0x0002;
    constexpr uint16_t EV_ONESHOT = 0x0010;
    constexpr uint16_t EV_CLEAR   = 0x0020;
    constexpr uint32_t kRW = static_cast<uint32_t>(PS5::Memory::PageProt::Read) |
                             static_cast<uint32_t>(PS5::Memory::PageProt::Write);

    constexpr uint64_t kChange = 0x1370000000ULL;
    constexpr uint64_t kEvent  = 0x1370010000ULL;
    CHECK(vmm.AllocateVirtual(kChange, 4096, kRW) == kChange);
    CHECK(vmm.AllocateVirtual(kEvent, 4096, kRW) == kEvent);
    auto* change = static_cast<GuestKevent*>(vmm.GvaToHva(kChange));
    auto* event  = static_cast<GuestKevent*>(vmm.GvaToHva(kEvent));

    auto& d = ProsperoSyscallDispatcher::Instance();
    const auto op = [](ProsperoSyscall sc) { return static_cast<uint32_t>(sc); };

    const auto make_kqueue = [&]() -> int {
        SyscallContext kq{};
        kq.rax = op(ProsperoSyscall::SC_SYS_kqueue);
        return static_cast<int>(d.Dispatch(kq));
    };
    const auto kevent = [&](int kq, int nchanges, int nevents) -> uint64_t {
        SyscallContext ev{};
        ev.rax = op(ProsperoSyscall::SC_SYS_kevent);
        ev.rdi = static_cast<uint64_t>(kq);
        ev.rsi = kChange;
        ev.rdx = static_cast<uint64_t>(nchanges);
        ev.rcx = kEvent;
        ev.r8  = static_cast<uint64_t>(nevents);
        return d.Dispatch(ev);
    };
    const auto put_change = [&](int slot, uint64_t ident, int16_t filter,
                                uint16_t flags, int64_t data, uint64_t udata) {
        GuestKevent kv{};
        kv.ident = ident; kv.filter = filter; kv.flags = flags;
        kv.data = data; kv.udata = udata;
        std::memcpy(change + slot, &kv, sizeof(kv));
    };

    const int kq = make_kqueue();
    CHECK(kq != 0);

    // B1. arm a 120 ms timer; not ready before the deadline.
    put_change(0, 0x77, EVFILT_HRTIMER, EV_ADD, 120000 /* us */, 0x5150);
    CHECK(kevent(kq, 1, 4) == 0);   // applies the changelist, nothing ready yet

    // B2. after the deadline it is delivered with ident/udata passthrough.
    std::this_thread::sleep_for(std::chrono::milliseconds(160));
    CHECK(kevent(kq, 0, 4) == 1);
    CHECK(event[0].filter == EVFILT_HRTIMER);
    CHECK(event[0].ident == 0x77);
    CHECK(event[0].udata == 0x5150);
    std::cout << "  [ok] timer knote delivered after its deadline\n";

    // B3. without ONESHOT the timer re-arms (periodic semantics).
    std::this_thread::sleep_for(std::chrono::milliseconds(140));
    CHECK(kevent(kq, 0, 4) == 1);
    CHECK(event[0].ident == 0x77);
    std::cout << "  [ok] timer without ONESHOT re-arms periodically\n";

    // B4. EV_DELETE cancels the periodic timer.
    put_change(0, 0x77, EVFILT_HRTIMER, EV_DELETE, 0, 0);
    CHECK(kevent(kq, 1, 4) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(140));
    CHECK(kevent(kq, 0, 4) == 0);
    std::cout << "  [ok] EV_DELETE cancels a pending timer\n";

    // B5. ONESHOT: delivered once, then the knote is gone.
    put_change(0, 0x88, EVFILT_HRTIMER, EV_ADD | EV_ONESHOT, 30000, 0);
    CHECK(kevent(kq, 1, 4) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    CHECK(kevent(kq, 0, 4) == 1);
    CHECK(event[0].ident == 0x88);
    CHECK(kevent(kq, 0, 4) == 0);   // erased, not re-armed
    std::cout << "  [ok] EV_ONESHOT timer fires exactly once\n";

    // B6. unsupported filters are skipped fail-closed (no crash, no event).
    put_change(0, 0x99, EVFILT_UNSUPPORTED, EV_ADD, 1000, 0);
    CHECK(kevent(kq, 1, 4) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(kevent(kq, 0, 4) == 0);
    std::cout << "  [ok] unsupported filter skipped fail-closed\n";

    // B7. an EVFILT_USER knote and an HRTIMER knote coexist; both delivered.
    //     (The user knote registers with EV_CLEAR so it de-arms after its
    //     first delivery, like a real guest would.)
    put_change(0, 0x42, -11 /* EVFILT_USER */, EV_ADD | EV_CLEAR, 0, 0xAAAA);
    put_change(1, 0x66, EVFILT_HRTIMER, EV_ADD | EV_ONESHOT, 30000, 0xBBBB);
    CHECK(kevent(kq, 2, 4) == 0);
    // NOTE_TRIGGER arms the user knote immediately.
    {
        GuestKevent kv{};
        kv.ident = 0x42; kv.filter = -11;
        kv.flags = 0; kv.fflags = 0x01000000 /* NOTE_TRIGGER */;
        std::memcpy(change, &kv, sizeof(kv));
    }
    CHECK(kevent(kq, 1, 4) == 1);           // user event ready now
    CHECK(event[0].ident == 0x42);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(kevent(kq, 0, 4) == 1);           // timer ready after the deadline
    CHECK(event[0].ident == 0x66);
    CHECK(event[0].udata == 0xBBBB);
    std::cout << "  [ok] user + timer knotes coexist on one kqueue\n";
}

// ---------------------------------------------------------------------------
// Part C (round 10): EVFILT_SIGNAL + EVFILT_GRAPHICS + the video-out wiring.
// ---------------------------------------------------------------------------
void PartCsignalsGraphicsAndVideoOut() {
    std::cout << "--- Part C: EVFILT_SIGNAL / EVFILT_GRAPHICS + video-out wiring ---\n";

    using PS5::CPU::ProsperoSyscall;
    using PS5::CPU::ProsperoSyscallDispatcher;
    using PS5::CPU::SyscallContext;
    using GuestKevent = ProsperoSyscallDispatcher::GuestKevent;
    auto& vmm = PS5::Memory::VirtualMemoryManager::Instance();

    constexpr int16_t  EVFILT_SIGNAL   = ProsperoSyscallDispatcher::KEV_EVFILT_SIGNAL;
    constexpr int16_t  EVFILT_GRAPHICS = ProsperoSyscallDispatcher::KEV_EVFILT_GRAPHICS;
    constexpr uint16_t EV_ADD     = 0x0001;
    constexpr uint16_t EV_DELETE  = 0x0002;
    constexpr uint16_t EV_ONESHOT = 0x0010;
    constexpr uint32_t kRW = static_cast<uint32_t>(PS5::Memory::PageProt::Read) |
                             static_cast<uint32_t>(PS5::Memory::PageProt::Write);

    constexpr uint64_t kChangeC = 0x1380000000ULL;
    constexpr uint64_t kEventC  = 0x1380010000ULL;
    CHECK(vmm.AllocateVirtual(kChangeC, 4096, kRW) == kChangeC);
    CHECK(vmm.AllocateVirtual(kEventC, 4096, kRW) == kEventC);
    auto* change = static_cast<GuestKevent*>(vmm.GvaToHva(kChangeC));
    auto* event  = static_cast<GuestKevent*>(vmm.GvaToHva(kEventC));

    auto& d = ProsperoSyscallDispatcher::Instance();
    const auto op = [](ProsperoSyscall sc) { return static_cast<uint32_t>(sc); };

    const auto make_kqueue = [&]() -> int {
        SyscallContext kq{};
        kq.rax = op(ProsperoSyscall::SC_SYS_kqueue);
        return static_cast<int>(d.Dispatch(kq));
    };
    const auto kevent = [&](int kq, int nchanges, int nevents) -> uint64_t {
        SyscallContext ev{};
        ev.rax = op(ProsperoSyscall::SC_SYS_kevent);
        ev.rdi = static_cast<uint64_t>(kq);
        ev.rsi = kChangeC;
        ev.rdx = static_cast<uint64_t>(nchanges);
        ev.rcx = kEventC;
        ev.r8  = static_cast<uint64_t>(nevents);
        return d.Dispatch(ev);
    };
    const auto put_change = [&](int slot, uint64_t ident, int16_t filter,
                                uint16_t flags, int64_t data, uint64_t udata) {
        GuestKevent kv{};
        kv.ident = ident; kv.filter = filter; kv.flags = flags;
        kv.data = data; kv.udata = udata;
        std::memcpy(change + slot, &kv, sizeof(kv));
    };

    // ===== EVFILT_SIGNAL =====================================================

    // C1. register a signal knote for SIGKILL (9); not ready while nothing
    //     is pending.
    const int kq = make_kqueue();
    CHECK(kq != 0);
    put_change(0, 9, EVFILT_SIGNAL, EV_ADD, 0, 0x5169);
    CHECK(kevent(kq, 1, 4) == 0);

    // C2. RaiseGuestSignal(9) makes it ready; delivery reports the pending
    //     count in `data` and consumes it (inherent EV_CLEAR).
    CHECK(d.RaiseGuestSignal(9, 1) == 1);
    CHECK(kevent(kq, 0, 4) == 1);
    CHECK(event[0].filter == EVFILT_SIGNAL);
    CHECK(event[0].ident == 9);
    CHECK(event[0].data == 1);
    CHECK(event[0].udata == 0x5169);
    CHECK(kevent(kq, 0, 4) == 0);   // pending count consumed
    std::cout << "  [ok] EVFILT_SIGNAL: raised -> delivered with count -> consumed\n";

    // C3. the count ACCUMULATES between kevent calls and is reported whole.
    CHECK(d.RaiseGuestSignal(9, 1) == 1);
    CHECK(d.RaiseGuestSignal(9, 2) == 3);
    CHECK(kevent(kq, 0, 4) == 1);
    CHECK(event[0].data == 3);
    CHECK(kevent(kq, 0, 4) == 0);
    std::cout << "  [ok] EVFILT_SIGNAL: pending count accumulates (3) and reports whole\n";

    // C4. ONESHOT signal knote: delivered once, then erased.
    put_change(0, 15, EVFILT_SIGNAL, EV_ADD | EV_ONESHOT, 0, 0x7070);
    CHECK(kevent(kq, 1, 4) == 0);
    CHECK(d.RaiseGuestSignal(15, 1) == 1);
    CHECK(kevent(kq, 0, 4) == 1);
    CHECK(event[0].ident == 15);
    CHECK(kevent(kq, 0, 4) == 0);   // knote erased by ONESHOT
    std::cout << "  [ok] EVFILT_SIGNAL: ONESHOT fires exactly once\n";

    // C5. EV_DELETE removes the SIGKILL knote; later raises go unheard.
    put_change(0, 9, EVFILT_SIGNAL, EV_DELETE, 0, 0);
    CHECK(kevent(kq, 1, 4) == 0);
    d.RaiseGuestSignal(9, 1);
    CHECK(kevent(kq, 0, 4) == 0);
    std::cout << "  [ok] EVFILT_SIGNAL: EV_DELETE cancels the knote\n";

    // ===== EVFILT_GRAPHICS + the video-out wiring ============================

    // C6. open a video-out handle; registration no longer fires anything
    //     (round 10 fidelity fix: registering is not flipping).
    int32_t vo = -1;
    CHECK(Libs::Graphics::VideoOut::VideoOutOpen(1, 0, 0, &vo) == 0);
    CHECK(vo > 0);

    // A syscall kqueue with an EVFILT_GRAPHICS knote for this handle.
    const int gfx_kq = make_kqueue();
    put_change(0, static_cast<uint64_t>(vo), EVFILT_GRAPHICS, EV_ADD, 0, 0);
    // (udata 0: the trigger carries its own udata)
    CHECK(kevent(gfx_kq, 1, 4) == 0);

    // Nothing fired yet: registration is silent and no flip happened.
    CHECK(kevent(gfx_kq, 0, 4) == 0);

    // C7. PostGraphicsEvent queues a flip trigger on the matching knote only.
    CHECK(d.PostGraphicsEvent(vo, ProsperoSyscallDispatcher::GRAPHICS_EVENT_FLIP,
                              7, 0xABCD) == 1);
    // A different video handle has no knote on this kqueue.
    CHECK(d.PostGraphicsEvent(vo + 1000,
                              ProsperoSyscallDispatcher::GRAPHICS_EVENT_FLIP, 8, 0) == 0);
    CHECK(kevent(gfx_kq, 0, 4) == 1);
    CHECK(event[0].filter == EVFILT_GRAPHICS);
    CHECK(event[0].ident == static_cast<uint64_t>(vo));
    CHECK(event[0].fflags == ProsperoSyscallDispatcher::GRAPHICS_EVENT_FLIP);
    CHECK(event[0].data == 7);
    CHECK(event[0].udata == 0xABCD);
    CHECK(kevent(gfx_kq, 0, 4) == 0);   // exactly one trigger
    std::cout << "  [ok] EVFILT_GRAPHICS: flip trigger queued, delivered once, "
                 "other handles unheard\n";

    // C8. multiple pending triggers deliver one per kevent call (FIFO).
    CHECK(d.PostGraphicsEvent(vo, ProsperoSyscallDispatcher::GRAPHICS_EVENT_FLIP, 21, 1) == 1);
    CHECK(d.PostGraphicsEvent(vo, ProsperoSyscallDispatcher::GRAPHICS_EVENT_VBLANK, 22, 2) == 1);
    CHECK(kevent(gfx_kq, 0, 4) == 1);
    CHECK(event[0].fflags == ProsperoSyscallDispatcher::GRAPHICS_EVENT_FLIP);
    CHECK(event[0].data == 21);
    CHECK(event[0].udata == 1);
    CHECK(kevent(gfx_kq, 0, 4) == 1);
    CHECK(event[0].fflags == ProsperoSyscallDispatcher::GRAPHICS_EVENT_VBLANK);
    CHECK(event[0].data == 22);
    CHECK(event[0].udata == 2);
    CHECK(kevent(gfx_kq, 0, 4) == 0);
    std::cout << "  [ok] EVFILT_GRAPHICS: two queued triggers deliver FIFO, one per call\n";

    // C9. THE WIRING: VideoOutSubmitFlip fans the flip event out to BOTH queue
    //     systems -- a libkernel Equeue (KernelWaitEqueue sees it) and the
    //     syscall kqueue (EVFILT_GRAPHICS trigger).
    Libs::LibKernel::EventQueue::KernelEqueue eq = -1;
    CHECK(Libs::LibKernel::EventQueue::KernelCreateEqueue(&eq, "flip_eq", 0) == 0);
    CHECK(eq > 0);
    CHECK(Libs::Graphics::VideoOut::VideoOutAddFlipEvent(vo, eq, 55, 0xFEED) == 0);
    // No flip yet: neither the equeue nor the kqueue has anything.
    {
        Libs::LibKernel::EventQueue::KernelEvent ev{};
        int32_t n = -1;
        uint32_t zero_timeout = 0;
        // A zero-wait poll: KernelWaitEqueue with timeout 0 means wait forever
        // in this ABI, so instead assert via the kqueue that nothing fired.
        CHECK(kevent(gfx_kq, 0, 4) == 0);
        (void)ev; (void)n; (void)zero_timeout;
    }
    CHECK(Libs::Graphics::VideoOut::VideoOutSubmitFlip(vo, 0, 0, 0) == 0);
    // The syscall-kqueue side:
    CHECK(kevent(gfx_kq, 0, 4) == 1);
    CHECK(event[0].filter == EVFILT_GRAPHICS);
    CHECK(event[0].ident == static_cast<uint64_t>(vo));
    CHECK(event[0].fflags == ProsperoSyscallDispatcher::GRAPHICS_EVENT_FLIP);
    CHECK(event[0].data == 55);
    CHECK(event[0].udata == 0xFEED);
    // The libkernel-Equeue side:
    {
        Libs::LibKernel::EventQueue::KernelEvent ev{};
        int32_t n = 0;
        uint32_t timeout = 50;   // ms
        CHECK(Libs::LibKernel::EventQueue::KernelWaitEqueue(eq, &ev, 4, &n, &timeout) == 0);
        CHECK(n == 1);
        CHECK(ev.filter == Libs::LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS);
        CHECK(ev.event_id == 55);
        CHECK(ev.udata == 0xFEED);
    }
    std::cout << "  [ok] video-out flip wired to BOTH the syscall kqueue and the "
                 "libkernel Equeue\n";

    // C10. DeleteFlipEvent removes the registration: the next flip is silent.
    CHECK(Libs::Graphics::VideoOut::VideoOutDeleteFlipEvent(vo, eq, 55) == 0);
    CHECK(Libs::Graphics::VideoOut::VideoOutSubmitFlip(vo, 0, 0, 0) == 0);
    CHECK(kevent(gfx_kq, 0, 4) == 0);
    {
        Libs::LibKernel::EventQueue::KernelEvent ev{};
        int32_t n = 0;
        uint32_t timeout = 50;
        CHECK(Libs::LibKernel::EventQueue::KernelWaitEqueue(eq, &ev, 4, &n, &timeout) ==
              35 /* ETIMEDOUT */);
        CHECK(n == 0);
    }
    std::cout << "  [ok] VideoOutDeleteFlipEvent removes the registration\n";

    // C11. vblank: each VideoOutGetVblankStatus poll IS a vblank tick in the
    //      headless model, firing the registered vblank events.
    CHECK(Libs::Graphics::VideoOut::VideoOutAddVblankEvent(vo, eq, 77, 0xBEF0) == 0);
    int32_t vblank_counter = 0;
    CHECK(Libs::Graphics::VideoOut::VideoOutGetVblankStatus(vo, &vblank_counter) == 0);
    CHECK(vblank_counter == 1);
    // The vblank event reached the libkernel equeue...
    {
        Libs::LibKernel::EventQueue::KernelEvent ev{};
        int32_t n = 0;
        uint32_t timeout = 50;
        CHECK(Libs::LibKernel::EventQueue::KernelWaitEqueue(eq, &ev, 4, &n, &timeout) == 0);
        CHECK(n == 1);
        CHECK(ev.event_id == 77);
        CHECK(ev.udata == 0xBEF0);
    }
    // ...and the syscall kqueue (kind = VBLANK, event id 77).
    CHECK(kevent(gfx_kq, 0, 4) == 1);
    CHECK(event[0].fflags == ProsperoSyscallDispatcher::GRAPHICS_EVENT_VBLANK);
    CHECK(event[0].data == 77);
    CHECK(event[0].udata == 0xBEF0);
    std::cout << "  [ok] vblank tick fires the vblank event on both queue systems\n";

    // C12. EV_DELETE cancels the graphics knote; later flips are unheard.
    put_change(0, static_cast<uint64_t>(vo), EVFILT_GRAPHICS, EV_DELETE, 0, 0);
    CHECK(kevent(gfx_kq, 1, 4) == 0);
    CHECK(Libs::Graphics::VideoOut::VideoOutSubmitFlip(vo, 0, 0, 0) == 0);
    CHECK(kevent(gfx_kq, 0, 4) == 0);
    std::cout << "  [ok] EVFILT_GRAPHICS: EV_DELETE cancels the knote\n";

    CHECK(Libs::Graphics::VideoOut::VideoOutClose(vo) == 0);
}

} // namespace

int main() {
    std::cout << "=== Kernel Event-Queue Deepening Test (rounds 9 + 10) ===\n";
    PartALibkernelEqueue();
    PartBKeventHrtimer();
    PartCsignalsGraphicsAndVideoOut();
    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    if (g_failures == 0) {
        std::cout << ">> [PASS] Equeue lifecycle + UAF fix + kevent EVFILT_HRTIMER + "
                     "EVFILT_SIGNAL/GRAPHICS + video-out flip/vblank wiring verified.\n";
    }
    return g_failures == 0 ? 0 : 1;
}
