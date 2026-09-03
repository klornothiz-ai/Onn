// ProsperoLayer PS5 emulator - shader subsystem implementation
#include "graphics/shader/shader.h"

#include <mutex>
#include <unordered_map>

namespace Libs::Graphics {

namespace {

std::mutex g_shader_mutex;
std::unordered_map<uint64_t, ShaderMappedData> g_mapped;

} // namespace

void ShaderInit() {
        std::lock_guard<std::mutex> lock(g_shader_mutex);
        g_mapped.clear();
}

void ShaderMapUserData(uint64_t base, const ShaderMappedData& map) {
        std::lock_guard<std::mutex> lock(g_shader_mutex);
        g_mapped[base] = map;
}

} // namespace Libs::Graphics
