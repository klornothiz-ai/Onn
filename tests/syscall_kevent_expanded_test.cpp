// ============================================================================
// ProsperoLayer RDNA2 Core - Syscall Expanded Test Suite (item #4)
// ============================================================================
// Extends syscall_dispatcher_test.cpp with the syscalls that were previously
// stubbed or unimplemented and are now real:
//
//   * kevent(EVFILT_USER): the changelist is actually applied to the kqueue's
//     knote set and ready knotes are returned through the eventlist --
//       - EV_ADD registers a user knote that is NOT ready until triggered,
//       - NOTE_TRIGGER makes it ready and kevent copies it to the eventlist
//         (preserving ident/udata and masking the control bits off fflags),
//       - EV_CLEAR re-arms (a second poll returns 0 until re-triggered),
//       - EV_ONESHOT deletes the knote after one delivery,
//       - EV_DELETE removes a knote,
//       - a bad kq descriptor fails closed (EBADF),
//       - the eventlist is bounded by nevents.
//   * evf_cancel: forces a blocked waiter to return failure, resets the
//     pattern, and reports the pre-cancel pattern to the guest out-pointer.
//   * open: opens a real host path through a guest-memory path string, returns
//     a usable fd (read back through the read syscall), and fails closed on a
//     null path and a missing file.
//
// Uses the same lightweight harness and the real VMM-backed dispatcher.
// ============================================================================

#include "cpu/prospero_syscalls.hpp"
#include "memory/virtual_memory_manager.hpp"
#include "kernel/event_flag.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>

namespace {

using PS5::CPU::ProsperoSyscall;
using PS5::CPU::ProsperoSyscallDispatcher;
using PS5::CPU::SyscallContext;
using GuestKevent = ProsperoSyscallDispatcher::GuestKevent;
using PS5::Memory::PageProt;
using PS5::Memory::VirtualMemoryManager;

constexpr uint32_t kRW = static_cast<uint32_t>(PageProt::Read) |
                         static_cast<uint32_t>(PageProt::Write);

bool Check(bool value, const char* expression, int line) {
    if (!value) {
        std::cerr << "check failed at line " << line << ": " << expression << '\n';
    }
    return value;
}

#define CHECK(expression) \
    do { \
        if (!Check((expression), #expression, __LINE__)) return false; \
    } while (false)

constexpr uint32_t Op(ProsperoSyscall sc) { return static_cast<uint32_t>(sc); }

// FreeBSD kevent constants mirrored for the test side.
constexpr int16_t  EVFILT_USER  = -11;
constexpr uint16_t EV_ADD       = 0x0001;
constexpr uint16_t EV_DELETE    = 0x0002;
constexpr uint16_t EV_ONESHOT   = 0x0010;
constexpr uint16_t EV_CLEAR     = 0x0020;
constexpr uint32_t NOTE_TRIGGER = 0x01000000;
constexpr uint32_t NOTE_FFOR    = 0x80000000; // OR incoming fflags into stored

struct StderrSilencer {
    std::ostringstream sink;
    std::streambuf* previous;
    StderrSilencer() : previous(std::cerr.rdbuf(sink.rdbuf())) {}
    ~StderrSilencer() { std::cerr.rdbuf(previous); }
};

// Write a value-initialized kevent into a guest region (avoids memset on a
// non-trivial type under -Werror=class-memaccess).
void WriteKevent(void* dst, const GuestKevent& kv) {
    std::memcpy(dst, &kv, sizeof(GuestKevent));
}

// Allocate a fresh guest region and return its GVA.
uint64_t AllocRegion(uint64_t gva, size_t size) {
    auto& vmm = VirtualMemoryManager::Instance();
    return vmm.AllocateVirtual(gva, size, kRW);
}

int MakeKqueue() {
    auto& d = ProsperoSyscallDispatcher::Instance();
    SyscallContext kq{}; kq.rax = Op(ProsperoSyscall::SC_SYS_kqueue);
    return static_cast<int>(d.Dispatch(kq));
}

// Issue kevent(kq, changelist, nchanges, eventlist, nevents).
uint64_t Kevent(int kq, uint64_t changelist, int nchanges, uint64_t eventlist, int nevents) {
    auto& d = ProsperoSyscallDispatcher::Instance();
    SyscallContext ev{};
    ev.rax = Op(ProsperoSyscall::SC_SYS_kevent);
    ev.rdi = static_cast<uint64_t>(kq);
    ev.rsi = changelist;
    ev.rdx = static_cast<uint64_t>(nchanges);
    ev.rcx = eventlist;
    ev.r8  = static_cast<uint64_t>(nevents);
    return d.Dispatch(ev);
}

// ---------------------------------------------------------------------------
// kevent EVFILT_USER: register, trigger, deliver, re-arm, oneshot, delete.
// ---------------------------------------------------------------------------
bool KeventUserFilterFullLifecycle() {
    auto& vmm = VirtualMemoryManager::Instance();
    const int kq = MakeKqueue();
    CHECK(kq != 0);

    constexpr uint64_t kChange = 0x1360000000ULL;
    constexpr uint64_t kEvent  = 0x1360010000ULL;
    CHECK(AllocRegion(kChange, 4096) == kChange);
    CHECK(AllocRegion(kEvent, 4096) == kEvent);
    auto* change = static_cast<GuestKevent*>(vmm.GvaToHva(kChange));
    auto* event  = static_cast<GuestKevent*>(vmm.GvaToHva(kEvent));

    // 1. EV_ADD a user knote (ident 0x42, udata 0xCAFE). Not yet triggered.
    {
        GuestKevent kv{};
        kv.ident = 0x42; kv.filter = EVFILT_USER;
        kv.flags = EV_ADD | EV_CLEAR; kv.udata = 0xCAFEULL;
        WriteKevent(change, kv);
    }
    CHECK(Kevent(kq, kChange, 1, 0, 0) == 0);

    // 2. Poll: nothing ready yet.
    CHECK(Kevent(kq, 0, 0, kEvent, 4) == 0);

    // 3. NOTE_TRIGGER it; a poll must now report exactly one event, preserving
    //    ident + udata and masking the control bits off fflags.
    {
        GuestKevent kv{};
        kv.ident = 0x42; kv.filter = EVFILT_USER;
        // FFOR OR-s the 0x7 user-data bits into the stored fflags; FFNOP would
        // (correctly) ignore them, matching FreeBSD EVFILT_USER semantics.
        kv.fflags = NOTE_TRIGGER | NOTE_FFOR | 0x7;
        WriteKevent(change, kv);
    }
    CHECK(Kevent(kq, kChange, 1, 0, 0) == 0);

    WriteKevent(event, GuestKevent{});
    CHECK(Kevent(kq, 0, 0, kEvent, 4) == 1);
    CHECK(event->ident == 0x42);
    CHECK(event->udata == 0xCAFEULL);
    CHECK((event->fflags & NOTE_TRIGGER) == 0); // control bits masked off
    CHECK((event->fflags & 0x7u) == 0x7u);      // user data survives

    // 4. EV_CLEAR means the event auto-clears after delivery: a second poll
    //    returns 0 until re-triggered.
    CHECK(Kevent(kq, 0, 0, kEvent, 4) == 0);

    // 5. EV_DELETE the knote; even after a trigger it must not fire.
    {
        GuestKevent kv{};
        kv.ident = 0x42; kv.filter = EVFILT_USER; kv.flags = EV_DELETE;
        WriteKevent(change, kv);
    }
    CHECK(Kevent(kq, kChange, 1, 0, 0) == 0);
    // Trigger a now-deleted ident: nothing should be ready.
    {
        GuestKevent kv{};
        kv.ident = 0x42; kv.filter = EVFILT_USER; kv.fflags = NOTE_TRIGGER;
        WriteKevent(change, kv);
    }
    CHECK(Kevent(kq, kChange, 1, 0, 0) == 0);
    CHECK(Kevent(kq, 0, 0, kEvent, 4) == 0);
    return true;
}

// EV_ONESHOT delivers exactly once, then removes the knote.
bool KeventOneshotDeliversOnce() {
    auto& vmm = VirtualMemoryManager::Instance();
    const int kq = MakeKqueue();
    constexpr uint64_t kChange = 0x1360020000ULL;
    constexpr uint64_t kEvent  = 0x1360030000ULL;
    CHECK(AllocRegion(kChange, 4096) == kChange);
    CHECK(AllocRegion(kEvent, 4096) == kEvent);
    auto* change = static_cast<GuestKevent*>(vmm.GvaToHva(kChange));

    {
        GuestKevent kv{};
        kv.ident = 0x99; kv.filter = EVFILT_USER;
        kv.flags = EV_ADD | EV_ONESHOT; kv.fflags = NOTE_TRIGGER;
        WriteKevent(change, kv);
    }
    CHECK(Kevent(kq, kChange, 1, 0, 0) == 0);

    CHECK(Kevent(kq, 0, 0, kEvent, 4) == 1); // delivered once
    CHECK(Kevent(kq, 0, 0, kEvent, 4) == 0); // knote gone
    return true;
}

// A bad kqueue descriptor fails closed with EBADF.
bool KeventBadDescriptorFailsClosed() {
    StderrSilencer quiet;
    CHECK(Kevent(/*kq=*/0x7fffffff, 0, 0, 0, 0) == 0x80020009ULL); // EBADF
    return true;
}

// The number of reported events is bounded by nevents.
bool KeventRespectsEventlistCapacity() {
    auto& vmm = VirtualMemoryManager::Instance();
    const int kq = MakeKqueue();
    constexpr uint64_t kChange = 0x1360040000ULL;
    constexpr uint64_t kEvent  = 0x1360050000ULL;
    CHECK(AllocRegion(kChange, 4096) == kChange);
    CHECK(AllocRegion(kEvent, 4096) == kEvent);
    auto* change = static_cast<GuestKevent*>(vmm.GvaToHva(kChange));

    // Register + trigger three user knotes.
    for (uint64_t id = 1; id <= 3; ++id) {
        GuestKevent kv{};
        kv.ident = id; kv.filter = EVFILT_USER;
        kv.flags = EV_ADD; kv.fflags = NOTE_TRIGGER;
        WriteKevent(change, kv);
        CHECK(Kevent(kq, kChange, 1, 0, 0) == 0);
    }
    // Ask for at most 2: exactly 2 delivered.
    CHECK(Kevent(kq, 0, 0, kEvent, 2) == 2);
    return true;
}

// ---------------------------------------------------------------------------
// evf_cancel: force a blocked waiter to fail, reset pattern, report old pattern.
// ---------------------------------------------------------------------------
bool EvfCancelWakesWaiterAndReportsPattern() {
    auto& d = ProsperoSyscallDispatcher::Instance();
    auto& vmm = VirtualMemoryManager::Instance();

    // Create an event flag (init pattern 0).
    SyscallContext create{};
    create.rax = Op(ProsperoSyscall::SC_SYS_evf_create);
    create.rdi = 0;   // attr
    create.rsi = 0;   // init pattern
    const uint32_t handle = static_cast<uint32_t>(d.Dispatch(create));
    CHECK(handle != 0);

    // Set a distinctive pattern so cancel can report it.
    SyscallContext set{};
    set.rax = Op(ProsperoSyscall::SC_SYS_evf_set);
    set.rdi = handle;
    set.rsi = 0xABCDULL;
    CHECK(d.Dispatch(set) == 0);

    // A thread blocks waiting for a bit that will never be set (0x1 in AND mode).
    std::atomic<bool> returned{false};
    std::atomic<uint64_t> wait_rc{0};
    std::thread waiter([&] {
        SyscallContext wait{};
        wait.rax = Op(ProsperoSyscall::SC_SYS_evf_wait);
        wait.rdi = handle;
        wait.rsi = 0x10000;   // bit 16: never set in 0xABCD, so the wait blocks
        wait.rdx = PS5::Kernel::SCE_KERNEL_EVF_WAITMODE_AND;
        wait.rcx = 0;     // no out-bits pointer
        wait_rc.store(d.Dispatch(wait));
        returned.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(!returned.load()); // still blocked

    // Cancel: reset pattern to 0, report the pre-cancel pattern into guest mem.
    constexpr uint64_t kOut = 0x1360060000ULL;
    CHECK(AllocRegion(kOut, 4096) == kOut);
    auto* out = static_cast<uint64_t*>(vmm.GvaToHva(kOut));
    *out = 0;
    SyscallContext cancel{};
    cancel.rax = Op(ProsperoSyscall::SC_SYS_evf_cancel);
    cancel.rdi = handle;
    cancel.rsi = 0;      // set pattern after cancel
    cancel.rdx = kOut;   // out pattern pointer
    CHECK(d.Dispatch(cancel) == 0);

    waiter.join();
    CHECK(returned.load());
    CHECK(wait_rc.load() != 0);        // the cancelled wait failed
    CHECK(*out == 0xABCDULL);          // pre-cancel pattern reported

    // Delete the flag.
    SyscallContext del{};
    del.rax = Op(ProsperoSyscall::SC_SYS_evf_delete);
    del.rdi = handle;
    CHECK(d.Dispatch(del) == 0);
    return true;
}

// evf_cancel on a bad handle fails closed.
bool EvfCancelBadHandleFailsClosed() {
    auto& d = ProsperoSyscallDispatcher::Instance();
    SyscallContext cancel{};
    cancel.rax = Op(ProsperoSyscall::SC_SYS_evf_cancel);
    cancel.rdi = 0x7fffffff;
    CHECK(d.Dispatch(cancel) != 0);
    return true;
}

// ---------------------------------------------------------------------------
// open: real host open through a guest path, read back, and fail-closed cases.
// ---------------------------------------------------------------------------
bool OpenReadsRealHostFile() {
    auto& d = ProsperoSyscallDispatcher::Instance();
    auto& vmm = VirtualMemoryManager::Instance();

    // Create a temp file with known content.
    const char* path = "/tmp/prosperolayer_open_test.txt";
    const char* content = "PROSPERO";
    {
        std::FILE* f = std::fopen(path, "wb");
        CHECK(f != nullptr);
        std::fwrite(content, 1, std::strlen(content), f);
        std::fclose(f);
    }

    // Put the path string into guest memory.
    constexpr uint64_t kPath = 0x1360070000ULL;
    constexpr uint64_t kBuf  = 0x1360080000ULL;
    CHECK(AllocRegion(kPath, 4096) == kPath);
    CHECK(AllocRegion(kBuf, 4096) == kBuf);
    std::strcpy(static_cast<char*>(vmm.GvaToHva(kPath)), path);

    // open(path, O_RDONLY, 0).
    SyscallContext open{};
    open.rax = Op(ProsperoSyscall::SC_SYS_open);
    open.rdi = kPath;
    open.rsi = 0; // O_RDONLY
    open.rdx = 0;
    const uint64_t fd = d.Dispatch(open);
    CHECK(fd < 0x80000000ULL); // a real, small fd, not an SCE error code
    CHECK(fd > 2);             // beyond stdio

    // read(fd, buf, 8) through the read syscall; verify the bytes.
    SyscallContext rd{};
    rd.rax = Op(ProsperoSyscall::SC_SYS_read);
    rd.rdi = fd;
    rd.rsi = kBuf;
    rd.rdx = 8;
    CHECK(d.Dispatch(rd) == 8);
    CHECK(std::memcmp(vmm.GvaToHva(kBuf), content, 8) == 0);

    // close(fd).
    SyscallContext cl{};
    cl.rax = Op(ProsperoSyscall::SC_SYS_close);
    cl.rdi = fd;
    CHECK(d.Dispatch(cl) == 0);

    std::remove(path);
    return true;
}

bool OpenFailsClosedOnNullAndMissing() {
    auto& d = ProsperoSyscallDispatcher::Instance();
    auto& vmm = VirtualMemoryManager::Instance();
    StderrSilencer quiet;

    // Null path -> EINVAL.
    SyscallContext open_null{};
    open_null.rax = Op(ProsperoSyscall::SC_SYS_open);
    open_null.rdi = 0;
    CHECK(d.Dispatch(open_null) == 0x8002000AULL);

    // Missing file -> ENOENT.
    constexpr uint64_t kPath = 0x1360090000ULL;
    CHECK(AllocRegion(kPath, 4096) == kPath);
    std::strcpy(static_cast<char*>(vmm.GvaToHva(kPath)),
                "/tmp/prosperolayer_does_not_exist_zzz.bin");
    SyscallContext open_missing{};
    open_missing.rax = Op(ProsperoSyscall::SC_SYS_open);
    open_missing.rdi = kPath;
    open_missing.rsi = 0;
    CHECK(d.Dispatch(open_missing) == 0x80020002ULL);
    return true;
}

struct TestCase {
    const char* name;
    bool (*function)();
};

} // namespace

int main() {
    std::cout << "=== Syscall Expanded Test Suite (kevent / evf_cancel / open) ===\n";
    const TestCase tests[] = {
        {"KeventUserFilterFullLifecycle", KeventUserFilterFullLifecycle},
        {"KeventOneshotDeliversOnce", KeventOneshotDeliversOnce},
        {"KeventBadDescriptorFailsClosed", KeventBadDescriptorFailsClosed},
        {"KeventRespectsEventlistCapacity", KeventRespectsEventlistCapacity},
        {"EvfCancelWakesWaiterAndReportsPattern", EvfCancelWakesWaiterAndReportsPattern},
        {"EvfCancelBadHandleFailsClosed", EvfCancelBadHandleFailsClosed},
        {"OpenReadsRealHostFile", OpenReadsRealHostFile},
        {"OpenFailsClosedOnNullAndMissing", OpenFailsClosedOnNullAndMissing},
    };

    size_t passed = 0;
    for (const auto& test : tests) {
        const bool success = test.function();
        std::cout << (success ? "[PASS] " : "[FAIL] ") << test.name << '\n';
        passed += success ? 1 : 0;
    }
    std::cout << passed << '/' << std::size(tests) << " checks passed\n";
    if (passed == std::size(tests)) {
        std::cout << ">> [PASS] Expanded syscalls verified "
                     "(real kevent EVFILT_USER, evf_cancel, open).\n";
    }
    return passed == std::size(tests) ? 0 : 1;
}
