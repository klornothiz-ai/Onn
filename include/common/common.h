#pragma once
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <memory>
#include <iostream>
#include <cstring>
#include <algorithm>

#define KYTY_PLATFORM_LINUX 1
#define KYTY_PLATFORM_WINDOWS 2
#define KYTY_PLATFORM_MACOS 3
#define KYTY_PLATFORM KYTY_PLATFORM_LINUX

#define KYTY_PROJECT_EMULATOR 1
#define KYTY_PROJECT KYTY_PROJECT_EMULATOR

#define KYTY_SYSV_ABI
#define KYTY_MS_ABI
#define KYTY_RESTRICT __restrict__

#define LOGF(fmt, ...) printf(fmt, ##__VA_ARGS__)

#ifndef OK
#endif

using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
using int8 = int8_t;
using int16 = int16_t;
using int32 = int32_t;
using int64 = int64_t;
using uptr = uintptr_t;
using sptr = intptr_t;

namespace Kyty::Core {
    using String = std::string;
}
namespace Kyty {
    using String = std::string;
    using Byte = uint8_t;
}

// Scheduler / cross-thread signaling helper.
namespace Common {
class CondVar {
public:
        // Wakes a thread sleeping on a host-side condition variable.
        static void SignalThread(uint64_t thread_id);
};
} // namespace Common
