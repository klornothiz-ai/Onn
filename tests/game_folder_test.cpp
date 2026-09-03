#include "loader/game_folder.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
    const auto root = std::filesystem::temp_directory_path() / "prospero-folder-test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "data", ec);
    assert(!ec);
    {
        std::ofstream elf(root / "eboot.bin", std::ios::binary);
        const char magic[] = {static_cast<char>(0x7f), 'E', 'L', 'F'};
        elf.write(magic, sizeof(magic));
    }
    { std::ofstream(root / "data" / "resource.dat") << "resource"; }

    PS5::Loader::GameFolder folder;
    std::string error;
    assert(PS5::Loader::GameFolderScanner::Scan(root, folder, error));
    assert(folder.root == std::filesystem::weakly_canonical(root));
    assert(folder.executable.filename() == "eboot.bin");
    assert(folder.resources.size() == 1);

    std::filesystem::remove_all(root, ec);
    std::cout << "game folder compatibility test passed\n";
}
