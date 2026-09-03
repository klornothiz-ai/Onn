#pragma once
// ProsperoLayer PS5 emulator - libkernel event flag subsystem (Kyty-compatible interface)
#include "common/common.h"
#include <cstdint>

namespace Libs::LibKernel {

namespace EventFlag {

using KernelEventFlag = int32_t;

constexpr uint32_t SCE_KERNEL_EVF_ATTR_THREAD = 0x01u;
constexpr uint32_t SCE_KERNEL_EVF_WAITMODE_AND       = 0x01u;
constexpr uint32_t SCE_KERNEL_EVF_WAITMODE_OR        = 0x02u;
constexpr uint32_t SCE_KERNEL_EVF_WAITMODE_CLEAR_ALL = 0x10u;
constexpr uint32_t SCE_KERNEL_EVF_WAITMODE_CLEAR_PAT = 0x20u;

int KYTY_SYSV_ABI KernelCreateEventFlag(KernelEventFlag* efa, const char* name, uint32_t attr,
                                        uint64_t init_pattern, const void* parent);
int KYTY_SYSV_ABI KernelDeleteEventFlag(KernelEventFlag efa);
int KYTY_SYSV_ABI KernelSetEventFlag(KernelEventFlag efa, uint64_t bits);
int KYTY_SYSV_ABI KernelClearEventFlag(KernelEventFlag efa, uint64_t bits);
int KYTY_SYSV_ABI KernelPollEventFlag(KernelEventFlag efa, uint64_t bits, uint32_t wait_mode,
                                      uint64_t* out_bits);
int KYTY_SYSV_ABI KernelWaitEventFlag(KernelEventFlag efa, uint64_t bits, uint32_t wait_mode,
                                      uint64_t* out_bits, uint32_t* timeout);
int KYTY_SYSV_ABI KernelCancelEventFlag(KernelEventFlag efa, uint64_t* out_bits,
                                        int32_t num_cancel, uint32_t* canceled_patterns);

} // namespace EventFlag

} // namespace Libs::LibKernel
