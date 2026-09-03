#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <unordered_map>
#include <vector>

namespace PS5::Memory {

constexpr size_t PAGE_SIZE_4KB = 4096;
constexpr size_t PAGE_SIZE_2MB = 2 * 1024 * 1024;
constexpr uint64_t GUEST_ARENA_SIZE = 16ULL * 1024 * 1024 * 1024;

enum class PageProt : uint32_t {
    None = 0,
    Read = 1,
    Write = 2,
    Exec = 4,
};

struct PageEntry {
    uint64_t gva{0};
    void* hva{nullptr};
    size_t size{0};
    uint32_t prot{0};
    bool is_committed{false};
};

// Round 18: one committed guest region with its bytes -- the basis of the
// fork snapshot (a private, eager copy of the whole address space).
struct CommittedRegion {
    uint64_t gva{0};
    size_t size{0};
    uint32_t prot{0};
    std::vector<uint8_t> bytes;
};

class VirtualMemoryManager {
public:
    static VirtualMemoryManager& Instance();
    ~VirtualMemoryManager();

    bool InitializeArena(uint64_t preferred_base = 0x1000000000ULL);

    // Round 20 (direct execution): true when the arena is mapped AT the
    // guest base address (host VA == guest VA identity mapping) -- the
    // precondition DirectExecutionBackend needs to run guest code natively
    // (raw guest addresses must be valid host addresses).
    bool IsArenaIdentityMapped() const;

    // These legacy pointer conversions now reject unmapped addresses and never expose raw host pointers.
    void* GvaToHva(uint64_t gva) const;
    uint64_t HvaToGva(const void* hva) const;

    bool IsGvaMapped(uint64_t gva) const;
    bool IsGvaExecutable(uint64_t gva) const;
    bool IsGvaReadable(uint64_t gva) const;
    bool IsGvaWritable(uint64_t gva) const;
    bool IsHvaInArena(const void* hva) const;
    uint32_t GetPageProt(uint64_t gva) const;

    // Checked copies are required when a guest-controlled range crosses the host boundary.
    bool CopyFromGuest(uint64_t gva, void* destination, size_t size,
                       uint32_t required_prot = static_cast<uint32_t>(PageProt::Read)) const;
    bool CopyToGuest(uint64_t gva, const void* source, size_t size,
                     uint32_t required_prot = static_cast<uint32_t>(PageProt::Write));
    bool ZeroGuest(uint64_t gva, size_t size,
                   uint32_t required_prot = static_cast<uint32_t>(PageProt::Write));

    uint64_t AllocateVirtual(uint64_t target_gva, size_t size, uint32_t prot,
                             bool is_direct_mem = false);
    bool FreeVirtual(uint64_t gva, size_t size);
    bool ProtectVirtual(uint64_t gva, size_t size, uint32_t new_prot);

    uint64_t GetArenaBaseGva() const { return m_arena_base_gva; }
    void* GetArenaHostBase() const { return m_host_arena_base; }
    size_t GetTotalCommittedBytes() const;

    // Round 20 (direct execution): when ON, committed regions whose guest
    // protection includes the Exec bit also become host-executable (code
    // pages R+X -> host R+X), so the DirectExecutionBackend can run guest
    // code natively inside the arena. Data pages keep their exact guest R/W
    // protection (an NX fetch still faults -- correct guest semantics).
    // Existing regions are re-applied immediately; allocations and
    // mprotects after the switch honour the flag. Returns false without a
    // reserved arena.
    bool SetDirectExecutionMode(bool enabled);
    bool IsDirectExecutionMode() const { return m_direct_exec; }

    // Round 20 (direct execution): invoked (outside the VMM lock) whenever
    // guest code bytes are rewritten (CopyToGuest into an Exec page,
    // FreeVirtual / ProtectVirtual of an Exec region). The
    // DirectExecutionBackend registers one to drop its ud2 patches + block
    // discovery for the affected range -- reloaded / self-modifying code is
    // re-scanned on the next execution instead of running through stale
    // patches (a stale registry could let an unpatched `syscall` reach the
    // HOST kernel with FreeBSD numbering).
    using CodeWriteNotifier = std::function<void(uint64_t gva, size_t size)>;
    void SetCodeWriteNotifier(CodeWriteNotifier notifier) {
        std::lock_guard<std::mutex> lock(m_vmm_mutex);
        m_code_write_notifier = std::move(notifier);
    }

    // Round 18: eager copy of every committed region (contents + extent +
    // protection). Used by the fork model to give the child a fully private
    // address space; writes in the child never reach the parent's VMM.
    std::vector<CommittedRegion> SnapshotCommitted() const;

private:
    VirtualMemoryManager();

    bool CheckRangeCollisionLocked(uint64_t start_gva, size_t size) const;
    const PageEntry* FindEntryForRangeLocked(uint64_t gva, size_t size,
                                             uint32_t required_prot) const;
    PageEntry* FindEntryForRangeLocked(uint64_t gva, size_t size,
                                       uint32_t required_prot);
    bool IsRangeInArenaLocked(uint64_t gva, size_t size) const;
    int HostProtection(uint32_t guest_prot) const;

    void* m_host_arena_base{nullptr};
    uint64_t m_arena_base_gva{0x1000000000ULL};
    size_t m_arena_capacity{GUEST_ARENA_SIZE};
    size_t m_committed_bytes{0};
    bool m_direct_exec{false};
    CodeWriteNotifier m_code_write_notifier{};

    mutable std::mutex m_vmm_mutex;
    std::unordered_map<uint64_t, PageEntry> m_page_table;
};

} // namespace PS5::Memory
