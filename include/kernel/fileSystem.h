#pragma once
// ProsperoLayer PS5 emulator - libkernel file system subsystem (Kyty-compatible interface)
#include "common/common.h"
#include <cstdint>
#include <cstddef>
#include <string>

namespace Libs::LibKernel {

namespace FileSystem {

struct FileStat {
        uint32_t st_dev;
        uint16_t st_mode;
        uint16_t st_uid;
        uint16_t st_gid;
        uint32_t st_size;
        uint32_t st_blksize;
        uint32_t st_blocks;
        uint32_t st_atime;
        uint32_t st_mtime;
        uint32_t st_ctime;

        // Kyty-compatible nanosecond timestamps (used by libAmpr).
        struct Timespec {
                int64_t tv_sec;
                int64_t tv_nsec;
        };
        Timespec st_atim{};
        Timespec st_mtim{};
        Timespec st_ctim{};
        Timespec st_birthtim{};
};

struct DirEntry {
        std::string name;
        uint32_t    type; // 0 = file, 1 = directory
};

int KYTY_SYSV_ABI KernelOpen(const char* path, int flags, uint16_t mode, int32_t* fd);
int KYTY_SYSV_ABI KernelClose(int32_t fd);
int KYTY_SYSV_ABI KernelRead(int32_t fd, void* buf, size_t nbytes);
int KYTY_SYSV_ABI KernelWrite(int32_t fd, const void* buf, size_t nbytes);
int64_t KYTY_SYSV_ABI KernelLseek(int32_t fd, int64_t offset, int whence);
int64_t KYTY_SYSV_ABI KernelPread(int32_t fd, void* buf, size_t nbytes, int64_t offset);
int64_t KYTY_SYSV_ABI KernelPwrite(int32_t fd, const void* buf, size_t nbytes, int64_t offset);
int KYTY_SYSV_ABI KernelStat(const char* path, FileStat* sb);
int KYTY_SYSV_ABI KernelFstat(int32_t fd, FileStat* sb);
int KYTY_SYSV_ABI KernelMkdir(const char* path, uint16_t mode);
int KYTY_SYSV_ABI KernelFtruncate(int32_t fd, int64_t length);
int KYTY_SYSV_ABI KernelFsync(int32_t fd);
int KYTY_SYSV_ABI KernelSync();
int KYTY_SYSV_ABI KernelRmdir(const char* path);
int KYTY_SYSV_ABI KernelUnlink(const char* path);
int KYTY_SYSV_ABI KernelRename(const char* old_path, const char* new_path);
int KYTY_SYSV_ABI KernelMount(const char* device, const char* mount_point, uint64_t flags,
                              const void* data, uint32_t data_len);
int KYTY_SYSV_ABI KernelUmount(const char* mount_point);
int KYTY_SYSV_ABI KernelGetdirentries(int32_t fd, void* buf, size_t nbytes, int64_t* basep);
int KYTY_SYSV_ABI KernelGetdents(int32_t fd, void* buf, size_t nbytes);
int KYTY_SYSV_ABI KernelCheckReachability(const char* path, int mode);

std::string GetRealFilename(const std::string& path);
int KYTY_SYSV_ABI Mount(const std::string& device, const std::string& mount_point);
int KYTY_SYSV_ABI Umount(const std::string& mount_point);

} // namespace FileSystem

} // namespace Libs::LibKernel