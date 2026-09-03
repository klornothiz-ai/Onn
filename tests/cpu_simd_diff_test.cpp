// cpu_simd_diff_test - differential verification of the full SIMD engine.
//
// Round 28. For EVERY form in tests/simd_forms_table.inc (a table produced
// by GNU as; see scripts/gen_simd_forms.py) this suite:
//   1. fills the guest register file + a scratch memory window with
//      deterministic pseudo-random data,
//   2. executes the exact encoded bytes through the x86-64 interpreter's
//      full SIMD engine over a FlatMemoryBus,
//   3. executes the SAME bytes natively on the host CPU through the wrapper
//      functions in tests/simd_native_forms.S (real hardware ground truth),
//   4. requires bit-identical results: ymm0..ymm3 (all 256 bits), the
//      GPRs (rcx/rax/rdx), the memory window and the arithmetic flags.
//
// Memory-operand forms run with guest RDI = input base and RSI = output
// base, exactly mirroring the native wrapper ABI, so [288+rdi]/[288+rsi]
// addressing resolves to the same buffers on both sides.
//
// rsqrtps/rsqrtss/rcpps/rcpss are approximation instructions whose exact
// bit patterns are microcode-defined; they are implemented as IEEE 1/x and
// 1/sqrt(x) and excluded from the comparison (documented approximation).
#include "cpu/x86_64_interpreter.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// native ground-truth wrappers (tests/simd_native_forms.S)
extern "C" void form_0(const void*, void*);
#include "simd_form_thunk.inc"

#include "simd_forms_table.inc"

using PS5::CPU::CpuState;
using PS5::CPU::FlatMemoryBus;
using PS5::CPU::X86Interpreter;
using PS5::CPU::ExecStatus;

static int g_checks = 0;
static int g_failures = 0;
static std::vector<std::string> g_failed;

#define CHECK(cond, msg)                                     \
    do {                                                     \
        ++g_checks;                                          \
        if (!(cond)) { ++g_failures; g_failed.push_back(msg); } \
    } while (0)

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint64_t NextRand() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static bool IsExcluded(const char* name) {
    return std::strstr(name, "rcpps") || std::strstr(name, "rcpss") ||
           std::strstr(name, "rsqrtps") || std::strstr(name, "rsqrtss");
}

int main() {
    std::printf("== CPU SIMD differential test: %d verified encodings ==\n",
                SimdForms::kFormCount);

    constexpr uint64_t kInBase = 0x100000;     // rdi / input buffer
    constexpr uint64_t kOutBase = 0x100000 + 0x8000;  // rsi / output buffer
    constexpr uint64_t kCode = 0x100000 + 0x10000;
    constexpr size_t kMemSize = 0x20000 + 0x1000;

    FlatMemoryBus bus(kInBase, kMemSize);

    alignas(32) uint8_t input[320];
    alignas(32) uint8_t native_out[320];
    alignas(32) uint8_t interp_out[320];

    int compared = 0, excluded = 0;

    for (int i = 0; i < SimdForms::kFormCount; ++i) {
        const SimdForms::Form& form = SimdForms::kForms[i];
        if (IsExcluded(form.name)) { ++excluded; continue; }

        g_rng = 0x9E3779B97F4A7C15ull + static_cast<uint64_t>(i) * 0x2545F4914F6CDD1Dull;
        for (size_t w = 0; w < sizeof(input); w += 8) {
            const uint64_t v = NextRand();
            const size_t n = (sizeof(input) - w < 8) ? sizeof(input) - w : 8;
            std::memcpy(input + w, &v, n);
        }
        // make a deterministic subset of float lanes "interesting"
        for (int lane = 0; lane < 32; lane += 4) {
            if (((input[lane] ^ i) & 3) == 0) {
                const float f = static_cast<float>((lane % 7) - 3) * 1.5f;
                std::memcpy(input + lane, &f, 4);
            }
        }
        std::memset(native_out, 0, sizeof(native_out));
        std::memset(interp_out, 0, sizeof(interp_out));

        if (const char* dbg = getenv("SIMD_DBG")) {
            (void)dbg;
            std::fprintf(stderr, "[form %d] %s\n", i, form.name);
            std::fflush(stderr);
        }
        // ---- native ground truth ------------------------------------------
        SimdFormThunks[i](input, native_out);

        // ---- interpreter execution ----------------------------------------
        if (getenv("SIMD_DBG")) { std::fprintf(stderr, "  [a] pre-write\n"); std::fflush(stderr); }
        bus.Write(kInBase, input, 320);
        if (getenv("SIMD_DBG")) { std::fprintf(stderr, "  [b] wrote in\n"); std::fflush(stderr); }
        bus.Write(kOutBase, interp_out, 320);
        if (getenv("SIMD_DBG")) { std::fprintf(stderr, "  [c] wrote out\n"); std::fflush(stderr); }

        CpuState state{};
        for (int r = 0; r < 4; r++) {
            CpuState::XmmReg lo{}, hi{};
            std::memcpy(&lo, input + r * 32, 16);
            std::memcpy(&hi, input + r * 32 + 16, 16);
            state.xmm[r] = lo;
            state.ymm_hi[r] = hi;
        }
        std::memcpy(&state.gpr[1], input + 256, 8);   // RCX
        std::memcpy(&state.gpr[0], input + 264, 8);   // RAX
        std::memcpy(&state.gpr[2], input + 272, 8);   // RDX
        state.gpr[6] = kOutBase;                       // RSI
        state.gpr[7] = kInBase;                        // RDI
        state.rip = kCode;

        bus.Write(kCode, form.bytes, form.nbytes);
        if (getenv("SIMD_DBG")) { std::fprintf(stderr, "  [d] pre-run\n"); std::fflush(stderr); }

        X86Interpreter interp(state, bus);
        // run exactly this one instruction: stop at the byte that follows it
        const auto result = interp.Run(4, kCode + form.nbytes);
        if (result.status != ExecStatus::Returned) {
            ++g_checks; ++g_failures;
            if (g_failed.size() < 12) {
                std::string bytes;
                char buf[8];
                for (int b = 0; b < form.nbytes; ++b) {
                    std::snprintf(buf, sizeof(buf), "%02X ", form.bytes[b]);
                    bytes += buf;
                }
                g_failed.push_back(std::string(form.name) + ": status " +
                    std::to_string(static_cast<int>(result.status)) + " fault=" +
                    std::to_string(result.fault_addr) + " bytes=[" + bytes + "]");
            }
        } else {
            ++g_checks;
        }

        for (int r = 0; r < 4; r++) {
            std::memcpy(interp_out + r * 32, &state.xmm[r], 16);
            std::memcpy(interp_out + r * 32 + 16, &state.ymm_hi[r], 16);
        }
        std::memcpy(interp_out + 256, &state.gpr[1], 8);
        std::memcpy(interp_out + 264, &state.gpr[0], 8);
        std::memcpy(interp_out + 272, &state.gpr[2], 8);
        const uint64_t flags = state.rflags & 0x8D5ull;   // CF|PF|AF|ZF|SF|OF
        std::memcpy(interp_out + 280, &flags, 8);
        bus.Read(kOutBase + 288, interp_out + 288, 32);

        if (std::memcmp(native_out, interp_out, 320) != 0) {
            ++g_checks; ++g_failures;
            if ((std::strncmp(form.name, "movss", 5) == 0 || std::strncmp(form.name, "paddsb", 6) == 0 || std::strncmp(form.name, "movq", 4) == 0 || std::strncmp(form.name, "vdpps", 5) == 0 || std::strncmp(form.name, "vpsllw", 6) == 0 || std::strncmp(form.name, "aesimc", 6) == 0 || std::strncmp(form.name, "aesdeclast", 10) == 0 || std::strncmp(form.name, "vmovss", 6) == 0 || std::strncmp(form.name, "vmovq", 5) == 0 || std::strncmp(form.name, "vfmaddsub", 9) == 0 || std::strncmp(form.name, "vfnmsub", 7) == 0 || std::strncmp(form.name, "vcvtph2ps", 9) == 0) && g_failed.size() < 30) {
                std::printf("   [%d] %s bytes:", i, form.name);
                for (int b = 0; b < form.nbytes; ++b) std::printf(" %02X", form.bytes[b]);
                std::printf("\n      nat :");
                for (int b = 0; b < 96; ++b) std::printf(" %02X", native_out[b]);
                std::printf("\n      int :");
                for (int b = 0; b < 96; ++b) std::printf(" %02X", interp_out[b]);
                std::printf("\n      in  :");
                for (int b = 0; b < 96; ++b) std::printf(" %02X", input[b]);
                std::printf("\n");
            }
            if (g_failed.size() < 6) {
                std::string msg = std::string(form.name) + ": diff at:";
                for (int b = 0; b < 320; b += 8) {
                    uint64_t n, m;
                    std::memcpy(&n, native_out + b, 8);
                    std::memcpy(&m, interp_out + b, 8);
                    if (n != m) msg += " +" + std::to_string(b) + " nat=" + std::to_string(n) + " int=" + std::to_string(m);
                }
                g_failed.push_back(msg);
            }
        } else {
            ++g_checks;
        }
        ++compared;
    }

    std::printf("   compared %d forms natively (excluded %d approximate)\n",
                compared, excluded);
    size_t shown = 0;
    for (const auto& f : g_failed) {
        if (shown++ < 25) std::printf("   FAIL: %s\n", f.c_str());
    }
    if (g_failed.size() > 25) {
        std::printf("   ... %zu total failures\n", g_failed.size());
    }
    std::printf("cpu_simd_diff_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
