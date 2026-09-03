// ============================================================================
// ProsperoLayer RDNA2 Core - Guest Boot End-to-End Test (round 11)
// ----------------------------------------------------------------------------
// Proves the full end-to-end boot flow a shadPS4-style emulator needs:
//
//   synthetic ET_EXEC ELF (code / data / PT_TLS / PT_DYNAMIC with DT_INIT +
//   DT_INIT_ARRAY + DT_FINI + DT_FINI_ARRAY)
//     -> RuntimeLinker::LoadProgram + RelocateProgram
//     -> GuestLauncher::Boot (map patched segments, allocate TLS, run init
//        functions, execute the ELF entry point through the extended
//        interpreter with SSE/SSE2 and FS-based TLS addressing)
//
// Part A boots the module and checks every observable side effect (SSE float
// math, packed arithmetic, TLS counters, init ordering, entry exit code).
// Part B exercises focused SSE semantics on a separate hand-mapped region.
// Part C proves per-thread TLS isolation; Part D proves fini ordering + the
// run-once guards.
// ============================================================================
#include "loader/guest_launcher.hpp"
#include "loader/runtimeLinker.h"
#include "memory/virtual_memory_manager.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

using Loader::GuestLauncher;
using Loader::RuntimeLinker;
using PS5::Memory::PageProt;
using PS5::Memory::VirtualMemoryManager;

int g_failures = 0;
int g_checks = 0;
bool Check(bool v, const char* e, int line) {
    ++g_checks;
    if (!v) { ++g_failures; std::cerr << "  [FAIL] line " << line << ": " << e << '\n'; }
    return v;
}
#define CHECK(e) Check((e), #e, __LINE__)

// ---------------------------------------------------------------------------
// tiny in-test assembler with RIP-relative fixups
// ---------------------------------------------------------------------------
struct Fixup {
    size_t code_off;        // where the rel32 placeholder sits
    uint64_t next_insn_gva; // GVA of the instruction AFTER the rel32
    uint64_t target_gva;
};

struct Asm {
    std::vector<uint8_t> code;
    uint64_t base;
    std::vector<Fixup> fixups;

    explicit Asm(uint64_t base_gva) : base(base_gva) {}
    size_t off() const { return code.size(); }
    uint64_t gva() const { return base + code.size(); }
    void b(uint8_t v) { code.push_back(v); }
    void imm32(uint32_t v) { for (int i = 0; i < 4; ++i) b((v >> (8 * i)) & 0xFF); }
    void imm64(uint64_t v) { for (int i = 0; i < 8; ++i) b((v >> (8 * i)) & 0xFF); }
    void rel32(uint64_t target) {
        const size_t off = code.size();
        for (int i = 0; i < 4; ++i) b(0);
        fixups.push_back({off, base + code.size(), target});
    }
    void patch() {
        for (const auto& f : fixups) {
            const int64_t disp = static_cast<int64_t>(f.target_gva) -
                                 static_cast<int64_t>(f.next_insn_gva);
            uint32_t bits = static_cast<uint32_t>(disp);
            for (int i = 0; i < 4; ++i) code[f.code_off + i] = (bits >> (8 * i)) & 0xFF;
        }
    }
};

// ---------------------------------------------------------------------------
// layout constants
// ---------------------------------------------------------------------------
constexpr uint64_t kCodeBase = 0x1000300000;
constexpr uint64_t kDataBase = 0x1000400000;
constexpr uint64_t kPartBBase = 0x1000600000; // RWX region for part B

// data offsets (within the data segment)
constexpr uint64_t g_out = kDataBase + 0;      // double
constexpr uint64_t g_f1 = kDataBase + 8;       // float 3.5
constexpr uint64_t g_f2 = kDataBase + 12;      // float 8.25
constexpr uint64_t g_vec = kDataBase + 16;     // 4 floats 1,2,3,4
constexpr uint64_t g_vec2 = kDataBase + 32;    // 4 floats 10,20,30,40
constexpr uint64_t g_psum = kDataBase + 48;    // 16 bytes
constexpr uint64_t g_seq = kDataBase + 64;     // dword
constexpr uint64_t g_probe = kDataBase + 68;   // dword (thread test)
constexpr uint64_t g_init_array = kDataBase + 72;  // [init2, init3]
constexpr uint64_t g_fini_array = kDataBase + 88;  // [fini1]
constexpr uint64_t g_dynamic = kDataBase + 96;     // dynamic entries
constexpr uint64_t g_tls_image = kDataBase + 208;  // 16-byte TLS template

struct GuestVars {  // host-side view of the data segment via GvaToHva
    double* out;
    float* f1;
    float* f2;
    float* vec;
    float* vec2;
    float* psum;
    uint32_t* seq;
    uint32_t* probe;
};

GuestVars Vars() {
    auto& vmm = VirtualMemoryManager::Instance();
    GuestVars v{};
    v.out = reinterpret_cast<double*>(vmm.GvaToHva(kDataBase + 0));
    v.f1 = reinterpret_cast<float*>(vmm.GvaToHva(kDataBase + 8));
    v.f2 = reinterpret_cast<float*>(vmm.GvaToHva(kDataBase + 12));
    v.vec = reinterpret_cast<float*>(vmm.GvaToHva(kDataBase + 16));
    v.vec2 = reinterpret_cast<float*>(vmm.GvaToHva(kDataBase + 32));
    v.psum = reinterpret_cast<float*>(vmm.GvaToHva(kDataBase + 48));
    v.seq = reinterpret_cast<uint32_t*>(vmm.GvaToHva(kDataBase + 64));
    v.probe = reinterpret_cast<uint32_t*>(vmm.GvaToHva(kDataBase + 68));
    return v;
}

std::vector<uint8_t> BuildElf(const std::vector<uint8_t>& code,
                              const std::vector<uint8_t>& data,
                              uint64_t entry_off, const std::vector<uint64_t>& init_array,
                              const std::vector<uint64_t>& fini_array,
                              uint64_t init_fn, uint64_t fini_fn) {
    using namespace Loader;
    constexpr uint64_t kCodeFileOff = 0x1000;
    constexpr uint64_t kDataFileOff = 0x2000;

    const size_t data_size = 224;  // through the TLS template at +208
    std::vector<uint8_t> img(kDataFileOff + data_size, 0);

    // data segment content
    std::memcpy(img.data() + kDataFileOff, data.data(), data.size());
    for (size_t i = 0; i < init_array.size(); ++i) {
        const uint64_t v = init_array[i];
        std::memcpy(img.data() + kDataFileOff + 72 + i * 8, &v, 8);
    }
    for (size_t i = 0; i < fini_array.size(); ++i) {
        const uint64_t v = fini_array[i];
        std::memcpy(img.data() + kDataFileOff + 88 + i * 8, &v, 8);
    }
    auto put_dyn = [&](size_t idx, int64_t tag, uint64_t val) {
        const uint64_t entry[2] = {static_cast<uint64_t>(tag), val};
        std::memcpy(img.data() + kDataFileOff + 96 + idx * 16, entry, 16);
    };
    put_dyn(0, DT_INIT, init_fn);
    put_dyn(1, DT_INIT_ARRAY, g_init_array);
    put_dyn(2, DT_INIT_ARRAYSZ, init_array.size() * 8);
    put_dyn(3, DT_FINI_ARRAY, g_fini_array);
    put_dyn(4, DT_FINI_ARRAYSZ, fini_array.size() * 8);
    put_dyn(5, DT_FINI, fini_fn);
    put_dyn(6, DT_NULL, 0);

    Elf64_Ehdr eh{};
    eh.e_ident[0] = 0x7f; eh.e_ident[1] = 'E'; eh.e_ident[2] = 'L'; eh.e_ident[3] = 'F';
    eh.e_ident[4] = 2; eh.e_ident[5] = 1; eh.e_ident[6] = 1;
    eh.e_type = 2;       // ET_EXEC
    eh.e_machine = 0x3e; // x86-64
    eh.e_version = 1;
    eh.e_entry = kCodeBase + entry_off;
    eh.e_phoff = sizeof(Elf64_Ehdr);
    eh.e_shoff = 0;
    eh.e_ehsize = sizeof(Elf64_Ehdr);
    eh.e_phentsize = sizeof(Elf64_Phdr);
    eh.e_phnum = 4;
    std::memcpy(img.data(), &eh, sizeof(eh));

    auto phdr = [&](uint32_t idx) -> Elf64_Phdr* {
        return reinterpret_cast<Elf64_Phdr*>(img.data() + sizeof(Elf64_Ehdr) +
                                             idx * sizeof(Elf64_Phdr));
    }
    ;
    {   // PT_LOAD R+X (code)
        auto* p = phdr(0);
        p->p_type = PT_LOAD; p->p_flags = 0x5; // R+X
        p->p_offset = kCodeFileOff; p->p_vaddr = kCodeBase;
        p->p_filesz = code.size(); p->p_memsz = code.size(); p->p_align = 0x1000;
    }
    {   // PT_LOAD R+W (data)
        auto* p = phdr(1);
        p->p_type = PT_LOAD; p->p_flags = 0x6; // R+W
        p->p_offset = kDataFileOff; p->p_vaddr = kDataBase;
        p->p_filesz = data_size; p->p_memsz = data_size; p->p_align = 0x1000;
    }
    {   // PT_TLS (template lives inside the data segment)
        auto* p = phdr(2);
        p->p_type = PT_TLS; p->p_flags = 0x4;
        p->p_offset = kDataFileOff + 208; p->p_vaddr = g_tls_image;
        p->p_filesz = 16; p->p_memsz = 24; p->p_align = 4;
    }
    {   // PT_DYNAMIC (view inside the data segment)
        auto* p = phdr(3);
        p->p_type = PT_DYNAMIC; p->p_flags = 0x6;
        p->p_offset = kDataFileOff + 96; p->p_vaddr = g_dynamic;
        p->p_filesz = 7 * 16; p->p_memsz = 7 * 16; p->p_align = 8;
    }

    std::memcpy(img.data() + kCodeFileOff, code.data(), code.size());
    return img;
}

bool WriteTempFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    FILE* f = fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    const size_t n = fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
    return n == bytes.size();
}

} // namespace

int main() {
    std::cout << "[guest_boot] round 11: end-to-end boot (TLS + init arrays + SSE)\n";
    auto& vmm = VirtualMemoryManager::Instance();

    // =====================================================================
    // Build the guest program.
    // =====================================================================
    Asm a(kCodeBase);
    const uint64_t entry_off = a.off();

    // ---- entry (main) ----
    a.b(0xF2); a.b(0x0F); a.b(0x10); a.b(0x05); a.rel32(g_out);    // movsd xmm0,[g_out]
    a.b(0xF2); a.b(0x0F); a.b(0x2C); a.b(0xC0);                     // cvttsd2si eax,xmm0
    a.b(0x64); a.b(0x8B); a.b(0x0C); a.b(0x25);
    a.imm32(8);                                                     // mov ecx, fs:[8]
    a.b(0x01); a.b(0xC8);                                           // add eax, ecx
    a.b(0x64); a.b(0x8B); a.b(0x0C); a.b(0x25);
    a.imm32(12);                                                    // mov ecx, fs:[12]
    a.b(0xC1); a.b(0xE9); a.b(0x04);                                // shr ecx, 4
    a.b(0x01); a.b(0xC8);                                           // add eax, ecx
    a.b(0xF3); a.b(0x0F); a.b(0x10); a.b(0x0D); a.rel32(g_psum);    // movss xmm1,[g_psum]
    a.b(0xF3); a.b(0x0F); a.b(0x2D); a.b(0xC9);                     // cvtss2si ecx, xmm1
    a.b(0x01); a.b(0xC8);                                           // add eax, ecx
    a.b(0xF3); a.b(0x0F); a.b(0x10); a.b(0x15); a.rel32(g_f2);      // movss xmm2,[g_f2]
    a.b(0x0F); a.b(0x2F); a.b(0x15); a.rel32(g_f1);                 // comiss xmm2,[g_f1]
    // ja .add7  (rel32 placeholder, patched after the layout is final)
    a.b(0x0F); a.b(0x87);
    const size_t ja_off = a.off();
    for (int i = 0; i < 4; ++i) a.b(0);
    const uint64_t ja_next = a.gva();
    // jmp .done  (rel32 placeholder, patched after the layout is final)
    a.b(0xE9);
    const size_t jmp_off = a.off();
    for (int i = 0; i < 4; ++i) a.b(0);
    const uint64_t jmp_next = a.gva();
    const uint64_t add7_off = a.off();
    a.b(0x83); a.b(0xC0); a.b(0x07);                                // add eax, 7
    const uint64_t done_off = a.off();
    a.b(0xC3);                                                       // ret

    // ---- init1 (DT_INIT): SSE store of (f1+f2) as double ----
    const uint64_t init1_off = a.off();
    a.b(0xF3); a.b(0x0F); a.b(0x10); a.b(0x05); a.rel32(g_f1);      // movss xmm0,[g_f1]
    a.b(0xF3); a.b(0x0F); a.b(0x58); a.b(0x05); a.rel32(g_f2);      // addss xmm0,[g_f2]
    a.b(0xF3); a.b(0x0F); a.b(0x5A); a.b(0xC0);                     // cvtss2sd xmm0,xmm0
    a.b(0xF2); a.b(0x0F); a.b(0x11); a.b(0x05); a.rel32(g_out);     // movsd [g_out],xmm0
    a.b(0xC7); a.b(0x05); a.rel32(g_seq); a.imm32(1);               // mov [g_seq],1
    a.b(0xC3);

    // ---- init2 (DT_INIT_ARRAY[0]): TLS counter += 5 ----
    const uint64_t init2_off = a.off();
    a.b(0x64); a.b(0x8B); a.b(0x04); a.b(0x25); a.imm32(8);         // mov eax, fs:[8]
    a.b(0x83); a.b(0xC0); a.b(0x05);                                // add eax, 5
    a.b(0x64); a.b(0x89); a.b(0x04); a.b(0x25); a.imm32(8);         // mov fs:[8], eax
    a.b(0xC7); a.b(0x05); a.rel32(g_seq); a.imm32(2);               // mov [g_seq],2
    a.b(0xC3);

    // ---- init3 (DT_INIT_ARRAY[1]): packed addps + TLS dword *2 ----
    const uint64_t init3_off = a.off();
    a.b(0x0F); a.b(0x28); a.b(0x05); a.rel32(g_vec);                // movaps xmm0,[g_vec]
    a.b(0x0F); a.b(0x58); a.b(0x05); a.rel32(g_vec2);               // addps xmm0,[g_vec2]
    a.b(0x0F); a.b(0x29); a.b(0x05); a.rel32(g_psum);               // movaps [g_psum],xmm0
    a.b(0x64); a.b(0x8B); a.b(0x04); a.b(0x25); a.imm32(12);        // mov eax, fs:[12]
    a.b(0x01); a.b(0xC0);                                           // add eax, eax
    a.b(0x64); a.b(0x89); a.b(0x04); a.b(0x25); a.imm32(12);        // mov fs:[12], eax
    a.b(0xC7); a.b(0x05); a.rel32(g_seq); a.imm32(3);               // mov [g_seq],3
    a.b(0xC3);

    // ---- fini1 (DT_FINI_ARRAY[0]) ----
    const uint64_t fini1_off = a.off();
    a.b(0xC7); a.b(0x05); a.rel32(g_seq); a.imm32(99);              // mov [g_seq],99
    a.b(0xC3);

    // ---- fini2 (DT_FINI) ----
    const uint64_t fini2_off = a.off();
    a.b(0xC7); a.b(0x05); a.rel32(g_seq); a.imm32(100);             // mov [g_seq],100
    a.b(0xC3);

    // ---- thread_bump: returns fs:[8]*2, stores edi (arg0) into g_probe ----
    const uint64_t thread_bump_off = a.off();
    a.b(0x64); a.b(0x8B); a.b(0x04); a.b(0x25); a.imm32(8);         // mov eax, fs:[8]
    a.b(0x01); a.b(0xC0);                                           // add eax, eax
    a.b(0x89); a.b(0x3D); a.rel32(g_probe);                         // mov [g_probe], edi
    a.b(0xC3);

    a.patch();
    // Patch the two intra-code jumps now that the layout is final.
    {
        const auto write32 = [&](size_t off, uint64_t target, uint64_t next) {
            const int64_t disp = static_cast<int64_t>(target) - static_cast<int64_t>(next);
            const uint32_t bits = static_cast<uint32_t>(disp);
            for (int k = 0; k < 4; ++k) a.code[off + k] = (bits >> (8 * k)) & 0xFF;
        };
        write32(ja_off, kCodeBase + add7_off, ja_next);
        write32(jmp_off, kCodeBase + done_off, jmp_next);
    }

    // data segment
    std::vector<uint8_t> data(208, 0);
    {   // g_out = 0.0
        double zero = 0.0;
        std::memcpy(data.data() + 0, &zero, 8);
    }
    {   // g_f1 = 3.5f, g_f2 = 8.25f
        const float f1 = 3.5f, f2 = 8.25f;
        std::memcpy(data.data() + 8, &f1, 4);
        std::memcpy(data.data() + 12, &f2, 4);
    }
    {   // g_vec = 1,2,3,4 ; g_vec2 = 10,20,30,40
        const float v1[4] = {1.0f, 2.0f, 3.0f, 4.0f};
        const float v2[4] = {10.0f, 20.0f, 30.0f, 40.0f};
        std::memcpy(data.data() + 16, v1, 16);
        std::memcpy(data.data() + 32, v2, 16);
    }
    {   // TLS template @ +208 handled inside BuildElf: dword 7, dword 100
        const uint32_t t[4] = {7, 100, 0, 0};
        // (written below via the file image; keep data vector 208 bytes)
        (void)t;
    }

    const uint64_t init1_gva = kCodeBase + init1_off;
    const uint64_t init2_gva = kCodeBase + init2_off;
    const uint64_t init3_gva = kCodeBase + init3_off;
    const uint64_t fini1_gva = kCodeBase + fini1_off;
    const uint64_t fini2_gva = kCodeBase + fini2_off;

    std::vector<uint8_t> elf = BuildElf(a.code, data, entry_off,
                                        {init2_gva, init3_gva}, {fini1_gva},
                                        init1_gva, fini2_gva);
    // write the TLS template into the file image (at data offset +208)
    {
        const uint32_t t[4] = {7, 100, 0, 0};
        std::memcpy(elf.data() + 0x2000 + 208, t, 16);
    }

    const std::string elf_path = "/tmp/prospero_guest_boot_test.elf";
    if (!WriteTempFile(elf_path, elf)) {
        std::cerr << "failed to write temp ELF\n";
        return 1;
    }

    // =====================================================================
    // Part A: boot.
    // =====================================================================
    std::cout << "[guest_boot] A: full boot flow\n";
    GuestLauncher launcher;
    const auto boot = launcher.Boot(elf_path);
    CHECK(boot.ok);
    CHECK(boot.error.empty());
    CHECK(boot.init_functions_run == 3);
    CHECK(boot.segments_mapped == 2);
    CHECK(boot.tls_base_gva != 0);
    CHECK(boot.exit_code == 53); // 11 + 12 + 12 + 11 + 7

    const auto vars = Vars();
    CHECK(vars.out != nullptr && *vars.out == 11.75);
    CHECK(*vars.seq == 3);
    CHECK(vars.psum[0] == 11.0f && vars.psum[1] == 22.0f &&
          vars.psum[2] == 33.0f && vars.psum[3] == 44.0f);

    // TLS block contents (initial-exec layout: self ptr, then image).
    const uint64_t tls = boot.tls_base_gva;
    const auto* tls_u64 = reinterpret_cast<const uint64_t*>(vmm.GvaToHva(tls));
    const auto* tls_u32 = reinterpret_cast<const uint32_t*>(vmm.GvaToHva(tls + 8));
    CHECK(tls_u64 != nullptr && *tls_u64 == tls);   // fs:[0] self pointer
    CHECK(tls_u32[0] == 12);                        // 7 + 5 (init2)
    CHECK(tls_u32[1] == 200);                       // 100 * 2 (init3)

    // Init function table query + ordering.
    std::vector<uint64_t> init_fns;
    Loader::ModuleInfo* program = RuntimeLinker::Instance().FindProgramByFileName(elf_path);
    CHECK(program != nullptr);
    CHECK(RuntimeLinker::Instance().GetInitFunctions(program, init_fns));
    CHECK(init_fns.size() == 3);
    CHECK(init_fns[0] == init1_gva && init_fns[1] == init2_gva && init_fns[2] == init3_gva);

    // Re-running the entry through the SAME TLS state is idempotent.
    CHECK(GuestLauncher::RunGuestFunction(boot.entry_gva, 0, 0, tls) == 53);

    // A second boot of the same image fails closed (segment collision).
    const auto boot2 = launcher.Boot(elf_path);
    CHECK(!boot2.ok);

    // =====================================================================
    // Part B: focused SSE semantics on a separate RWX region.
    // =====================================================================
    std::cout << "[guest_boot] B: SSE semantics\n";
    {
        constexpr uint32_t kRWX = static_cast<uint32_t>(PageProt::Read) |
                                  static_cast<uint32_t>(PageProt::Write) |
                                  static_cast<uint32_t>(PageProt::Exec);
        const uint64_t region = vmm.AllocateVirtual(kPartBBase, 0x20000, kRWX);
        CHECK(region == kPartBBase);
        Asm b(kPartBBase);
        // data at +0x10000
        const uint64_t d_neg = kPartBBase + 0x10000;
        const uint64_t d_nan = kPartBBase + 0x10008;
        const uint64_t d_one = kPartBBase + 0x1000C;
        const uint64_t d_a = kPartBBase + 0x10010;
        const uint64_t d_b = kPartBBase + 0x10014;
        const uint64_t d_144 = kPartBBase + 0x10018;
        const uint64_t d_one_d = kPartBBase + 0x10020;  // 1.0 as a double

        // fn_trunc: (int)(-3.7) == -3
        const uint64_t fn_trunc = b.gva();
        b.b(0xF2); b.b(0x0F); b.b(0x10); b.b(0x05); b.rel32(d_neg);
        b.b(0xF2); b.b(0x0F); b.b(0x2C); b.b(0xC0);
        b.b(0xC3);
        // fn_nan: comiss(NaN, 1.0) -> jp taken -> 2
        const uint64_t fn_nan = b.gva();
        b.b(0xF3); b.b(0x0F); b.b(0x10); b.b(0x05); b.rel32(d_nan);
        b.b(0x0F); b.b(0x2F); b.b(0x05); b.rel32(d_one);
        b.b(0x70 + 0x0A); b.b(0x06);       // jp +6 (skips mov eax,1; ret)
        b.b(0xB8); b.imm32(1); b.b(0xC3);
        b.b(0xB8); b.imm32(2); b.b(0xC3);
        // fn_minmax: min(5,3) + max(5,3) == 8
        const uint64_t fn_minmax = b.gva();
        b.b(0xF3); b.b(0x0F); b.b(0x10); b.b(0x05); b.rel32(d_a);  // movss xmm0,[a]
        b.b(0xF3); b.b(0x0F); b.b(0x5D); b.b(0x05); b.rel32(d_b);  // minss xmm0,[b]
        b.b(0xF3); b.b(0x0F); b.b(0x2D); b.b(0xC0);                // cvtss2si eax,xmm0
        b.b(0xF3); b.b(0x0F); b.b(0x10); b.b(0x0D); b.rel32(d_a);  // movss xmm1,[a]
        b.b(0xF3); b.b(0x0F); b.b(0x5F); b.b(0x0D); b.rel32(d_b);  // maxss xmm1,[b]
        b.b(0xF3); b.b(0x0F); b.b(0x2D); b.b(0xC9);                // cvtss2si ecx,xmm1
        b.b(0x01); b.b(0xC8); b.b(0xC3);
        // fn_movq: movq xmm0, rax; movq rcx, xmm0 round trip
        const uint64_t fn_movq = b.gva();
        b.b(0x48); b.b(0xB8); b.imm64(0x1122334455667788ULL);      // mov rax, imm64
        b.b(0x66); b.b(0x48); b.b(0x0F); b.b(0x6E); b.b(0xC0);     // movq xmm0, rax
        b.b(0x66); b.b(0x48); b.b(0x0F); b.b(0x7E); b.b(0xC1);     // movq rcx, xmm0
        b.b(0x48); b.b(0x89); b.b(0xC8); b.b(0xC3);                // mov rax, rcx; ret
        // fn_pxor: pxor zeroes a loaded qword
        const uint64_t fn_pxor = b.gva();
        b.b(0x48); b.b(0xC7); b.b(0xC0); b.imm32(0x1234);          // mov rax, 0x1234
        b.b(0x66); b.b(0x48); b.b(0x0F); b.b(0x6E); b.b(0xC0);     // movq xmm0, rax
        b.b(0x66); b.b(0x0F); b.b(0xEF); b.b(0xC0);                // pxor xmm0, xmm0
        b.b(0x66); b.b(0x48); b.b(0x0F); b.b(0x7E); b.b(0xC0);     // movq rax, xmm0
        b.b(0xC3);
        // fn_sqrtsd: sqrt(144.0) == 12
        const uint64_t fn_sqrtsd = b.gva();
        b.b(0xF2); b.b(0x0F); b.b(0x10); b.b(0x05); b.rel32(d_144); // movsd xmm0,[144]
        b.b(0xF2); b.b(0x0F); b.b(0x51); b.b(0xC8);                 // sqrtsd xmm1, xmm0
        b.b(0xF2); b.b(0x0F); b.b(0x2C); b.b(0xC1);                 // cvttsd2si eax, xmm1
        b.b(0xC3);
        // fn_ucomisd_lt: 1.0 < 144.0 -> seta al == 0
        const uint64_t fn_ucomisd_lt = b.gva();
        b.b(0xF2); b.b(0x0F); b.b(0x10); b.b(0x05); b.rel32(d_one_d); // movsd xmm0,[1.0]
        b.b(0x66); b.b(0x0F); b.b(0x2E); b.b(0x05); b.rel32(d_144);   // ucomisd xmm0,[144]
        b.b(0x0F); b.b(0x97); b.b(0xC0);                            // seta al
        b.b(0x0F); b.b(0xB6); b.b(0xC0);                            // movzx eax, al
        b.b(0xC3);
        b.patch();

        // write code + data
        CHECK(vmm.CopyToGuest(kPartBBase, b.code.data(), b.code.size(), kRWX));
        {
            const double neg = -3.7;
            const float nan_f = std::nanf("");
            const float one_f = 1.0f, a_f = 5.0f, bb_f = 3.0f;
            const double d144 = 144.0;
            uint8_t dbytes[40] = {};
            std::memcpy(dbytes + 0, &neg, 8);
            std::memcpy(dbytes + 8, &nan_f, 4);
            std::memcpy(dbytes + 12, &one_f, 4);
            std::memcpy(dbytes + 16, &a_f, 4);
            std::memcpy(dbytes + 20, &bb_f, 4);
            std::memcpy(dbytes + 24, &d144, 8);
            const double one_d = 1.0;
            std::memcpy(dbytes + 32, &one_d, 8);
            CHECK(vmm.CopyToGuest(d_neg, dbytes, sizeof(dbytes), kRWX));
        }

        // cvttsd2si eax zero-extends the 32-bit result into rax.
        CHECK(GuestLauncher::RunGuestFunction(fn_trunc, 0, 0, 0) == 0xFFFFFFFDull);
        CHECK(GuestLauncher::RunGuestFunction(fn_nan, 0, 0, 0) == 2);
        CHECK(GuestLauncher::RunGuestFunction(fn_minmax, 0, 0, 0) == 8);
        CHECK(GuestLauncher::RunGuestFunction(fn_movq, 0, 0, 0) == 0x1122334455667788ULL);
        CHECK(GuestLauncher::RunGuestFunction(fn_pxor, 0, 0, 0) == 0);
        CHECK(GuestLauncher::RunGuestFunction(fn_sqrtsd, 0, 0, 0) == 12);
        CHECK(GuestLauncher::RunGuestFunction(fn_ucomisd_lt, 0, 0, 0) == 0);
    }

    // =====================================================================
    // Part C: per-thread TLS isolation.
    // =====================================================================
    std::cout << "[guest_boot] C: per-thread TLS isolation\n";
    {
        const uint64_t thread_bump_gva = kCodeBase + thread_bump_off;
        const uint64_t tls2 = launcher.AllocateThreadTls(*program);
        const auto* tls2_u32 = reinterpret_cast<const uint32_t*>(vmm.GvaToHva(tls2 + 8));
        CHECK(tls2 != 0 && tls2 != tls);
        CHECK(tls2_u32 != nullptr && tls2_u32[0] == 7);   // fresh template image
        CHECK(tls2_u32[1] == 100);

        const uint64_t r = GuestLauncher::RunGuestFunction(thread_bump_gva, 0x9999, 0, tls2);
        CHECK(r == 14);                                   // 7 * 2 through the new FS
        CHECK(*vars.probe == 0x9999);                     // arg0 stored
        CHECK(tls2_u32[0] == 7);                          // untouched
        const auto* tls1_u32 = reinterpret_cast<const uint32_t*>(vmm.GvaToHva(tls + 8));
        CHECK(tls1_u32[0] == 12);                         // main TLS untouched
    }

    // =====================================================================
    // Part D: fini ordering + run-once guards.
    // =====================================================================
    std::cout << "[guest_boot] D: fini ordering\n";
    {
        std::vector<uint64_t> fini_fns;
        CHECK(RuntimeLinker::Instance().GetFiniFunctions(program, fini_fns));
        CHECK(fini_fns.size() == 2);
        CHECK(fini_fns[0] == fini1_gva && fini_fns[1] == fini2_gva); // reverse array, then DT_FINI

        CHECK(launcher.Shutdown(program, tls) == 2);
        CHECK(*vars.seq == 100);   // fini2 (DT_FINI) ran last
        CHECK(launcher.Shutdown(program, tls) == 0);  // run once
        CHECK(*vars.seq == 100);
    }

    std::cout << "[guest_boot] " << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
