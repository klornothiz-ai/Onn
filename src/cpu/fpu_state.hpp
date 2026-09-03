#pragma once
#include <cstdint>
#include <array>
#include <cstring>

namespace PS5::CPU {

    struct alignas(16) FXSaveArea {
        uint16_t fcw;
        uint16_t fsw;
        uint8_t  ftw;
        uint8_t  reserved1;
        uint16_t fop;
        uint64_t fip;
        uint64_t fdp;
        uint32_t mxcsr;
        uint32_t mxcsr_mask;
        std::array<uint8_t, 128> st_mmx;
        std::array<uint8_t, 256> xmm;
        std::array<uint8_t, 96>  reserved2;
    };

    class FPUStateManager {
    public:
        static inline void SaveFPUState(FXSaveArea& out_state) {
            #if defined(__x86_64__) || defined(_M_X64)
            __asm__ __volatile__("fxsave64 %0" : "=m"(out_state) : : "memory");
            #endif
        }

        static inline void RestoreFPUState(const FXSaveArea& in_state) {
            #if defined(__x86_64__) || defined(_M_X64)
            __asm__ __volatile__("fxrstor64 %0" : : "m"(in_state) : "memory");
            #endif
        }

        static inline void InitializeGuestFPU(FXSaveArea& state) {
            std::memset(&state, 0, sizeof(FXSaveArea));
            state.fcw = 0x037F;
            state.mxcsr = 0x1F80;
            state.mxcsr_mask = 0xFFFF;
        }
    };

}
