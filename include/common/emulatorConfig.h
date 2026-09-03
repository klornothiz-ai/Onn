#pragma once
// ProsperoLayer PS5 emulator - emulator configuration (Kyty-compatible)
#include "common/common.h"
#include <cstdint>
#include <string>

namespace Config {

int  GetScreenWidth();
int  GetScreenHeight();
bool RenderDocEnabled();
int  GetConsoleLanguage();
std::string GetTitleId();
std::string GetContentPath();
bool IsNeoMode();
bool IsVerboseLogging();
bool CommandBufferDumpEnabled();
bool IsPm4DumpEnabled();
bool GraphicsDebugDumpEnabled();
std::string GetCommandBufferDumpFolder();
int  GetPrintfDirection();

// Where guest printf output is routed.
enum class OutputDirection : int {
        StdOut = 0,
        Silent = 1,
        File   = 2,
};

} // namespace Config
