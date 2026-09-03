#pragma once

// ============================================================================
// ProsperoLayer RDNA2 Core - Extended x86-64 Interpreter
// ----------------------------------------------------------------------------
// Unlike x86_64_subset_interpreter (a deliberately tiny, fail-closed scanner),
// this is a real execution core: it decodes and executes a broad subset of the
// x86-64 integer ISA against a guest memory bus. It supports:
//   * REX prefixes (W/R/X/B) and the 0x66 operand-size override
//   * 8/16/32/64-bit operand sizes, including the REX byte-register remap
//   * Full ModRM + SIB addressing: [base + index*scale + disp8/disp32],
//     RIP-relative addressing, and register-direct operands
//   * ALU ops: add/or/adc/sbb/and/sub/xor/cmp (r/m<->r and imm forms),
//     inc/dec, neg/not, test, imul (2- and 3-operand)
//   * Data movement: mov (all encodings), movzx/movsx, lea, xchg, push/pop
//   * Shifts/rotates: shl/shr/sar (imm8, by-1, and by-CL)
//   * Control flow: jmp/call (rel and indirect), ret, jcc (rel8/rel32),
//     setcc, cmovcc, loop/loope/loopne
//   * RFLAGS computation (CF, PF, AF, ZF, SF, OF) matching hardware semantics
//   * syscall (delegated to a host handler)
//
// The interpreter is memory-safe: every guest access goes through GuestMemoryBus
// and a failed access halts execution with a MemoryFault rather than touching
// host memory directly.
// ============================================================================

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace PS5::CPU {

// x86-64 GPR index order: rax,rcx,rdx,rbx,rsp,rbp,rsi,rdi,r8..r15.
enum Reg : uint8_t {
    RAX = 0, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
    R8, R9, R10, R11, R12, R13, R14, R15,
};

// RFLAGS bit positions we model.
namespace Flags {
constexpr uint64_t CF = 1ull << 0;
constexpr uint64_t PF = 1ull << 2;
constexpr uint64_t AF = 1ull << 4;
constexpr uint64_t ZF = 1ull << 6;
constexpr uint64_t SF = 1ull << 7;
constexpr uint64_t DF = 1ull << 10;
constexpr uint64_t OF = 1ull << 11;
} // namespace Flags

struct CpuState {
    std::array<uint64_t, 16> gpr{};
    uint64_t rip{0};
    uint64_t rflags{0};

    // Round 11: SSE/SSE2 register file (16 x 128-bit XMM registers) and the
    // FS segment base (thread pointer). fs_base is what the TLS block
    // allocator loads; the 0x64 prefix routes memory operands through it.
    struct alignas(16) XmmReg {
        uint64_t lo;
        uint64_t hi;
    };
    std::array<XmmReg, 16> xmm{};
    // Round 18: the UPPER 128 bits of each YMM register (AVX.256). YMM i is
    // {xmm[i] (low), ymm_hi[i] (high)} -- the same split real hardware
    // makes. VEX.128 register writes ZERO the upper half (hardware rule);
    // VZEROUPPER/VZEROALL clear it explicitly.
    std::array<XmmReg, 16> ymm_hi{};
    uint64_t fs_base{0};

    // Round 29: x87 FPU model (interpreter path). x87_st holds the eight
    // stack slots addressed as st(i) = x87_st[(x87_top + i) & 7]; on an
    // x86-64 host, long double IS the 80-bit x87 extended format, so guest
    // arithmetic matches bit-for-bit by construction. x87_cw/x87_sw are the
    // control/status words (exception flags are not modelled -- documented
    // simplification; the condition codes C0..C3 in x87_sw are).
    long double x87_st[8]{};
    uint8_t x87_top{0};
    uint16_t x87_cw{0x037F};
    uint16_t x87_sw{0};

    uint64_t& operator[](size_t i) { return gpr[i]; }
    uint64_t operator[](size_t i) const { return gpr[i]; }
};

enum class ExecStatus {
    Running,
    Halted,            // hlt executed
    Returned,          // ret with empty/again-past-limit stack sentinel reached
    InstructionLimit,  // ran out of the instruction budget
    UnsupportedOpcode, // decoder hit an opcode outside the modelled subset
    DecodeFault,       // truncated instruction stream
    MemoryFault,       // guest memory access denied by the bus
    SyscallDenied,     // syscall handler refused the request
};

// Round 20: the VIRTUAL Zen 2 host models shared by BOTH execution engines
// (the interpreter executes rdtsc/rdtscp/cpuid through these; the direct
// execution backend patches those opcodes to ud2 and replays them through
// the SAME functions, so both paths return bit-identical values).
namespace HostModels {

// A fixed monotonic TSC ticking at the PS5 CPU's 3.5 GHz: ticks =
// nanoseconds * 3.5 (computed without floating point). Invariant TSC (the
// value is wall-clock derived, never frequency-scaled).
uint64_t ReadVirtualTsc();

// A conservative AMD Zen 2 (Cilantro-class) CPUID model. Only features the
// emulator can ACTUALLY serve on every host are advertised: SSE..SSE4.2,
// POPCNT, SSE4A, BMI1, BMI2, LZCNT/ABM, MOVBE, AVX (both engines model the
// VEX forms), plus the "AuthenticAMD" vendor and a real Zen 2 desktop
// signature (family 0x17, model 0x71 -- a Ryzen 9 3900X EAX=0x00870F10).
// Anything outside the modelled leaves returns zeros.
void CpuidModel(uint32_t leaf, uint32_t subleaf,
                uint32_t& eax, uint32_t& ebx, uint32_t& ecx, uint32_t& edx);

} // namespace HostModels

// Abstract guest memory. Implementations translate a guest virtual address to
// storage and enforce their own protection model. Return false to fault.
class GuestMemoryBus {
public:
    virtual ~GuestMemoryBus() = default;
    virtual bool Read(uint64_t addr, void* dst, size_t size) = 0;
    virtual bool Write(uint64_t addr, const void* src, size_t size) = 0;
};

// A simple flat-memory bus backed by a contiguous host vector mapped at
// base_addr. Ideal for unit tests and for running self-contained guest blobs.
class FlatMemoryBus final : public GuestMemoryBus {
public:
    FlatMemoryBus(uint64_t base_addr, size_t size)
        : m_base(base_addr), m_storage(size, 0) {}

    bool Read(uint64_t addr, void* dst, size_t size) override;
    bool Write(uint64_t addr, const void* src, size_t size) override;

    uint64_t base() const { return m_base; }
    size_t size() const { return m_storage.size(); }
    uint8_t* data() { return m_storage.data(); }
    const uint8_t* data() const { return m_storage.data(); }

    // Convenience: load a code/data blob at an absolute guest address.
    bool LoadBlob(uint64_t addr, std::span<const uint8_t> bytes);

private:
    bool InRange(uint64_t addr, size_t size) const;
    uint64_t m_base;
    std::vector<uint8_t> m_storage;
};

// Return false to deny a syscall. rax holds the syscall number on entry; a
// successful handler writes the return value back into rax.
using SyscallHandler = std::function<bool(CpuState&, GuestMemoryBus&)>;

struct RunResult {
    ExecStatus status{ExecStatus::Running};
    uint64_t fault_addr{0};
    size_t executed{0};
};

// Round 20: an instruction the DIRECT execution backend must intercept. The
// same classification feeds the JIT block cache, so both execution engines
// agree on exactly which instructions are host-dependent.
//
// Kinds:
//   * Syscall  -- the guest's `syscall` (FreeBSD-9 numbering) must NEVER reach
//                 the host kernel: it is rewritten to ud2 and serviced through
//                 ProsperoSyscallDispatcher.
//   * Cpuid/Rdtsc/Rdtscp -- virtualised (a fixed Zen 2 model + a monotonic
//                 virtual TSC) so guest code sees a deterministic CPU.
//   * Sse4a/Bmi1/Bmi2/Movbe/Tzcnt -- Zen 2 extension instructions that are NOT
//                 on every x86-64 host (and tzcnt/lzcnt differ from bsf/bsr on
//                 hosts without BMI1): ALWAYS rewritten to ud2 and emulated by
//                 the interpreter core, so behaviour is bit-identical on every
//                 machine (the "rare unsupported instruction" rule of direct
//                 execution).
enum class PatchKind : uint8_t {
    None = 0,
    Syscall,
    Cpuid,
    Rdtsc,
    Rdtscp,
    Sse4a,
    Bmi1,
    Bmi2,
    Movbe,
    Tzcnt,
};

// One patchable instruction occurrence inside a scanned block.
struct PatchSiteInfo {
    uint32_t offset{0};       // byte offset of the instruction inside the block
    uint32_t length{0};       // full instruction length in bytes (<= 15)
    PatchKind kind{PatchKind::None};
};

// Round 16: block-inspection result for the JIT block scanner. Uses the REAL
// decoder (the same ISA coverage as execution) instead of the fail-closed
// subset scanner.
struct BlockInspectResult {
    ExecStatus status{ExecStatus::Running};
    size_t instruction_count{0};
    size_t code_size{0};
    uint64_t next_rip{0};       // rip after the terminator
    uint64_t fault_addr{0};
    bool contains_syscall{false};
    bool ends_in_ret{false};
    bool ends_in_branch{false};
    bool ends_in_call{false};
    // Round 20: every host-dependent instruction the walk crossed (the block
    // still ends at the first terminator; sites only annotate).
    std::vector<PatchSiteInfo> sites;
};

class X86Interpreter {
public:
    X86Interpreter(CpuState& state, GuestMemoryBus& memory)
        : m_state(state), m_mem(memory) {}

    void SetSyscallHandler(SyscallHandler handler) { m_syscall = std::move(handler); }

    // Round 20: read-only access to the installed syscall handler so the
    // direct execution backend can service trapped guest syscalls through
    // the SAME lambda the interpreter uses (identical semantics).
    const SyscallHandler& GetSyscallHandlerForDirect() const { return m_syscall; }

    // Execute starting at state.rip until a stop condition. Execution stops
    // cleanly when rip reaches stop_rip (a return sentinel pushed by the caller)
    // or when the instruction budget is exhausted.
    RunResult Run(size_t instruction_limit = 100000,
                  uint64_t stop_rip = kNoStopRip);

    // Execute exactly one instruction from state.rip.
    RunResult Step();

    // Round 16: scan a code block with the full decoder until the first
    // block-terminating instruction (ret / branch / call / syscall / hlt) or a
    // fault. `code` starts at guest_rip. Static: pure decode, no state side
    // effects (a scratch CpuState is used for prefix/ModRM decoding).
    static BlockInspectResult InspectBlock(std::span<const uint8_t> code,
                                            uint64_t guest_rip,
                                            size_t max_instructions = 512,
                                            size_t max_bytes = 4096);

    static constexpr uint64_t kNoStopRip = ~0ull;

    CpuState& state() { return m_state; }

private:
    // --- fetch helpers -------------------------------------------------------
    bool Fetch8(uint64_t& rip, uint8_t& out);
    bool Fetch16(uint64_t& rip, uint16_t& out);
    bool Fetch32(uint64_t& rip, uint32_t& out);
    bool Fetch64(uint64_t& rip, uint64_t& out);

    // --- operand access ------------------------------------------------------
    struct Prefixes {
        bool rex{false};
        bool rex_w{false};
        bool rex_r{false};
        bool rex_x{false};
        bool rex_b{false};
        bool opsize{false}; // 0x66
        bool addrsize{false}; // 0x67 (accepted, 64-bit addressing assumed)
        bool seg_fs{false}; // 0x64 FS segment override (round 11: TLS)
        bool lock{false}; // 0xF0 lock prefix (round 31: tracked for atomic ops)
        uint8_t rep{0}; // 0xF3/0xF2 group (SSE opcode selector / rep)
    };

    struct ModRMOperand {
        bool is_reg{false};
        uint8_t reg{0};      // register index when is_reg
        uint64_t addr{0};    // effective address when !is_reg
    };

    int OperandSize(const Prefixes& p) const; // bytes
    uint64_t ReadReg(uint8_t idx, int size) const;
    void WriteReg(uint8_t idx, int size, uint64_t value);
    uint64_t ReadRegByte(uint8_t idx, bool rex) const;
    void WriteRegByte(uint8_t idx, bool rex, uint8_t value);

    bool DecodeModRM(uint64_t& rip, const Prefixes& p, uint8_t modrm,
                     ModRMOperand& out, uint8_t& reg_field);

    bool ReadRM(const ModRMOperand& op, int size, bool rex, uint64_t& out);
    bool WriteRM(const ModRMOperand& op, int size, bool rex, uint64_t value);

    // --- flags ---------------------------------------------------------------
    void SetFlag(uint64_t mask, bool on);
    bool GetFlag(uint64_t mask) const;
    void UpdateFlagsAdd(uint64_t a, uint64_t b, uint64_t res, int size, bool with_carry, bool carry_in);
    void UpdateFlagsSub(uint64_t a, uint64_t b, uint64_t res, int size, bool with_borrow, bool borrow_in);
    void UpdateFlagsLogic(uint64_t res, int size);
    void UpdateSZP(uint64_t res, int size);
    bool EvalCondition(uint8_t tttn) const; // for jcc/setcc/cmovcc

    static uint64_t Mask(int size);
    static uint64_t SignExtend(uint64_t value, int size);

    // --- SSE/SSE2 (round 11) --------------------------------------------------
    // Executes one 0x0F-prefixed SSE/SSE2 instruction. Returns true when the
    // opcode was handled; on failure the RunResult status/fault are set.
    bool ExecSse(uint64_t& rip, const Prefixes& p, uint8_t op2, RunResult& r);
    // Round 16: 0F 38 / 0F 3A (SSE3/SSSE3/SSE4.1) integer + blend/round ops.
    bool ExecSse3(uint64_t& rip, const Prefixes& p, bool has_3a,
                  uint8_t op2, RunResult& r);
    // Round 16: legacy-0F integer SIMD (punpck*/pack*/pcmpgt*/pcmpeq*,
    // padd*/psub*/pand/por/pandn/pshufd/psll*/psrl*/psra*/pslldq/psrldq/
    // pmuludq/pmullw/pmaddwd/paddq/psubq/pextrw/pinsrw, movdqa/movdqu).
    bool ExecSseInt(uint64_t& rip, const Prefixes& p, uint8_t op2, RunResult& r);
    // Round 16: string ops (movs/stos/lods/scas/cmps) with REP/REPE/REPNE.
    bool ExecStringOp(uint64_t& rip, const Prefixes& p, uint8_t opcode,
                      RunResult& r);
    // Round 16: 0F A3/AB/B3/BB + BA /4../7 (bt/bts/btr/btc) and BC/BD
    // (bsf/bsr, plus popcnt/tzcnt via the 0xF3 prefix).
    bool ExecBitOp(uint64_t& rip, const Prefixes& p, uint8_t op2, RunResult& r);

    // Round 29: the complete x87 engine (opcodes 0xD8..0xDF plus FWAIT).
    // Implemented in x86_64_x87.cpp. Returns false on decode/memory faults
    // (the caller must NOT commit rip in that case).
    bool ExecX87(uint64_t& rip, const Prefixes& p, uint8_t opcode, RunResult& r);
    // Round 16: VEX (C4/C5) AVX.128 forms of the modelled SSE ops.
    bool ExecVex(uint64_t& rip, uint8_t vex1, RunResult& r);
    // Round 20: AMD SSE4a (insertq/extrq/movntss/movntsd) -- semantics verified
    // against QEMU's helper_extrq/helper_insertq (target/i386/ops_sse.h) and
    // the encodings against LLVM X86InstrSSE.td + QEMU decode-new.c.inc:
    //   66 0F 78 /r ib ib = EXTRQ   xmm, len, idx   (unary, 2 imm8)
    //   66 0F 79 /r      = EXTRQ   xmm1, xmm2      (len=src.B0, idx=src.B1)
    //   F2 0F 78 /r ib ib = INSERTQ xmm1, xmm2, len, idx
    //   F2 0F 79 /r      = INSERTQ xmm1, xmm2      (len=src.B8, idx=src.B9)
    //   F3 0F 2B /r      = MOVNTSS m32, xmm        (plain 4-byte store)
    //   F2 0F 2B /r      = MOVNTSD m64, xmm        (plain 8-byte store)
    // Only the LOW 64 bits of the destination change (QEMU helper semantics);
    // len==0 masks all 64 bits; len/idx are taken modulo 64.
    bool ExecSse4a(uint64_t& rip, const Prefixes& p, uint8_t op2, RunResult& r);
    // XMM helpers: register-file access and variable-width XMM loads/stores
    // through the guest memory bus (4/8/16 bytes).
    bool ReadXmmOperand(const ModRMOperand& op, const Prefixes& p, int width,
                        CpuState::XmmReg& out);
    bool WriteXmmOperand(const ModRMOperand& op, const Prefixes& p, int width,
                         const CpuState::XmmReg& value);
    static float XmmFloat(const CpuState::XmmReg& v, int lane);
    static double XmmDouble(const CpuState::XmmReg& v, int lane);
    static void SetXmmFloat(CpuState::XmmReg& v, int lane, float f);
    static void SetXmmDouble(CpuState::XmmReg& v, int lane, double d);
    void SetComparedFlags(float a, float b);
    void SetComparedFlags(double a, double b);

    // Round 28: the FULL systematic SIMD engine (x86_64_simd_full.cpp).
    // Covers the complete legacy SSE1..SSE4.2 maps plus the VEX (AVX/AVX2/FMA)
    // maps at 128- and 256-bit, verified instruction-by-instruction against
    // real hardware through the cpu_simd_diff_test differential harness
    // (tests/simd_forms_table.inc, 1100+ GNU-as-verified encodings).
    //
    // Dispatch contract: the caller fetches the opcode byte (op2, or op3 for
    // the 0F38/0F3A maps) and passes it together with the map number (1/2/3)
    // and, for VEX forms, the decoded VEX info. ExecSimdFull FIRST checks
    // coverage on (map, opcode, prefix, W, L) WITHOUT consuming any bytes;
    // only a covered combination proceeds to ModRM/immediate decode. When it
    // returns false the stream position is unchanged and the legacy handlers
    // keep running exactly as before (fail-closed ordering preserved).
    struct SimdVexInfo {
        uint8_t vreg{0};   // inverted vvvv (register index 0..15)
        bool L{false};     // vector length: false = 128, true = 256
        bool W{false};     // VEX.W
    };
    bool ExecSimdFull(uint64_t& rip, const Prefixes& p, uint8_t op2, int map,
                      const SimdVexInfo* vex, RunResult& r);

    CpuState& m_state;
    GuestMemoryBus& m_mem;
    SyscallHandler m_syscall{};
};

} // namespace PS5::CPU
