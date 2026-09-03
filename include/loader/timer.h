#pragma once
#include "common/common.h"
#include <chrono>
#include <string>

namespace Loader {
    class Timer {
    public:
        static Timer GetTime() { return Timer(); }
        std::string ToString(const char*) const { return "00:00:00.000"; }
    };
}
namespace Log {
    enum class Direction { Silent, Console };
    enum class Color { Cyan, Red, Green, BrightRed, BrightMagenta, BrightGreen, Yellow };
    inline Direction GetDirection() { return Direction::Console; }
}
#define LOGF_COLOR(color, fmt, ...) printf(fmt, ##__VA_ARGS__)
