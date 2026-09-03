#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace PS5::Loader {

struct GameFolder {
    std::filesystem::path root;
    std::filesystem::path executable;
    std::vector<std::filesystem::path> resources;
};

class GameFolderScanner {
public:
    static bool Scan(const std::filesystem::path& root, GameFolder& out,
                     std::string& error, std::uintmax_t max_file_size = 512ULL * 1024ULL * 1024ULL);
};

} // namespace PS5::Loader
