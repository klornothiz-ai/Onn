#include "loader/game_folder.hpp"

#include "loader/elf_types.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>

namespace PS5::Loader {
namespace {
constexpr std::uint8_t kElfMagic[] = {0x7f, 'E', 'L', 'F'};

bool IsElf(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::uint8_t magic[sizeof(kElfMagic)]{};
    return stream.read(reinterpret_cast<char*>(magic), sizeof(magic)) &&
           std::equal(std::begin(magic), std::end(magic), std::begin(kElfMagic));
}
} // namespace

bool GameFolderScanner::Scan(const std::filesystem::path& root, GameFolder& out,
                             std::string& error, std::uintmax_t max_file_size) {
    out = {};
    error.clear();
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(root, ec);
    if (ec || !std::filesystem::is_directory(canonical, ec)) {
        error = "game path is not a readable directory";
        return false;
    }

    for (std::filesystem::recursive_directory_iterator it(canonical, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec) || ec) continue;
        const auto path = it->path();
        const auto size = it->file_size(ec);
        if (ec || size > max_file_size) continue;
        if (out.executable.empty() && IsElf(path)) out.executable = path;
        else out.resources.push_back(path);
    }
    if (ec) { error = "failed while scanning game directory"; return false; }
    if (out.executable.empty()) { error = "no ELF executable found in game directory"; return false; }
    out.root = canonical;
    return true;
}
} // namespace PS5::Loader
