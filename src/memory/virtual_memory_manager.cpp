#include "memory/virtual_memory_manager.hpp"

#include <cstring>
#include <iostream>
#include <limits>

namespace PS5::Memory {

VirtualMemoryManager& VirtualMemoryManager::Instance() {
    static VirtualMemoryManager instance;
    return instance;
}

VirtualMemoryManager::VirtualMemoryManager() {
    InitializeArena();
}

VirtualMemoryManager::~VirtualMemoryManager() {
    if (m_host_arena_base != nullptr && m_host_arena_base != MAP_FAILED) {
        munmap(m_host_arena_base, m_arena_capacity);
    }
}

bool VirtualMemoryManager::InitializeArena(uint64_t preferred_base) {
    std::lock_guard<std::mutex> lock(m_vmm_mutex);
    if (m_host_arena_base != nullptr) {
        return true;
    }

    m_arena_base_gva = preferred_base;

    // Round 20 (direct execution): reserve the arena AT the guest base
    // address itself -- an IDENTITY mapping (host VA == guest VA) for the
    // whole 16 GB window. Guest code then runs natively on the host CPU
    // with absolute, RIP-relative, stack and FS-based addressing working
    // on raw guest addresses unchanged (the DirectExecutionBackend
    // trampoline jumps to the literal guest entry address).
    // MAP_FIXED_NOREPLACE never clobbers an existing host mapping; if the
    // guest base is occupied (exotic host layout) we fall back to a
    // kernel-chosen base and direct execution DECLINES (fail-closed).
    m_host_arena_base = mmap(reinterpret_cast<void*>(preferred_base),
                             m_arena_capacity, PROT_NONE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE |
                                 MAP_FIXED_NOREPLACE,
                             -1, 0);
    if (m_host_arena_base == MAP_FAILED) {
        m_host_arena_base = mmap(nullptr, m_arena_capacity, PROT_NONE,
                                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                                 -1, 0);
    }
    if (m_host_arena_base == MAP_FAILED) {
        m_host_arena_base = nullptr;
        std::cerr << "[VMM] Failed to reserve guest memory arena.\n";
        return false;
    }
    return true;
}

bool VirtualMemoryManager::IsArenaIdentityMapped() const {
    std::lock_guard<std::mutex> lock(m_vmm_mutex);
    return m_host_arena_base != nullptr &&
           m_host_arena_base == reinterpret_cast<void*>(m_arena_base_gva);
}

bool VirtualMemoryManager::IsRangeInArenaLocked(uint64_t gva, size_t size) const {
    if (m_host_arena_base == nullptr || size == 0 || gva < m_arena_base_gva) {
        return false;
    }
    const uint64_t offset = gva - m_arena_base_gva;
    return offset <= m_arena_capacity && size <= m_arena_capacity - offset;
}

const PageEntry* VirtualMemoryManager::FindEntryForRangeLocked(uint64_t gva, size_t size,
                                                                 uint32_t required_prot) const {
    if (!IsRangeInArenaLocked(gva, size)) {
        return nullptr;
    }
    for (const auto& [base, entry] : m_page_table) {
        if (!entry.is_committed || gva < base) {
            continue;
        }
        const uint64_t offset = gva - base;
        if (offset > entry.size || size > entry.size - offset) {
            continue;
        }
        if ((entry.prot & required_prot) != required_prot) {
            return nullptr;
        }
        return &entry;
    }
    return nullptr;
}

PageEntry* VirtualMemoryManager::FindEntryForRangeLocked(uint64_t gva, size_t size,
                                                           uint32_t required_prot) {
    return const_cast<PageEntry*>(static_cast<const VirtualMemoryManager*>(this)
                                      ->FindEntryForRangeLocked(gva, size, required_prot));
}

int VirtualMemoryManager::HostProtection(uint32_t guest_prot) const {
    int protection = PROT_NONE;
    if ((guest_prot & static_cast<uint32_t>(PageProt::Read)) != 0) {
        protection |= PROT_READ;
    }
    if ((guest_prot & static_cast<uint32_t>(PageProt::Write)) != 0) {
        protection |= PROT_WRITE;
    }
    // Guest execute permission maps to a real host EXEC bit only while the
    // direct-execution mode is on (round 20): the interpreter never needs it,
    // the DirectExecutionBackend cannot live without it.
    if (m_direct_exec && (guest_prot & static_cast<uint32_t>(PageProt::Exec)) != 0) {
        protection |= PROT_EXEC;
    }
    return protection;
}

// Round 20: flip the arena's host-executable bit and re-apply protections to
// every committed region.
bool VirtualMemoryManager::SetDirectExecutionMode(bool enabled) {
    std::lock_guard<std::mutex> lock(m_vmm_mutex);
    if (m_host_arena_base == nullptr || m_host_arena_base == MAP_FAILED) {
        return false;
    }
    if (m_direct_exec == enabled) {
        return true;
    }
    m_direct_exec = enabled;
    for (auto& [base, entry] : m_page_table) {
        if (!entry.is_committed || entry.hva == nullptr) {
            continue;
        }
        mprotect(entry.hva, entry.size, HostProtection(entry.prot));
    }
    return true;
}

bool VirtualMemoryManager::IsGvaMapped(uint64_t gva) const {
    std::lock_guard<std::mutex> lock(m_vmm_mutex);
    return FindEntryForRangeLocked(gva, 1, 0) != nullptr;
}

uint32_t VirtualMemoryManager::GetPageProt(uint64_t gva) const {
    std::lock_guard<std::mutex> lock(m_vmm_mutex);
    const auto* entry = FindEntryForRangeLocked(gva, 1, 0);
    return entry == nullptr ? 0 : entry->prot;
}

bool VirtualMemoryManager::IsGvaExecutable(uint64_t gva) const {
    return (GetPageProt(gva) & static_cast<uint32_t>(PageProt::Exec)) != 0;
}

bool VirtualMemoryManager::IsGvaReadable(uint64_t gva) const {
    return (GetPageProt(gva) & static_cast<uint32_t>(PageProt::Read)) != 0;
}

bool VirtualMemoryManager::IsGvaWritable(uint64_t gva) const {
    return (GetPageProt(gva) & static_cast<uint32_t>(PageProt::Write)) != 0;
}

bool VirtualMemoryManager::IsHvaInArena(const void* hva) const {
    if (m_host_arena_base == nullptr || hva == nullptr) {
        return false;
    }
    const auto* base = static_cast<const uint8_t*>(m_host_arena_base);
    const auto* pointer = static_cast<const uint8_t*>(hva);
    return pointer >= base && pointer < base + m_arena_capacity;
}

void* VirtualMemoryManager::GvaToHva(uint64_t gva) const {
    std::lock_guard<std::mutex> lock(m_vmm_mutex);
    const auto* entry = FindEntryForRangeLocked(gva, 1, 0);
    if (entry == nullptr) {
        return nullptr;
    }
    return static_cast<uint8_t*>(entry->hva) + (gva - entry->gva);
}

uint64_t VirtualMemoryManager::HvaToGva(const void* hva) const {
    if (!IsHvaInArena(hva)) {
        return 0;
    }
    const auto* base = static_cast<const uint8_t*>(m_host_arena_base);
    const auto* pointer = static_cast<const uint8_t*>(hva);
    const uint64_t gva = m_arena_base_gva + static_cast<uint64_t>(pointer - base);
    return IsGvaMapped(gva) ? gva : 0;
}

bool VirtualMemoryManager::CopyFromGuest(uint64_t gva, void* destination, size_t size,
                                          uint32_t required_prot) const {
    if (destination == nullptr || size == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_vmm_mutex);
    const uint32_t access = required_prot | static_cast<uint32_t>(PageProt::Read);
    const auto* entry = FindEntryForRangeLocked(gva, size, access);
    if (entry == nullptr) {
        return false;
    }
    std::memcpy(destination, static_cast<const uint8_t*>(entry->hva) + (gva - entry->gva), size);
    return true;
}

bool VirtualMemoryManager::CopyToGuest(uint64_t gva, const void* source, size_t size,
                                        uint32_t required_prot) {
    if (source == nullptr || size == 0) {
        return false;
    }
    bool code_touched = false;
    {
        std::lock_guard<std::mutex> lock(m_vmm_mutex);
        const uint32_t access = required_prot | static_cast<uint32_t>(PageProt::Write);
        auto* entry = FindEntryForRangeLocked(gva, size, access);
        if (entry == nullptr) {
            return false;
        }
        std::memcpy(static_cast<uint8_t*>(entry->hva) + (gva - entry->gva), source, size);
        // Round 20: a write into a page that carries the guest EXEC bit can
        // invalidate the DirectExecutionBackend's patch registry (the code
        // under its ud2 sites changed). Fire the notifier OUTSIDE the lock.
        code_touched = (entry->prot & static_cast<uint32_t>(PageProt::Exec)) != 0;
    }
    if (code_touched && m_code_write_notifier) {
        m_code_write_notifier(gva, size);
    }
    return true;
}

bool VirtualMemoryManager::ZeroGuest(uint64_t gva, size_t size, uint32_t required_prot) {
    if (size == 0) {
        return true;
    }
    std::lock_guard<std::mutex> lock(m_vmm_mutex);
    const uint32_t access = required_prot | static_cast<uint32_t>(PageProt::Write);
    auto* entry = FindEntryForRangeLocked(gva, size, access);
    if (entry == nullptr) {
        return false;
    }
    std::memset(static_cast<uint8_t*>(entry->hva) + (gva - entry->gva), 0, size);
    return true;
}

bool VirtualMemoryManager::CheckRangeCollisionLocked(uint64_t start_gva, size_t size) const {
    for (const auto& [base, entry] : m_page_table) {
        if (start_gva < base + entry.size && base < start_gva + size) {
            return true;
        }
    }
    return false;
}

uint64_t VirtualMemoryManager::AllocateVirtual(uint64_t target_gva, size_t size, uint32_t prot,
                                                bool) {
    if (size == 0 || size > std::numeric_limits<size_t>::max() - (PAGE_SIZE_4KB - 1)) {
        return 0;
    }
    const size_t aligned_size = (size + PAGE_SIZE_4KB - 1) & ~(PAGE_SIZE_4KB - 1);

    std::lock_guard<std::mutex> lock(m_vmm_mutex);
    if (m_host_arena_base == nullptr || aligned_size > m_arena_capacity) {
        return 0;
    }

    uint64_t allocated_gva = target_gva;
    if (allocated_gva == 0) {
        allocated_gva = m_arena_base_gva;
        while (IsRangeInArenaLocked(allocated_gva, aligned_size) &&
               CheckRangeCollisionLocked(allocated_gva, aligned_size)) {
            allocated_gva += PAGE_SIZE_4KB;
        }
    }

    if ((allocated_gva & (PAGE_SIZE_4KB - 1)) != 0 ||
        !IsRangeInArenaLocked(allocated_gva, aligned_size) ||
        CheckRangeCollisionLocked(allocated_gva, aligned_size)) {
        return 0;
    }

    const uint64_t offset = allocated_gva - m_arena_base_gva;
    void* host_address = static_cast<uint8_t*>(m_host_arena_base) + offset;
    if (mprotect(host_address, aligned_size, HostProtection(prot)) != 0) {
        return 0;
    }

    m_page_table.emplace(allocated_gva, PageEntry{
        .gva = allocated_gva,
        .hva = host_address,
        .size = aligned_size,
        .prot = prot,
        .is_committed = true,
    });
    m_committed_bytes += aligned_size;
    return allocated_gva;
}

bool VirtualMemoryManager::FreeVirtual(uint64_t gva, size_t size) {
    if (size == 0 || size > std::numeric_limits<size_t>::max() - (PAGE_SIZE_4KB - 1)) {
        return false;
    }
    const size_t aligned_size = (size + PAGE_SIZE_4KB - 1) & ~(PAGE_SIZE_4KB - 1);

    CodeWriteNotifier notify;
    {
        std::lock_guard<std::mutex> lock(m_vmm_mutex);
        auto it = m_page_table.find(gva);
        if (it == m_page_table.end() || aligned_size != it->second.size) {
            return false;
        }
        if (mprotect(it->second.hva, it->second.size, PROT_NONE) != 0) {
            return false;
        }
        m_committed_bytes -= it->second.size;
        // Round 20: freed pages invalidate every code patch inside them.
        if ((it->second.prot & static_cast<uint32_t>(PageProt::Exec)) != 0) {
            notify = m_code_write_notifier;
        }
        m_page_table.erase(it);
    }
    if (notify) {
        notify(gva, aligned_size);
    }
    return true;
}

bool VirtualMemoryManager::ProtectVirtual(uint64_t gva, size_t size, uint32_t new_prot) {
    if (size == 0 || size > std::numeric_limits<size_t>::max() - (PAGE_SIZE_4KB - 1)) {
        return false;
    }
    const size_t aligned_size = (size + PAGE_SIZE_4KB - 1) & ~(PAGE_SIZE_4KB - 1);

    CodeWriteNotifier notify;
    {
        std::lock_guard<std::mutex> lock(m_vmm_mutex);
        auto* entry = FindEntryForRangeLocked(gva, aligned_size, 0);
        if (entry == nullptr || gva != entry->gva || aligned_size != entry->size) {
            return false;
        }
        if (mprotect(entry->hva, entry->size, HostProtection(new_prot)) != 0) {
            return false;
        }
        const bool was_exec =
            (entry->prot & static_cast<uint32_t>(PageProt::Exec)) != 0;
        entry->prot = new_prot;
        // Round 20: a protection change on a code region invalidates the
        // direct-execution patch registry for that range.
        if (was_exec || (new_prot & static_cast<uint32_t>(PageProt::Exec)) != 0) {
            notify = m_code_write_notifier;
        }
    }
    if (notify) {
        notify(gva, aligned_size);
    }
    return true;
}

size_t VirtualMemoryManager::GetTotalCommittedBytes() const {
    std::lock_guard<std::mutex> lock(m_vmm_mutex);
    return m_committed_bytes;
}

// Round 18: eager fork snapshot.
std::vector<CommittedRegion> VirtualMemoryManager::SnapshotCommitted() const {
    std::lock_guard<std::mutex> lock(m_vmm_mutex);
    std::vector<CommittedRegion> out;
    out.reserve(m_page_table.size());
    for (const auto& [gva, entry] : m_page_table) {
        if (!entry.is_committed) {
            continue;
        }
        CommittedRegion region;
        region.gva = entry.gva;
        region.size = entry.size;
        region.prot = entry.prot;
        region.bytes.resize(entry.size);
        if (entry.hva != nullptr) {
            std::memcpy(region.bytes.data(), entry.hva, entry.size);
        }
        out.push_back(std::move(region));
    }
    return out;
}

} // namespace PS5::Memory
