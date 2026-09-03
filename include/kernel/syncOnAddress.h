#pragma once
// ProsperoLayer PS5 emulator - libkernel sync-on-address subsystem (Kyty-compatible interface)
#include "common/common.h"
#include <cstdint>

namespace Libs::LibKernel {

namespace SyncOnAddress {

int KYTY_SYSV_ABI Wait32(volatile uint32_t* address, uint32_t expected, int64_t timeout_micros,
                         void (*dispatch_signal)(void));
int KYTY_SYSV_ABI Wait64(volatile uint64_t* address, uint64_t expected, int64_t timeout_micros,
                         void (*dispatch_signal)(void));
int KYTY_SYSV_ABI Wake(volatile void* address, int32_t count);

} // namespace SyncOnAddress

} // namespace Libs::LibKernel
