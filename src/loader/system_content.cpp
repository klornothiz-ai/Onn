// ProsperoLayer PS5 emulator - system content (param.sfo) helpers
#include "loader/systemContent.h"

#include <fstream>
#include <sstream>

namespace Loader {

static const char* k_content_id = "CUSA00000";
static const char* k_title_id   = "CUSA00000";
[[maybe_unused]] static const char* k_title_id_keep = k_title_id;
static const char* k_title      = "ProsperoLayer Homebrew";

std::string GetSystemContentId() {
        return k_content_id;
}

std::string GetSystemContentTitle() {
        return k_title;
}

std::string GetSystemContentPath() {
        return "/app0";
}

bool SystemContentGetChunksNum(uint32_t* out_chunks_num) {
        if (out_chunks_num == nullptr) {
                return false;
        }
        *out_chunks_num = 1;
        return true;
}

uint64_t GetSystemContentChunkSize() {
        return 32ull * 1024ull * 1024ull;
}

bool SystemContentParamSfoGetString(const std::string& key, std::string* out) {
        if (out == nullptr) {
                return false;
        }
        if (key == "CONTENT_ID" || key == "TITLE_ID") {
                *out = k_content_id;
                return true;
        }
        if (key == "TITLE") {
                *out = k_title;
                return true;
        }
        return false;
}

bool SystemContentParamSfoGetInt(const std::string& key, int32_t* out) {
        if (out == nullptr) {
                return false;
        }
        if (key == "FORMAT" || key == "ATTRIBUTE" || key == "PARENTAL_LEVEL") {
                *out = 0;
                return true;
        }
        return false;
}

bool SystemContentParamSfoGetInt64(const std::string& key, int64_t* out) {
        if (out == nullptr) {
                return false;
        }
        if (key == "CONTENT_SIZE" || key == "APP_VER") {
                *out = 0;
                return true;
        }
        return false;
}

} // namespace Loader
