// ============================================================================
// ProsperoLayer RDNA2 Core - ET_DYN (PIE) boot test (round 18)
// ----------------------------------------------------------------------------
// Round 11 could only boot ET_EXEC images ("ET_DYN load bias not supported
// yet"). Round 18 implements the real loader behaviour:
//
//   * the launcher picks a FREE load bias for the whole image span and
//     records it in ModuleInfo::base_addr,
//   * RelocateProgram applies R_X86_64_RELATIVE relocations relative to that
//     bias,
//   * segments map at bias + p_vaddr and execution starts at bias + e_entry.
//
// The guest program below is genuinely position-independent: it loads the
// RELOCATED pointer (base + 0x2008), computes the RUNTIME address of the same
// slot with rip-relative lea, and subtracts -- the exit code is 40 only when
// the relocation bias matches the actual mapping (at any base the loader
// chooses). Part C checks the fail-closed path for other ELF types.
// ============================================================================
#include "loader/guest_launcher.hpp"
#include "loader/runtimeLinker.h"
#include "memory/virtual_memory_manager.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

using Loader::GuestLauncher;
using Loader::RuntimeLinker;
using PS5::Memory::VirtualMemoryManager;

int g_failures = 0;
int g_checks = 0;
bool Check(bool v, const char* e, int line) {
    ++g_checks;
    if (!v) { ++g_failures; std::cerr << "  [FAIL] line " << line << ": " << e << '\n'; }
    return v;
}
#define CHECK(e) Check((e), #e, __LINE__)

// link-time layout (a real PIE puts small vaddrs here)
constexpr uint64_t kCodeVaddr = 0x1000;
constexpr uint64_t kDataVaddr = 0x2000;
constexpr uint64_t kSlotVaddr = kDataVaddr + 0;     // RELATIVE-relocated ptr
constexpr uint64_t kPointeeVaddr = kDataVaddr + 8;  // what it must point at
constexpr uint64_t kRelaVaddr = kDataVaddr + 16;    // one Elf64_Rela
constexpr uint64_t kDynVaddr = kDataVaddr + 40;     // 4 dynamic entries

constexpr uint64_t kCodeFileOff = 0x1000;
constexpr uint64_t kDataFileOff = 0x2000;

struct Asm {
    std::vector<uint8_t> code;
    uint64_t base = kCodeVaddr;
    struct Fixup { size_t off; uint64_t next_insn; uint64_t target; };
    std::vector<Fixup> fixups;
    void b(uint8_t v) { code.push_back(v); }
    void rel32(uint64_t target) {
        const size_t off = code.size();
        for (int i = 0; i < 4; ++i) b(0);
        fixups.push_back({off, base + code.size(), target});
    }
    void patch() {
        for (const auto& f : fixups) {
            const int64_t disp = static_cast<int64_t>(f.target) -
                                 static_cast<int64_t>(f.next_insn);
            const uint32_t bits = static_cast<uint32_t>(disp);
            for (int i = 0; i < 4; ++i) code[f.off + i] = (bits >> (8 * i)) & 0xFF;
        }
    }
};

// Builds the PIE image: code + data (slot, rela, dynamic).
std::vector<uint8_t> BuildPie(const std::vector<uint8_t>& code, uint16_t e_type) {
    using namespace Loader;
    const size_t data_size = 40 + 4 * 16;
    std::vector<uint8_t> img(kDataFileOff + data_size, 0);

    // .rela.dyn: R_X86_64_RELATIVE slot <- addend kPointeeVaddr
    const Elf64_Rela rela = {
        kSlotVaddr,                       // r_offset
        ((0ull << 32) | 8ull),             // r_info: sym 0, type 8 = RELATIVE
        static_cast<int64_t>(kPointeeVaddr),  // r_addend
    };
    std::memcpy(img.data() + kDataFileOff + 16, &rela, sizeof(rela));

    // .dynamic
    const auto put_dyn = [&](size_t idx, int64_t tag, uint64_t val) {
        const uint64_t entry[2] = {static_cast<uint64_t>(tag), val};
        std::memcpy(img.data() + kDataFileOff + 40 + idx * 16, entry, 16);
    };
    put_dyn(0, DT_RELA, kRelaVaddr);
    put_dyn(1, DT_RELASZ, sizeof(Elf64_Rela));
    put_dyn(2, DT_RELAENT, sizeof(Elf64_Rela));
    put_dyn(3, DT_NULL, 0);

    Elf64_Ehdr eh{};
    eh.e_ident[0] = 0x7f; eh.e_ident[1] = 'E'; eh.e_ident[2] = 'L'; eh.e_ident[3] = 'F';
    eh.e_ident[4] = 2; eh.e_ident[5] = 1; eh.e_ident[6] = 1;
    eh.e_type = e_type;
    eh.e_machine = 0x3e;
    eh.e_version = 1;
    eh.e_entry = kCodeVaddr;
    eh.e_phoff = sizeof(Elf64_Ehdr);
    eh.e_ehsize = sizeof(Elf64_Ehdr);
    eh.e_phentsize = sizeof(Elf64_Phdr);
    eh.e_phnum = 3;
    std::memcpy(img.data(), &eh, sizeof(eh));

    const auto phdr = [&](uint32_t idx) -> Elf64_Phdr* {
        return reinterpret_cast<Elf64_Phdr*>(img.data() + sizeof(Elf64_Ehdr) +
                                             idx * sizeof(Elf64_Phdr));
    };
    {
        auto* p = phdr(0);   // PT_LOAD R+X (code)
        p->p_type = PT_LOAD; p->p_flags = 0x5;
        p->p_offset = kCodeFileOff; p->p_vaddr = kCodeVaddr;
        p->p_filesz = code.size(); p->p_memsz = code.size(); p->p_align = 0x1000;
    }
    {
        auto* p = phdr(1);   // PT_LOAD R+W (data)
        p->p_type = PT_LOAD; p->p_flags = 0x6;
        p->p_offset = kDataFileOff; p->p_vaddr = kDataVaddr;
        p->p_filesz = data_size; p->p_memsz = data_size; p->p_align = 0x1000;
    }
    {
        auto* p = phdr(2);   // PT_DYNAMIC (view inside the data segment)
        p->p_type = PT_DYNAMIC; p->p_flags = 0x6;
        p->p_offset = kDataFileOff + 40; p->p_vaddr = kDynVaddr;
        p->p_filesz = 4 * 16; p->p_memsz = 4 * 16; p->p_align = 8;
    }

    std::memcpy(img.data() + kCodeFileOff, code.data(), code.size());
    return img;
}

} // namespace

int main() {
    std::cout << "[et-dyn] round 18: PIE load bias\n";
    const char* kPath = "/tmp/prospero_et_dyn_test.elf";

    // ---- the position-independent guest program ---------------------------
    Asm a;
    a.b(0x48); a.b(0x8B); a.b(0x05); a.rel32(kSlotVaddr);   // mov rax,[rip+d]
    a.b(0x48); a.b(0x8D); a.b(0x0D); a.rel32(kPointeeVaddr);// lea rcx,[rip+d]
    a.b(0x48); a.b(0x29); a.b(0xC8);                        // sub rax,rcx
    a.b(0x48); a.b(0x83); a.b(0xC0); a.b(40);               // add rax,40
    a.b(0xC3);                                              // ret
    a.patch();

    const std::vector<uint8_t> elf = BuildPie(a.code, /*ET_DYN=*/3);
    {
        FILE* f = std::fopen(kPath, "wb");
        std::fwrite(elf.data(), 1, elf.size(), f);
        std::fclose(f);
    }

    // =====================================================================
    // A: boot the PIE -- the relocation bias must equal the mapping base.
    // =====================================================================
    std::cout << "[et-dyn] A: PIE boot + relocation at the bias\n";
    {
        Loader::GuestBootResult res = GuestLauncher().Boot(kPath, 1'000'000);
        CHECK(res.ok);
        CHECK(res.error.empty());
        CHECK(res.exit_code == 40);
        CHECK(res.segments_mapped == 2);

        // The boot mapped the PIE; the module is discoverable by its entry
        // mapping address (FindProgramByAddr resolves the containing module).
        Loader::ModuleInfo* mod =
            RuntimeLinker::Instance().FindProgramByAddr(res.entry_gva);
        CHECK(mod != nullptr);
        if (mod != nullptr) {
            CHECK(mod->base_addr != 0);              // a real bias was chosen
            CHECK(mod->base_addr >=
                  VirtualMemoryManager::Instance().GetArenaBaseGva());
            // The relocated slot must hold base + kPointeeVaddr, and the
            // mapping must exist at exactly that address.
            const uint64_t slot_gva = mod->base_addr + kSlotVaddr;
            const uint64_t pointee_gva = mod->base_addr + kPointeeVaddr;
            uint64_t value = 0;
            CHECK(VirtualMemoryManager::Instance().CopyFromGuest(
                slot_gva, &value, 8));
            CHECK(value == pointee_gva);
            CHECK(VirtualMemoryManager::Instance().IsGvaMapped(pointee_gva));
            CHECK(VirtualMemoryManager::Instance().IsGvaExecutable(
                mod->base_addr + kCodeVaddr));
        }
    }

    // =====================================================================
    // B: a SECOND PIE at a DIFFERENT bias -- the interpreter/VMM state from
    //    part A is still resident, so the new probe must land after it; the
    //    program still computes 40 (bias independence).
    // =====================================================================
    std::cout << "[et-dyn] B: second PIE at a fresh bias\n";
    {
        // Force a different module by writing the same image under a new name.
        const char* kPath2 = "/tmp/prospero_et_dyn_test2.elf";
        {
            FILE* f = std::fopen(kPath2, "wb");
            std::fwrite(elf.data(), 1, elf.size(), f);
            std::fclose(f);
        }
        Loader::GuestBootResult res = GuestLauncher().Boot(kPath2, 1'000'000);
        CHECK(res.ok);
        CHECK(res.exit_code == 40);
    }

    // =====================================================================
    // C: other ELF types fail closed with the documented error.
    // =====================================================================
    std::cout << "[et-dyn] C: unsupported type fails closed\n";
    {
        const std::vector<uint8_t> rel_elf = BuildPie(a.code, /*ET_REL=*/1);
        const char* kPath3 = "/tmp/prospero_et_rel_test.elf";
        {
            FILE* f = std::fopen(kPath3, "wb");
            std::fwrite(rel_elf.data(), 1, rel_elf.size(), f);
            std::fclose(f);
        }
        Loader::GuestBootResult res = GuestLauncher().Boot(kPath3, 1'000'000);
        CHECK(!res.ok);
        CHECK(res.error.find("unsupported ELF type") != std::string::npos);
    }

    std::cout << "[et-dyn] " << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
