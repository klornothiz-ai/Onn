// ============================================================================
// ProsperoLayer RDNA2 Core - Syscall depth test (round 17)
// ----------------------------------------------------------------------------
// Proves the new deep syscall layer: GVA-backed _umtx_op (WAIT/WAKE pairing
// across REAL guest threads), sysctl identity, getrusage, sigaction state,
// writev, honest ENOSYS/EPERM refusals.
// ============================================================================
#include "cpu/prospero_syscalls.hpp"
#include "cpu/guest_threads.hpp"
#include "memory/virtual_memory_manager.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <unistd.h>

namespace {

using PS5::CPU::GuestThreadManager;
using PS5::CPU::ProsperoSyscallDispatcher;
using PS5::Memory::PageProt;
using PS5::Memory::VirtualMemoryManager;

int g_failures = 0;
int g_checks = 0;
bool Check(bool v, const char* e, int line) {
    ++g_checks;
    if (!v) { ++g_failures; std::cerr << "  [FAIL] line " << line << ": " << e << '\n'; }
    return v;
}
#define CHECK(e) Check((e), #e, __LINE__)

constexpr uint32_t kRw = static_cast<uint32_t>(PageProt::Read) |
                         static_cast<uint32_t>(PageProt::Write);
constexpr uint32_t kRwx = kRw | static_cast<uint32_t>(PageProt::Exec);

uint64_t Dispatch1(uint64_t nr, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0,
                   uint64_t d = 0, uint64_t e = 0, uint64_t f = 0) {
    PS5::CPU::SyscallContext ctx{};
    ctx.rax = nr; ctx.rdi = a; ctx.rsi = b; ctx.rdx = c;
    ctx.rcx = d; ctx.r8 = e; ctx.r9 = f;
    return ProsperoSyscallDispatcher::Instance().Dispatch(ctx);
}

} // namespace

int main() {
    auto& vmm = VirtualMemoryManager::Instance();
    auto& disp = ProsperoSyscallDispatcher::Instance();

    std::cout << "== Part A: coverage count + honest refusals ==\n";
    {
        CHECK(disp.GetSyscallCount() >= 75); // was 37 at round 14
        CHECK(Dispatch1(59) == 1);           // execve -> EPERM
        CHECK(Dispatch1(66) == 0x80020016);  // vfork -> ENOSYS
        // pipe(42) gained a real host-backed implementation in round 26: a
        // null guest buffer still fails closed (EFAULT), and a valid guest
        // buffer receives two real, distinct, usable pipe descriptors.
        CHECK(Dispatch1(42, 0) == 14); // pipe(NULL) -> EFAULT, not ENOSYS
        {
            const uint64_t fds_addr = 0x1000A00000ULL;
            CHECK(vmm.AllocateVirtual(fds_addr, 0x1000, kRw) != 0);
            CHECK(Dispatch1(42, fds_addr) == 0); // pipe(&fds) -> success
            int fds[2] = {-1, -1};
            CHECK(vmm.CopyFromGuest(fds_addr, fds, sizeof(fds), kRw));
            CHECK(fds[0] >= 0 && fds[1] >= 0 && fds[0] != fds[1]);
            const char msg[5] = {'p', 'i', 'n', 'g', 0};
            CHECK(::write(fds[1], msg, sizeof(msg)) == sizeof(msg));
            char readback[5] = {};
            CHECK(::read(fds[0], readback, sizeof(readback)) == sizeof(readback));
            CHECK(std::memcmp(msg, readback, sizeof(msg)) == 0);
            ::close(fds[0]);
            ::close(fds[1]);
            vmm.FreeVirtual(fds_addr, 0x1000);
        }
        CHECK(Dispatch1(999999) == 0x80020016); // unknown -> ENOSYS
    }

    std::cout << "== Part B: _umtx_op on real guest memory ==\n";
    {
        const uint64_t word = 0x1000900000ULL;
        CHECK(vmm.AllocateVirtual(word, 0x1000, kRw) != 0);
        uint32_t v = 7;
        CHECK(vmm.CopyToGuest(word, &v, 4, kRw));

        // WAIT with immediate timeout: value unchanged -> ETIMEDOUT-style
        CHECK(Dispatch1(436, word, 0, 7, 0, 1000) == 0x8002000e);
        // WAIT when value differs: instant success
        CHECK(Dispatch1(436, word, 0, 8, 0, 0) == 0);
        // LOCK/UNLOCK (the umtx word must be 0 = free first)
        v = 0;
        CHECK(vmm.CopyToGuest(word, &v, 4, kRw));
        CHECK(Dispatch1(436, word, 2) == 0);
        uint32_t cur = 0;
        vmm.CopyFromGuest(word, &cur, 4, kRw);
        CHECK(cur == 1);
        CHECK(Dispatch1(436, word, 3) == 0);
        vmm.CopyFromGuest(word, &cur, 4, kRw);
        CHECK(cur == 0);
        // MUTEX_LOCK/UNLOCK
        CHECK(Dispatch1(436, word, 4) == 0);
        vmm.CopyFromGuest(word, &cur, 4, kRw);
        CHECK(cur != 0);
        CHECK(Dispatch1(436, word, 5) == 0);
        // unknown sub-op fails closed
        CHECK(Dispatch1(436, word, 99) == 0x80020016);
    }

    std::cout << "== Part C: umtx WAIT/WAKE across real guest threads ==\n";
    {
        // The futex pattern games use: a worker thread holds the "lock"
        // value; the main guest thread umtx-WAITs on it; the worker stores a
        // new value (the wake); the wait completes.
        const uint64_t word = 0x1000901000ULL;
        CHECK(vmm.AllocateVirtual(word, 0x1000, kRw) != 0);
        const uint64_t code = 0x1000A00000ULL;
        CHECK(vmm.AllocateVirtual(code, 0x1000, kRwx) != 0);
        uint32_t v = 0xAAAA;
        vmm.CopyToGuest(word, &v, 4, kRw);
        // worker(rdi=word): mov eax,0x1234; mov [rdi],eax; ret
        const uint8_t worker[] = {0xB8, 0x34, 0x12, 0x00, 0x00, 0x89, 0x07, 0xC3};
        vmm.CopyToGuest(code, worker, sizeof(worker), kRw);

        // Spawn the worker; it will publish 0x1234 "soon". The main thread
        // umtx-WAITs for the value to differ from 0xAAAA with a 5 s budget.
        const uint32_t tid = GuestThreadManager::Instance().SpawnThread(
            code, word, "umtx-waker", 128 * 1024, 100000);
        CHECK(tid != 0);
        const uint64_t r = Dispatch1(436, word, 0, 0xAAAA, 0, 5000000);
        CHECK(r == 0); // woken by the worker's store (or already changed)
        uint32_t final_v = 0;
        vmm.CopyFromGuest(word, &final_v, 4, kRw);
        CHECK(final_v == 0x1234);
        CHECK(GuestThreadManager::Instance().JoinThread(tid, 5000000));
    }

    std::cout << "== Part D: sysctl identity + getrusage ==\n";
    {
        const uint64_t name = 0x1000902000ULL, out = 0x1000902100ULL, lenp = 0x1000902200ULL;
        CHECK(vmm.AllocateVirtual(name, 0x1000, kRw) != 0);
        const int kern_ostype[2] = {1, 1};
        vmm.CopyToGuest(name, kern_ostype, 8, kRw);
        size_t cap = 64;
        vmm.CopyToGuest(lenp, &cap, 8, kRw);
        CHECK(Dispatch1(200, name, 2, out, lenp) == 0);
        char buf[64] = {};
        vmm.CopyFromGuest(out, buf, 16, kRw);
        CHECK(std::strcmp(buf, "FreeBSD") == 0);
        size_t outlen = 0;
        vmm.CopyFromGuest(lenp, &outlen, 8, kRw);
        CHECK(outlen == 8);

        const int hw_ncpu[2] = {6, 3};
        vmm.CopyToGuest(name, hw_ncpu, 8, kRw);
        cap = 64;
        vmm.CopyToGuest(lenp, &cap, 8, kRw);
        CHECK(Dispatch1(200, name, 2, out, lenp) == 0);
        int ncpu = 0;
        vmm.CopyFromGuest(out, &ncpu, 4, kRw);
        CHECK(ncpu >= 1);

        const int bogus[2] = {99, 99};
        vmm.CopyToGuest(name, bogus, 8, kRw);
        CHECK(Dispatch1(200, name, 2, out, lenp) == 2); // ENOENT: honest

        const uint64_t ru_gva = 0x1000903000ULL;
        CHECK(vmm.AllocateVirtual(ru_gva, 0x1000, kRw) != 0);
        CHECK(Dispatch1(117, 0, ru_gva) == 0);
        uint64_t utime_micros = 0;
        vmm.CopyFromGuest(ru_gva, &utime_micros, 8, kRw); // timeval tv_usec
        CHECK(utime_micros < 1000000);
    }

    std::cout << "== Part E: sigaction state + writev ==\n";
    {
        const uint64_t sa_gva = 0x1000904000ULL;
        CHECK(vmm.AllocateVirtual(sa_gva, 0x1000, kRw) != 0);
        CHECK(Dispatch1(46, 5, 0x1234, sa_gva) == 0);  // sigaction(5, handler)
        uint64_t old = 0;
        vmm.CopyFromGuest(sa_gva, &old, 8, kRw);
        CHECK(old == 0x1234);                            // old action read back
        CHECK(Dispatch1(46, 0, 0, 0) == 22);             // signo 0 -> EINVAL
        CHECK(Dispatch1(46, 200, 0, 0) == 22);           // signo 200 -> EINVAL

        // writev: two iovecs {len 2, len 3} -> returns 5, prints "helloworld"
        const uint64_t iov = 0x1000905000ULL, s1 = 0x1000905100ULL, s2 = 0x1000905200ULL;
        vmm.AllocateVirtual(iov, 0x1000, kRw);
        vmm.CopyToGuest(s1, "hello", 5, kRw);
        vmm.CopyToGuest(s2, "world", 5, kRw);
        const uint64_t iovs[2][2] = {{s1, 2}, {s2, 3}};
        vmm.CopyToGuest(iov, iovs, 32, kRw);
        CHECK(Dispatch1(121, 1, iov, 2) == 5);
        std::cout << "\n";
    }

    std::cout << "syscall_depth_test: " << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
