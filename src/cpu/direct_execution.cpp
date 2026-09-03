// ============================================================================
// ProsperoLayer RDNA2 Core - DirectExecutionBackend (round 20)
// ----------------------------------------------------------------------------
// See include/cpu/direct_execution.hpp for the design. This file contains:
//   * the assembly trampoline that switches a host thread into the guest
//     context (registers, FS base, stack) and jumps to the guest entry,
//   * the signal handlers (the ONLY exits from native guest execution),
//   * the incremental block patcher (ud2 interception of syscalls / cpuid /
//     rdtsc / SSE4a / BMI / MOVBE / TZCNT),
//   * the run loop (discover -> enter -> handle trap -> resume),
//   * the per-thread seccomp allowlist (deny-by-default defence in depth
//     against a host syscall escaping through the never-executed-block race).
// ============================================================================
#include "cpu/direct_execution.hpp"
#include "cpu/hle_trampoline.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <setjmp.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#endif

extern "C" {
// The trampoline: rdi = DirectThreadCtx* (layout below). NEVER returns --
// control leaves through a signal + siglongjmp.
void ps5_direct_enter(void* ctx);
}

#if defined(__x86_64__) && defined(__linux__)
__asm__(
".text\n"
".globl ps5_direct_enter\n"
".type ps5_direct_enter, @function\n"
"ps5_direct_enter:\n"
"    # rdi = ctx. Guest-stack scratch: s = guest_rsp - 56, layout\n"
"    # [s]=rsi [s+8]=rcx [s+16]=rax [s+24]=r11 [s+32]=r10 [s+40]=rdi [s+48]=rip\n"
"    movq    8(%rdi), %r10\n"
"    subq    $56, %r10\n"
"    movq    0(%rdi), %rax\n"
"    movq    %rax, 48(%r10)\n"
"    movq    32+8*7(%rdi), %rax\n"
"    movq    %rax, 40(%r10)\n"
"    movq    32+8*10(%rdi), %rax\n"
"    movq    %rax, 32(%r10)\n"
"    movq    32+8*11(%rdi), %rax\n"
"    movq    %rax, 24(%r10)\n"
"    movq    32+8*0(%rdi), %rax\n"
"    movq    %rax, 16(%r10)\n"
"    movq    32+8*1(%rdi), %rax\n"
"    movq    %rax, 8(%r10)\n"
"    movq    32+8*6(%rdi), %rax\n"
"    movq    %rax, 0(%r10)\n"
"    # guest XMM/YMM state: results of EMULATED instructions (SSE4a/BMI/xmm\n"
"    # forms) live in the CpuState and must reach the native register file --\n"
"    # without this reload every post-trap xmm read sees the SIGNAL HANDLER's\n"
"    # leftovers. On AVX hosts the full ymm staging array is loaded (a legacy\n"
"    # movdqu would ZERO the upper half); on non-AVX hosts only the xmm halves.\n"
"    cmpb    $0, 0x2A0(%rdi)\n"
"    je      8f\n"
"    vmovdqu 0x2B0+32*0(%rdi), %ymm0\n"
"    vmovdqu 0x2B0+32*1(%rdi), %ymm1\n"
"    vmovdqu 0x2B0+32*2(%rdi), %ymm2\n"
"    vmovdqu 0x2B0+32*3(%rdi), %ymm3\n"
"    vmovdqu 0x2B0+32*4(%rdi), %ymm4\n"
"    vmovdqu 0x2B0+32*5(%rdi), %ymm5\n"
"    vmovdqu 0x2B0+32*6(%rdi), %ymm6\n"
"    vmovdqu 0x2B0+32*7(%rdi), %ymm7\n"
"    vmovdqu 0x2B0+32*8(%rdi), %ymm8\n"
"    vmovdqu 0x2B0+32*9(%rdi), %ymm9\n"
"    vmovdqu 0x2B0+32*10(%rdi), %ymm10\n"
"    vmovdqu 0x2B0+32*11(%rdi), %ymm11\n"
"    vmovdqu 0x2B0+32*12(%rdi), %ymm12\n"
"    vmovdqu 0x2B0+32*13(%rdi), %ymm13\n"
"    vmovdqu 0x2B0+32*14(%rdi), %ymm14\n"
"    vmovdqu 0x2B0+32*15(%rdi), %ymm15\n"
"    jmp     9f\n"
"8:\n"
"    movdqu  0xA0+16*0(%rdi), %xmm0\n"
"    movdqu  0xA0+16*1(%rdi), %xmm1\n"
"    movdqu  0xA0+16*2(%rdi), %xmm2\n"
"    movdqu  0xA0+16*3(%rdi), %xmm3\n"
"    movdqu  0xA0+16*4(%rdi), %xmm4\n"
"    movdqu  0xA0+16*5(%rdi), %xmm5\n"
"    movdqu  0xA0+16*6(%rdi), %xmm6\n"
"    movdqu  0xA0+16*7(%rdi), %xmm7\n"
"    movdqu  0xA0+16*8(%rdi), %xmm8\n"
"    movdqu  0xA0+16*9(%rdi), %xmm9\n"
"    movdqu  0xA0+16*10(%rdi), %xmm10\n"
"    movdqu  0xA0+16*11(%rdi), %xmm11\n"
"    movdqu  0xA0+16*12(%rdi), %xmm12\n"
"    movdqu  0xA0+16*13(%rdi), %xmm13\n"
"    movdqu  0xA0+16*14(%rdi), %xmm14\n"
"    movdqu  0xA0+16*15(%rdi), %xmm15\n"
"9:\n"
"    # deterministic FPU control state (host stack scratch)\n"
"    subq    $16, %rsp\n"
"    movl    $0x1F80, (%rsp)\n"
"    ldmxcsr (%rsp)\n"
"    movl    $0x037F, (%rsp)\n"
"    fldcw   (%rsp)\n"
"    # guest rflags (arithmetic flags only; IF/TF never set from guest state)\n"
"    movq    24(%rdi), %rax\n"
"    movq    %rax, 8(%rsp)\n"
"    addq    $8, %rsp\n"
"    popfq\n"
"    # load the syscall-safe guest GPRs BEFORE rdi is consumed\n"
"    movq    32+8*2(%rdi), %rdx\n"
"    movq    32+8*3(%rdi), %rbx\n"
"    movq    32+8*5(%rdi), %rbp\n"
"    movq    32+8*8(%rdi), %r8\n"
"    movq    32+8*9(%rdi), %r9\n"
"    movq    32+8*12(%rdi), %r12\n"
"    movq    32+8*13(%rdi), %r13\n"
"    movq    32+8*14(%rdi), %r14\n"
"    movq    32+8*15(%rdi), %r15\n"
"    # FS base = guest TLS (arch_prctl clobbers rax, rcx, r11 -- all spilled)\n"
"    movq    16(%rdi), %rsi\n"
"    movq    $0x1003, %rdi\n"
"    movq    $158, %rax\n"
"    syscall\n"
"    # switch to the guest scratch stack and pop the spilled values\n"
"    movq    %r10, %rsp\n"
"    popq    %rsi\n"
"    popq    %rcx\n"
"    popq    %rax\n"
"    popq    %r11\n"
"    popq    %r10\n"
"    popq    %rdi\n"
"    retq\n"
".size ps5_direct_enter, .-ps5_direct_enter\n"
);
#endif

namespace PS5::CPU {
namespace {

// ---------------------------------------------------------------------------
// Per-thread execution context. The first 0x4B0 bytes have a FIXED layout
// the trampoline addresses with hardcoded offsets -- static_asserts keep
// them in sync.
// ---------------------------------------------------------------------------
struct alignas(16) DirectThreadCtx {
    uint64_t rip;        // 0x000
    uint64_t rsp;        // 0x008
    uint64_t fs_base;    // 0x010
    uint64_t rflags;     // 0x018
    uint64_t gpr[16];    // 0x020..0x09F  (RAX..R15, the CpuState order)
    CpuState::XmmReg xmm[16];   // 0x0A0..0x19F (guest XMM halves)
    CpuState::XmmReg ymm_hi[16]; // 0x1A0..0x29F (AVX.256 upper halves)
    uint8_t have_avx;    // 0x2A0: host AVX -> load full ymm staging
    uint8_t _pad[15];    // alignment
    uint8_t ymm_staging[16 * 32];  // 0x2B0..0x4AF: ymm i = {xmm[i], ymm_hi[i]}

    // ---- C++-only part ----
    sigjmp_buf env{};    // 0x4B0
    CpuState* cpu{nullptr};
    int stop_sig{0};
    uint64_t fault_addr{0};
    bool active{false};
};
static_assert(offsetof(DirectThreadCtx, rip) == 0x00, "trampoline layout");
static_assert(offsetof(DirectThreadCtx, rsp) == 0x08, "trampoline layout");
static_assert(offsetof(DirectThreadCtx, fs_base) == 0x10, "trampoline layout");
static_assert(offsetof(DirectThreadCtx, rflags) == 0x18, "trampoline layout");
static_assert(offsetof(DirectThreadCtx, gpr) == 0x20, "trampoline layout");
static_assert(offsetof(DirectThreadCtx, xmm) == 0xA0, "trampoline layout");
static_assert(offsetof(DirectThreadCtx, ymm_hi) == 0x1A0, "trampoline layout");
static_assert(offsetof(DirectThreadCtx, have_avx) == 0x2A0, "trampoline layout");
static_assert(offsetof(DirectThreadCtx, ymm_staging) == 0x2B0, "trampoline layout");
static_assert(offsetof(DirectThreadCtx, env) == 0x4B0, "env after the register file");

// Host AVX capability (constant for the process; the trampoline's VEX
// loads are gated on it so a non-AVX host never executes a #UD).
bool HostHasAvx() {
#if defined(__x86_64__)
    return __builtin_cpu_supports("avx");
#else
    return false;
#endif
}

thread_local DirectThreadCtx* t_direct_ctx = nullptr;

// gregs index -> CpuState GPR index (REG_* from sys/ucontext.h:
// R8=0 R9=1 R10=2 R11=3 R12=4 R13=5 R14=6 R15=7 RDI=8 RSI=9 RBP=10 RBX=11
// RDX=12 RAX=13 RCX=14 RSP=15 RIP=16 EFL=17 -- verified against glibc).
constexpr int kGregToGpr[18] = {
    /*REG_R8*/ 8, /*REG_R9*/ 9, /*REG_R10*/ 10, /*REG_R11*/ 11,
    /*REG_R12*/ 12, /*REG_R13*/ 13, /*REG_R14*/ 14, /*REG_R15*/ 15,
    /*REG_RDI*/ 7, /*REG_RSI*/ 6, /*REG_RBP*/ 5, /*REG_RBX*/ 3,
    /*REG_RDX*/ 2, /*REG_RAX*/ 0, /*REG_RCX*/ 1, /*REG_RSP*/ 4,
    -1 /*RIP*/, -1 /*EFL*/,
};

// Exact FPU capture from the signal frame's saved state (NOT the live
// registers: the C++ handler itself may clobber xmm0-15 before this runs).
// The fxsave legacy region of the kernel fpstate has XMM0-15 at byte offsets
// 160..415 -- identical for plain FXSAVE and the XSAVE legacy region (the
// first 512 bytes share the layout). When the kernel saved AVX state
// (xstate_bv bit 2 set in the xsave header at fp+512), the YMM upper halves
// live in the extended area at offset 576 + 16*i.
void SaveFpuFromFrame(CpuState& cpu, const ucontext_t& u) {
    const auto* fp = reinterpret_cast<const uint8_t*>(u.uc_mcontext.fpregs);
    if (fp == nullptr) {
        return;
    }
    for (int i = 0; i < 16; ++i) {
        std::memcpy(&cpu.xmm[i], fp + 160 + 16 * i, 16);
    }
    const uint64_t bv = *reinterpret_cast<const uint64_t*>(fp + 512);
    if ((bv & 0x4ull) != 0) {  // xstate_bv bit 2: YMM state present
        for (int i = 0; i < 16; ++i) {
            std::memcpy(&cpu.ymm_hi[i], fp + 576 + 16 * i, 16);
        }
    }
}

void SyncFromUcontext(CpuState& cpu, const ucontext_t& u) {
    const auto& g = u.uc_mcontext.gregs;
    for (int i = 0; i < 16; ++i) {
        if (kGregToGpr[i] >= 0) {
            cpu.gpr[static_cast<size_t>(kGregToGpr[i])] = static_cast<uint64_t>(g[i]);
        }
    }
    cpu.rip = static_cast<uint64_t>(g[REG_RIP]);
    cpu.rflags = static_cast<uint64_t>(g[REG_EFL]);
}

void DirectTrapHandler(int sig, siginfo_t* info, void* uctx_v) {
    DirectThreadCtx* ctx = t_direct_ctx;
    if (ctx == nullptr || !ctx->active) {
        // Not executing guest code (a stale pending timer or a host-side
        // fault): timers are benign, everything else fails LOUD.
        if (sig == SIGALRM) {
            return;
        }
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    ucontext_t* u = static_cast<ucontext_t*>(uctx_v);
    SyncFromUcontext(*ctx->cpu, *u);
    SaveFpuFromFrame(*ctx->cpu, *u);
    ctx->stop_sig = sig;
    ctx->fault_addr = (sig == SIGSEGV || sig == SIGBUS)
        ? reinterpret_cast<uint64_t>(info->si_addr) : 0;
    ctx->active = false;
    siglongjmp(ctx->env, 1);
}

constexpr int kHandledSignals[] = {SIGSEGV, SIGILL, SIGFPE, SIGTRAP, SIGBUS, SIGALRM};

// ---------------------------------------------------------------------------
// The seccomp guard (round 29 rewrite): DENY-BY-DEFAULT allowlist.
//
// Round 20 shipped a DENYLIST: 19 process-escaping syscalls were denied and
// EVERYTHING ELSE was allowed. The user's audit flagged the hole: the guest
// uses FreeBSD-9 syscall numbers, which collide with arbitrary Linux x86-64
// numbers, so a raw guest `syscall` slipping through the (documented,
// nanosecond-scale) never-executed-block race could still reach open(2),
// socket(2), clone-as-process and friends on the host kernel with
// unvalidated guest arguments.
//
// The policy is now inverted: a fixed ALLOWLIST covers exactly the host
// syscall surface the emulator itself needs on a trampoline thread, and
// EVERYTHING ELSE returns EPERM:
//   * fork / vfork / execve / execveat / ptrace / mount / reboot / module
//     loading / kexec / key management / unshare / setns / perf_event_open /
//     bpf / io_uring_setup / clone3 are denied BY DEFAULT -- strictly
//     stronger than the round-20 denylist (all of its entries are still
//     blocked, plus everything not explicitly listed);
//   * open(2) is denied: guest file mediation goes through openat(2)
//     exclusively;
//   * clone(56) is allowed ONLY with CLONE_THREAD set (the form
//     pthread_create uses to spawn guest threads); process-creating clone
//     forms return EPERM;
//   * mmap(9) is allowed ONLY with MAP_ANONYMOUS set (the VMM arena and the
//     host allocator only ever map anonymous memory; file mappings are not
//     part of the trampoline-thread surface).
//
// Documented residual risk: a racing guest syscall whose FreeBSD number
// happens to collide with an ALLOWED Linux syscall (openat / socket / ...)
// executes with unvalidated guest arguments -- exactly the emulator's own
// mediation surface. The guard bounds the worst case to that surface;
// process escape (fork/exec/ptrace/...) is closed. Guest fork() emulation
// therefore only works on the interpreter path (which installs no filter);
// under the native path a guest fork gets EPERM -- fail-closed, by design.
// ---------------------------------------------------------------------------
#if defined(__linux__)
// Host x86-64 syscall numbers, grouped by the emulator path that needs them.
constexpr uint32_t kAllowedSyscalls[] = {
    // glibc thread startup (child threads inherit the filter and run these
    // in their prologue before the trampoline).
    13,   // rt_sigaction
    14,   // rt_sigprocmask
    15,   // rt_sigreturn (returning from a signal frame)
    131,  // sigaltstack
    218,  // set_tid_address
    273,  // set_robust_list
    334,  // rseq
    // Memory: the VMM arena (MAP_FIXED_NOREPLACE anonymous) + glibc malloc.
    // (mmap itself is arg-checked: see the MAP_ANONYMOUS gate below.)
    10,   // mprotect
    11,   // munmap
    12,   // brk
    28,   // madvise
    // Scheduling / threads.
    24,   // sched_yield
    186,  // gettid
    202,  // futex (mutexes, umtx emulation, thread joins)
    60,   // exit (thread teardown must not hang)
    231,  // exit_group (process teardown must not hang)
    // Clocks (guest clock_gettime / nanosleep emulation + logging).
    36,   // getitimer (budget timer query)
    38,   // setitimer (the wall-clock budget: ITIMER_REAL + SIGALRM)
    96,   // gettimeofday
    228,  // clock_gettime
    229,  // clock_getres
    230,  // clock_nanosleep
    35,   // nanosleep
    // Mediated file I/O (guest open/read/write, save data, host-backed
    // unlink/rename/mkdir/rmdir/access syscalls).
    0,    // read
    1,    // write
    3,    // close
    5,    // fstat
    8,    // lseek
    16,   // ioctl (isatty on log streams)
    17,   // pread64
    18,   // pwrite64
    19,   // readv
    20,   // writev
    21,   // access
    22,   // pipe
    32,   // dup
    33,   // dup2
    72,   // fcntl
    77,   // ftruncate
    82,   // rename
    83,   // mkdir
    84,   // rmdir
    87,   // unlink
    217,  // getdents64
    257,  // openat (the ONLY file-open path; open(2) itself is denied)
    262,  // newfstatat
    263,  // unlinkat
    269,  // faccessat
    288,  // accept4
    291,  // epoll_create1
    292,  // dup3
    295,  // preadv
    296,  // pwritev
    318,  // getrandom
    332,  // statx
    359,  // pipe2
    439,  // faccessat2
    // Mediated sockets (libNet POSIX backend, round 28).
    41,   // socket
    42,   // connect
    43,   // accept
    44,   // sendto
    45,   // recvfrom
    46,   // sendmsg
    47,   // recvmsg
    48,   // shutdown
    49,   // bind
    50,   // listen
    51,   // getsockname
    52,   // getpeername
    53,   // socketpair
    54,   // getsockopt
    55,   // setsockopt
    7,    // poll
    23,   // select
};

thread_local bool t_seccomp_installed = false;

void InstallSeccompGuard() {
    if (t_seccomp_installed) {
        return;
    }
    t_seccomp_installed = true;

    // Layout (jump offsets patched after assembly):
    //   [0]      LD arch
    //   [1]      JEQ X86_64 -> fall through; mismatch -> ALLOW (cannot be a
    //            guest thread on this kernel; preserves round-20 behaviour)
    //   [2]      LD nr
    //   [3..A]   one JEQ per allowed syscall -> ALLOW
    //   clone gate: JEQ 56 -> inspect args[0].lo; JSET CLONE_THREAD -> ALLOW
    //               else EPERM (process-creating clone is denied)
    //   mmap gate:  JEQ 9  -> inspect args[3].lo; JSET MAP_ANONYMOUS -> ALLOW
    //               else EPERM (file mappings are not in the surface)
    //   RET EPERM; RET ALLOW
    constexpr uint32_t kSysClone = 56;
    constexpr uint32_t kSysMmap = 9;
    constexpr uint32_t kCloneThreadFlag = 0x00010000u;   // CLONE_THREAD
    constexpr uint32_t kMapAnonymousFlag = 0x00000020u;  // MAP_ANONYMOUS
    // offsetof(struct seccomp_data, args[i]) = 16 + 8*i; BPF loads the low
    // 32 bits on little-endian x86-64.
    constexpr uint32_t kArg0Lo = 16;
    constexpr uint32_t kArg3Lo = 16 + 8 * 3;

    const size_t kAllowCount = sizeof(kAllowedSyscalls) / sizeof(uint32_t);
    std::vector<sock_filter> f;
    f.reserve(8 + kAllowCount + 6);
    f.push_back((sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                      offsetof(struct seccomp_data, arch)));
    f.push_back((sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                      AUDIT_ARCH_X86_64, 0, 0));
    f.push_back((sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                      offsetof(struct seccomp_data, nr)));
    for (const uint32_t nr : kAllowedSyscalls) {
        f.push_back((sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 0));
    }
    // clone(56): only the thread-spawning form is allowed.
    const size_t idx_clone_jeq = f.size();
    f.push_back((sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kSysClone, 0, 0));
    f.push_back((sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, kArg0Lo));
    const size_t idx_clone_jset = f.size();
    f.push_back((sock_filter)BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K,
                                      kCloneThreadFlag, 0, 0));
    // mmap(9): only anonymous mappings (VMM arena + allocator).
    const size_t idx_mmap_jeq = f.size();
    f.push_back((sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, kSysMmap, 0, 0));
    f.push_back((sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, kArg3Lo));
    const size_t idx_mmap_jset = f.size();
    f.push_back((sock_filter)BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K,
                                      kMapAnonymousFlag, 0, 0));
    const size_t idx_eperm = f.size();
    f.push_back((sock_filter)BPF_STMT(BPF_RET | BPF_K,
                                      SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)));
    const size_t idx_allow = f.size();
    f.push_back((sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));

    // Patch the jump offsets. Classic-BPF semantics: a jump at instruction
    // p lands at p + 1 + offset, so reaching index t from p needs
    // offset = t - p - 1 (verified against the kernel's bounds checker --
    // an out-of-range offset makes prctl(PR_SET_SECCOMP) fail with EINVAL
    // and the filter silently never installs).
    auto patch = [&](size_t at, size_t target_true, size_t target_false) {
        f[at].jt = static_cast<uint8_t>(target_true - at - 1);
        f[at].jf = static_cast<uint8_t>(target_false - at - 1);
    };
    patch(1, 2, idx_allow);                    // arch match -> LD nr; mismatch -> ALLOW
    for (size_t i = 0; i < kAllowCount; ++i) { // allowlist -> ALLOW
        patch(3 + i, idx_allow, 3 + i + 1);
    }
    // The gates CHAIN: not clone -> try the mmap gate; not mmap -> EPERM.
    patch(idx_clone_jeq, idx_clone_jeq + 1, idx_mmap_jeq);
    patch(idx_clone_jset, idx_allow, idx_eperm);
    patch(idx_mmap_jeq, idx_mmap_jeq + 1, idx_eperm);
    patch(idx_mmap_jset, idx_allow, idx_eperm);

    struct sock_fprog prog{};
    prog.len = static_cast<unsigned short>(f.size());
    prog.filter = f.data();

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        return;
    }
    // A rejected filter must be LOUD, not silent: the guard is defence in
    // depth for the never-executed-block race, and an install failure means
    // that bound is absent (round 29: the previous code ignored the return
    // value, which is exactly how an invalid filter would have hidden).
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) {
        const int err = errno;
        std::fprintf(stderr,
                     "[DirectExecution] seccomp guard FAILED to install "
                     "(errno %d) -- the deny-by-default bound is ABSENT on "
                     "this thread\n",
                     err);
    }
}
#endif // __linux__

// Restores the host thread's integer/FPU discipline after a guest run: DF=0,
// default MXCSR/x87 control word, and the host FS base back (guest TLS
// clobbered it).
void RestoreHostFpuState() {
    __asm__ volatile("cld" ::: "memory");
    alignas(8) unsigned char buf[8];
    *reinterpret_cast<uint32_t*>(buf) = 0x1F80;
    __asm__ volatile("ldmxcsr %0" : : "m"(*reinterpret_cast<uint32_t*>(buf)));
    *reinterpret_cast<uint16_t*>(buf) = 0x037F;
    __asm__ volatile("fldcw %0" : : "m"(*reinterpret_cast<uint16_t*>(buf)));
    __asm__ volatile("emms");
}

uint64_t GetHostFsBase() {
    uint64_t fs = 0;
    const long rc = syscall(158 /*__NR_arch_prctl*/, 0x1004 /*ARCH_GET_FS*/, &fs);
    (void)rc;
    return fs;
}

void SetHostFsBase(uint64_t fs) {
    syscall(158 /*__NR_arch_prctl*/, 0x1003 /*ARCH_SET_FS*/, fs);
}

// ---------------------------------------------------------------------------
// A bus that serves instruction fetches from the SAVED original bytes of one
// patched instruction and routes everything else to the real guest bus:
// replaying a patched instruction through X86Interpreter is therefore
// bit-identical to interpreting it in the first place (parity by
// construction).
// ---------------------------------------------------------------------------
class SingleInsnBus final : public GuestMemoryBus {
public:
    SingleInsnBus(GuestMemoryBus& real, uint64_t insn_gva,
                  const uint8_t* bytes, size_t len)
        : m_real(real), m_base(insn_gva), m_bytes(bytes), m_len(len) {}

    bool Read(uint64_t addr, void* dst, size_t size) override {
        if (addr >= m_base && addr - m_base + size <= m_len) {
            std::memcpy(dst, m_bytes + (addr - m_base), size);
            return true;
        }
        return m_real.Read(addr, dst, size);
    }
    bool Write(uint64_t addr, const void* src, size_t size) override {
        return m_real.Write(addr, src, size);
    }

private:
    GuestMemoryBus& m_real;
    uint64_t m_base;
    const uint8_t* m_bytes;
    size_t m_len;
};

// Extracts the DIRECT branch/call target of a block terminator, if the last
// instruction is a rel8/rel32 form. Returns 0 when the terminator is
// indirect (ret / jmp r / call r). CONDITIONAL branch targets are returned
// too: the taken side of a jcc is a block entry exactly like the fall-through,
// and the first execution of that side must be intercepted so its sites get
// patched BEFORE any native instruction of the block runs (otherwise a
// `syscall` on the taken path would reach the HOST kernel -- the guest uses
// FreeBSD-9 numbering, so even an allowed syscall number means the wrong
// call). Indirect targets stay undiscovered by construction; the seccomp
// ALLOWLIST (deny-by-default, see InstallSeccompGuard) bounds that
// (documented) worst case.
uint64_t DirectTargetOf(const std::vector<uint8_t>& code, uint64_t base,
                        const BlockInspectResult& scan) {
    if (code.empty() || scan.code_size == 0 || scan.code_size > code.size()) {
        return 0;
    }
    const size_t end = scan.code_size;
    // Scan back at most 15 bytes for a terminator whose rel8/rel32 payload
    // ends exactly at `end` (the prefixes before the opcode are skipped).
    for (size_t start = end >= 15 ? end - 15 : 0; start < end; ++start) {
        size_t p = start;
        while (p < end) {
            const uint8_t b = code[p];
            if (b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
                b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 ||
                b == 0x65 || (b >= 0x40 && b <= 0x4F)) {
                ++p;
                continue;
            }
            break;
        }
        if (p >= end) {
            continue;
        }
        const uint8_t op = code[p];
        int rel_bytes = -1;
        size_t after = 0;
        if (op == 0xE8 || op == 0xE9) {                 // call/jmp rel32
            rel_bytes = 4;
            after = p + 1 + 4;
        } else if (op == 0xEB || (op >= 0x70 && op <= 0x7F)) {  // jmp/jcc rel8
            rel_bytes = 1;
            after = p + 2;
        } else if (op == 0x0F && p + 1 < end && code[p + 1] >= 0x80 &&
                   code[p + 1] <= 0x8F) {               // jcc rel32
            rel_bytes = 4;
            after = p + 2 + 4;
        } else {
            continue;
        }
        if (after != end) {
            continue;
        }
        int64_t disp = 0;
        if (rel_bytes == 4) {
            uint32_t d = 0;
            std::memcpy(&d, code.data() + after - 4, 4);
            disp = static_cast<int64_t>(static_cast<int32_t>(d));
        } else {
            disp = static_cast<int8_t>(code[after - 1]);
        }
        const uint64_t target = base + after + static_cast<uint64_t>(disp);
        // Every DIRECT target -- unconditional AND conditional -- gets a
        // pre-armed entry (idempotent: already-discovered or already-armed
        // addresses are skipped inside ArmEntryTrapLocked).
        return target;
    }
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Backend singleton
// ---------------------------------------------------------------------------
DirectExecutionBackend& DirectExecutionBackend::Instance() {
    static DirectExecutionBackend instance;
    return instance;
}

const char* ToString(DirectStopReason reason) {
    switch (reason) {
        case DirectStopReason::None: return "none";
        case DirectStopReason::Returned: return "returned";
        case DirectStopReason::ThreadExit: return "thread-exit";
        case DirectStopReason::GuestFault: return "guest-fault";
        case DirectStopReason::FpuFault: return "fpu-fault";
        case DirectStopReason::IllegalInstruction: return "illegal-instruction";
        case DirectStopReason::Trap: return "trap";
        case DirectStopReason::Timeout: return "timeout";
        case DirectStopReason::HostError: return "host-error";
    }
    return "?";
}

void DirectExecutionBackend::EnsureHandlersInstalled() {
    if (m_handlers_installed) {
        return;
    }
    m_handlers_installed = true;
    struct sigaction sa{};
    sa.sa_sigaction = &DirectTrapHandler;
    sa.sa_flags = SA_SIGINFO;
    // Block every handled signal while a handler runs: a fault INSIDE the
    // handler is a host bug and fails loudly (default disposition).
    sigemptyset(&sa.sa_mask);
    for (const int s : kHandledSignals) {
        sigaddset(&sa.sa_mask, s);
    }
    for (const int s : kHandledSignals) {
        sigaction(s, &sa, nullptr);
    }
}

bool DirectExecutionBackend::Enable() {
    auto& vmm = Memory::VirtualMemoryManager::Instance();
    if (vmm.GetArenaHostBase() == nullptr &&
        !vmm.InitializeArena()) {
        return false;
    }
    // Identity mapping is a hard precondition: the trampoline (and every
    // native guest instruction) addresses memory with RAW guest addresses.
    if (!vmm.IsArenaIdentityMapped()) {
        return false;
    }
    if (!vmm.SetDirectExecutionMode(true)) {
        return false;
    }
    // Reloaded / self-modifying code must invalidate the patch registry:
    // a stale registry would let a syscall site that the new code moved (or
    // removed) go unpatched -- the guest's FreeBSD-9 syscall would reach the
    // HOST kernel. The VMM fires this on every write into an Exec page and
    // on Free/Protect of an Exec region.
    vmm.SetCodeWriteNotifier([this](uint64_t gva, size_t size) {
        InvalidateRange(gva, size);
    });
    // Round 30: freshly emitted HLE trampoline stubs are reached through
    // indirect PLT jumps — arm their syscall sites immediately instead of
    // letting the magic number reach the seccomp guard natively.
    HleTrampolines::SetStubPreDiscoverHook([](uint64_t stub_gva) {
        bool ok = false;
        (void)Instance().DiscoverBlock(stub_gva, &ok);
    });
    EnsureHandlersInstalled();
    m_enabled = true;
    return true;
}

void DirectExecutionBackend::Disable() {
    if (!m_enabled) {
        return;
    }
    Memory::VirtualMemoryManager::Instance().SetCodeWriteNotifier({});
    ResetAllPatches();
    Memory::VirtualMemoryManager::Instance().SetDirectExecutionMode(false);
    m_enabled = false;
}

void DirectExecutionBackend::InvalidateRange(uint64_t gva, size_t size) {
    std::lock_guard<std::mutex> lock(m_registry_mutex);
    // Drop every interception site inside the range. Restore-if-still-ud2
    // semantics: when the guest's write did NOT cover a planted ud2, the
    // original (still-current) bytes go back; when it DID cover it, the ud2
    // is already gone (the guest's own bytes are authoritative) and the
    // restore is skipped.
    for (auto it = m_sites.begin(); it != m_sites.end();) {
        const uint64_t at = it->first;
        if (at >= gva && at < gva + size) {
            RestoreSiteLocked(at, it->second);
            it = m_sites.erase(it);
        } else {
            ++it;
        }
    }
    // Forget every discovered block ENTRY inside the range: the next
    // execution re-scans whatever bytes are there now.
    for (auto it = m_discovered.begin(); it != m_discovered.end();) {
        if (*it >= gva && *it < gva + size) {
            it = m_discovered.erase(it);
        } else {
            ++it;
        }
    }
}

bool DirectExecutionBackend::FetchCode(uint64_t gva, size_t max_bytes,
                                       std::vector<uint8_t>& code) const {
    code.clear();
    auto& vmm = Memory::VirtualMemoryManager::Instance();
    const uint32_t exec = static_cast<uint32_t>(Memory::PageProt::Read) |
                          static_cast<uint32_t>(Memory::PageProt::Exec);
    if (!vmm.IsGvaMapped(gva) || !vmm.IsGvaExecutable(gva)) {
        return false;
    }
    code.resize(max_bytes);
    size_t got = 0;
    while (got < max_bytes) {
        const size_t chunk = std::min<size_t>(64, max_bytes - got);
        if (!vmm.CopyFromGuest(gva + got, code.data() + got, chunk, exec)) {
            break;
        }
        got += chunk;
    }
    code.resize(got);
    return got > 0;
}

bool DirectExecutionBackend::WriteCodeBytes(uint64_t gva, const uint8_t* bytes,
                                             size_t len) {
    auto& vmm = Memory::VirtualMemoryManager::Instance();
    if (!vmm.IsGvaMapped(gva)) {
        return false;
    }
    void* hva = vmm.GvaToHva(gva);
    if (hva == nullptr) {
        return false;
    }
    // The page(s) holding [gva, gva+len) -- code pages are R+X, so lift them
    // to RWX for the (2-byte, atomic) patch and restore right after.
    const uintptr_t lo = reinterpret_cast<uintptr_t>(hva) & ~uintptr_t(0xFFFull);
    const uintptr_t hi = (reinterpret_cast<uintptr_t>(hva) + len + 0xFFFull) &
                         ~uintptr_t(0xFFFull);
    const size_t span = static_cast<size_t>(hi - lo);
    const uint32_t guest_prot = vmm.GetPageProt(gva);
    int restore = PROT_NONE;
    if ((guest_prot & static_cast<uint32_t>(Memory::PageProt::Read)) != 0) {
        restore |= PROT_READ;
    }
    if ((guest_prot & static_cast<uint32_t>(Memory::PageProt::Write)) != 0) {
        restore |= PROT_WRITE;
    }
    if ((guest_prot & static_cast<uint32_t>(Memory::PageProt::Exec)) != 0) {
        restore |= PROT_EXEC;
    }
    if (mprotect(reinterpret_cast<void*>(lo), span, restore | PROT_WRITE | PROT_EXEC) != 0) {
        return false;
    }
    std::memcpy(hva, bytes, len);
    mprotect(reinterpret_cast<void*>(lo), span, restore);
    return true;
}

BlockInspectResult DirectExecutionBackend::DiscoverBlock(uint64_t gva, bool* ok) {
    BlockInspectResult result{};
    *ok = false;

    std::vector<uint8_t> code;
    if (!FetchCode(gva, 4096, code)) {
        result.status = ExecStatus::MemoryFault;
        result.fault_addr = gva;
        return result;
    }

    // The fetched bytes may still contain ARMED entry traps inside this
    // block (mid-block branch targets armed before the block itself was
    // discovered). The scan MUST see the TRUE instruction stream -- decoding
    // a planted ud2 as a 2-byte instruction would shift every later offset,
    // misplacing the interception sites (a real `syscall` would stay
    // unpatched). Restore each armed trap's original bytes IN THE COPY (the
    // live patches stay armed: execution reaches them and they disarm
    // themselves through the normal SIGILL path).
    {
        std::lock_guard<std::mutex> lock(m_registry_mutex);
        for (const auto& [site_gva, rec] : m_sites) {
            if (!rec.armed_entry) {
                continue;
            }
            if (site_gva >= gva && site_gva + 2 <= gva + code.size()) {
                std::memcpy(code.data() + (site_gva - gva), rec.original, 2);
            }
        }
    }

    result = X86Interpreter::InspectBlock(code, gva);
    if (result.status != ExecStatus::Running &&
        result.status != ExecStatus::Halted) {
        // Halted (hlt) is a legitimate terminator: the guest will fault on it
        // naturally; sites before it still get patched.
        if (result.status != ExecStatus::Halted) {
            return result;
        }
    }
    if (result.code_size == 0) {
        return result;
    }

    std::lock_guard<std::mutex> lock(m_registry_mutex);
    // Patch every interception site inside the block.
    for (const PatchSiteInfo& site : result.sites) {
        const uint64_t site_gva = gva + site.offset;
        if (m_sites.count(site_gva) != 0) {
            continue;   // already patched (idempotent across threads)
        }
        SiteRecord rec{};
        rec.length = static_cast<uint8_t>(site.length);
        rec.patched = 2;
        rec.kind = site.kind;
        if (site.length > sizeof(rec.original) || site.length < 2) {
            continue;   // cannot happen (max instruction 15 bytes) -- guard
        }
        std::memcpy(rec.original, code.data() + site.offset, site.length);
        m_sites.emplace(site_gva, rec);
        const uint8_t ud2[2] = {0x0F, 0x0B};
        if (!WriteCodeBytes(site_gva, ud2, 2)) {
            // A site we cannot rewrite is one we cannot intercept: running
            // the block natively would let a host syscall through. Fail the
            // whole discovery (the run loop declines/aborts).
            m_sites.erase(site_gva);
            m_fatal_error = "cannot patch interception site at GVA 0x" +
                            std::to_string(site_gva);
            *ok = false;
            return result;
        }
        m_lifetime.sites_patched++;
    }
    m_discovered.insert(gva);

    // Arm an entry trap at the fall-through successor (and at a direct
    // call/jmp target) so the next undiscovered block stops us the same way.
    const auto arm = [&](uint64_t target) {
        if (target == 0) {
            return;
        }
        ArmEntryTrapLocked(target);
    };
    arm(result.next_rip);
    arm(DirectTargetOf(code, gva, result));

    *ok = true;
    m_lifetime.blocks_discovered++;
    return result;
}

bool DirectExecutionBackend::ArmEntryTrapLocked(uint64_t gva) {
    auto& vmm = Memory::VirtualMemoryManager::Instance();
    if (m_discovered.count(gva) != 0 || m_sites.count(gva) != 0) {
        return false;   // already known: execution flows through naturally
    }
    if (!vmm.IsGvaMapped(gva) || !vmm.IsGvaExecutable(gva)) {
        return false;   // successor outside code: a natural fault handles it
    }
    std::vector<uint8_t> orig;
    if (!FetchCode(gva, 2, orig) || orig.size() < 2) {
        return false;
    }
    SiteRecord rec{};
    rec.length = 2;
    rec.patched = 2;
    rec.armed_entry = true;
    rec.kind = PatchKind::None;
    std::memcpy(rec.original, orig.data(), 2);
    const uint8_t ud2[2] = {0x0F, 0x0B};
    if (!WriteCodeBytes(gva, ud2, 2)) {
        return false;
    }
    m_sites.emplace(gva, rec);
    return true;
}

void DirectExecutionBackend::DisarmEntryTrapLocked(uint64_t gva) {
    const auto it = m_sites.find(gva);
    if (it == m_sites.end() || !it->second.armed_entry) {
        return;
    }
    RestoreSiteLocked(gva, it->second);
    m_sites.erase(it);
}

void DirectExecutionBackend::RestoreSiteLocked(uint64_t gva, SiteRecord& rec) {
    // Only restore when the patch is still in place (the region may have
    // been freed and reloaded with different code since).
    std::vector<uint8_t> cur;
    if (FetchCode(gva, 2, cur) && cur.size() == 2 && cur[0] == 0x0F && cur[1] == 0x0B) {
        (void)WriteCodeBytes(gva, rec.original, rec.patched);
    }
}

bool DirectExecutionBackend::EmulatePatchedInstruction(CpuState& cpu,
                                                        GuestMemoryBus& bus,
                                                        uint64_t gva) {
    SiteRecord rec{};
    {
        std::lock_guard<std::mutex> lock(m_registry_mutex);
        const auto it = m_sites.find(gva);
        if (it == m_sites.end() || it->second.armed_entry) {
            return false;
        }
        rec = it->second;
    }

    // Replay the ORIGINAL bytes through the interpreter core. Instruction
    // fetches are served from the saved bytes; every data access goes to the
    // real guest bus -- emulation is by construction identical to
    // interpreting the same instruction.
    CpuState tmp = cpu;
    tmp.rip = gva;
    SingleInsnBus insn_bus(bus, gva, rec.original, rec.length);
    X86Interpreter interp(tmp, insn_bus);
    const RunResult rr = interp.Step();
    if (rr.status != ExecStatus::Running) {
        return false;
    }
    // Commit: GPRs, flags, rip (already advanced), XMM file.
    cpu = tmp;
    return true;
}
uint64_t DirectExecutionBackend::RunFunction(CpuState& cpu, GuestMemoryBus& bus,
                                             const SyscallHandler& syscalls,
                                             uint64_t stop_sentinel,
                                             uint64_t budget_ms,
                                             DirectRunOutcome& out) {
    out = DirectRunOutcome{};
#if !defined(__x86_64__) || !defined(__linux__)
    (void)cpu; (void)bus; (void)syscalls; (void)stop_sentinel; (void)budget_ms;
    out.reason = DirectStopReason::HostError;
    out.message = "direct execution requires an x86-64 Linux host";
    return 0;
#else
    if (!m_enabled) {
        out.reason = DirectStopReason::HostError;
        out.message = "backend not enabled";
        return 0;
    }
    auto& vmm = Memory::VirtualMemoryManager::Instance();
    if (vmm.GetArenaHostBase() == nullptr) {
        out.reason = DirectStopReason::HostError;
        out.message = "VMM arena not initialized";
        return 0;
    }
    if (!vmm.IsArenaIdentityMapped()) {
        out.reason = DirectStopReason::HostError;
        out.message = "arena is not identity-mapped (guest addresses are not "
                      "valid host addresses)";
        return 0;
    }
    if (!vmm.IsGvaMapped(cpu.rip) || !vmm.IsGvaExecutable(cpu.rip)) {
        out.reason = DirectStopReason::HostError;
        out.message = "entry point is not executable guest memory";
        return 0;
    }
    if (!vmm.IsGvaWritable(cpu.gpr[RSP])) {
        out.reason = DirectStopReason::HostError;
        out.message = "guest stack pointer is not writable guest memory";
        return 0;
    }

    InstallSeccompGuard();

    // Discover + patch the entry block BEFORE the first native instruction.
    {
        bool ok = false;
        (void)DiscoverBlock(cpu.rip, &ok);
        out.stats.blocks_discovered++;
        if (!m_fatal_error.empty()) {
            out.reason = DirectStopReason::HostError;
            out.message = m_fatal_error;
            m_fatal_error.clear();
            return 0;
        }
    }

    DirectThreadCtx ctx{};
    ctx.cpu = &cpu;

    const uint64_t host_fs = GetHostFsBase();

    // Wall-clock budget via ITIMER_REAL + SIGALRM (the handler longjmps out).
    struct itimerval timer{};
    struct itimerval timer_off{};
    if (budget_ms != 0) {
        timer.it_value.tv_sec = static_cast<time_t>(budget_ms / 1000);
        timer.it_value.tv_usec = static_cast<suseconds_t>((budget_ms % 1000) * 1000);
        setitimer(ITIMER_REAL, &timer, nullptr);
    }

    m_active_runs++;

    for (;;) {
        // Load the trampoline context from the (possibly updated) CpuState.
        ctx.rip = cpu.rip;
        ctx.rsp = cpu.gpr[RSP];
        ctx.fs_base = cpu.fs_base;
        ctx.rflags = (cpu.rflags & 0xCD5ull) | 0x2ull;  // arithmetic flags only
        std::memcpy(ctx.gpr, cpu.gpr.data(), sizeof(ctx.gpr));
        // Guest vector state: the trampoline reloads the whole XMM file (and
        // the YMM upper halves on AVX hosts) so results of EMULATED
        // instructions reach the native stream on re-entry.
        std::memcpy(ctx.xmm, cpu.xmm.data(), sizeof(ctx.xmm));
        std::memcpy(ctx.ymm_hi, cpu.ymm_hi.data(), sizeof(ctx.ymm_hi));
        ctx.have_avx = HostHasAvx() ? 1 : 0;
        for (size_t i = 0; i < 16; ++i) {
            std::memcpy(ctx.ymm_staging + 32 * i, &cpu.xmm[i], 16);
            std::memcpy(ctx.ymm_staging + 32 * i + 16, &cpu.ymm_hi[i], 16);
        }
        ctx.stop_sig = 0;
        ctx.fault_addr = 0;
        ctx.active = true;

        t_direct_ctx = &ctx;
        if (sigsetjmp(ctx.env, 1) == 0) {
            ps5_direct_enter(&ctx);
            __builtin_unreachable();
        }
        // siglongjmp lands here (or a nested trap: impossible, they are
        // blocked while the handler runs).
        t_direct_ctx = nullptr;
        ctx.active = false;
        out.stats.reentries++;

        if (ctx.stop_sig == SIGALRM) {
            out.reason = DirectStopReason::Timeout;
            out.message = "wall-clock budget exhausted";
            break;
        }
        if (ctx.stop_sig == SIGSEGV || ctx.stop_sig == SIGBUS) {
            // A native `ret` to the synthetic stop address faults before the
            // CPU can commit RIP to that unmapped address.  In that case the
            // signal frame still points at the RET instruction and si_addr
            // is platform-dependent (some kernels report 0).  Recognize the
            // sentinel from the guest stack instead of relying on the faulting
            // address alone.
            bool returned = (cpu.rip == stop_sentinel);
            if (!returned && vmm.IsGvaExecutable(cpu.rip) &&
                vmm.IsGvaMapped(cpu.gpr[RSP])) {
                uint8_t op = 0;
                uint64_t target = 0;
                if (vmm.CopyFromGuest(cpu.rip, &op, 1,
                                      static_cast<uint32_t>(Memory::PageProt::Read) |
                                      static_cast<uint32_t>(Memory::PageProt::Exec)) &&
                    op == 0xC3 &&
                    vmm.CopyFromGuest(cpu.gpr[RSP], &target, sizeof(target),
                                      static_cast<uint32_t>(Memory::PageProt::Read)) &&
                    target == stop_sentinel) {
                    returned = true;
                }
            }
            if (returned) {
                cpu.rip = stop_sentinel;
                out.reason = DirectStopReason::Returned;
            } else {
                out.reason = DirectStopReason::GuestFault;
                out.fault_gva = ctx.fault_addr;
                out.fault_rip = cpu.rip;
                out.stats.faults++;
                out.message = "guest page fault";
            }
            break;
        }
        if (ctx.stop_sig == SIGBUS) {
            out.reason = DirectStopReason::GuestFault;
            out.fault_gva = ctx.fault_addr;
            out.fault_rip = cpu.rip;
            out.stats.faults++;
            out.message = "guest bus fault";
            break;
        }
        if (ctx.stop_sig == SIGFPE) {
            out.reason = DirectStopReason::FpuFault;
            out.fault_rip = cpu.rip;
            out.stats.faults++;
            break;
        }
        if (ctx.stop_sig == SIGTRAP) {
            out.reason = DirectStopReason::Trap;
            out.fault_rip = cpu.rip;
            break;
        }
        if (ctx.stop_sig != SIGILL) {
            out.reason = DirectStopReason::HostError;
            out.message = "unexpected stop signal";
            break;
        }

        // ---- SIGILL: either an interception we planted or a guest ud2 ----
        const uint64_t at = cpu.rip;
        PatchKind kind = PatchKind::None;
        uint32_t insn_len = 0;
        bool is_armed_entry = false;
        {
            std::lock_guard<std::mutex> lock(m_registry_mutex);
            const auto it = m_sites.find(at);
            if (it != m_sites.end()) {
                kind = it->second.kind;
                insn_len = it->second.length;
                is_armed_entry = it->second.armed_entry;
                if (is_armed_entry) {
                    // Undo the trap bytes; the block becomes discoverable.
                    DisarmEntryTrapLocked(at);
                }
            }
        }

        if (is_armed_entry) {
            if (m_discovered.count(at) == 0) {
                bool ok = false;
                (void)DiscoverBlock(at, &ok);
                out.stats.blocks_discovered++;
                if (!m_fatal_error.empty()) {
                    out.reason = DirectStopReason::HostError;
                    out.message = m_fatal_error;
                    out.fault_rip = at;
                    m_fatal_error.clear();
                    break;
                }
            }
            continue;   // re-enter at the same rip (original bytes restored)
        }
        if (kind == PatchKind::Syscall) {
            if (!syscalls) {
                out.reason = DirectStopReason::ThreadExit;
                break;
            }
            if (!syscalls(cpu, bus)) {
                out.reason = DirectStopReason::ThreadExit;
                break;
            }
            cpu.rip = at + insn_len;   // skip the (ud2-masked) syscall
            out.stats.syscalls_serviced++;
            continue;
        }
        if (kind != PatchKind::None) {
            if (!EmulatePatchedInstruction(cpu, bus, at)) {
                out.reason = DirectStopReason::IllegalInstruction;
                out.fault_rip = at;
                out.message = "patched-instruction replay failed";
                break;
            }
            out.stats.instructions_emulated++;
            continue;
        }
        // A genuine guest ud2 (or an unknown illegal encoding): fail closed.
        out.reason = DirectStopReason::IllegalInstruction;
        out.fault_rip = at;
        break;
    }

    // The function result is RAX at the stop (Returned / faults). For
    // ThreadExit the caller inspects `reason` (the exit code path is the
    // caller's contract, e.g. thr_exit unwinds with its own bookkeeping).
    const uint64_t result = cpu.gpr[RAX];

    m_active_runs--;
    if (budget_ms != 0) {
        setitimer(ITIMER_REAL, &timer_off, nullptr);
    }

    // Restore the host thread's discipline (FS base, DF, MXCSR, x87).
    RestoreHostFpuState();
    SetHostFsBase(host_fs);

    // Fold into the lifetime stats.
    m_lifetime.blocks_discovered += out.stats.blocks_discovered;
    m_lifetime.syscalls_serviced += out.stats.syscalls_serviced;
    m_lifetime.instructions_emulated += out.stats.instructions_emulated;
    m_lifetime.reentries += out.stats.reentries;
    m_lifetime.faults += out.stats.faults;

    return result;
#endif
}

size_t DirectExecutionBackend::PatchedSiteCount() const {
    std::lock_guard<std::mutex> lock(m_registry_mutex);
    return m_sites.size();
}

size_t DirectExecutionBackend::DiscoveredBlockCount() const {
    std::lock_guard<std::mutex> lock(m_registry_mutex);
    return m_discovered.size();
}

bool DirectExecutionBackend::HasSiteAt(uint64_t gva) const {
    std::lock_guard<std::mutex> lock(m_registry_mutex);
    return m_sites.count(gva) != 0;
}

PatchKind DirectExecutionBackend::SiteKindAt(uint64_t gva) const {
    std::lock_guard<std::mutex> lock(m_registry_mutex);
    const auto it = m_sites.find(gva);
    return it == m_sites.end() ? PatchKind::None : it->second.kind;
}

void DirectExecutionBackend::ResetAllPatches() {
    std::lock_guard<std::mutex> lock(m_registry_mutex);
    for (auto& [gva, rec] : m_sites) {
        RestoreSiteLocked(gva, rec);
    }
    m_sites.clear();
    m_discovered.clear();
}

} // namespace PS5::CPU
