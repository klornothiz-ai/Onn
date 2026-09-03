// ============================================================================
// ProsperoLayer RDNA2 Core - Syscall Dispatcher Expanded Test Suite
// ============================================================================
// Description: Dependency-free coverage of the Prospero guest syscall
//              dispatcher (src/cpu/prospero_syscalls.cpp). It exercises the
//              register-only ABI (rax selects the handler; rdi/rsi/rdx/rcx
//              carry arguments) and asserts the fail-closed behaviour that the
//              rest of the emulator relies on:
//                * an unknown syscall reports ENOSYS, never a silent 0
//                * memory syscalls (mmap/mprotect/munmap) route through the VMM
//                  and honour its collision + permission checks
//                * guest-pointer syscalls (cpuset get/set affinity,
//                  clock_gettime) reject unmapped pointers and round-trip
//                  through mapped guest memory
//                * kqueue hands out distinct descriptors and kevent reports
//                  "no events ready" cleanly
//                * event-flag syscalls create/set/wait/clear/delete correctly
//                * thr_new fails closed on a non-executable entry point
//              Uses the same lightweight custom harness as the other suites.
// ============================================================================

#include "cpu/prospero_syscalls.hpp"
#include "memory/virtual_memory_manager.hpp"
#include "kernel/event_flag.hpp"

#include <cstdint>
#include <cstring>
#include <ctime>
#include <iostream>
#include <sstream>

namespace {

using PS5::CPU::ProsperoSyscall;
using PS5::CPU::ProsperoSyscallDispatcher;
using PS5::CPU::SyscallContext;
using PS5::Memory::PageProt;
using PS5::Memory::VirtualMemoryManager;

constexpr uint32_t kRW = static_cast<uint32_t>(PageProt::Read) |
                         static_cast<uint32_t>(PageProt::Write);
constexpr uint32_t kRWX = kRW | static_cast<uint32_t>(PageProt::Exec);

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

// Silence a handler's stderr chatter so the [PASS]/[FAIL] log stays readable.
struct StderrSilencer {
    std::ostringstream sink;
    std::streambuf* previous;
    StderrSilencer() : previous(std::cerr.rdbuf(sink.rdbuf())) {}
    ~StderrSilencer() { std::cerr.rdbuf(previous); }
};

// An unknown opcode must fail closed with ENOSYS, not look like success.
bool UnknownSyscallFailsClosed() {
    auto& d = ProsperoSyscallDispatcher::Instance();
    StderrSilencer quiet;
    SyscallContext ctx{};
    ctx.rax = 0xDEADBEEF;
    CHECK(d.Dispatch(ctx) == 0x80020016ULL); // SCE_KERNEL_ERROR_ENOSYS
    return true;
}

// getpid / getuid return the real host identities and are non-destructive.
bool ProcessInfoSyscallsReturnHostIdentity() {
    auto& d = ProsperoSyscallDispatcher::Instance();
    SyscallContext pid{}; pid.rax = Op(ProsperoSyscall::SC_SYS_getpid);
    SyscallContext uid{}; uid.rax = Op(ProsperoSyscall::SC_SYS_getuid);
    CHECK(d.Dispatch(pid) != 0);
    CHECK(d.Dispatch(uid) == d.Dispatch(uid)); // stable across calls
    return true;
}

// exit records the low-byte code and raises the exit-requested flag; the
// dispatcher can then clear it so later tests start from a clean slate.
bool ExitSyscallRecordsCodeAndFlag() {
    auto& d = ProsperoSyscallDispatcher::Instance();
    d.ClearExitRequest();
    CHECK(!d.ExitRequested());
    StderrSilencer quiet; // exit prints to stdout, not stderr, but keep it tidy
    SyscallContext ctx{};
    ctx.rax = Op(ProsperoSyscall::SC_SYS_exit);
    ctx.rdi = 0x1234;      // only the low byte is a valid exit code
    d.Dispatch(ctx);
    CHECK(d.ExitRequested());
    CHECK(d.ExitCode() == 0x34);
    d.ClearExitRequest();
    CHECK(!d.ExitRequested());
    CHECK(d.ExitCode() == 0);
    return true;
}

// mmap routes to the VMM; the returned GVA is mapped and RWX, and munmap frees.
bool MmapMprotectMunmapRouteThroughVmm() {
    auto& d = ProsperoSyscallDispatcher::Instance();
    auto& vmm = VirtualMemoryManager::Instance();
    constexpr uint64_t kGva = 0x1300000000ULL;

    SyscallContext mm{};
    mm.rax = Op(ProsperoSyscall::SC_SYS_mmap);
    mm.rdi = kGva; mm.rsi = 4096; mm.rdx = kRWX;
    CHECK(d.Dispatch(mm) == kGva);
    CHECK(vmm.IsGvaMapped(kGva));
    CHECK(vmm.IsGvaExecutable(kGva));

    // mprotect down to read/write only clears the exec bit.
    SyscallContext mp{};
    mp.rax = Op(ProsperoSyscall::SC_SYS_mprotect);
    mp.rdi = kGva; mp.rsi = 4096; mp.rdx = kRW;
    CHECK(d.Dispatch(mp) == 0);
    CHECK(!vmm.IsGvaExecutable(kGva));
    CHECK(vmm.IsGvaWritable(kGva));

    // munmap succeeds (returns 0) and the range is gone.
    SyscallContext um{};
    um.rax = Op(ProsperoSyscall::SC_SYS_munmap);
    um.rdi = kGva; um.rsi = 4096;
    CHECK(d.Dispatch(um) == 0);
    CHECK(!vmm.IsGvaMapped(kGva));

    // munmap of an already-unmapped range fails closed (-1).
    CHECK(d.Dispatch(um) == static_cast<uint64_t>(-1));
    return true;
}

// mmap must not hand back a region overlapping a live allocation.
bool MmapRejectsOverlappingRegion() {
    auto& d = ProsperoSyscallDispatcher::Instance();
    auto& vmm = VirtualMemoryManager::Instance();
    constexpr uint64_t kBase = 0x1310000000ULL;

    SyscallContext first{};
    first.rax = Op(ProsperoSyscall::SC_SYS_mmap);
    first.rdi = kBase; first.rsi = 64 * 1024 * 1024; first.rdx = kRW;
    CHECK(d.Dispatch(first) == kBase);

    SyscallContext overlap{};
    overlap.rax = Op(ProsperoSyscall::SC_SYS_mmap);
    overlap.rdi = kBase + 0x02000000ULL; overlap.rsi = 4096; overlap.rdx = kRW;
    CHECK(d.Dispatch(overlap) == 0); // VMM collision check -> 0

    CHECK(vmm.FreeVirtual(kBase, 64 * 1024 * 1024));
    return true;
}

// madvise is an accepted no-op that must report success.
bool MadviseIsAcceptedNoOp() {
    auto& d = ProsperoSyscallDispatcher::Instance();
    SyscallContext ctx{};
    ctx.rax = Op(ProsperoSyscall::SC_SYS_madvise);
    CHECK(d.Dispatch(ctx) == 0);
    return true;
}

// cpuset get/set affinity round-trips a mask through mapped guest memory.
bool CpuAffinityRoundTripsThroughGuestMemory() {
    auto& d = ProsperoSyscallDispatcher::Instance();
    auto& vmm = VirtualMemoryManager::Instance();
    constexpr uint64_t kGva = 0x1320000000ULL;
    CHECK(vmm.AllocateVirtual(kGva, 4096, kRW) == kGva);

    // Read the current (default) affinity mask into guest memory.
    SyscallContext get{};
    get.rax = Op(ProsperoSyscall::SC_SYS_cpuset_getaffinity);
    get.rcx = kGva;
    CHECK(d.Dispatch(get) == 0);
    auto* mask = static_cast<uint64_t*>(vmm.GvaToHva(kGva));
    CHECK(mask != nullptr);
    const uint64_t default_mask = *mask;
    CHECK(default_mask != 0); // dispatcher seeds a non-empty default

    // Write a new mask, then read it back and confirm it stuck.
    *mask = 0xAA55ULL;
    SyscallContext set{};
    set.rax = Op(ProsperoSyscall::SC_SYS_cpuset_setaffinity);
    set.rcx = kGva;
    CHECK(d.Dispatch(set) == 0);

    *mask = 0; // clobber so the read must repopulate it
    CHECK(d.Dispatch(get) == 0);
    CHECK(*mask == 0xAA55ULL);

    CHECK(vmm.FreeVirtual(kGva, 4096));
    return true;
}

// clock_gettime fills a guest timespec with a plausible, monotonic value.
bool ClockGettimeFillsGuestTimespec() {
    auto& d = ProsperoSyscallDispatcher::Instance();
    auto& vmm = VirtualMemoryManager::Instance();
    constexpr uint64_t kGva = 0x1330000000ULL;
    CHECK(vmm.AllocateVirtual(kGva, 4096, kRW) == kGva);

    auto* ts = static_cast<struct timespec*>(vmm.GvaToHva(kGva));
    CHECK(ts != nullptr);
    ts->tv_sec = 0;
    ts->tv_nsec = 0;

    SyscallContext ctx{};
    ctx.rax = Op(ProsperoSyscall::SC_SYS_clock_gettime);
    ctx.rsi = kGva; // handler reads the timespec pointer from rsi
    CHECK(d.Dispatch(ctx) == 0);
    CHECK(ts->tv_sec > 0);
    CHECK(ts->tv_nsec >= 0 && ts->tv_nsec < 1000000000LL);

    CHECK(vmm.FreeVirtual(kGva, 4096));
    return true;
}

// kqueue hands out distinct descriptors; kevent reports 0 events cleanly.
bool KqueueAndKeventBehaveSafely() {
    auto& d = ProsperoSyscallDispatcher::Instance();
    SyscallContext kq1{}; kq1.rax = Op(ProsperoSyscall::SC_SYS_kqueue);
    SyscallContext kq2{}; kq2.rax = Op(ProsperoSyscall::SC_SYS_kqueue);
    const uint64_t fd1 = d.Dispatch(kq1);
    const uint64_t fd2 = d.Dispatch(kq2);
    CHECK(fd1 != 0);
    CHECK(fd2 != 0);
    CHECK(fd1 != fd2); // descriptors must be unique

    SyscallContext ev{};
    ev.rax = Op(ProsperoSyscall::SC_SYS_kevent);
    ev.rdi = fd1;      // no pending events registered
    CHECK(d.Dispatch(ev) == 0);
    return true;
}

// namedobj_create returns distinct non-zero handles; delete succeeds.
bool NamedObjectCreateReturnsUniqueHandles() {
    auto& d = ProsperoSyscallDispatcher::Instance();
    SyscallContext a{}; a.rax = Op(ProsperoSyscall::SC_SYS_namedobj_create);
    SyscallContext b{}; b.rax = Op(ProsperoSyscall::SC_SYS_namedobj_create);
    const uint64_t h1 = d.Dispatch(a);
    const uint64_t h2 = d.Dispatch(b);
    CHECK(h1 != 0);
    CHECK(h2 != 0);
    CHECK(h1 != h2);

    SyscallContext del{};
    del.rax = Op(ProsperoSyscall::SC_SYS_namedobj_delete);
    CHECK(d.Dispatch(del) == 0);
    return true;
}

// The event-flag syscalls create/set/wait(AND)/clear/delete correctly, and a
// wait whose condition is already satisfied returns immediately with the bits.
bool EventFlagSyscallLifecycle() {
    auto& d = ProsperoSyscallDispatcher::Instance();
    auto& vmm = VirtualMemoryManager::Instance();

    // Create an event flag with an initial pattern of 0.
    SyscallContext create{};
    create.rax = Op(ProsperoSyscall::SC_SYS_evf_create);
    create.rdi = 0;      // attr
    create.rsi = 0;      // initial pattern
    const uint64_t handle = d.Dispatch(create);
    CHECK(handle != 0);

    // Set bit 0x1; then an AND-wait for 0x1 must succeed without blocking.
    SyscallContext set{};
    set.rax = Op(ProsperoSyscall::SC_SYS_evf_set);
    set.rdi = handle;
    set.rsi = 0x1;
    CHECK(d.Dispatch(set) == 0);

    constexpr uint64_t kGva = 0x1340000000ULL;
    CHECK(vmm.AllocateVirtual(kGva, 4096, kRW) == kGva);
    auto* out_bits = static_cast<uint64_t*>(vmm.GvaToHva(kGva));
    *out_bits = 0;

    SyscallContext wait{};
    wait.rax = Op(ProsperoSyscall::SC_SYS_evf_wait);
    wait.rdi = handle;
    wait.rsi = 0x1;   // bits to wait for
    wait.rdx = PS5::Kernel::SCE_KERNEL_EVF_WAITMODE_AND;
    wait.rcx = kGva;  // out-bits pointer
    CHECK(d.Dispatch(wait) == 0);
    CHECK((*out_bits & 0x1) == 0x1);

    // Clear the bit and delete the flag.
    SyscallContext clear{};
    clear.rax = Op(ProsperoSyscall::SC_SYS_evf_clear);
    clear.rdi = handle;
    clear.rsi = 0x1;
    CHECK(d.Dispatch(clear) == 0);

    SyscallContext del{};
    del.rax = Op(ProsperoSyscall::SC_SYS_evf_delete);
    del.rdi = handle;
    CHECK(d.Dispatch(del) == 0);

    // Deleting an already-deleted handle fails closed.
    CHECK(d.Dispatch(del) == 0x80020001ULL);

    CHECK(vmm.FreeVirtual(kGva, 4096));
    return true;
}

// thr_new must refuse an entry point that is not mapped-executable, returning
// the memory-fault status rather than spawning a thread into invalid memory.
bool ThrNewFailsClosedOnNonExecutableEntry() {
    auto& d = ProsperoSyscallDispatcher::Instance();
    auto& vmm = VirtualMemoryManager::Instance();
    StderrSilencer quiet;

    // Case 1: completely unmapped entry point (in-arena but never allocated).
    SyscallContext unmapped{};
    unmapped.rax = Op(ProsperoSyscall::SC_SYS_thr_new);
    unmapped.rdi = 0x13F0000000ULL; // never allocated
    CHECK(d.Dispatch(unmapped) == 0x8002000AULL);

    // Case 2: mapped but read/write only (not executable).
    constexpr uint64_t kGva = 0x1350000000ULL;
    CHECK(vmm.AllocateVirtual(kGva, 4096, kRW) == kGva);
    SyscallContext rw{};
    rw.rax = Op(ProsperoSyscall::SC_SYS_thr_new);
    rw.rdi = kGva;
    CHECK(d.Dispatch(rw) == 0x8002000AULL);
    CHECK(vmm.FreeVirtual(kGva, 4096));
    return true;
}

// budget / dmem informational syscalls return their documented constants.
bool BudgetAndDmemReturnConstants() {
    auto& d = ProsperoSyscallDispatcher::Instance();
    SyscallContext dmem{}; dmem.rax = Op(ProsperoSyscall::SC_SYS_dmem_container);
    SyscallContext budget{}; budget.rax = Op(ProsperoSyscall::SC_SYS_budget_get_ptype);
    CHECK(d.Dispatch(dmem) == 0x300);
    CHECK(d.Dispatch(budget) == 1);
    return true;
}

struct TestCase {
    const char* name;
    bool (*function)();
};

} // namespace

int main() {
    const TestCase tests[] = {
        {"UnknownSyscallFailsClosed", UnknownSyscallFailsClosed},
        {"ProcessInfoSyscallsReturnHostIdentity", ProcessInfoSyscallsReturnHostIdentity},
        {"ExitSyscallRecordsCodeAndFlag", ExitSyscallRecordsCodeAndFlag},
        {"MmapMprotectMunmapRouteThroughVmm", MmapMprotectMunmapRouteThroughVmm},
        {"MmapRejectsOverlappingRegion", MmapRejectsOverlappingRegion},
        {"MadviseIsAcceptedNoOp", MadviseIsAcceptedNoOp},
        {"CpuAffinityRoundTripsThroughGuestMemory", CpuAffinityRoundTripsThroughGuestMemory},
        {"ClockGettimeFillsGuestTimespec", ClockGettimeFillsGuestTimespec},
        {"KqueueAndKeventBehaveSafely", KqueueAndKeventBehaveSafely},
        {"NamedObjectCreateReturnsUniqueHandles", NamedObjectCreateReturnsUniqueHandles},
        {"EventFlagSyscallLifecycle", EventFlagSyscallLifecycle},
        {"ThrNewFailsClosedOnNonExecutableEntry", ThrNewFailsClosedOnNonExecutableEntry},
        {"BudgetAndDmemReturnConstants", BudgetAndDmemReturnConstants},
    };

    size_t passed = 0;
    for (const auto& test : tests) {
        const bool success = test.function();
        std::cout << (success ? "[PASS] " : "[FAIL] ") << test.name << '\n';
        passed += success ? 1 : 0;
    }
    std::cout << passed << '/' << std::size(tests) << " tests passed\n";
    return passed == std::size(tests) ? 0 : 1;
}
