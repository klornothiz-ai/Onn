// ============================================================================
// ProsperoLayer RDNA2 Core - Real guest threads test (round 15)
// ----------------------------------------------------------------------------
// Proves guest threads are REAL now:
//
//   Part A: thr_exit path -- a spawned thread runs real ISA (SSE packed add +
//           call/ret on its own VMM stack) and terminates through the
//           thr_exit syscall with its exit code propagated to the joiner.
//   Part B: natural-return path -- exit code = RAX at the entry `ret`.
//   Part C: the raw syscall route end-to-end: a MAIN guest program (through
//           CPUJitEngine::ExecuteGuestFull) calls thr_new(worker, arg); the
//           worker runs on a real host thread; the main program polls guest
//           memory until the worker publishes its result.
//   Part D: per-thread TLS isolation -- with a TLS allocator installed every
//           thread observes a distinct fs:[0] self-pointer (4 parallel
//           threads), and VMM writes from parallel threads land intact.
//   Part E: unsupported opcode unwinds the thread (no hang), join timeout
//           semantics, detach semantics.
// ============================================================================
#include "cpu/guest_threads.hpp"
#include "cpu/jit_executor.hpp"
#include "memory/virtual_memory_manager.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

using PS5::CPU::CPUJitEngine;
using PS5::CPU::GuestThreadManager;
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

constexpr uint32_t kRwx = static_cast<uint32_t>(PageProt::Read) |
                          static_cast<uint32_t>(PageProt::Write) |
                          static_cast<uint32_t>(PageProt::Exec);
constexpr uint32_t kRw = static_cast<uint32_t>(PageProt::Read) |
                         static_cast<uint32_t>(PageProt::Write);

constexpr uint64_t kCodeBase = 0x1000700000ULL;
constexpr uint64_t kDataBase = 0x1000800000ULL;

struct Asm {
    std::vector<uint8_t> code;
    uint64_t base;
    explicit Asm(uint64_t b) : base(b) {}
    uint64_t gva() const { return base + code.size(); }
    void b(uint8_t v) { code.push_back(v); }
    void imm32(uint32_t v) { for (int i = 0; i < 4; ++i) b((v >> (8 * i)) & 0xFF); }
    void imm64(uint64_t v) { for (int i = 0; i < 8; ++i) b((v >> (8 * i)) & 0xFF); }
};

bool MapCode(uint64_t gva, const std::vector<uint8_t>& code) {
    auto& vmm = VirtualMemoryManager::Instance();
    const uint64_t page = gva & ~0xFFFULL;
    const size_t span = (gva - page) + code.size();
    const size_t pages = ((span + 0xFFF) / 0x1000) * 0x1000;
    if (!vmm.IsGvaMapped(page)) {
        if (vmm.AllocateVirtual(page, pages, kRwx) == 0) return false;
    }
    return vmm.CopyToGuest(gva, code.data(), code.size(), kRw);
}

template <typename T>
bool GuestRead(uint64_t gva, T& out) {
    return VirtualMemoryManager::Instance().CopyFromGuest(gva, &out, sizeof(T), kRw);
}
template <typename T>
bool GuestWrite(uint64_t gva, const T& v) {
    return VirtualMemoryManager::Instance().CopyToGuest(gva, &v, sizeof(T), kRw);
}

} // namespace

int main() {
    auto& vmm = VirtualMemoryManager::Instance();
    auto& mgr = GuestThreadManager::Instance();

    // Install a real TLS allocator up front: every spawned thread gets a fresh
    // 4 KB block with a self-pointer at fs:[0] (GuestLauncher's layout).
    mgr.SetTlsAllocator([]() -> uint64_t {
        auto& v = VirtualMemoryManager::Instance();
        const uint64_t block = v.AllocateVirtual(0, 0x1000, kRw);
        if (block == 0) return 0;
        v.ZeroGuest(block, 0x1000);
        v.CopyToGuest(block, &block, 8, kRw);
        return block;
    });

    std::cout << "== Part A: thr_exit path with real ISA on a VMM stack ==\n";
    {
        // data layout @ kDataBase:
        //  +0  u32 in_a          +4  u32 in_b
        //  +8  u32 flag          +12 u32 sum
        //  +16 16B SSE result    +32 16B vec_a   +48 16B vec_b
        //  +64 u64 fs_self
        CHECK(vmm.AllocateVirtual(kDataBase, 0x1000, kRw) != 0);
        GuestWrite<uint32_t>(kDataBase + 0, 20);
        GuestWrite<uint32_t>(kDataBase + 4, 22);
        const float va[4] = {1.f, 2.f, 3.f, 4.f};
        const float vb[4] = {10.f, 20.f, 30.f, 40.f};
        vmm.CopyToGuest(kDataBase + 32, va, 16, kRw);
        vmm.CopyToGuest(kDataBase + 48, vb, 16, kRw);

        Asm a(kCodeBase);
        // worker(rdi = data):
        //   call helper          ; exercises the per-thread VMM stack
        //   movaps xmm0,[rdi+32] ; + addps [rdi+48] ; movaps [rdi+16],xmm0
        //   mov eax,[rdi] ; add eax,[rdi+4] ; mov [rdi+12],eax
        //   mov rax,[fs:0] ; mov [rdi+64],rax
        //   mov eax,1 ; mov [rdi+8],eax     ; flag publish (release-style)
        //   mov eax,431 ; mov edi,77 ; syscall   (thr_exit(77))
        // helper: mov eax,[rdi] ; add eax,0x1000 ; ret
        const uint64_t helper = kCodeBase + 0x60;
        a.b(0xE8); a.imm32(static_cast<uint32_t>(helper - (a.gva() + 5))); // call helper
        a.b(0x0F); a.b(0x28); a.b(0x47); a.b(0x20);            // movaps xmm0,[rdi+0x20]
        a.b(0x0F); a.b(0x58); a.b(0x47); a.b(0x30);            // addps  xmm0,[rdi+0x30]
        a.b(0x0F); a.b(0x29); a.b(0x47); a.b(0x10);            // movaps [rdi+0x10],xmm0
        a.b(0x8B); a.b(0x07);                                  // mov eax,[rdi]
        a.b(0x03); a.b(0x47); a.b(0x04);                       // add eax,[rdi+4]
        a.b(0x89); a.b(0x47); a.b(0x0C);                       // mov [rdi+0xC],eax
        a.b(0x64); a.b(0x48); a.b(0x8B); a.b(0x04); a.b(0x25); a.imm32(0); // mov rax,fs:[0]
        a.b(0x48); a.b(0x89); a.b(0x87); a.imm32(64);          // mov [rdi+64],rax
        a.b(0xB8); a.imm32(1);                                 // mov eax,1
        a.b(0x89); a.b(0x47); a.b(0x08);                       // mov [rdi+8],eax
        a.b(0xB8); a.imm32(431);                               // mov eax,431
        a.b(0xBF); a.imm32(77);                                // mov edi,77
        a.b(0x0F); a.b(0x05);                                  // syscall
        a.b(0xC3);                                             // ret (never reached)
        CHECK(a.code.size() <= 0x60);
        while (a.code.size() < 0x60) a.b(0x90);
        a.b(0x8B); a.b(0x07);                                  // helper: mov eax,[rdi]
        a.b(0x05); a.imm32(0x1000);                            // add eax,0x1000
        a.b(0xC3);                                             // ret
        CHECK(MapCode(kCodeBase, a.code));

        const uint32_t tid = mgr.SpawnThread(kCodeBase, kDataBase, "partA", 256 * 1024, 1000000);
        CHECK(tid != 0);
        int code = -1;
        CHECK(mgr.JoinThread(tid, 5 * 1000 * 1000, &code));     // 5 s
        CHECK(code == 77);                                      // thr_exit(77)
        uint32_t sum = 0, flag = 0;
        CHECK(GuestRead(kDataBase + 12, sum));
        CHECK(sum == 42);
        CHECK(GuestRead(kDataBase + 8, flag));
        CHECK(flag == 1);
        float out[4] = {0, 0, 0, 0};
        CHECK(vmm.CopyFromGuest(kDataBase + 16, out, 16, kRw));
        CHECK(out[0] == 11.f && out[1] == 22.f && out[2] == 33.f && out[3] == 44.f);
        uint64_t fs_self = 0;
        CHECK(GuestRead(kDataBase + 64, fs_self));
        CHECK(fs_self != 0); // TLS allocator installed -> real per-thread block
    }

    std::cout << "== Part B: natural return (exit code = RAX) ==\n";
    {
        const uint64_t code_b = kCodeBase + 0x200;
        Asm a(code_b);
        a.b(0x89); a.b(0xF8);          // mov eax,edi (arg as value)
        a.b(0x83); a.b(0xC0); a.b(0x0D); // add eax,13
        a.b(0xC3);                     // ret -> exit code = arg + 13
        CHECK(MapCode(code_b, a.code));
        const uint32_t tid = mgr.SpawnThread(code_b, 100, "partB", 128 * 1024, 100000);
        CHECK(tid != 0);
        int code = -1;
        CHECK(mgr.JoinThread(tid, 5 * 1000 * 1000, &code));
        CHECK(code == 113);
    }

    std::cout << "== Part C: raw thr_new syscall end-to-end from guest code ==\n";
    {
        const uint64_t main_c = kCodeBase + 0x300;
        const uint64_t worker_c = kCodeBase + 0x400;
        // main(rdi = data): thr_new(worker, data); spin until [data+8]!=0;
        //                     result = [data+12]; ret
        Asm m(main_c);
        m.b(0xB8); m.imm32(432);                      // mov eax,432 (thr_new)
        m.b(0x48); m.b(0xBF); m.imm64(worker_c);      // mov rdi,worker
        m.b(0x48); m.b(0xBE); m.imm64(kDataBase);     // mov rsi,data
        m.b(0x0F); m.b(0x05);                         // syscall
        m.b(0x48); m.b(0x89); m.b(0xF7);             // mov rdi,rsi (restore data ptr)
        const size_t spin_at = m.code.size();
        m.b(0x8B); m.b(0x47); m.b(0x08);              // mov eax,[rdi+8]
        m.b(0x85); m.b(0xC0);                         // test eax,eax
        m.b(0x74); m.b(static_cast<uint8_t>(-7));     // jz spin (3+2+2 bytes back)
        m.b(0x8B); m.b(0x47); m.b(0x0C);              // mov eax,[rdi+12]
        m.b(0xC3);                                    // ret
        (void)spin_at;
        CHECK(MapCode(main_c, m.code));

        // worker(rdi = data): [data+12] = [data]*3 ; [data+8] = 1 ; ret
        Asm w(worker_c);
        w.b(0x8B); w.b(0x07);                         // mov eax,[rdi]
        w.b(0x6B); w.b(0xC0); w.b(0x03);              // imul eax,eax,3
        w.b(0x89); w.b(0x47); w.b(0x0C);              // mov [rdi+0xC],eax
        w.b(0xB8); w.imm32(1);                        // mov eax,1
        w.b(0x89); w.b(0x47); w.b(0x08);              // mov [rdi+8],eax
        w.b(0xB8); w.imm32(9);                        // mov eax,9 (exit code)
        w.b(0xC3);                                    // ret
        CHECK(MapCode(worker_c, w.code));

        GuestWrite<uint32_t>(kDataBase + 8, 0);
        GuestWrite<uint32_t>(kDataBase + 12, 0);

        const uint64_t ret = CPUJitEngine::Instance().ExecuteGuestFull(main_c, kDataBase, 0,
                                                                       10000000);
        CHECK(ret == 60);                             // 20*3
        // Reap the thread the guest spawned.
        bool reaped = false;
        for (int i = 0; i < 50 && !reaped; ++i) {
            for (const uint32_t tid : mgr.ThreadIds()) {
                if (mgr.JoinThread(tid, 100 * 1000)) { reaped = true; break; }
            }
        }
        CHECK(reaped);
    }

    std::cout << "== Part D: per-thread TLS isolation + parallel VMM writes ==\n";
    {
        // Install a real TLS allocator: fresh 4KB block per thread with a
        // self-pointer at offset 0 (same layout as GuestLauncher).
        // (already installed at the top of main; re-asserting the same layout)
        const uint64_t tls_slots = kDataBase + 0x200;
        const uint64_t code_d = kCodeBase + 0x500;
        // worker(rdi = slot): burn ~200k loop iterations FIRST so all four
        // threads provably overlap in time (VMM reuses freed TLS blocks, so
        // sequential runs could otherwise observe identical GVs), then store
        // fs:[0] (the thread's TLS self-pointer) into the slot and return.
        Asm w(code_d);
        w.b(0xB9); w.imm32(50000);                      // mov ecx,50000
        const size_t loop_at = w.code.size();
        w.b(0xE2); w.b(static_cast<uint8_t>(loop_at - (loop_at + 2))); // loop self
        w.b(0x64); w.b(0x48); w.b(0x8B); w.b(0x04); w.b(0x25); w.imm32(0); // mov rax,fs:[0]
        w.b(0x48); w.b(0x89); w.b(0x07);        // mov [rdi],rax
        w.b(0xC3);                              // ret
        CHECK(MapCode(code_d, w.code));

        constexpr int kThreads = 4;
        uint32_t tids[kThreads];
        for (int i = 0; i < kThreads; ++i) {
            tids[i] = mgr.SpawnThread(code_d, tls_slots + 16ull * i, "tls", 128 * 1024, 1000000);
            CHECK(tids[i] != 0);
        }
        for (int i = 0; i < kThreads; ++i) {
            CHECK(mgr.JoinThread(tids[i], 5 * 1000 * 1000));
        }
        uint64_t seen[kThreads];
        for (int i = 0; i < kThreads; ++i) {
            CHECK(GuestRead(tls_slots + 16ull * i, seen[i]));
            CHECK(seen[i] != 0);                       // real TLS block address
            for (int j = 0; j < i; ++j) {
                CHECK(seen[i] != seen[j]);             // per-thread isolation
            }
        }
    }

    std::cout << "== Part E: unwind on unsupported opcode, join/detach rules ==\n";
    {
        const uint64_t code_e = kCodeBase + 0x600;
        Asm a(code_e);
        a.b(0xB8); a.imm32(9);   // mov eax,9
        a.b(0x0F); a.b(0x0B);    // ud2 -> UnsupportedOpcode unwind
        a.b(0xC3);
        CHECK(MapCode(code_e, a.code));

        const uint32_t tid = mgr.SpawnThread(code_e, 0, "ud2", 128 * 1024, 100000);
        CHECK(tid != 0);
        int code = -1;
        CHECK(mgr.JoinThread(tid, 5 * 1000 * 1000, &code));
        CHECK(code == 9);        // rax preserved at the faulting instruction

        // Join timeout: a long spin thread does NOT finish in 50 ms.
        const uint64_t code_spin = kCodeBase + 0x700;
        Asm s(code_spin);
        const size_t spin = s.code.size();
        s.b(0x90); s.b(0xEB); s.b(static_cast<uint8_t>(spin - (spin + 3))); // jmp self
        CHECK(MapCode(code_spin, s.code));
        const uint32_t spin_tid = mgr.SpawnThread(code_spin, 0, "spin", 128 * 1024, 2000000);
        CHECK(spin_tid != 0);
        CHECK(!mgr.JoinThread(spin_tid, 50 * 1000));   // still running: timeout
        CHECK(mgr.RunningThreadCount() >= 1);

        // Detach: double-detach fails; the record disappears.
        CHECK(mgr.DetachThread(spin_tid));
        CHECK(!mgr.DetachThread(spin_tid));
        // Join refuses a detached thread.
        CHECK(!mgr.JoinThread(spin_tid, 1000));
    }

    std::cout << "== Part F: thr_self inside a guest thread ==\n";
    {
        const uint64_t code_f = kCodeBase + 0x800;
        // worker(rdi = slot): syscall thr_self (rax=433) ; mov [rdi],eax ; ret
        Asm w(code_f);
        w.b(0xB8); w.imm32(433);
        w.b(0x0F); w.b(0x05);
        w.b(0x89); w.b(0x07);
        w.b(0xC3);
        CHECK(MapCode(code_f, w.code));
        const uint64_t slot = kDataBase + 0x300;
        const uint32_t tid = mgr.SpawnThread(code_f, slot, "self", 128 * 1024, 100000);
        CHECK(tid != 0);
        CHECK(mgr.JoinThread(tid, 5 * 1000 * 1000));
        uint32_t seen_tid = 0;
        CHECK(GuestRead(slot, seen_tid));
        CHECK(seen_tid == tid);   // thr_self from inside the thread == manager tid
    }

    std::cout << "guest_threads_test: " << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
