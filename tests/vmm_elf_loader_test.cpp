#include "loader/elf_loader.hpp"
#include "memory/virtual_memory_manager.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

using PS5::Loader::Elf64_Ehdr;
using PS5::Loader::Elf64_Phdr;
using PS5::Loader::ElfLoader;
using PS5::Memory::PageProt;
using PS5::Memory::VirtualMemoryManager;

bool Check(bool value, const char* expression, int line) {
    if (!value) {
        std::cerr << "check failed at line " << line << ": " << expression << '\n';
    }
    return value;
}

#define CHECK(expression) \
    do { \
        if (!Check((expression), #expression, __LINE__)) return false; \
    } while (false)

uint32_t Prot(PageProt value) {
    return static_cast<uint32_t>(value);
}

bool CheckedCopiesRejectUnmappedRanges() {
    auto& vmm = VirtualMemoryManager::Instance();
    const uint64_t base = 0x1000800000ULL;
    const uint32_t read_write = Prot(PageProt::Read) | Prot(PageProt::Write);
    CHECK(vmm.AllocateVirtual(base, 4096, read_write) == base);

    const std::array<uint8_t, 4> input = {1, 2, 3, 4};
    std::array<uint8_t, 4> output{};
    CHECK(vmm.CopyToGuest(base, input.data(), input.size()));
    CHECK(vmm.CopyFromGuest(base, output.data(), output.size()));
    CHECK(output == input);
    CHECK(vmm.GvaToHva(base - 1) == nullptr);
    // Round 20: the arena is IDENTITY-mapped (host VA == guest VA), so a
    // GVA-looking pointer IS a valid host address now. The rejection check
    // needs a pointer that is genuinely outside the arena: a stack address.
    uint64_t stack_addr = 0;
    CHECK(vmm.HvaToGva(&stack_addr) == 0);
    CHECK(vmm.HvaToGva(reinterpret_cast<void*>(0x7F0000000000ULL)) == 0);
    CHECK(!vmm.CopyFromGuest(base + 4094, output.data(), output.size()));
    CHECK(vmm.FreeVirtual(base, 4096));

    const uint64_t read_only_base = base + 4096;
    CHECK(vmm.AllocateVirtual(read_only_base, 4096, Prot(PageProt::Read)) == read_only_base);
    CHECK(!vmm.CopyToGuest(read_only_base, input.data(), input.size(), Prot(PageProt::Read)));
    CHECK(!vmm.ZeroGuest(read_only_base, input.size(), Prot(PageProt::Read)));
    CHECK(vmm.FreeVirtual(read_only_base, 4096));
    return true;
}

bool ElfLoaderLoadsRxSegmentWithoutHostExecution() {
    constexpr uint64_t kSegmentGva = 0x1000900000ULL;
    constexpr uint64_t kEntryGva = kSegmentGva;
    constexpr size_t kPayloadOffset = 0x100;
    constexpr std::array<uint8_t, 3> kPayload = {0x90, 0x90, 0xc3};

    std::vector<uint8_t> image(kPayloadOffset + kPayload.size());
    auto* header = reinterpret_cast<Elf64_Ehdr*>(image.data());
    header->e_ident[0] = 0x7f;
    header->e_ident[1] = 'E';
    header->e_ident[2] = 'L';
    header->e_ident[3] = 'F';
    header->e_ident[4] = 2;
    header->e_machine = 0x3e;
    header->e_entry = kEntryGva;
    header->e_phoff = sizeof(Elf64_Ehdr);
    header->e_phentsize = sizeof(Elf64_Phdr);
    header->e_phnum = 1;

    auto* segment = reinterpret_cast<Elf64_Phdr*>(image.data() + header->e_phoff);
    segment->p_type = PS5::Loader::PT_LOAD;
    segment->p_flags = PS5::Loader::PF_R | PS5::Loader::PF_X;
    segment->p_offset = kPayloadOffset;
    segment->p_vaddr = kSegmentGva;
    segment->p_filesz = kPayload.size();
    segment->p_memsz = 32;
    std::memcpy(image.data() + kPayloadOffset, kPayload.data(), kPayload.size());

    ElfLoader loader;
    CHECK(loader.LoadExecutable(image.data(), image.size()));
    CHECK(loader.GetEntryPointGva() == kEntryGva);
    CHECK(loader.GetSegments().size() == 1);

    auto& vmm = VirtualMemoryManager::Instance();
    std::array<uint8_t, 32> loaded{};
    const uint32_t required = Prot(PageProt::Read) | Prot(PageProt::Exec);
    CHECK(vmm.CopyFromGuest(kSegmentGva, loaded.data(), loaded.size(), required));
    CHECK(std::memcmp(loaded.data(), kPayload.data(), kPayload.size()) == 0);
    for (size_t index = kPayload.size(); index < loaded.size(); ++index) {
        CHECK(loaded[index] == 0);
    }
    CHECK(!vmm.CopyToGuest(kSegmentGva, kPayload.data(), kPayload.size()));
    CHECK(!vmm.CopyToGuest(kSegmentGva, kPayload.data(), kPayload.size(), Prot(PageProt::Read)));
    CHECK(vmm.FreeVirtual(kSegmentGva, 32));
    return true;
}

bool ElfLoaderRejectsOutOfBoundsSegment() {
    std::vector<uint8_t> image(sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr));
    auto* header = reinterpret_cast<Elf64_Ehdr*>(image.data());
    header->e_ident[0] = 0x7f;
    header->e_ident[1] = 'E';
    header->e_ident[2] = 'L';
    header->e_ident[3] = 'F';
    header->e_ident[4] = 2;
    header->e_machine = 0x3e;
    header->e_phoff = sizeof(Elf64_Ehdr);
    header->e_phentsize = sizeof(Elf64_Phdr);
    header->e_phnum = 1;

    auto* segment = reinterpret_cast<Elf64_Phdr*>(image.data() + header->e_phoff);
    segment->p_type = PS5::Loader::PT_LOAD;
    segment->p_flags = PS5::Loader::PF_R | PS5::Loader::PF_X;
    segment->p_offset = image.size() - 1;
    segment->p_filesz = 4;
    segment->p_memsz = 4;
    segment->p_vaddr = 0x1000a00000ULL;

    ElfLoader loader;
    CHECK(!loader.LoadExecutable(image.data(), image.size()));
    return true;
}

bool ElfLoaderRejectsEntryOutsideExecutableSegment() {
    constexpr uint64_t kSegmentGva = 0x1000b00000ULL;
    std::vector<uint8_t> image(0x101U);
    auto* header = reinterpret_cast<Elf64_Ehdr*>(image.data());
    header->e_ident[0] = 0x7f;
    header->e_ident[1] = 'E';
    header->e_ident[2] = 'L';
    header->e_ident[3] = 'F';
    header->e_ident[4] = 2;
    header->e_machine = 0x3e;
    header->e_entry = kSegmentGva + 128U;
    header->e_phoff = sizeof(Elf64_Ehdr);
    header->e_phentsize = sizeof(Elf64_Phdr);
    header->e_phnum = 1;

    auto* segment = reinterpret_cast<Elf64_Phdr*>(image.data() + header->e_phoff);
    segment->p_type = PS5::Loader::PT_LOAD;
    segment->p_flags = PS5::Loader::PF_R | PS5::Loader::PF_X;
    segment->p_offset = 0x100;
    segment->p_filesz = 1;
    segment->p_memsz = 1;
    segment->p_vaddr = kSegmentGva;
    image[0x100] = 0xc3;

    ElfLoader loader;
    CHECK(!loader.LoadExecutable(image.data(), image.size()));
    return true;
}

struct TestCase {
    const char* name;
    bool (*function)();
};

} // namespace

int main() {
    const TestCase tests[] = {
        {"CheckedCopiesRejectUnmappedRanges", CheckedCopiesRejectUnmappedRanges},
        {"ElfLoaderLoadsRxSegmentWithoutHostExecution", ElfLoaderLoadsRxSegmentWithoutHostExecution},
        {"ElfLoaderRejectsOutOfBoundsSegment", ElfLoaderRejectsOutOfBoundsSegment},
        {"ElfLoaderRejectsEntryOutsideExecutableSegment", ElfLoaderRejectsEntryOutsideExecutableSegment},
    };

    size_t passed = 0;
    for (const auto& test : tests) {
        const bool success = test.function();
        std::cout << (success ? "[PASS] " : "[FAIL] ") << test.name << '\n';
        passed += success ? 1 : 0;
    }
    std::cout << passed << '/' << std::size(tests) << " tests passed\n";
    return passed == std::size(tests) ? 0 : 1;
}
