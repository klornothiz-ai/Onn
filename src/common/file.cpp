// ProsperoLayer PS5 emulator - host file helpers implementation
#include "common/file.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace Common::File {

bool IsDirectoryExisting(const std::string& path) {
        std::error_code ec;
        return fs::is_directory(path, ec);
}

bool IsFileExisting(const std::string& path) {
        std::error_code ec;
        return fs::is_regular_file(path, ec);
}

bool CreateDirectories(const std::string& path) {
        std::error_code ec;
        fs::create_directories(path, ec);
        return !ec;
}

bool DeleteDirectory(const std::string& path) {
        std::error_code ec;
        fs::remove_all(path, ec);
        return !ec;
}

bool DeleteFile(const std::string& path) {
        std::error_code ec;
        fs::remove(path, ec);
        return !ec;
}

bool Rename(const std::string& old_path, const std::string& new_path) {
        std::error_code ec;
        fs::rename(old_path, new_path, ec);
        return !ec;
}

bool Copy(const std::string& src, const std::string& dst) {
        std::error_code ec;
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        return !ec;
}

uint64_t Size(const std::string& path) {
        std::error_code ec;
        const auto sz = fs::file_size(path, ec);
        return ec ? 0 : static_cast<uint64_t>(sz);
}

std::vector<std::string> GetDirEntries(const std::string& path) {
        std::vector<std::string> out;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(path, ec)) {
                out.push_back(entry.path().string());
        }
        return out;
}

std::vector<DirEntry> GetDirEntriesEx(const std::string& path) {
        std::vector<DirEntry> out;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(path, ec)) {
                DirEntry e;
                e.name          = entry.path().filename().string();
                e.is_file       = entry.is_regular_file(ec);
                e.is_directory  = entry.is_directory(ec);
                out.push_back(std::move(e));
        }
        return out;
}

std::string GetCurrentDirectory() {
        std::error_code ec;
        return fs::current_path(ec).string();
}

std::string GetUserDirectory() {
        const char* home = std::getenv("HOME");
        return home != nullptr ? std::string(home) : std::string(".");
}

std::string GetSharedDirectory() {
        return GetUserDirectory() + "/.local/share/kyty";
}

std::string GetHomeDirectory() {
        return GetUserDirectory();
}

std::string GetContentsDirectory() {
        const char* cwd = std::getenv("PWD");
        return cwd != nullptr ? std::string(cwd) : GetCurrentDirectory();
}

std::string GetHostPath(const std::string& guest_path) {
        // Guest paths under /app0 map to the contents directory; other
        // absolute guest paths pass through unchanged.
        if (guest_path.rfind("/app0/", 0) == 0) {
                return GetContentsDirectory() + guest_path.substr(5);
        }
        if (guest_path == "/app0") {
                return GetContentsDirectory();
        }
        return guest_path;
}

void GetLastAccessAndWriteTimeUTC(const std::string& path, uint64_t* access_time,
                                  uint64_t* write_time) {
        std::error_code ec;
        const auto ftime = fs::last_write_time(path, ec);
        if (ec) {
                if (access_time != nullptr) {
                        *access_time = 0;
                }
                if (write_time != nullptr) {
                        *write_time = 0;
                }
                return;
        }
        const auto sys_time =
            std::chrono::time_point_cast<std::chrono::microseconds>(ftime);
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                            sys_time.time_since_epoch())
                            .count();
        const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
        if (write_time != nullptr) {
                *write_time = static_cast<uint64_t>(us > 0 ? us : now_us);
        }
        if (access_time != nullptr) {
                *access_time = static_cast<uint64_t>(now_us);
        }
}

// ===========================================================================
// File (RAII wrapper)
// ===========================================================================

File::File(const std::string& path, Mode mode) {
        Open(path, mode);
}

File::~File() {
        Close();
}

bool File::Open(const std::string& path, Mode mode) {
        Close();
        const char* fmode = nullptr;
        switch (mode) {
        case Mode::Read:
                fmode = "rb";
                break;
        case Mode::Write:
                fmode = "wb";
                break;
        case Mode::ReadWrite:
                fmode = "r+b";
                break;
        case Mode::Append:
                fmode = "ab";
                break;
        default:
                fmode = "rb";
                break;
        }
        m_file = std::fopen(path.c_str(), fmode);
        m_open = m_file != nullptr;
        m_path = path;
        return m_open;
}

void File::Close() {
        if (m_file != nullptr) {
                std::fclose(m_file);
                m_file = nullptr;
        }
        m_open = false;
}

bool File::Read(void* buf, size_t size) {
        return m_file != nullptr && std::fread(buf, 1, size, m_file) == size;
}

bool File::Write(const void* buf, size_t size) {
        return m_file != nullptr && std::fwrite(buf, 1, size, m_file) == size;
}

int64_t File::Seek(int64_t offset, int whence) {
        if (m_file == nullptr) {
                return -1;
        }
        return std::fseek(m_file, static_cast<long>(offset), whence) == 0
                   ? static_cast<int64_t>(std::ftell(m_file))
                   : -1;
}

int64_t File::Tell() const {
        return m_file != nullptr ? static_cast<int64_t>(std::ftell(m_file)) : -1;
}

int64_t File::Size() {
        if (m_file == nullptr) {
                return -1;
        }
        const long cur = std::ftell(m_file);
        std::fseek(m_file, 0, SEEK_END);
        const long end = std::ftell(m_file);
        std::fseek(m_file, cur, SEEK_SET);
        return end;
}

bool File::Flush() {
        return m_file != nullptr && std::fflush(m_file) == 0;
}

bool File::IsFileExisting(const std::string& path) {
        std::error_code ec;
        return fs::is_regular_file(path, ec);
}

bool File::IsDirectoryExisting(const std::string& path) {
        std::error_code ec;
        return fs::is_directory(path, ec);
}

} // namespace Common::File
