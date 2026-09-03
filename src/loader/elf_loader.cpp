#include "loader/elf_loader.hpp"

#include <fstream>
#include <iostream>
#include <limits>

namespace PS5::Loader {
namespace {

constexpr uint32_t kLoadProtection = static_cast<uint32_t>(Memory::PageProt::Read) |
                                     static_cast<uint32_t>(Memory::PageProt::Write);

} // namespace

bool ElfLoader::LoadExecutable(const uint8_t* raw_data, size_t data_size) {
    auto& vmm = Memory::VirtualMemoryManager::Instance();
    const auto reset_loaded_segments = [&] {
        for (const auto& segment : m_segments) {
            vmm.FreeVirtual(segment.gva, segment.size);
        }
        m_segments.clear();
        m_entry_point_gva = 0;
    };
    reset_loaded_segments();

    if (raw_data == nullptr || data_size < sizeof(Elf64_Ehdr)) {
        return false;
    }

    const auto* header = reinterpret_cast<const Elf64_Ehdr*>(raw_data);
    if (header->e_ident[0] != 0x7f || header->e_ident[1] != 'E' ||
        header->e_ident[2] != 'L' || header->e_ident[3] != 'F' ||
        header->e_ident[4] != 2 || header->e_machine != 0x3e) {
        return false;
    }

    if (header->e_phoff == 0 || header->e_phnum == 0 ||
        header->e_phentsize != sizeof(Elf64_Phdr) || header->e_phoff > data_size ||
        header->e_phnum > (data_size - header->e_phoff) / sizeof(Elf64_Phdr)) {
        return false;
    }

    const auto* program_headers = reinterpret_cast<const Elf64_Phdr*>(raw_data + header->e_phoff);
    for (uint16_t index = 0; index < header->e_phnum; ++index) {
        const auto& segment = program_headers[index];
        if (segment.p_type != PT_LOAD) {
            continue;
        }
        if (segment.p_memsz == 0 || segment.p_memsz < segment.p_filesz ||
            segment.p_memsz > std::numeric_limits<size_t>::max() ||
            segment.p_offset > data_size || segment.p_filesz > data_size - segment.p_offset ||
            segment.p_vaddr % Memory::PAGE_SIZE_4KB != 0) {
            reset_loaded_segments();
            return false;
        }

        uint32_t final_protection = 0;
        if ((segment.p_flags & PF_R) != 0) {
            final_protection |= static_cast<uint32_t>(Memory::PageProt::Read);
        }
        if ((segment.p_flags & PF_W) != 0) {
            final_protection |= static_cast<uint32_t>(Memory::PageProt::Write);
        }
        if ((segment.p_flags & PF_X) != 0) {
            // The interpreter fetches through the checked read path, so executable guest code is readable.
            final_protection |= static_cast<uint32_t>(Memory::PageProt::Read);
            final_protection |= static_cast<uint32_t>(Memory::PageProt::Exec);
        }

        // Load through a writable guest mapping, then enforce the guest's final permissions.
        const uint64_t gva = vmm.AllocateVirtual(segment.p_vaddr, segment.p_memsz, kLoadProtection);
        if (gva == 0 ||
            (segment.p_filesz != 0 &&
             !vmm.CopyToGuest(gva, raw_data + segment.p_offset,
                              static_cast<size_t>(segment.p_filesz))) ||
            (segment.p_memsz > segment.p_filesz &&
             !vmm.ZeroGuest(gva + segment.p_filesz,
                            static_cast<size_t>(segment.p_memsz - segment.p_filesz))) ||
            !vmm.ProtectVirtual(gva, static_cast<size_t>(segment.p_memsz), final_protection)) {
            if (gva != 0) {
                vmm.FreeVirtual(gva, static_cast<size_t>(segment.p_memsz));
            }
            reset_loaded_segments();
            return false;
        }

        m_segments.push_back({
            .gva = gva,
            .size = static_cast<size_t>(segment.p_memsz),
            .prot = final_protection,
            .host_ptr = vmm.GvaToHva(gva),
        });
    }

    bool entry_in_executable_segment = false;
    for (const auto& segment : m_segments) {
        const uint64_t entry_offset = header->e_entry >= segment.gva
            ? header->e_entry - segment.gva
            : std::numeric_limits<uint64_t>::max();
        if ((segment.prot & static_cast<uint32_t>(Memory::PageProt::Exec)) != 0 &&
            entry_offset < segment.size) {
            entry_in_executable_segment = true;
            break;
        }
    }
    if (m_segments.empty() || !entry_in_executable_segment) {
        reset_loaded_segments();
        return false;
    }

    m_entry_point_gva = header->e_entry;
    return true;
}

bool ElfLoader::LoadFromFile(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return false;
    }

    const std::streamsize file_size = file.tellg();
    if (file_size <= 0 || static_cast<uint64_t>(file_size) > std::numeric_limits<size_t>::max()) {
        return false;
    }

    std::vector<uint8_t> buffer(static_cast<size_t>(file_size));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), file_size)) {
        return false;
    }
    return LoadExecutable(buffer.data(), buffer.size());
}

} // namespace PS5::Loader
