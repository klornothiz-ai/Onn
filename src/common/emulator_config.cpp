// ProsperoLayer PS5 emulator - configuration implementation
#include "common/emulatorConfig.h"

#include <cstdlib>

namespace Config {

namespace {

struct ConfigValues {
        int         screen_width{1920};
        int         screen_height{1080};
        bool        render_doc{false};
        int         console_language{0}; // 0 = English (primary)
        std::string title_id;
        std::string content_path;
        bool        neo_mode{false};
        bool        verbose_logging{false};
        bool        cmd_buffer_dump{false};
        bool        pm4_dump{false};
        bool        gfx_debug_dump{false};
        std::string cmd_buffer_folder;
        int         printf_direction{0}; // 0 = stdout
};

ConfigValues& Values() {
        static ConfigValues v;
        return v;
}

int EnvInt(const char* name, int def) {
        const char* v = std::getenv(name);
        return v != nullptr ? std::atoi(v) : def;
}

} // namespace

int GetScreenWidth() { return Values().screen_width; }
int GetScreenHeight() { return Values().screen_height; }
bool RenderDocEnabled() { return Values().render_doc; }
int GetConsoleLanguage() { return Values().console_language; }
std::string GetTitleId() { return Values().title_id; }
std::string GetContentPath() { return Values().content_path; }
bool IsNeoMode() { return Values().neo_mode; }
bool IsVerboseLogging() { return Values().verbose_logging || EnvInt("KYTY_VERBOSE", 0) != 0; }
bool CommandBufferDumpEnabled() { return Values().cmd_buffer_dump; }
bool IsPm4DumpEnabled() { return Values().pm4_dump; }
bool GraphicsDebugDumpEnabled() { return Values().gfx_debug_dump || EnvInt("KYTY_GFX_DUMP", 0) != 0; }
std::string GetCommandBufferDumpFolder() { return Values().cmd_buffer_folder; }
int GetPrintfDirection() { return Values().printf_direction; }

} // namespace Config
