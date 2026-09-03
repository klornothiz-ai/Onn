// ============================================================================
// ProsperoLayer RDNA2 Core - fork/wait4/pid test (round 18)
// ----------------------------------------------------------------------------
// Proves the REAL process model replaces the round-17 honest refusals:
//
//   * fork() snapshots the address space and the caller's CPU state; the
//     child resumes after the syscall with rax=0, the parent gets the pid,
//   * memory ISOLATION: a store the child makes to a page it inherited is
//     invisible to the parent,
//   * wait4() blocks until the child exits, returns the pid and writes the
//     FreeBSD wait status (exit code << 8),
//   * the child's getpid()/getppid() report the child pid / the main pid
//     (proven through the exit codes the children compute),
//   * WNOHANG over a fully reaped child set reports ECHILD,
//   * execve still refuses honestly (EPERM).
//
// The guest program encodes every check into bits of the parent's exit code
// (0x1F = all five passed); the child-side effects are observed via wait4.
// ============================================================================
#include "cpu/x86_64_interpreter.hpp"
#include "cpu/prospero_syscalls.hpp"
#include "cpu/fork_process.hpp"
#include "cpu/vmm_memory_bus.hpp"
#include "memory/virtual_memory_manager.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

using namespace PS5::CPU;
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

constexpr uint64_t kCodeGva = 0x1000001000;
constexpr uint64_t kDataGva = 0x1000002000;
constexpr uint64_t kStackGva = 0x1000003000;
constexpr uint64_t kStopSentinel = 0x0000FEEDDEAD0000ULL;

constexpr uint64_t g_witness = kDataGva + 0;     // dword the child overwrites
constexpr uint64_t g_status_a = kDataGva + 8;    // wait4 status slot (child A)
constexpr uint64_t g_status_b = kDataGva + 16;   // wait4 status slot (child B)

// Label-based assembler: two-pass (labels recorded, rel32s patched).
struct Asm {
    std::vector<uint8_t> code;
    uint64_t base = kCodeGva;
    struct Rel { size_t off; uint64_t next_insn; std::string label; };
    std::vector<Rel> rels;
    struct LabelDef { std::string name; uint64_t gva; };
    std::vector<LabelDef> labels;

    void b(uint8_t v) { code.push_back(v); }
    void imm32(uint32_t v) { for (int i = 0; i < 4; ++i) b((v >> (8 * i)) & 0xFF); }
    void imm64(uint64_t v) { for (int i = 0; i < 8; ++i) b((v >> (8 * i)) & 0xFF); }
    uint64_t here() const { return base + code.size(); }
    void label(const std::string& name) { labels.push_back({name, here()}); }
    uint64_t label_addr(const std::string& name) const {
        for (const auto& l : labels) {
            if (l.name == name) return l.gva;
        }
        return 0;
    }
    void rel32(const std::string& target) {
        const size_t off = code.size();
        for (int i = 0; i < 4; ++i) b(0);
        rels.push_back({off, base + code.size(), target});
    }
    void rip32(uint64_t target) {
        const size_t off = code.size();
        for (int i = 0; i < 4; ++i) b(0);
        // rip = address after the disp32
        rels.push_back({off, base + code.size(), std::string()});
        labels.push_back({"__rip_target_" + std::to_string(off), target});
        // store the absolute target in a side map via a marker: reuse rels
        // with the label carrying the raw target
        rels.back().label = "ABS:" + std::to_string(target);
    }
    void patch() {
        for (const auto& r : rels) {
            uint64_t target = 0;
            if (r.label.compare(0, 4, "ABS:") == 0) {
                target = std::strtoull(r.label.c_str() + 4, nullptr, 10);
            } else {
                target = label_addr(r.label);
            }
            const int64_t disp = static_cast<int64_t>(target) -
                                 static_cast<int64_t>(r.next_insn);
            const uint32_t bits = static_cast<uint32_t>(disp);
            for (int i = 0; i < 4; ++i) code[r.off + i] = (bits >> (8 * i)) & 0xFF;
        }
    }
};

} // namespace

int main() {
    std::cout << "[fork] round 18: real fork / wait4 / pids\n";
    auto& vmm = VirtualMemoryManager::Instance();
    const uint32_t rw = static_cast<uint32_t>(PageProt::Read) |
                        static_cast<uint32_t>(PageProt::Write);
    const uint32_t rx = static_cast<uint32_t>(PageProt::Read) |
                        static_cast<uint32_t>(PageProt::Exec);

    // Code pages are mapped RW first (so the host can copy in), then
    // protected RX -- the same two-step GuestLauncher uses.
    CHECK(vmm.AllocateVirtual(kCodeGva, 0x1000, rw) != 0);
    CHECK(vmm.AllocateVirtual(kDataGva, 0x1000, rw) != 0);
    CHECK(vmm.AllocateVirtual(kStackGva, 0x2000, rw) != 0);

    Asm a;
    // ---- fork A ----
    a.b(0xB8); a.imm32(2);                     // mov eax, SYS_fork
    a.b(0x0F); a.b(0x05);                      // syscall
    a.b(0x48); a.b(0x85); a.b(0xC0);           // test rax, rax
    a.b(0x0F); a.b(0x85); a.rel32("parent_a");  // jne parent_a
    // ---- child A ----
    a.b(0xC7); a.b(0x05); a.rip32(g_witness); a.imm32(0x00C0DE00u); // write!
    a.b(0xB8); a.imm32(20);                    // mov eax, SYS_getpid
    a.b(0x0F); a.b(0x05);                      // syscall
    a.b(0xC1); a.b(0xE8); a.b(8);              // shr eax, 8
    a.b(0x25); a.imm32(0xFF);                  // and eax, 0xFF
    a.b(0x89); a.b(0xC7);                      // mov edi, eax
    a.b(0xB8); a.imm32(1);                     // mov eax, SYS_exit
    a.b(0x0F); a.b(0x05);                      // syscall
    // ---- parent A ----
    a.label("parent_a");
    a.b(0x49); a.b(0x89); a.b(0xC7);           // mov r15, rax (child pid)
    a.b(0xB8); a.imm32(7);                     // mov eax, SYS_wait4
    a.b(0xBF); a.imm32(0xFFFFFFFF);            // mov edi, -1
    a.b(0x48); a.b(0xBE); a.imm64(g_status_a); // movabs rsi, g_status_a
    a.b(0x31); a.b(0xD2);                      // xor edx, edx (blocking)
    a.b(0x0F); a.b(0x05);                      // syscall -> rax = pid A
    a.b(0x49); a.b(0x39); a.b(0xC7);           // cmp r15, rax
    a.b(0x0F); a.b(0x94); a.b(0xC3);           // sete bl (pid match)
    a.b(0x44); a.b(0x0F); a.b(0xB6); a.b(0xF3);// movzx r14d, bl
    a.b(0x8B); a.b(0x05); a.rip32(g_status_a); // mov eax, [rip+g_status_a]
    a.b(0xC1); a.b(0xE8); a.b(8);              // shr eax, 8
    a.b(0x3D); a.imm32(0x20);                  // cmp eax, 0x20 (pidA>>8&FF)
    a.b(0x0F); a.b(0x94); a.b(0xC0);           // sete al
    a.b(0x21); a.b(0xC3);                      // and ebx, eax     (0x01)
    a.b(0x41); a.b(0xC1); a.b(0xE6); a.b(1);   // shl r14d, 1
    a.b(0x44); a.b(0x09); a.b(0xF3);           // or ebx, r14d     (0x02)
    // isolation: the parent's witness dword is still zero
    a.b(0x8B); a.b(0x05); a.rip32(g_witness);  // mov eax, [rip+g_witness]
    a.b(0x85); a.b(0xC0);                      // test eax, eax
    a.b(0x0F); a.b(0x94); a.b(0xC0);           // sete al
    a.b(0xC1); a.b(0xE0); a.b(2);              // shl eax, 2
    a.b(0x09); a.b(0xC3);                      // or ebx, eax      (0x04)
    // ---- fork B ----
    a.b(0xB8); a.imm32(2);                     // mov eax, SYS_fork
    a.b(0x0F); a.b(0x05);                      // syscall
    a.b(0x48); a.b(0x85); a.b(0xC0);           // test rax, rax
    a.b(0x0F); a.b(0x85); a.rel32("parent_b");  // jne parent_b
    // ---- child B ----
    a.b(0xB8); a.imm32(39);                    // mov eax, SYS_getppid
    a.b(0x0F); a.b(0x05);                      // syscall
    a.b(0xC1); a.b(0xE8); a.b(8);              // shr eax, 8
    a.b(0x25); a.imm32(0xFF);                  // and eax, 0xFF
    a.b(0x89); a.b(0xC7);                      // mov edi, eax
    a.b(0xB8); a.imm32(1);                     // mov eax, SYS_exit
    a.b(0x0F); a.b(0x05);                      // syscall
    // ---- parent B ----
    a.label("parent_b");
    a.b(0xB8); a.imm32(7);                     // mov eax, SYS_wait4
    a.b(0xBF); a.imm32(0xFFFFFFFF);            // mov edi, -1
    a.b(0x48); a.b(0xBE); a.imm64(g_status_b); // movabs rsi, g_status_b
    a.b(0x31); a.b(0xD2);                      // xor edx, edx
    a.b(0x0F); a.b(0x05);                      // syscall
    a.b(0x8B); a.b(0x05); a.rip32(g_status_b); // mov eax, [rip+g_status_b]
    a.b(0xC1); a.b(0xE8); a.b(8);              // shr eax, 8
    a.b(0x3D); a.imm32(0x10);                  // cmp eax, 0x10 (ppid>>8&FF)
    a.b(0x0F); a.b(0x94); a.b(0xC0);           // sete al
    a.b(0xC1); a.b(0xE0); a.b(3);              // shl eax, 3
    a.b(0x09); a.b(0xC3);                      // or ebx, eax      (0x08)
    // ---- reaped-out WNOHANG -> ECHILD ----
    a.b(0xB8); a.imm32(7);                     // mov eax, SYS_wait4
    a.b(0xBF); a.imm32(0xFFFFFFFF);            // mov edi, -1
    a.b(0x48); a.b(0xBE); a.imm64(g_status_a); // movabs rsi, g_status_a
    a.b(0xBA); a.imm32(1);                     // mov edx, WNOHANG
    a.b(0x0F); a.b(0x05);                      // syscall
    a.b(0x3D); a.imm32(10);                    // cmp eax, ECHILD
    a.b(0x0F); a.b(0x94); a.b(0xC0);           // sete al
    a.b(0xC1); a.b(0xE0); a.b(4);              // shl eax, 4
    a.b(0x09); a.b(0xC3);                      // or ebx, eax      (0x10)
    a.b(0x89); a.b(0xD8);                      // mov eax, ebx
    a.b(0xC3);                                 // ret
    a.patch();

    CHECK(vmm.CopyToGuest(kCodeGva, a.code.data(), a.code.size()));
    CHECK(vmm.ProtectVirtual(kCodeGva, 0x1000, rx));

    // ---- machine state -----------------------------------------------------
    CpuState st{};
    st.rip = kCodeGva;
    uint64_t rsp = (kStackGva + 0x1000) & ~15ull;
    rsp -= 8;
    CHECK(vmm.CopyToGuest(rsp, &kStopSentinel, 8));
    st.gpr[RSP] = rsp;

    VmmMemoryBus bus(vmm);
    X86Interpreter interp(st, bus);
    interp.SetSyscallHandler([](CpuState& s, GuestMemoryBus&) -> bool {
        SyscallContext ctx{};
        ctx.rax = s.gpr[RAX];
        ctx.rdi = s.gpr[RDI];
        ctx.rsi = s.gpr[RSI];
        ctx.rdx = s.gpr[RDX];
        ctx.rcx = s.gpr[R10];
        ctx.r8 = s.gpr[R8];
        ctx.r9 = s.gpr[R9];
        ctx.cpu = &s;
        if (s.gpr[RAX] == static_cast<uint64_t>(ProsperoSyscall::SC_SYS_exit)) {
            // process-wide exit on the MAIN thread: record + unwind
            ProsperoSyscallDispatcher::Instance().Dispatch(ctx);
            return false;
        }
        s.gpr[RAX] = ProsperoSyscallDispatcher::Instance().Dispatch(ctx);
        return true;
    });
    const RunResult res = interp.Run(10'000'000, kStopSentinel);
    CHECK(res.status == ExecStatus::Returned);

    const int exit_code = static_cast<int>(st.gpr[RAX] & 0xFF);
    CHECK(exit_code == 0x1F);
    if (exit_code != 0x1F) {
        std::cerr << "  parent exit code bits: 0x" << std::hex << exit_code
                  << std::dec << " (0x1F = all checks)\n";
    }
    CHECK(ForkProcessManager::Instance().UnreapedChildCount() == 0);
    CHECK(ForkProcessManager::CurrentPid() == ForkProcessManager::MainPid());

    // execve keeps refusing honestly (one guest image per boot).
    {
        SyscallContext ctx{};
        ctx.rax = static_cast<uint64_t>(ProsperoSyscall::SC_SYS_execve);
        CHECK(ProsperoSyscallDispatcher::Instance().Dispatch(ctx) == 1);
    }

    std::cout << "[fork] " << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
