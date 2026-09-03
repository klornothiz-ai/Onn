// hle_entropy_test.cpp — round 29: the libSceRandom entropy fix.
//
// RandomGetRandomNumber (NID PI7jIZj4pcE) used a FIXED-SEED LCG: every process
// run produced the same "random" byte stream -- a correctness bug for any
// guest logic consuming it. This round it is backed by getrandom(2).
//
// The test resolves the NID through the real SymbolDatabase registration path
// and verifies: (a) two consecutive draws differ (real entropy), (b) many
// draws have sane byte statistics (not a degenerate sequence), (c) the
// documented validation contract (null buffer with nonzero size, size cap).
#include "libs/libs.h"
#include "loader/symbolDatabase.h"
#include "ps_errno.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using Loader::SymbolDatabase;



int g_checks = 0;
int g_failures = 0;
bool Check(bool v, const char* e, int line) {
    ++g_checks;
    if (!v) {
        ++g_failures;
        std::fprintf(stderr, "  [FAIL] %s (line %d)\n", e, line);
    }
    return v;
}
#define CHECK(e) Check((e), #e, __LINE__)

} // namespace

namespace Libs::LibRandom {
// Mirrors the LIB_DEFINE body in libNet.cpp (same symbol name, so the linker
// resolves this declaration to that definition).
void InitPlatform_1_Random(Loader::SymbolDatabase* s);
}

// libNet.cpp DECLARES (but does not define -- libJson2.cpp does, and it needs
// optional nlohmann-json headers) its Json2 registrar; the entropy test does
// not exercise Json2 NIDs, so a local empty definition satisfies the link.
namespace Libs::LibJson2 {
void InitNet_1_Json2(Loader::SymbolDatabase*) {}
}

void LibRandomInit(Loader::SymbolDatabase* s) {
    Libs::LibRandom::InitPlatform_1_Random(s);
}

int main() {
    std::printf("== libSceRandom real entropy (round 29) ==\n");

    auto& db = SymbolDatabase::Instance();
    LibRandomInit(&db);

    // Resolve through the guest dlsym surface (NID keyed, like the guest).
    const void* sym = db.FindSymbol("PI7jIZj4pcE");
    CHECK(sym != nullptr);
    if (sym == nullptr) {
        std::printf("hle_entropy_test: NID unresolved\n");
        return 1;
    }
    using RandomFn = int (*)(void*, size_t);
    const auto random = reinterpret_cast<RandomFn>(
        const_cast<void*>(sym));

    // Two draws must differ (the old fixed-seed LCG produced the SAME first
    // bytes on every call sequence -- and across runs).
    uint8_t a[32] = {}, b[32] = {};
    CHECK(random(a, sizeof(a)) == 0);
    CHECK(random(b, sizeof(b)) == 0);
    CHECK(std::memcmp(a, b, sizeof(a)) != 0);
    std::printf("  [ok] consecutive draws differ\n");

    // Entropy sanity over many draws: byte value coverage and no constant
    // stream (the LCG's high byte cycled with a tiny period).
    {
        uint8_t buf[64] = {};
        int seen[256] = {};
        bool all_distinct = true;
        for (int d = 0; d < 16; ++d) {
            uint8_t cur[64] = {};
            CHECK(random(cur, sizeof(cur)) == 0);
            if (std::memcmp(cur, buf, sizeof(cur)) == 0) all_distinct = false;
            std::memcpy(buf, cur, sizeof(cur));
            for (const uint8_t v : cur) ++seen[v];
        }
        CHECK(all_distinct);
        int distinct_values = 0;
        for (const int c : seen) {
            if (c > 0) ++distinct_values;
        }
        // 16*64 = 1024 uniform bytes should touch far more than 100 of the
        // 256 possible values (a degenerate generator would not).
        CHECK(distinct_values > 100);
        std::printf("  [ok] %d/256 byte values covered across 1024 draws\n",
                    distinct_values);
    }

    // Validation contract (unchanged from the stub era).
    CHECK(random(nullptr, 0) == 0);          // null+0 is legal (no-op)
    CHECK(random(nullptr, 8) != 0);          // null+size invalid
    uint8_t big[65] = {};
    CHECK(random(big, sizeof(big)) != 0);    // over the 64-byte cap

    std::printf("hle_entropy_test: %d checks, %d failures\n", g_checks,
                g_failures);
    if (g_failures == 0) std::printf(">> [PASS] real kernel entropy\n");
    return g_failures == 0 ? 0 : 1;
}
