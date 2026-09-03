// seccomp_guard_test — round 29: PROOF that the deny-by-default seccomp
// allowlist actually enforces on a thread that entered native guest
// execution.
//
// Method: enable the DirectExecutionBackend, run one tiny guest function
// NATIVELY on THIS thread (which installs the filter at trampoline entry),
// then issue raw host syscalls from the same thread and assert:
//   * allowed surface keeps working (gettid, write, clock_gettime,
//     anonymous mmap);
//   * open(2) is denied (mediation goes through openat);
//   * ptrace is denied (process escape closed);
//   * fork() is denied;
//   * clone WITHOUT CLONE_THREAD is denied (process-creating clone forms);
//   * an arbitrary unlisted syscall number is denied (deny-by-default:
//     without a filter it would return ENOSYS, with the filter EPERM --
//     this is the discriminator that proves the policy inversion).
//
// The test is Linux-only; on other platforms it self-skips.
#include "cpu/direct_execution.hpp"
#include "cpu/vmm_memory_bus.hpp"
#include "cpu/x86_64_interpreter.hpp"
#include "memory/virtual_memory_manager.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

int g_checks = 0;
int g_failures = 0;
bool Check(bool v, const char* e, int line) {
    ++g_checks;
    if (!v) {
        ++g_failures;
        std::fprintf(stderr, "  [FAIL] %s (line %d)\n", e, line);
    }
    return v;
}
#define CHECK(e) Check((e), #e, __LINE__)

} // namespace

#if defined(__linux__)
int main() {
    std::printf("== seccomp guard: deny-by-default allowlist enforcement ==\n");

    // 1) Enter native guest execution once -- the trampoline installs the
    //    filter on THIS thread (thread_local, exactly like production).
    auto& direct = PS5::CPU::DirectExecutionBackend::Instance();
    if (!Check(direct.Enable(), "DirectExecutionBackend::Enable", __LINE__)) {
        std::printf("seccomp_guard_test: backend cannot enable here; FAIL\n");
        return 1;
    }
    {
        // Guest code: mov eax, 7; ret  -- runs natively, no syscalls.
        unsigned char code[] = {0xB8, 0x07, 0x00, 0x00, 0x00, 0xC3};
        auto& vmm = PS5::Memory::VirtualMemoryManager::Instance();
        constexpr uint64_t kGva = 0x1000A00000ull;
        constexpr uint64_t kStackBase = 0x1000A00000ull + 0x10000ull;
        constexpr uint64_t kStackTop = kStackBase + 0x10000ull;
        if (!Check(vmm.AllocateVirtual(kGva, 0x1000, 0x7) != 0,
                   "allocate guest code page", __LINE__)) {
            return 1;
        }
        if (!Check(vmm.AllocateVirtual(kStackBase, 0x10000, 0x3) != 0,
                   "allocate guest stack", __LINE__)) {
            return 1;
        }
        if (!Check(vmm.CopyToGuest(kGva, code, sizeof(code), 0x7),
                   "map guest code", __LINE__)) {
            return 1;
        }
        const uint64_t sentinel = 0x5A5A5A5A5A000000ull;
        if (!Check(vmm.CopyToGuest(kStackTop - 16, &sentinel, 8, 0x3),
                   "map guest stack sentinel", __LINE__)) {
            return 1;
        }
        PS5::CPU::CpuState cpu{};
        cpu.rip = kGva;
        cpu.gpr[PS5::CPU::RSP] = kStackTop - 16;
        PS5::CPU::VmmMemoryBus bus(vmm);
        PS5::CPU::DirectRunOutcome outcome{};
        const uint64_t r = direct.RunFunction(
            cpu, bus, nullptr, sentinel, 1000, outcome);
        Check(r == 7ull, "native guest function returns 7", __LINE__);
        Check(outcome.reason == PS5::CPU::DirectStopReason::Returned,
              "native stop reason", __LINE__);
    }
    std::printf("  [ok] filter installed (native execution entered)\n");

    // 2) Allowed surface still works.
    {
        const long tid = syscall(SYS_gettid);
        Check(tid > 0, "gettid allowed", __LINE__);
        const long w = syscall(SYS_write, 2, "", 0);
        Check(w == 0, "write allowed", __LINE__);
        struct timespec ts{};
        const long cg = syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);
        Check(cg == 0 && ts.tv_sec > 0, "clock_gettime allowed", __LINE__);
        void* p = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        Check(p != MAP_FAILED, "anonymous mmap allowed", __LINE__);
        if (p != MAP_FAILED) munmap(p, 4096);
    }
    std::printf("  [ok] allowlisted syscalls still work\n");

    // 3) open(2) denied (mediation is openat-only).
    {
        errno = 0;
        const long r = syscall(SYS_open, "/etc/passwd", O_RDONLY);
        Check(r == -1 && errno == EPERM, "open(2) denied with EPERM",
              __LINE__);
    }

    // 4) ptrace denied (process escape).
    {
        errno = 0;
        const long r = syscall(SYS_ptrace, 0 /*PTRACE_TRACEME*/, 0, 0, 0);
        Check(r == -1 && errno == EPERM, "ptrace denied with EPERM",
              __LINE__);
    }

    // 5) fork denied.
    {
        errno = 0;
        const long r = syscall(SYS_fork);
        if (r == 0) {
            // Only reachable if the filter is broken; leave quietly so the
            // parent still observes the failure.
            _exit(0);
        }
        Check(r == -1 && errno == EPERM, "fork denied with EPERM", __LINE__);
    }

    // 6) clone WITHOUT CLONE_THREAD denied (process-creating clone form).
    {
        errno = 0;
        // CLONE_VM | SIGCHLD: a process-creating clone. If the filter were
        // broken this would return twice; the child branch exits at once.
        const long r = syscall(SYS_clone, CLONE_VM | SIGCHLD, nullptr,
                               nullptr, nullptr, 0);
        if (r == 0) {
            _exit(0);
        }
        Check(r == -1 && errno == EPERM,
              "clone(without CLONE_THREAD) denied with EPERM", __LINE__);
    }

    // 7) Deny-by-default discriminator: an unlisted, INVALID syscall number
    //    returns EPERM (not ENOSYS) -- only a default-deny filter does that.
    {
        errno = 0;
        const long r = syscall(0x999);
        Check(r == -1 && errno == EPERM,
              "unlisted syscall number denied with EPERM (not ENOSYS)",
              __LINE__);
    }

    std::printf("seccomp_guard_test: %d checks, %d failures\n", g_checks,
                g_failures);
    if (g_failures == 0) {
        std::printf(">> [PASS] deny-by-default allowlist enforced\n");
    }
    return g_failures == 0 ? 0 : 1;
}
#else
int main() {
    std::printf("seccomp_guard_test: skipped (not Linux)\n");
    return 0;
}
#endif
