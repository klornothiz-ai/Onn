#pragma once
// ProsperoLayer PS5 emulator - system content info interface (Kyty-compatible)
#include "common/common.h"
#include <cstdint>
#include <string>

namespace Loader {

// Returns the current game/content ID (e.g. "CUSA12345").
std::string GetSystemContentId();
// Returns the number of PlayGo chunks for the current content.
bool SystemContentGetChunksNum(uint32_t* out_chunks_num);
// Returns the PlayGo chunk size in bytes.
uint64_t GetSystemContentChunkSize();
// Returns the game title name.
std::string GetSystemContentTitle();
// Returns the path of the current content (eboot dir).
std::string GetSystemContentPath();

// Parameter (param.sfo) lookup helpers.
bool SystemContentParamSfoGetString(const std::string& key, std::string* out);
bool SystemContentParamSfoGetInt(const std::string& key, int32_t* out);
bool SystemContentParamSfoGetInt64(const std::string& key, int64_t* out);

} // namespace Loader
