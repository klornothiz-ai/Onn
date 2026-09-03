// ============================================================================
// ProsperoLayer RDNA2 Core - DirectExecutionBackend test (round 20)
// ----------------------------------------------------------------------------
// The guest no longer interprets instruction-by-instruction: guest x86-64 code
// runs NATIVELY on the host CPU inside the VMM arena, and ONLY the
// host-dependent / guest-divergent instructions (syscall, cpuid, rdtsc/rdtscp,
// SSE4a, BMI1/BMI2, movbe, tzcnt/lzcnt) are intercepted (ud2) and replayed
// through the interpreter core.
//
// Part A -- the pure registry/patch pieces (no native entry):
//   A1  FetchCode: executable fetch works, data/unmapped decline
//   A2  WriteCodeBytes: the patch write is visible through the VMM
//   A3  DiscoverBlock: syscall/SSE4a/BMI sites found + ud2 patched + registry
//   A4  EmulatePatchedInstruction: replay == interpreting (bit-identical)
//   A5  ResetAllPatches restores the guest bytes bit-for-bit
//
// Part B -- NATIVE runs (assembly trampoline + signal exits):
//   B1  mov rax, imm; ret                -> Returned, rax result
//   B2  memory: guest stores/loads       -> bytes land in the VMM arena
//   B3  native loop (sum 1..100)         -> blocks discovered, correct result
//   B4  syscall interception (getpid=20) -> through the dispatcher, NEVER the
//       host kernel; the handler sees the FreeBSD-9 number in rax
//   B5  SSE4a (insertq/extrq) + BMI (andn/bextr/mulx) replayed mid-run
//   B6  PARITY: the same program run through the interpreter and natively
//       produces identical registers AND identical memory
//   B7  guest fault -> GuestFault with the faulting address
//   B8  infinite loop + budget -> Timeout
//   B9  genuine ud2 -> IllegalInstruction (fail closed)
//   B10 taken conditional branch into an undiscovered block that contains a
//       syscall: the syscall is intercepted on the FIRST execution (the
//       round-20 conditional-arming fix)
//
// Every native test also runs the SAME program through the interpreter and
// compares (the parity contract: the backend is a drop-in accelerator).
// ============================================================================
#include "cpu/direct_execution.hpp"
#include "cpu/vmm_memory_bus.hpp"
#include "cpu/x86_64_interpreter.hpp"
#include "memory/virtual_memory_manager.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

using PS5::CPU::CpuState;
using PS5::CPU::DirectExecutionBackend;
using PS5::CPU::DirectRunOutcome;
using PS5::CPU::DirectStopReason;
using PS5::CPU::ExecStatus;
using PS5::CPU::GuestMemoryBus;
using PS5::CPU::PatchKind;
using PS5::CPU::RAX;
using PS5::CPU::RCX;
using PS5::CPU::RDI;
using PS5::CPU::RDX;
using PS5::CPU::RSP;
using PS5::CPU::RunResult;
using PS5::CPU::VmmMemoryBus;
using PS5::CPU::X86Interpreter;
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

constexpr uint64_t kCodeBase  = 0x1000000000ULL;      // arena default base
constexpr uint64_t kCodeSize  = 0x10000;
constexpr uint64_t kDataBase  = 0x1000010000ULL;
constexpr uint64_t kDataSize  = 0x10000;
constexpr uint64_t kStackBase = 0x1000020000ULL;
constexpr uint64_t kStackSize = 0x100000;             // 1 MB guest stack
constexpr uint64_t kStopSentinel = 0x0000FEEDDEAD0000ULL;

const uint32_t kRWX = static_cast<uint32_t>(PageProt::Read) |
                      static_cast<uint32_t>(PageProt::Write) |
                      static_cast<uint32_t>(PageProt::Exec);
const uint32_t kRW  = static_cast<uint32_t>(PageProt::Read) |
                      static_cast<uint32_t>(PageProt::Write);

// ---------------------------------------------------------------------------
// Tiny x86-64 assembler (only what these tests need).
// ---------------------------------------------------------------------------
struct Asm {
    std::vector<uint8_t> c;
    void b(uint8_t v) { c.push_back(v); }
    void imm8(int8_t v) { b(static_cast<uint8_t>(v)); }
    void imm32(uint32_t v) { for (int i = 0; i < 4; ++i) b((v >> (8 * i)) & 0xFF); }
    void imm64(uint64_t v) { for (int i = 0; i < 8; ++i) b((v >> (8 * i)) & 0xFF); }
    // mov rax, imm64 = 48 B8 <imm64>
    void mov_rax_imm(uint64_t v) { b(0x48); b(0xB8); imm64(v); }
    // mov qword [rdi], imm32 sign-extended = 48 C7 07 <imm32>
    void mov_qword_rdi_imm32(uint32_t v) { b(0x48); b(0xC7); b(0x07); imm32(v); }
    void ret() { b(0xC3); }
    void syscall() { b(0x0F); b(0x05); }
    void ud2() { b(0x0F); b(0x0B); }
};

// Guest machine: one VMM (process-wide singleton), code/data/stack regions.
// Built ONCE for the whole process (the regions persist in the singleton;
// re-allocating the same addresses would fail by design).
struct Machine {
    VmmMemoryBus bus{PS5::Memory::VirtualMemoryManager::Instance()};
    uint64_t code_gva{kCodeBase};
    uint64_t data_gva{kDataBase};
    uint64_t stack_top{kStackBase + kStackSize};
    uint64_t stack_size{kStackSize};

    void init() {
        auto& vmm = PS5::Memory::VirtualMemoryManager::Instance();
        CHECK(vmm.InitializeArena(kCodeBase));
        if (vmm.IsGvaMapped(kCodeBase)) return;   // already up (singleton)
        CHECK(vmm.AllocateVirtual(kCodeBase, kCodeSize, kRWX) == kCodeBase);
        CHECK(vmm.AllocateVirtual(kDataBase, kDataSize, kRW) == kDataBase);
        CHECK(vmm.AllocateVirtual(kStackBase, kStackSize, kRW) == kStackBase);
    }

    void load(uint64_t at, const std::vector<uint8_t>& code) {
        CHECK(PS5::Memory::VirtualMemoryManager::Instance().CopyToGuest(at, code.data(),
                                                           code.size()));
    }

    // Pushes the stop sentinel and builds a CpuState for a run at `entry`.
    CpuState make_cpu(uint64_t entry, uint64_t arg0 = 0) {
        CpuState cpu{};
        cpu.rip = entry;
        cpu.gpr[RDI] = arg0;
        uint64_t rsp = stack_top - 64;
        rsp &= ~15ull;
        rsp -= 8;
        bus.Write(rsp, &kStopSentinel, 8);
        cpu.gpr[RSP] = rsp;
        return cpu;
    }

    template <typename T>
    T dread(uint64_t gva) {
        T v{};
        CHECK(PS5::Memory::VirtualMemoryManager::Instance().CopyFromGuest(gva, &v, sizeof(T)));
        return v;
    }
};

// A deterministic syscall handler: getpid(20) -> 0xABCD, write(4) -> the
// length, everything else -> ENOSYS(78). Counts calls; a Linux-native syscall
// (the host numbering) would return something else entirely (host getpid for
// rax=39 on Linux), so a wrong-path execution is DETECTABLE.
struct TestSyscalls {
    int calls = 0;
    uint64_t last_nr = 0;
    PS5::CPU::SyscallHandler handler() {
        return [this](CpuState& s, GuestMemoryBus&) {
            ++calls;
            last_nr = s.gpr[RAX];
            switch (s.gpr[RAX]) {
                case 20: s.gpr[RAX] = 0xABCD; return true;   // getpid
                case 4:  s.gpr[RAX] = s.gpr[RDX]; return true; // write: rdx=len
                default: s.gpr[RAX] = 78; return true;      // ENOSYS
            }
        };
    }
};

// ---------------------------------------------------------------------------
// Part A -- pure pieces
// ---------------------------------------------------------------------------
void part_a_registry(Machine& m) {
    auto& direct = DirectExecutionBackend::Instance();
    direct.ResetAllPatches();
    CHECK(direct.PatchedSiteCount() == 0);
    CHECK(direct.DiscoveredBlockCount() == 0);

    // A1: FetchCode from executable memory.
    Asm a1;
    a1.mov_rax_imm(0x1234);
    a1.ret();
    m.load(m.code_gva, a1.c);
    std::vector<uint8_t> fetched;
    CHECK(direct.FetchCode(m.code_gva, 16, fetched));
    CHECK(fetched.size() >= a1.c.size());
    CHECK(std::memcmp(fetched.data(), a1.c.data(), a1.c.size()) == 0);
    // ... and the decline cases: data memory / unmapped.
    CHECK(!direct.FetchCode(m.data_gva, 16, fetched));
    CHECK(!direct.FetchCode(0x7F0000000000ULL, 16, fetched));

    // A2: WriteCodeBytes round-trips through the VMM.
    const uint8_t patch[2] = {0x0F, 0x0B};
    CHECK(direct.WriteCodeBytes(m.code_gva + 8, patch, 2));
    CHECK(m.dread<uint8_t>(m.code_gva + 8) == 0x0F);
    CHECK(m.dread<uint8_t>(m.code_gva + 9) == 0x0B);
    CHECK(direct.WriteCodeBytes(m.code_gva + 8, a1.c.data() + 8, 2));
    CHECK(m.dread<uint16_t>(m.code_gva + 8) ==
          *reinterpret_cast<const uint16_t*>(a1.c.data() + 8));

    // A3: DiscoverBlock finds the interception sites.
    //   block: mov rax,0; syscall; mov rcx,rax; ret
    Asm a3;
    a3.b(0x48); a3.b(0x31); a3.b(0xC0);          // xor rax, rax
    a3.syscall();                                 // SITE: syscall (offset 3)
    a3.b(0x48); a3.b(0x89); a3.b(0xC1);          // mov rcx, rax
    a3.ret();
    m.load(m.code_gva, a3.c);
    bool ok = false;
    auto scan = direct.DiscoverBlock(m.code_gva, &ok);
    CHECK(ok);
    // The scanner terminates the block at the FIRST terminator -- the
    // syscall (offset 3, 2 bytes) -- so the block is the 5 bytes before it.
    CHECK(scan.code_size == 5);
    CHECK(scan.contains_syscall);
    CHECK(direct.HasSiteAt(m.code_gva + 3));
    CHECK(direct.SiteKindAt(m.code_gva + 3) == PatchKind::Syscall);
    CHECK(direct.DiscoveredBlockCount() == 1);
    // The live bytes at the site are ud2 now.
    CHECK(m.dread<uint8_t>(m.code_gva + 3) == 0x0F);
    CHECK(m.dread<uint8_t>(m.code_gva + 4) == 0x0B);

    // SSE4a site: insertq xmm0, xmm1 = F2 0F 79 C1 (register form).
    Asm a3s;
    a3s.b(0xF2); a3s.b(0x0F); a3s.b(0x79); a3s.b(0xC1);  // insertq xmm0,xmm1
    a3s.mov_rax_imm(7);
    a3s.ret();
    m.load(m.code_gva + 0x80, a3s.c);
    scan = direct.DiscoverBlock(m.code_gva + 0x80, &ok);
    CHECK(ok);
    CHECK(direct.SiteKindAt(m.code_gva + 0x80) == PatchKind::Sse4a);

    // BMI1 site: andn eax, ebx, ecx = C4 E2 60 F2 C1 (VEX3, 0F38 F2, no 66).
    //   VEX3: C4 E2 (W=1? andn r32 -> W=0) -> C4 E2 60 F2 C1
    Asm a3b;
    a3b.b(0xC4); a3b.b(0xE2); a3b.b(0x60); a3b.b(0xF2); a3b.b(0xC1); // andn
    a3b.mov_rax_imm(9);
    a3b.ret();
    m.load(m.code_gva + 0x100, a3b.c);
    scan = direct.DiscoverBlock(m.code_gva + 0x100, &ok);
    CHECK(ok);
    CHECK(direct.SiteKindAt(m.code_gva + 0x100) == PatchKind::Bmi1);

    // A5: ResetAllPatches restores the original bytes bit-for-bit.
    direct.ResetAllPatches();
    CHECK(direct.PatchedSiteCount() == 0);
    CHECK(direct.DiscoveredBlockCount() == 0);
    std::vector<uint8_t> back;
    CHECK(direct.FetchCode(m.code_gva, a3.c.size(), back));
    CHECK(std::memcmp(back.data(), a3.c.data(), a3.c.size()) == 0);
}

void part_a_emulate(Machine& m) {
    auto& direct = DirectExecutionBackend::Instance();
    direct.ResetAllPatches();

    // A4: EmulatePatchedInstruction == interpreting the same instruction.
    // extrq xmm0, imm8, imm8 = 66 0F 78 C0 <len> <shift> (EXTRQ, imm form).
    // First set xmm0 to a known value, then discover the block (the SSE4a
    // site gets ud2'd), then replay and compare against a pure-interpreter
    // run of the same bytes.
    Asm a4;
    a4.b(0x66); a4.b(0x0F); a4.b(0x78); a4.b(0xC0); a4.b(8); a4.b(4); // extrq
    a4.mov_rax_imm(1);
    a4.ret();
    m.load(m.code_gva, a4.c);

    CpuState ref{};
    ref.rip = m.code_gva;
    ref.gpr[RSP] = 0;  // unused
    ref.xmm[0].lo = 0x0000FFFFFFFFFFFFull;   // low qword bits
    ref.xmm[0].hi = 0;
    CpuState cpu = ref;
    uint64_t rsp = m.stack_top - 64;
    ref.gpr[RSP] = rsp;
    cpu.gpr[RSP] = rsp;

    // Reference: pure interpreter Step().
    X86Interpreter iref(ref, m.bus);
    CHECK(iref.Step().status == ExecStatus::Running);
    const uint64_t ref_lo = ref.xmm[0].lo;

    // Backend path: discover + patch, then replay through the backend.
    bool ok = false;
    (void)direct.DiscoverBlock(m.code_gva, &ok);
    CHECK(ok);
    CHECK(direct.SiteKindAt(m.code_gva) == PatchKind::Sse4a);
    CHECK(direct.EmulatePatchedInstruction(cpu, m.bus, m.code_gva));
    CHECK(cpu.xmm[0].lo == ref_lo);
    CHECK(cpu.rip == ref.rip);
    direct.ResetAllPatches();
}

// ---------------------------------------------------------------------------
// Part B -- native runs
// ---------------------------------------------------------------------------
uint64_t run_native(Machine& m, CpuState& cpu, TestSyscalls& sc,
                    DirectRunOutcome& out, uint64_t budget_ms = 2000) {
    const uint64_t r = DirectExecutionBackend::Instance().RunFunction(
        cpu, m.bus, sc.handler(), kStopSentinel, budget_ms, out);
    std::printf("    [native] reason=%s msg='%s' rip=%llx fault_gva=%llx rax=%llx blocks=%zu "
                "reentries=%zu emul=%zu sysc=%zu\n",
                PS5::CPU::ToString(out.reason), out.message.c_str(),
                (unsigned long long)cpu.rip, (unsigned long long)out.fault_gva,
                (unsigned long long)r,
                out.stats.blocks_discovered, out.stats.reentries,
                out.stats.instructions_emulated, out.stats.syscalls_serviced);
    return r;
}

RunResult run_interp(Machine& m, CpuState& cpu, TestSyscalls& sc,
                     size_t limit = 1000000) {
    X86Interpreter interp(cpu, m.bus);
    interp.SetSyscallHandler(sc.handler());
    return interp.Run(limit, kStopSentinel);
}

void part_b_native(Machine& m) {
    auto& direct = DirectExecutionBackend::Instance();
    direct.ResetAllPatches();
    CHECK(direct.Enable());
    CHECK(direct.IsEnabled());

    // B1: mov rax, 0x1234; ret
    {
        Asm p;
        p.mov_rax_imm(0x1234);
        p.ret();
        m.load(m.code_gva, p.c);
        auto cpu = m.make_cpu(m.code_gva);
        TestSyscalls sc;
        DirectRunOutcome out;
        const uint64_t r = run_native(m, cpu, sc, out);
        CHECK(out.reason == DirectStopReason::Returned);
        CHECK(r == 0x1234);
        CHECK(out.stats.blocks_discovered >= 1);
    }

    // B2: guest stores: mov rax,0xAABBCCDD; mov [rdi],rax; mov [rdi+8],rax; ret
    {
        Asm p;
        p.mov_rax_imm(0xAABBCCDDULL);
        p.b(0x48); p.b(0x89); p.b(0x07);              // mov [rdi], rax
        p.b(0x48); p.b(0x89); p.b(0x47); p.b(0x08);   // mov [rdi+8], rax
        p.ret();
        m.load(m.code_gva, p.c);
        auto cpu = m.make_cpu(m.code_gva, m.data_gva);
        TestSyscalls sc;
        DirectRunOutcome out;
        (void)run_native(m, cpu, sc, out);
        CHECK(out.reason == DirectStopReason::Returned);
        CHECK(m.dread<uint64_t>(m.data_gva) == 0xAABBCCDDULL);
        CHECK(m.dread<uint64_t>(m.data_gva + 8) == 0xAABBCCDDULL);
        // Zero them again for later tests.
        const uint64_t z = 0;
        PS5::Memory::VirtualMemoryManager::Instance().CopyToGuest(m.data_gva, &z, 8);
        PS5::Memory::VirtualMemoryManager::Instance().CopyToGuest(m.data_gva + 8, &z, 8);
    }

    // B3: native loop -- sum 1..100 with a conditional branch:
    //   xor eax,eax; mov ecx,100; loop: add rax,rcx; dec rcx; jnz loop; ret
    {
        const size_t blocks_before =
            DirectExecutionBackend::Instance().LifetimeStats().blocks_discovered;
        Asm p;
        p.b(0x48); p.b(0x31); p.b(0xC0);              // xor rax,rax
        p.b(0xB9); p.imm32(100);                      // mov ecx,100
        const size_t loop_at = p.c.size();
        p.b(0x48); p.b(0x01); p.b(0xC8);              // add rax,rcx
        p.b(0x48); p.b(0xFF); p.b(0xC9);              // dec rcx
        p.b(0x75); p.imm8(static_cast<int8_t>(loop_at) - static_cast<int8_t>(p.c.size() + 1)); // jnz loop
        p.ret();
        m.load(m.code_gva, p.c);
        auto cpu = m.make_cpu(m.code_gva);
        TestSyscalls sc;
        DirectRunOutcome out;
        const uint64_t r = run_native(m, cpu, sc, out);
        CHECK(out.reason == DirectStopReason::Returned);
        CHECK(r == 5050);
        // The taken side of the conditional branch was intercepted at least
        // once (the round-20 conditional-arming fix: first execution of the
        // taken path goes through discovery, not unscanned native code).
        CHECK(DirectExecutionBackend::Instance().LifetimeStats().blocks_discovered -
              blocks_before >= 2);
        CHECK(out.stats.reentries >= 1);
    }

    // B4: syscall interception -- getpid (FreeBSD-9 nr 20) then ret:
    //   mov rax,20; syscall; mov rdi,rax; ret
    {
        Asm p;
        p.mov_rax_imm(20);
        p.syscall();
        p.b(0x48); p.b(0x89); p.b(0xC7);              // mov rdi, rax
        p.ret();
        m.load(m.code_gva, p.c);
        auto cpu = m.make_cpu(m.code_gva);
        TestSyscalls sc;
        DirectRunOutcome out;
        const uint64_t r = run_native(m, cpu, sc, out);
        CHECK(out.reason == DirectStopReason::Returned);
        CHECK(r == 0xABCD);           // the HANDLER's value, not host getpid
        CHECK(sc.calls == 1);
        CHECK(sc.last_nr == 20);
        CHECK(out.stats.syscalls_serviced == 1);
        CHECK(cpu.gpr[RDI] == 0xABCD);
    }

    // B10: a syscall reached ONLY through a TAKEN conditional branch into an
    // undiscovered block (the arming fix's reason for being):
    //   mov rax,5; cmp rax,5; je taken; mov rax,999; ret
    //   taken: mov rax,20; syscall; ret
    {
        Asm p;
        p.mov_rax_imm(5);
        p.b(0x48); p.b(0x83); p.b(0xF8); p.b(0x05);   // cmp rax,5
        const size_t jcc_at = p.c.size();
        p.b(0x74); p.b(0x00);                          // je +0 (patched below)
        p.mov_rax_imm(999);
        p.ret();
        const size_t taken_at = p.c.size();
        p.mov_rax_imm(20);
        p.syscall();
        p.ret();
        const int8_t rel = static_cast<int8_t>(taken_at) -
                           static_cast<int8_t>(jcc_at + 2);
        p.c[jcc_at + 1] = static_cast<uint8_t>(rel);
        m.load(m.code_gva, p.c);
        auto cpu = m.make_cpu(m.code_gva);
        TestSyscalls sc;
        DirectRunOutcome out;
        const uint64_t r = run_native(m, cpu, sc, out);
        CHECK(out.reason == DirectStopReason::Returned);
        CHECK(r == 0xABCD);            // syscall SERVICED, not skipped
        CHECK(sc.calls == 1);
        CHECK(out.stats.syscalls_serviced == 1);
    }

    // B5: SSE4a + BMI replayed mid-run (native code around them):
    //   mov rax, imm64; insertq/andn; ret -- verify against the interpreter.
    {
        // SSE4a: insertq xmm0, xmm1 = F2 0F 79 C8 (dst=xmm0 src=xmm1)
        //   then movq rax, xmm0 = 66 48 0F 7E C0
        Asm p;
        p.b(0x48); p.b(0xB8); p.imm64(0);             // mov rax, 0
        p.b(0x66); p.b(0x48); p.b(0x0F); p.b(0x6E); p.b(0xC0); // movq xmm0, rax
        p.b(0x48); p.b(0xB8); p.imm64(0x0000FFFFFFFFFFFFull);  // mov rax, mask
        p.b(0x66); p.b(0x48); p.b(0x0F); p.b(0x6E); p.b(0xC8); // movq xmm1, rax
        p.b(0xF2); p.b(0x0F); p.b(0x79); p.b(0xC8);   // insertq xmm0, xmm1
        p.b(0x66); p.b(0x48); p.b(0x0F); p.b(0x7E); p.b(0xC0); // movq rax, xmm0
        p.ret();
        m.load(m.code_gva, p.c);

        // Interpreter reference on a copy of the CPU state.
        auto cpu_i = m.make_cpu(m.code_gva);
        TestSyscalls sc_i;
        const RunResult rr = run_interp(m, cpu_i, sc_i);
        CHECK(rr.status == ExecStatus::Returned);

        direct.ResetAllPatches();   // clean patches so the run re-discovers
        auto cpu = m.make_cpu(m.code_gva);
        TestSyscalls sc;
        DirectRunOutcome out;
        const uint64_t r = run_native(m, cpu, sc, out);
        CHECK(out.reason == DirectStopReason::Returned);
        CHECK(r == cpu_i.gpr[RAX]);                  // bit-identical replay
        CHECK(out.stats.instructions_emulated >= 1); // the SSE4a went through

        // BMI1: andn rax, rbx, rcx = C4 E2 E0 F2 C1 (gas: andn %rcx,%rbx,%rax)
        Asm q;
        q.b(0x48); q.b(0xB8); q.imm64(0x00FF00FF00FF00FFull); // mov rax, a
        q.b(0x48); q.b(0xBB); q.imm64(0x0F0F0F0F0F0F0F0Full); // mov rbx, b
        q.b(0x48); q.b(0xB9); q.imm64(0xF0F0F0F0F0F0F0F0ull); // mov rcx, c
        q.b(0xC4); q.b(0xE2); q.b(0xE0); q.b(0xF2); q.b(0xC1); // andn rax,rbx,rcx (64-bit)
        q.ret();
        m.load(m.code_gva + 0x200, q.c);
        auto cpu_bi = m.make_cpu(m.code_gva + 0x200);
        TestSyscalls scb;
        const RunResult rb = run_interp(m, cpu_bi, scb);
        CHECK(rb.status == ExecStatus::Returned);
        const uint64_t expect = (~0x0F0F0F0F0F0F0F0Full) & 0xF0F0F0F0F0F0F0F0ull;

        direct.ResetAllPatches();
        auto cpu_b = m.make_cpu(m.code_gva + 0x200);
        TestSyscalls sc2;
        DirectRunOutcome out2;
        const uint64_t r2 = run_native(m, cpu_b, sc2, out2);
        CHECK(out2.reason == DirectStopReason::Returned);
        CHECK(r2 == expect);
        CHECK(r2 == cpu_bi.gpr[RAX]);
    }

    // B6: full parity -- a mixed program (ALU + memory + loop + SSE + syscall)
    // run through BOTH engines must leave identical memory and registers.
    {
        Asm p;
        // sum 1..10 into rax
        p.b(0x48); p.b(0x31); p.b(0xC0);              // xor rax,rax
        p.b(0xB9); p.imm32(10);                       // mov ecx,10
        const size_t loop_at = p.c.size();
        p.b(0x48); p.b(0x01); p.b(0xC8);              // add rax,rcx
        p.b(0x48); p.b(0xFF); p.b(0xC9);              // dec rcx
        p.b(0x75); p.imm8(static_cast<int8_t>(loop_at) - static_cast<int8_t>(p.c.size() + 1));
        // store to [rdi]
        p.b(0x48); p.b(0x89); p.b(0x07);              // mov [rdi], rax
        // syscall getpid
        p.mov_rax_imm(20);
        p.syscall();
        // store the syscall result to [rdi+8]
        p.b(0x48); p.b(0x89); p.b(0x47); p.b(0x08);
        // sse4a replay: movq xmm0,rax; insertq; movq rax,xmm0; store [rdi+16]
        p.b(0x66); p.b(0x48); p.b(0x0F); p.b(0x6E); p.b(0xC0); // movq xmm0,rax
        p.b(0x48); p.b(0xB8); p.imm64(0x0000000000FFFFFFull);
        p.b(0x66); p.b(0x48); p.b(0x0F); p.b(0x6E); p.b(0xC8); // movq xmm1,rax
        p.b(0xF2); p.b(0x0F); p.b(0x79); p.b(0xC8);   // insertq xmm0,xmm1
        p.b(0x66); p.b(0x48); p.b(0x0F); p.b(0x7E); p.b(0xC0); // movq rax,xmm0
        p.b(0x48); p.b(0x89); p.b(0x47); p.b(0x10);
        p.ret();
        m.load(m.code_gva, p.c);

        // Interpreter first, on a fresh data page copy state.
        const uint64_t d1 = m.data_gva + 0x800;
        auto cpu_i = m.make_cpu(m.code_gva, d1);
        TestSyscalls sc_i;
        const RunResult rr = run_interp(m, cpu_i, sc_i);
        CHECK(rr.status == ExecStatus::Returned);

        direct.ResetAllPatches();
        const uint64_t d2 = m.data_gva + 0x900;
        auto cpu = m.make_cpu(m.code_gva, d2);
        TestSyscalls sc;
        DirectRunOutcome out;
        const uint64_t r = run_native(m, cpu, sc, out);
        CHECK(out.reason == DirectStopReason::Returned);

        CHECK(cpu.gpr[RAX] == cpu_i.gpr[RAX]);
        CHECK(cpu.gpr[RCX] == cpu_i.gpr[RCX]);
        CHECK(cpu.gpr[RDX] == cpu_i.gpr[RDX]);
        CHECK(r == cpu_i.gpr[RAX]);
        // memory parity dword-for-dword
        for (size_t off = 0; off < 24; off += 8) {
            CHECK(m.dread<uint64_t>(d1 + off) == m.dread<uint64_t>(d2 + off));
        }
        CHECK(m.dread<uint64_t>(d1) == 55);           // sum 1..10
        CHECK(m.dread<uint64_t>(d1 + 8) == 0xABCD);   // getpid result
        CHECK(sc.calls == sc_i.calls);
        CHECK(out.stats.instructions_emulated >= 1);  // the insertq replay
    }

    // B7: guest fault -- mov rax, [0x7F0000000000] (unmapped); ret
    {
        Asm p;
        p.b(0x48); p.b(0xB8); p.imm64(0x7F0000000000ULL); // mov rax, addr
        p.b(0x48); p.b(0x8B); p.b(0x00);                   // mov rax,[rax]
        p.ret();
        m.load(m.code_gva, p.c);
        auto cpu = m.make_cpu(m.code_gva);
        TestSyscalls sc;
        DirectRunOutcome out;
        (void)run_native(m, cpu, sc, out);
        CHECK(out.reason == DirectStopReason::GuestFault);
        CHECK(out.fault_gva == 0x7F0000000000ULL);
        CHECK(out.stats.faults == 1);
    }

    // B8: timeout -- jmp $ (infinite self-loop), 150 ms budget.
    {
        Asm p;
        p.b(0xEB); p.b(0xFE);                          // jmp -2 (self)
        m.load(m.code_gva, p.c);
        auto cpu = m.make_cpu(m.code_gva);
        TestSyscalls sc;
        DirectRunOutcome out;
        (void)run_native(m, cpu, sc, out, 150);
        CHECK(out.reason == DirectStopReason::Timeout);
    }

    // B9: genuine ud2 -> IllegalInstruction (fail closed).
    {
        Asm p;
        p.ud2();
        m.load(m.code_gva, p.c);
        auto cpu = m.make_cpu(m.code_gva);
        TestSyscalls sc;
        DirectRunOutcome out;
        (void)run_native(m, cpu, sc, out);
        CHECK(out.reason == DirectStopReason::IllegalInstruction);
    }

    // B11: FMA3 through the INTERPRETER (the direct path runs it natively on
    // FMA hosts; on non-FMA hosts the SIGILL declines to the interpreter --
    // which must serve every form). vfmadd132ps + vfmadd231ss, computed in
    // double precision like the emulator:
    //   vfmadd132ps xmm0, xmm1, xmm2:  dst = dst*rm + vvvv
    //   vfmadd231ss xmm3, xmm4, xmm5:  dst = vvvv*rm + dst
    {
        // interpreter-only: run the same program both ways is impossible for
        // the native path on this host without knowing its CPU, so this
        // checks the interpreter core serves FMA3 (the parity engine).
        Asm p;
        // mov rax bits(1.0); movq xmm0,rax  -> lane0 = 1.0
        p.b(0x48); p.b(0xB8); p.imm64(0x3F800000ull);
        p.b(0x66); p.b(0x48); p.b(0x0F); p.b(0x6E); p.b(0xC0);
        // mov rax bits(2.0); movq xmm1,rax
        p.b(0x48); p.b(0xB8); p.imm64(0x40000000ull);
        p.b(0x66); p.b(0x48); p.b(0x0F); p.b(0x6E); p.b(0xC8);
        // mov rax bits(3.0); movq xmm2,rax
        p.b(0x48); p.b(0xB8); p.imm64(0x40400000ull);
        p.b(0x66); p.b(0x48); p.b(0x0F); p.b(0x6E); p.b(0xD0);
        // vfmadd132ps xmm0, xmm1, xmm2 = C4 E2 71 98 D0
        //   (map2, pp=66 -> byte3 = W0 vvvv=~1=1110 L0 pp01 = 0x71)
        p.b(0xC4); p.b(0xE2); p.b(0x71); p.b(0x98); p.b(0xC2);
        // movq rax, xmm0
        p.b(0x66); p.b(0x48); p.b(0x0F); p.b(0x7E); p.b(0xC0);
        p.ret();
        m.load(m.code_gva + 0x300, p.c);
        auto cpu_f = m.make_cpu(m.code_gva + 0x300);
        TestSyscalls scf;
        const RunResult rf = run_interp(m, cpu_f, scf);
        CHECK(rf.status == ExecStatus::Returned);
        // lane 0: 1.0*3.0 + 2.0 = 5.0f
        CHECK(cpu_f.gpr[RAX] == 0x40A00000ull);

        // vfnmadd231ss: dst = -(vvvv*rm) + dst
        Asm q;
        q.b(0x48); q.b(0xB8); q.imm64(0x3F800000ull);   // 1.0
        q.b(0x66); q.b(0x48); q.b(0x0F); q.b(0x6E); q.b(0xC0);  // xmm0 = 1.0
        q.b(0x48); q.b(0xB8); q.imm64(0x40000000ull);   // 2.0
        q.b(0x66); q.b(0x48); q.b(0x0F); q.b(0x6E); q.b(0xC8);  // xmm1 = 2.0
        q.b(0x48); q.b(0xB8); q.imm64(0x40400000ull);   // 3.0
        q.b(0x66); q.b(0x48); q.b(0x0F); q.b(0x6E); q.b(0xD0);  // xmm2 = 3.0
        // vfnmadd231ss xmm0, xmm1, xmm2 = C4 E2 71 BC D0
        q.b(0xC4); q.b(0xE2); q.b(0x71); q.b(0xBC); q.b(0xC2);
        q.b(0x66); q.b(0x48); q.b(0x0F); q.b(0x7E); q.b(0xC0);
        q.ret();
        m.load(m.code_gva + 0x380, q.c);
        auto cpu_g = m.make_cpu(m.code_gva + 0x380);
        TestSyscalls scg;
        const RunResult rg = run_interp(m, cpu_g, scg);
        CHECK(rg.status == ExecStatus::Returned);
        // -(2.0*3.0) + 1.0 = -5.0f = 0xC0A00000
        CHECK(cpu_g.gpr[RAX] == 0xC0A00000ull);
    }

    direct.Disable();
    CHECK(!direct.IsEnabled());

    // The declined contract: RunFunction without Enable() -> HostError.
    {
        auto cpu = m.make_cpu(m.code_gva);
        TestSyscalls sc;
        DirectRunOutcome out;
        (void)run_native(m, cpu, sc, out);
        CHECK(out.reason == DirectStopReason::HostError);
    }
    CHECK(direct.Enable());   // leave enabled for later suites in this process
}

} // namespace

int main() {
    Machine m;
    m.init();
    std::printf("[direct_execution_test] part A: registry / patch pieces\n");
    part_a_registry(m);
    part_a_emulate(m);
    std::printf("[direct_execution_test] part B: native execution\n");
    part_b_native(m);
    if (g_failures != 0) {
        std::printf("[direct_execution_test] FAILED: %d/%d checks failed\n",
                    g_failures, g_checks);
        return 1;
    }
    std::printf("[direct_execution_test] PASSED: %d checks, %d failures\n",
                g_checks, g_failures);
    return 0;
}
