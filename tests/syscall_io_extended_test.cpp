#include "cpu/prospero_syscalls.hpp"
#include "memory/virtual_memory_manager.hpp"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <iostream>

using namespace PS5::CPU;
using PS5::Memory::VirtualMemoryManager;

int main() {
    auto& vmm = VirtualMemoryManager::Instance();
    vmm.InitializeArena();
    const uint64_t base = vmm.AllocateVirtual(0, 0x4000,
        static_cast<uint32_t>(PS5::Memory::PageProt::Read) |
        static_cast<uint32_t>(PS5::Memory::PageProt::Write));
    assert(base != 0);

    auto& d = ProsperoSyscallDispatcher::Instance();

    char path[] = "/tmp/prosperolayer-syscall-io-test";
    vmm.CopyToGuest(base, path, sizeof(path), static_cast<uint32_t>(PS5::Memory::PageProt::Write));
    SyscallContext open{}; open.rax = static_cast<uint64_t>(ProsperoSyscall::SC_SYS_open);
    open.rdi = base; open.rsi = O_CREAT | O_TRUNC | O_RDWR; open.rdx = 0600;
    const int fd = static_cast<int>(d.Dispatch(open));
    assert(fd >= 0);

    const char msg[] = "prospero";
    vmm.CopyToGuest(base + 0x200, msg, sizeof(msg)-1, static_cast<uint32_t>(PS5::Memory::PageProt::Write));
    SyscallContext wr{}; wr.rax = static_cast<uint64_t>(ProsperoSyscall::SC_SYS_write);
    wr.rdi = fd; wr.rsi = base + 0x200; wr.rdx = sizeof(msg)-1;
    assert(d.Dispatch(wr) == sizeof(msg)-1);

    assert(::lseek(fd, 0, SEEK_SET) == 0);
    SyscallContext rd{}; rd.rax = static_cast<uint64_t>(ProsperoSyscall::SC_SYS_read);
    rd.rdi = fd; rd.rsi = base + 0x300; rd.rdx = sizeof(msg)-1;
    assert(d.Dispatch(rd) == sizeof(msg)-1);
    char got[8]{};
    assert(vmm.CopyFromGuest(base + 0x300, got, sizeof(got), static_cast<uint32_t>(PS5::Memory::PageProt::Read)));
    assert(std::memcmp(got, "prospero", 8) == 0);

    // pipe() now creates a real host pipe and returns both descriptors to guest memory.
    SyscallContext pipe{}; pipe.rax = static_cast<uint64_t>(ProsperoSyscall::SC_SYS_pipe); pipe.rdi = base + 0x400;
    assert(d.Dispatch(pipe) == 0);
    int fds[2]{};
    assert(vmm.CopyFromGuest(base + 0x400, fds, sizeof(fds), static_cast<uint32_t>(PS5::Memory::PageProt::Read)));
    assert(fds[0] >= 0 && fds[1] >= 0);

    SyscallContext dup{}; dup.rax = static_cast<uint64_t>(ProsperoSyscall::SC_SYS_dup); dup.rdi = fd;
    const int dupfd = static_cast<int>(d.Dispatch(dup));
    assert(dupfd >= 0 && dupfd != fd);

    SyscallContext close1{}; close1.rax = static_cast<uint64_t>(ProsperoSyscall::SC_SYS_close); close1.rdi = fds[0]; assert(d.Dispatch(close1) == 0);
    SyscallContext close2{}; close2.rax = static_cast<uint64_t>(ProsperoSyscall::SC_SYS_close); close2.rdi = fds[1]; assert(d.Dispatch(close2) == 0);
    close2.rdi = dupfd; assert(d.Dispatch(close2) == 0);
    close2.rdi = fd; assert(d.Dispatch(close2) == 0);
    ::unlink(path);
    std::cout << "syscall IO extended test passed\n";
    return 0;
}
