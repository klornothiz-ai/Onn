// Tests for basic block chaining in the JIT executor.
#include "cpu/jit_executor.hpp"
#include "memory/virtual_memory_manager.hpp"

#include <array>
#include <cstdint>
#include <iostream>

namespace {

using PS5::CPU::CPUJitEngine;
using PS5::CPU::GuestExecutionStatus;
using PS5::CPU::JITBasicBlock;
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

// Two single-block snippets laid out back-to-back. The first block ends with a
// conditional branch (jne) that falls through to the second block; the second
// ends with ret. Each block therefore ends at the first branch/ret and the JIT
// basic-block discipline splits them.
//   block A @ kBlockA: mov rax,rdi ; add rax,rsi ; jne +0  (falls through)
//   block B @ kBlockB: nop ; ret
constexpr uint64_t kBlockA = 0x1000c20000ULL;
constexpr uint64_t kBlockB = 0x1000c20008ULL; // 8 bytes after A (mov+add+jne)
constexpr uint32_t kReadWrite =
    static_cast<uint32_t>(PageProt::Read) | static_cast<uint32_t>(PageProt::Write);
constexpr uint32_t kReadExecute =
    static_cast<uint32_t>(PageProt::Read) | static_cast<uint32_t>(PageProt::Exec);

// block A: mov rax,rdi (3) ; add rax,rsi (3) ; jne rel8=0 (2) -> 8 bytes,
// but the branch target is the next instruction (block B starts at +8). We use
// jne with displacement 0 so it conditionally branches to itself-fallthrough.
// To keep block A ending at the branch and block B starting right after, we
// place a one-byte nop at block B then ret.
const std::array<uint8_t, 10> kCode = {
    0x48, 0x89, 0xf8,             // mov rax, rdi
    0x48, 0x01, 0xf0,             // add rax, rsi
    0x75, 0x00,                   // jne +0 (falls through to block B)
    0x90,                         // nop  (block B)
    0xc3,                         // ret
};

bool SetupGuestCode() {
    auto& vmm = VirtualMemoryManager::Instance();
    CHECK(vmm.AllocateVirtual(kBlockA, 4096, kReadWrite) == kBlockA);
    CHECK(vmm.CopyToGuest(kBlockA, kCode.data(), kCode.size()));
    CHECK(vmm.ProtectVirtual(kBlockA, 4096, kReadExecute));
    return true;
}

bool TeardownGuestCode() {
    auto& vmm = VirtualMemoryManager::Instance();
    CHECK(vmm.FreeVirtual(kBlockA, 4096));
    return true;
}

bool BlockCompilationAndCaching() {
    CHECK(SetupGuestCode());

    auto& engine = CPUJitEngine::Instance();
    const uint64_t compiled_before = engine.GetTotalBlocksCompiled();

    const auto* block_a = engine.CompileBasicBlock(kBlockA);
    CHECK(block_a != nullptr);
    CHECK(block_a->guest_rip == kBlockA);
    CHECK(block_a->is_compiled);
    CHECK(block_a->code_size == 8); // mov, add, jne
    CHECK(block_a->instruction_count == 3);
    // Chaining fields start unchained.
    CHECK(block_a->next_block_rip == 0);
    CHECK(block_a->prev_block_rip == 0);
    CHECK(block_a->chain_count == 0);

    const auto* block_b = engine.CompileBasicBlock(kBlockB);
    CHECK(block_b != nullptr);
    CHECK(block_b->guest_rip == kBlockB);
    CHECK(block_b->code_size == 2); // nop, ret
    CHECK(block_b->instruction_count == 2);

    // Caching: recompiling the same RIP returns the cached block (no new
    // compile counted).
    const uint64_t compiled_after = engine.GetTotalBlocksCompiled();
    CHECK(compiled_after == compiled_before + 2);
    const auto* block_a_again = engine.CompileBasicBlock(kBlockA);
    CHECK(block_a_again == block_a);
    CHECK(engine.GetTotalBlocksCompiled() == compiled_after);
    CHECK(engine.GetCachedBlockCount() >= 2);

    CHECK(TeardownGuestCode());
    return true;
}

bool BlockChainingBetweenTwoBlocks() {
    CHECK(SetupGuestCode());

    auto& engine = CPUJitEngine::Instance();
    JITBasicBlock* block_a = engine.CompileBasicBlock(kBlockA);
    JITBasicBlock* block_b = engine.CompileBasicBlock(kBlockB);
    CHECK(block_a != nullptr);
    CHECK(block_b != nullptr);

    const uint64_t chains_before = engine.GetTotalChainsCreated();
    CHECK(engine.ChainBlocks(kBlockA, kBlockB));
    CHECK(engine.GetTotalChainsCreated() == chains_before + 1);

    // The chain is recorded on block A and the reverse link on block B.
    CHECK(block_a->next_block_rip == kBlockB);
    CHECK(block_a->chain_count == 1);
    CHECK(block_b->prev_block_rip == kBlockA);

    // Chaining a nonexistent block fails closed.
    CHECK(!engine.ChainBlocks(kBlockA, 0xDEADBEEFULL));
    CHECK(!engine.ChainBlocks(0xDEADBEEFULL, kBlockB));
    // No extra chain counted on failure.
    CHECK(engine.GetTotalChainsCreated() == chains_before + 1);

    CHECK(TeardownGuestCode());
    return true;
}

bool ChainHitSkipsCacheLookup() {
    CHECK(SetupGuestCode());

    auto& engine = CPUJitEngine::Instance();
    JITBasicBlock* block_a = engine.CompileBasicBlock(kBlockA);
    JITBasicBlock* block_b = engine.CompileBasicBlock(kBlockB);
    CHECK(block_a != nullptr);
    CHECK(block_b != nullptr);
    CHECK(engine.ChainBlocks(kBlockA, kBlockB));

    const uint64_t hits_before = engine.GetTotalChainHits();
    // Following the chain from A returns block B directly.
    JITBasicBlock* chained = engine.GetChainedBlock(kBlockA);
    CHECK(chained != nullptr);
    CHECK(chained == block_b);
    CHECK(engine.GetTotalChainHits() == hits_before + 1);

    // A second follow increments the hit counter again.
    chained = engine.GetChainedBlock(kBlockA);
    CHECK(chained == block_b);
    CHECK(engine.GetTotalChainHits() == hits_before + 2);

    // An unchained block returns nullptr (caller does normal lookup).
    CHECK(engine.GetChainedBlock(kBlockB) == nullptr);

    // An unknown RIP returns nullptr.
    CHECK(engine.GetChainedBlock(0xCAFEBABEULL) == nullptr);

    CHECK(TeardownGuestCode());
    return true;
}

bool StatisticsAreTrackedCorrectly() {
    // Use a fresh guest page so the blocks are newly compiled (not cached from
    // earlier tests); the statistics counters are cumulative across the
    // singleton's lifetime.
    constexpr uint64_t kStatA = 0x1000c30000ULL;
    constexpr uint64_t kStatB = 0x1000c30008ULL;
    auto& vmm = VirtualMemoryManager::Instance();
    CHECK(vmm.AllocateVirtual(kStatA, 4096, kReadWrite) == kStatA);
    CHECK(vmm.CopyToGuest(kStatA, kCode.data(), kCode.size()));
    CHECK(vmm.ProtectVirtual(kStatA, 4096, kReadExecute));

    auto& engine = CPUJitEngine::Instance();
    const uint64_t compiled0 = engine.GetTotalBlocksCompiled();
    const uint64_t chains0 = engine.GetTotalChainsCreated();
    const uint64_t hits0 = engine.GetTotalChainHits();

    // Compile two blocks.
    engine.CompileBasicBlock(kStatA);
    engine.CompileBasicBlock(kStatB);
    CHECK(engine.GetTotalBlocksCompiled() == compiled0 + 2);

    // Create a chain.
    CHECK(engine.ChainBlocks(kStatA, kStatB));
    CHECK(engine.GetTotalChainsCreated() == chains0 + 1);

    // Use the chain twice.
    engine.GetChainedBlock(kStatA);
    engine.GetChainedBlock(kStatA);
    CHECK(engine.GetTotalChainHits() == hits0 + 2);

    CHECK(vmm.FreeVirtual(kStatA, 4096));
    return true;
}

bool InvalidatingABlockClearsItsChains() {
    // Use a fresh guest page so the blocks are newly compiled and isolated
    // from earlier tests' cached entries.
    constexpr uint64_t kInvA = 0x1000c40000ULL;
    constexpr uint64_t kInvB = 0x1000c40008ULL;
    auto& vmm = VirtualMemoryManager::Instance();
    CHECK(vmm.AllocateVirtual(kInvA, 4096, kReadWrite) == kInvA);
    CHECK(vmm.CopyToGuest(kInvA, kCode.data(), kCode.size()));
    CHECK(vmm.ProtectVirtual(kInvA, 4096, kReadExecute));

    auto& engine = CPUJitEngine::Instance();
    JITBasicBlock* block_a = engine.CompileBasicBlock(kInvA);
    JITBasicBlock* block_b = engine.CompileBasicBlock(kInvB);
    CHECK(block_a != nullptr);
    CHECK(block_b != nullptr);
    CHECK(engine.ChainBlocks(kInvA, kInvB));
    CHECK(block_a->next_block_rip == kInvB);
    CHECK(block_b->prev_block_rip == kInvA);

    // Invalidate block A. Its forward chain to B is cleared, and B's reverse
    // link back to A is cleared.
    engine.InvalidateBlock(kInvA);
    CHECK(block_b->prev_block_rip == 0);
    CHECK(block_b->next_block_rip == 0); // B was never chained forward

    // The chained lookup from A now fails closed because A is gone.
    CHECK(engine.GetChainedBlock(kInvA) == nullptr);

    // Block B is still cached and recompilable.
    JITBasicBlock* block_b_again = engine.CompileBasicBlock(kInvB);
    CHECK(block_b_again == block_b);

    // Invalidating block B removes it from the cache (count drops), then
    // recompiling restores a fresh, unchained block. We verify via the cache
    // count rather than pointer inequality, since the allocator may reuse the
    // freed block's address.
    const size_t cached_before_inv = engine.GetCachedBlockCount();
    engine.InvalidateBlock(kInvB);
    CHECK(engine.GetCachedBlockCount() == cached_before_inv - 1);
    JITBasicBlock* block_b_recompiled = engine.CompileBasicBlock(kInvB);
    CHECK(block_b_recompiled != nullptr);
    CHECK(engine.GetCachedBlockCount() == cached_before_inv);
    CHECK(block_b_recompiled->next_block_rip == 0);
    CHECK(block_b_recompiled->prev_block_rip == 0);

    // Invalidating an unknown RIP is a no-op.
    engine.InvalidateBlock(0x12345678ULL);

    // Recompiling block A works after invalidation.
    JITBasicBlock* re_a = engine.CompileBasicBlock(kInvA);
    CHECK(re_a != nullptr);
    CHECK(re_a->next_block_rip == 0);
    CHECK(re_a->prev_block_rip == 0);

    CHECK(vmm.FreeVirtual(kInvA, 4096));
    return true;
}

struct TestCase {
    const char* name;
    bool (*function)();
};

} // namespace

int main() {
    const TestCase tests[] = {
        {"BlockCompilationAndCaching", BlockCompilationAndCaching},
        {"BlockChainingBetweenTwoBlocks", BlockChainingBetweenTwoBlocks},
        {"ChainHitSkipsCacheLookup", ChainHitSkipsCacheLookup},
        {"StatisticsAreTrackedCorrectly", StatisticsAreTrackedCorrectly},
        {"InvalidatingABlockClearsItsChains", InvalidatingABlockClearsItsChains},
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
