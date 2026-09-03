#pragma once
#include "elf_types.hpp"
#include "memory/virtual_memory_manager.hpp"
#include <string>
#include <vector>

namespace PS5::Loader {

    struct LoadedSegment {
        uint64_t gva{0};
        size_t size{0};
        uint32_t prot{0};
        void* host_ptr{nullptr};
    };

    class ElfLoader {
    public:
        ElfLoader() = default;

        bool LoadExecutable(const uint8_t* raw_data, size_t data_size);
        bool LoadFromFile(const std::string& filepath);

        uint64_t GetEntryPointGva() const { return m_entry_point_gva; }
        const std::vector<LoadedSegment>& GetSegments() const { return m_segments; }

    private:
        uint64_t m_entry_point_gva{0};
        std::vector<LoadedSegment> m_segments;
    };

}
