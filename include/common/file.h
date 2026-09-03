#pragma once
// ProsperoLayer PS5 emulator - host file helpers (Kyty-compatible)
#include "common/common.h"
#include <cstdint>
#include <string>
#include <vector>

namespace Common::File {

bool IsDirectoryExisting(const std::string& path);
bool IsFileExisting(const std::string& path);
bool CreateDirectories(const std::string& path);
bool DeleteDirectory(const std::string& path);
bool DeleteFile(const std::string& path);
bool Rename(const std::string& old_path, const std::string& new_path);
bool Copy(const std::string& src, const std::string& dst);
uint64_t Size(const std::string& path);
std::vector<std::string> GetDirEntries(const std::string& path);

// Kyty-compatible directory entry (name + type flags).
struct DirEntry {
        std::string name;
        bool        is_file{false};
        bool        is_directory{false};
};

// Returns directory entries as structs (name, is_file, is_directory).
std::vector<DirEntry> GetDirEntriesEx(const std::string& path);
std::string GetCurrentDirectory();
std::string GetUserDirectory();
std::string GetSharedDirectory();
std::string GetHomeDirectory();
std::string GetContentsDirectory();
std::string GetHostPath(const std::string& guest_path);

// Open modes used by the guest-facing file wrapper.
enum class Mode : uint32_t {
        Read = 0,
        Write = 1,
        ReadWrite = 2,
        Append = 3,
};

// Simple RAII file wrapper used by the higher-level libraries.
class File {
public:
        File() = default;
        explicit File(const std::string& path, Mode mode);
        ~File();

        bool Open(const std::string& path, Mode mode);
        void Close();
        bool IsOpen() const { return m_open; }
        bool Read(void* buf, size_t size);
        bool Write(const void* buf, size_t size);
        int64_t Seek(int64_t offset, int whence);
        int64_t Tell() const;
        int64_t Size();
        bool Flush();
        const std::string& GetPath() const { return m_path; }

        // Kyty-compatible convenience overloads (single-argument form).
        bool Create(const std::string& path) { return Open(path, Mode::Write); }
        bool IsInvalid() const { return !m_open; }
        bool Seek(int64_t offset) { return Seek(offset, SEEK_SET) >= 0; }
        bool Read(void* buf, size_t size, uint64_t* out_read) {
                if (out_read != nullptr) {
                        *out_read = Read(buf, size) ? size : 0;
                }
                return m_open;
        }
        bool Read(void* buf, size_t size, uint32_t* out_read) {
                if (out_read != nullptr) {
                        *out_read = Read(buf, size) ? static_cast<uint32_t>(size) : 0;
                }
                return m_open;
        }
        FILE* GetHandle() const { return m_file; }

        static bool IsFileExisting(const std::string& path);
        static bool IsDirectoryExisting(const std::string& path);

private:
        std::string m_path;
        FILE*       m_file{nullptr};
        bool        m_open{false};
};

// Returns the last-access and last-write times as a unix-microseconds pair.
void GetLastAccessAndWriteTimeUTC(const std::string& path, uint64_t* access_time,
                                  uint64_t* write_time);

} // namespace Common::File
