// ============================================================================
// ProsperoLayer RDNA2 Core - Real Vulkan compute executor test (item #1)
// ----------------------------------------------------------------------------
// Proves the full GPU compute pipeline: RDNA2 -> SPIR-V -> real VkBuffers
// (SSBOs) + descriptor sets + compute pipeline -> vkCmdDispatch -> results read
// BACK from GPU memory. When a Vulkan device is present (llvmpipe/lavapipe here,
// or a real GPU on the user's machine) the readback values are asserted exactly.
// On a host with no Vulkan loader the executor reports Unavailable and the
// value checks are skipped -- the suite still passes so `make unit` is portable.
// ============================================================================
#include "gpu/vulkan_compute_executor.hpp"
#include "gpu/shader_spirv_recompiler.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
using namespace PS5::GPU;

int g_failures = 0;
int g_checks = 0;
bool Check(bool v, const char* e, int line) {
    ++g_checks;
    if (!v) { ++g_failures; std::cerr << "  [FAIL] line " << line << ": " << e << '\n'; }
    return v;
}
#define CHECK(e) Check((e), #e, __LINE__)

uint32_t EncVop1(RDNA2_VOP1_Op op, uint32_t dst, uint32_t src0) {
    // Real VOP1 hardware layout: op[24:17], vdst[16:9], src0[8:0].
    return 0x7e000000U | (static_cast<uint32_t>(op) << 17U) | (dst << 9U) | src0;
}
uint32_t EncVop2(uint32_t op, uint32_t dst, uint32_t src1_vgpr, uint32_t src0) {
    return (op << 25U) | (dst << 17U) | (src1_vgpr << 9U) | src0;
}
uint32_t EncSopp(RDNA2_SOPP_Op op, uint32_t simm16 = 0) {
    return 0xbf800000U | (static_cast<uint32_t>(op) << 16U) | simm16;
}
uint32_t V(uint32_t n) { return 256U + n; } // VGPR source encoding

float AsFloat(uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return f; }
uint32_t AsBits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }

} // namespace

int main() {
    std::cout << "=== Real Vulkan Compute Executor Test (item #1) ===\n";

    VulkanComputeExecutor exec;
    const bool available = exec.Initialize();
    if (available) {
        std::cout << "[info] Vulkan device: " << exec.DeviceName() << "\n";
    } else {
        std::cout << "[info] No Vulkan device available; value checks skipped "
                     "(structural path still exercised).\n";
    }

    std::vector<float> input;
    for (uint32_t i = 0; i < 128; ++i) input.push_back(static_cast<float>(i + 1));

    // --- Program 1: out = sqrt(in) ------------------------------------------
    {
        std::vector<uint32_t> code = {
            EncVop1(RDNA2_VOP1_Op::V_SQRT_F32, 0, V(0)),
            EncSopp(RDNA2_SOPP_Op::S_ENDPGM),
        };
        auto r = exec.RunRDNA2Float(code.data(), code.size(), input);
        if (available) {
            CHECK(r.status == ComputeExecStatus::Ok);
            CHECK(r.hardware);
            CHECK(r.output.size() == input.size());
            if (r.output.size() == input.size()) {
                int bad = 0;
                for (size_t i = 0; i < input.size(); ++i) {
                    if (std::fabs(AsFloat(r.output[i]) - std::sqrt(input[i])) > 1e-3f) ++bad;
                }
                CHECK(bad == 0);
                std::cout << "  [ok] sqrt kernel: " << (input.size() - bad)
                          << "/" << input.size() << " lanes correct on GPU\n";
            }
        } else {
            CHECK(r.status == ComputeExecStatus::Unavailable);
        }
    }

    // --- Program 2: out = in * in  (V_MUL_F32, opcode 8) --------------------
    {
        std::vector<uint32_t> code = {
            EncVop2(8U, 0, /*src1 vgpr*/ 0, V(0)), // v0 = v0 * v0
            EncSopp(RDNA2_SOPP_Op::S_ENDPGM),
        };
        auto r = exec.RunRDNA2Float(code.data(), code.size(), input);
        if (available) {
            CHECK(r.status == ComputeExecStatus::Ok);
            if (r.output.size() == input.size()) {
                int bad = 0;
                for (size_t i = 0; i < input.size(); ++i) {
                    if (std::fabs(AsFloat(r.output[i]) - input[i] * input[i]) > 1e-2f) ++bad;
                }
                CHECK(bad == 0);
                std::cout << "  [ok] square kernel: " << (input.size() - bad)
                          << "/" << input.size() << " lanes correct on GPU\n";
            }
        } else {
            CHECK(r.status == ComputeExecStatus::Unavailable);
        }
    }

    // --- Program 3: identity mov (out = in) via V_MOV_B32 -------------------
    {
        std::vector<uint32_t> code = {
            EncVop1(RDNA2_VOP1_Op::V_MOV_B32, 0, V(0)),
            EncSopp(RDNA2_SOPP_Op::S_ENDPGM),
        };
        std::vector<uint32_t> bits(input.size());
        for (size_t i = 0; i < input.size(); ++i) bits[i] = AsBits(input[i]);
        auto r = exec.RunRDNA2(code.data(), code.size(), bits);
        if (available) {
            CHECK(r.status == ComputeExecStatus::Ok);
            CHECK(r.output == bits);
            std::cout << "  [ok] identity kernel: exact bitwise readback matches input\n";
        } else {
            CHECK(r.status == ComputeExecStatus::Unavailable);
        }
    }

    // --- Program 4: compile failure is reported, not faked ------------------
    {
        std::vector<uint32_t> bad_code = { 0xDEADBEEFU }; // no S_ENDPGM, junk
        auto r = exec.RunRDNA2(bad_code.data(), bad_code.size(), {1u, 2u});
        CHECK(r.status == ComputeExecStatus::CompileFailed ||
              r.status == ComputeExecStatus::Unavailable);
        CHECK(!static_cast<bool>(r));
    }

    // --- Program 5: multi-op chain out = sqrt(in*in) = |in| -----------------
    {
        std::vector<uint32_t> code = {
            EncVop2(8U, 0, 0, V(0)),                       // v0 = v0*v0
            EncVop1(RDNA2_VOP1_Op::V_SQRT_F32, 0, V(0)),   // v0 = sqrt(v0)
            EncSopp(RDNA2_SOPP_Op::S_ENDPGM),
        };
        auto r = exec.RunRDNA2Float(code.data(), code.size(), input);
        if (available && r.output.size() == input.size()) {
            int bad = 0;
            for (size_t i = 0; i < input.size(); ++i) {
                if (std::fabs(AsFloat(r.output[i]) - std::fabs(input[i])) > 1e-2f) ++bad;
            }
            CHECK(bad == 0);
            std::cout << "  [ok] sqrt(in*in) chain: " << (input.size() - bad)
                      << "/" << input.size() << " lanes correct on GPU\n";
        } else if (!available) {
            CHECK(r.status == ComputeExecStatus::Unavailable);
        }
    }

    // --- Program 6: real VkImage lifecycle (the "images" half of item #1) ----
    {
        auto r = exec.ClearImage(32, 32, 0.25f, 0.5f, 0.75f, 1.0f);
        if (available) {
            CHECK(r.status == ComputeExecStatus::Ok);
            CHECK(r.hardware);
            CHECK(r.width == 32 && r.height == 32);
            CHECK(r.pixels.size() == 32u * 32u * 4u);
            if (r.pixels.size() == 32u * 32u * 4u) {
                // 0.25/0.5/0.75/1.0 in UNORM8 ~= 64/128/191/255. Check a few texels.
                bool colour_ok = true;
                for (size_t px = 0; px < r.pixels.size(); px += 4) {
                    if (!(r.pixels[px] >= 60 && r.pixels[px] <= 68 &&
                          r.pixels[px + 1] >= 124 && r.pixels[px + 1] <= 132 &&
                          r.pixels[px + 2] >= 187 && r.pixels[px + 2] <= 195 &&
                          r.pixels[px + 3] == 255)) { colour_ok = false; break; }
                }
                CHECK(colour_ok);
                std::cout << "  [ok] VkImage clear+readback: all 1024 texels match on GPU\n";
            }
        } else {
            CHECK(r.status == ComputeExecStatus::Unavailable);
        }
    }

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    if (g_failures == 0) {
        std::cout << ">> [PASS] Real Vulkan compute executor verified "
                  << (available ? "(GPU readback asserted)." : "(headless-safe, device absent).")
                  << "\n";
    }
    return g_failures == 0 ? 0 : 1;
}
