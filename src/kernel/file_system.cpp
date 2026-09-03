// ProsperoLayer PS5 emulator - libkernel file system subsystem implementation
#include "kernel/fileSystem.h"
#include "common/file.h"
#include "common/logging/log.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Libs::LibKernel {

namespace FileSystem {

namespace {

// Maps guest mount points (e.g. "/app0") to host directories.
std::unordered_map<std::string, std::string> g_mounts;
std::mutex                                   g_mounts_mutex;

// Guest fd table (small, host-backed).
std::mutex                       g_fd_mutex;
std::unordered_map<int32_t, int> g_fd_map;
int32_t                          g_next_fd = 3;

int32_t AllocateFd(int host_fd) {
        if (host_fd < 0) {
                return -1;
        }
        std::lock_guard<std::mutex> lock(g_fd_mutex);
        const int32_t fd = g_next_fd++;
        g_fd_map[fd]     = host_fd;
        return fd;
}

int HostFd(int32_t fd) {
        std::lock_guard<std::mutex> lock(g_fd_mutex);
        const auto it = g_fd_map.find(fd);
        return it != g_fd_map.end() ? it->second : -1;
}

void ReleaseFd(int32_t fd) {
        std::lock_guard<std::mutex> lock(g_fd_mutex);
        g_fd_map.erase(fd);
}

std::string ResolvePath(const std::string& path) {
        std::lock_guard<std::mutex> lock(g_mounts_mutex);
        for (const auto& [guest_prefix, host_dir] : g_mounts) {
                if (path.rfind(guest_prefix, 0) == 0) {
                        std::string rest = path.substr(guest_prefix.size());
                        if (!rest.empty() && rest[0] == '/') {
                                rest.erase(rest.begin());
                        }
                        return host_dir + "/" + rest;
                }
        }
        return path;
}

} // namespace

// Round 28: the REAL mount-point resolver, exposed through the public
// header. Guest paths like "/savedata0/PARAM.bin" are translated against the
// g_mounts table (registered by FileSystem::Mount / KernelMount) into host
// paths; unmounted paths pass through unchanged (same rule the internal
// ResolvePath applies).
std::string GetRealFilename(const std::string& path) {
        return ResolvePath(path);
}

int KYTY_SYSV_ABI KernelOpen(const char* path, int flags, uint16_t mode, int32_t* fd) {
        if (path == nullptr || fd == nullptr) {
                return static_cast<int>(0x80020002); // EINVAL-ish
        }
        const std::string host = ResolvePath(path);
        int host_flags = O_RDONLY;
        if ((flags & 0x1) != 0) {
                host_flags = O_WRONLY;
        }
        if ((flags & 0x2) != 0) {
                host_flags = O_RDWR;
        }
        if ((flags & 0x200) != 0) {
                host_flags |= O_CREAT;
        }
        if ((flags & 0x400) != 0) {
                host_flags |= O_TRUNC;
        }
        if ((flags & 0x800) != 0) {
                host_flags |= O_APPEND;
        }
        const int host_fd = ::open(host.c_str(), host_flags, mode != 0 ? mode : 0644);
        if (host_fd < 0) {
                return -errno;
        }
        *fd = AllocateFd(host_fd);
        return *fd >= 0 ? OK : -1;
}

int KYTY_SYSV_ABI KernelClose(int32_t fd) {
        const int host_fd = HostFd(fd);
        if (host_fd < 0) {
                return static_cast<int>(0x80020009); // EBADF-ish
        }
        ::close(host_fd);
        ReleaseFd(fd);
        return OK;
}

int KYTY_SYSV_ABI KernelRead(int32_t fd, void* buf, size_t nbytes) {
        const int host_fd = HostFd(fd);
        if (host_fd < 0) {
                return -1;
        }
        const ssize_t n = ::read(host_fd, buf, nbytes);
        return n < 0 ? -errno : static_cast<int>(n);
}

int KYTY_SYSV_ABI KernelWrite(int32_t fd, const void* buf, size_t nbytes) {
        const int host_fd = HostFd(fd);
        if (host_fd < 0) {
                return -1;
        }
        const ssize_t n = ::write(host_fd, buf, nbytes);
        return n < 0 ? -errno : static_cast<int>(n);
}

int64_t KYTY_SYSV_ABI KernelLseek(int32_t fd, int64_t offset, int whence) {
        const int host_fd = HostFd(fd);
        if (host_fd < 0) {
                return -1;
        }
        const off_t r = ::lseek(host_fd, static_cast<off_t>(offset), whence);
        return r < 0 ? -errno : static_cast<int64_t>(r);
}

int KYTY_SYSV_ABI KernelStat(const char* path, FileStat* sb) {
        if (path == nullptr || sb == nullptr) {
                return static_cast<int>(0x80020002);
        }
        const std::string host = ResolvePath(path);
        struct stat st {};
        if (::stat(host.c_str(), &st) != 0) {
                return -errno;
        }
        *sb = FileStat{};
        sb->st_dev     = static_cast<uint32_t>(st.st_dev);
        sb->st_mode    = static_cast<uint16_t>(st.st_mode);
        sb->st_uid     = static_cast<uint16_t>(st.st_uid);
        sb->st_gid     = static_cast<uint16_t>(st.st_gid);
        sb->st_size    = static_cast<uint32_t>(st.st_size);
        sb->st_blksize = static_cast<uint32_t>(st.st_blksize);
        sb->st_blocks  = static_cast<uint32_t>(st.st_blocks);
        sb->st_atime   = static_cast<uint32_t>(st.st_atime);
        sb->st_mtime   = static_cast<uint32_t>(st.st_mtime);
        sb->st_ctime   = static_cast<uint32_t>(st.st_ctime);
        sb->st_atim.tv_sec  = st.st_atim.tv_sec;
        sb->st_atim.tv_nsec = st.st_atim.tv_nsec;
        sb->st_mtim.tv_sec  = st.st_mtim.tv_sec;
        sb->st_mtim.tv_nsec = st.st_mtim.tv_nsec;
        sb->st_ctim.tv_sec  = st.st_ctim.tv_sec;
        sb->st_ctim.tv_nsec = st.st_ctim.tv_nsec;
        return OK;
}

int KYTY_SYSV_ABI KernelFstat(int32_t fd, FileStat* sb) {
        const int host_fd = HostFd(fd);
        if (host_fd < 0 || sb == nullptr) {
                return static_cast<int>(0x80020009);
        }
        struct stat st {};
        if (::fstat(host_fd, &st) != 0) {
                return -errno;
        }
        *sb = FileStat{};
        sb->st_dev     = static_cast<uint32_t>(st.st_dev);
        sb->st_mode    = static_cast<uint16_t>(st.st_mode);
        sb->st_size    = static_cast<uint32_t>(st.st_size);
        sb->st_blksize = static_cast<uint32_t>(st.st_blksize);
        sb->st_blocks  = static_cast<uint32_t>(st.st_blocks);
        sb->st_atime   = static_cast<uint32_t>(st.st_atime);
        sb->st_mtime   = static_cast<uint32_t>(st.st_mtime);
        return OK;
}

int KYTY_SYSV_ABI KernelMkdir(const char* path, uint16_t mode) {
        if (path == nullptr) {
                return static_cast<int>(0x80020002);
        }
        const std::string host = ResolvePath(path);
        if (::mkdir(host.c_str(), mode != 0 ? mode : 0755) != 0 && errno != EEXIST) {
                return -errno;
        }
        return OK;
}

int KYTY_SYSV_ABI KernelUnlink(const char* path) {
        if (path == nullptr) {
                return static_cast<int>(0x80020002);
        }
        const std::string host = ResolvePath(path);
        if (::unlink(host.c_str()) != 0) {
                return -errno;
        }
        return OK;
}

int KYTY_SYSV_ABI KernelRmdir(const char* path) {
        if (path == nullptr) {
                return static_cast<int>(0x80020002);
        }
        const std::string host = ResolvePath(path);
        if (::rmdir(host.c_str()) != 0) {
                return -errno;
        }
        return OK;
}

int KYTY_SYSV_ABI KernelRename(const char* old_path, const char* new_path) {
        if (old_path == nullptr || new_path == nullptr) {
                return static_cast<int>(0x80020002);
        }
        if (::rename(ResolvePath(old_path).c_str(), ResolvePath(new_path).c_str()) != 0) {
                return -errno;
        }
        return OK;
}

int KYTY_SYSV_ABI KernelMount(const char* device, const char* mount_point, uint64_t flags,
                              const void* data, uint32_t data_len) {
        (void)device;
        (void)flags;
        (void)data;
        (void)data_len;
        if (mount_point == nullptr) {
                return static_cast<int>(0x80020002);
        }
        std::lock_guard<std::mutex> lock(g_mounts_mutex);
        g_mounts[std::string(mount_point)] = device != nullptr ? device : "";
        return OK;
}

int KYTY_SYSV_ABI KernelUmount(const char* mount_point) {
        if (mount_point == nullptr) {
                return static_cast<int>(0x80020002);
        }
        std::lock_guard<std::mutex> lock(g_mounts_mutex);
        g_mounts.erase(std::string(mount_point));
        return OK;
}

int KYTY_SYSV_ABI Mount(const std::string& device, const std::string& mount_point) {
        return KernelMount(device.c_str(), mount_point.c_str(), 0, nullptr, 0);
}

int KYTY_SYSV_ABI Umount(const std::string& mount_point) {
        return KernelUmount(mount_point.c_str());
}

int KYTY_SYSV_ABI KernelGetdirentries(int32_t fd, void* buf, size_t nbytes, int64_t* basep) {
        (void)fd;
        (void)buf;
        (void)nbytes;
        (void)basep;
        return 0;
}

int KYTY_SYSV_ABI KernelGetdents(int32_t fd, void* buf, size_t nbytes) {
        (void)fd;
        (void)buf;
        (void)nbytes;
        return 0;
}

int64_t KYTY_SYSV_ABI KernelPread(int32_t fd, void* buf, size_t nbytes, int64_t offset) {
        const int host_fd = HostFd(fd);
        if (host_fd < 0) {
                return -1;
        }
        const ssize_t n = ::pread(host_fd, buf, nbytes, static_cast<off_t>(offset));
        return n < 0 ? -errno : static_cast<int64_t>(n);
}

int64_t KYTY_SYSV_ABI KernelPwrite(int32_t fd, const void* buf, size_t nbytes, int64_t offset) {
        const int host_fd = HostFd(fd);
        if (host_fd < 0) {
                return -1;
        }
        const ssize_t n = ::pwrite(host_fd, buf, nbytes, static_cast<off_t>(offset));
        return n < 0 ? -errno : static_cast<int64_t>(n);
}

int KYTY_SYSV_ABI KernelCheckReachability(const char* path, int mode) {
        (void)mode;
        if (path == nullptr) {
                return static_cast<int>(0x80020002);
        }
        const std::string host = ResolvePath(path);
        return ::access(host.c_str(), F_OK) == 0 ? OK : -errno;
}

} // namespace FileSystem

} // namespace Libs::LibKernel
