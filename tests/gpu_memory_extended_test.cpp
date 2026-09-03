#include "gpu/gcn_decoder.hpp"
#include "gpu/rdna2_compute_compiler.hpp"
#include "gpu/gpu_guest_memory.hpp"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <utility>
#include <vector>

using namespace PS5::GPU;

class Mem final : public GpuGuestMemory {
public:
    explicit Mem(size_t n) : d(n, 0) {}
    bool ReadDwords(uint64_t a, uint32_t* out, size_t n) override {
        if ((a & 3u) || a / 4 > d.size() || n > d.size() - a / 4) return false;
        std::memcpy(out, d.data() + a / 4, n * 4); return true;
    }
    bool WriteDwords(uint64_t a, const uint32_t* in, size_t n) override {
        if ((a & 3u) || a / 4 > d.size() || n > d.size() - a / 4) return false;
        std::memcpy(d.data() + a / 4, in, n * 4); return true;
    }
    std::vector<uint32_t> d;
};

static uint32_t Sopp(uint32_t op, int simm = 0) {
    return 0xBF800000u | (op << 16u) | (static_cast<uint32_t>(simm) & 0xffffu);
}
// SMEM is a 64-bit (two-dword) instruction: w0 carries op/sbase/sdst, and the
// offset lives in the high bits of w1 (see gcn_decoder.cpp's smem_offset
// extraction: (w01 >> 32) & 0x1FFFFF). Returns {w0, w1} so callers place both
// dwords in the instruction stream -- folding off into w0 (e.g. via XOR)
// would silently corrupt the opcode/sbase/sdst fields for any off != 0.
static std::pair<uint32_t, uint32_t> Smem(uint32_t op, uint32_t sbase, uint32_t sdst, uint32_t off) {
    const uint32_t w0 = 0xC0000000u | (op << 18u) | ((sdst & 0x7fu) << 6u) |
                         ((sbase / 2u) & 0x3fu);
    const uint32_t w1 = off & 0x1fffffu;
    return {w0, w1};
}

int main() {
    Mem mem(256);
    // base_gva=128 is a BYTE address -> Mem::ReadDwords(a) indexes d[a/4],
    // so descriptor 0 {128, 8, 1} resolves to d[32]/d[33]. This is the data
    // the good-path load below must return.
    mem.d[32] = 0x12345678u;
    mem.d[33] = 0xAABBCCDDu;
    const std::vector<GcnBufferResource> buffers = {{128, 8, 1}};
    // Canary bytes at an unrelated address: never read by either case below;
    // their presence just guards against an address-math regression that
    // would accidentally land here instead.
    mem.d[128] = 0xCAFEBABEu;
    mem.d[129] = 0xDEADC0DEu;

    // S_BUFFER_LOAD_DWORDX2: sbase=4 maps to descriptor 0.
    const uint32_t op = GcnOp::S_BUFFER_LOAD_DWORDX2;
    const auto [w0, w1] = Smem(op, /*sbase=*/4u, /*sdst=*/2u, /*off=*/0u);
    const uint32_t end = Sopp(0x001);
    const uint32_t code[] = {w0, w1, end};
    GcnSwExecutor sw;
    std::vector<uint32_t> out;
    auto r = sw.Run(code, 3, 1, {0}, 1, 2, out, &mem, &buffers);
    // buf.base_gva=128 is a BYTE address; Mem::ReadDwords indexes by a/4, so
    // this reads mem.d[32]/[33] -- the data actually placed at that address.
    assert(r.ok);
    assert(sw.GetSgpr(2) == 0x12345678u);
    assert(sw.GetSgpr(3) == 0xAABBCCDDu);

    // RDNA2ComputeCompiler takes ComputeCompilerOptions BY VALUE at
    // construction, so opt.buffers must be populated first -- setting it
    // afterward would silently compile against an empty buffer table.
    ComputeCompilerOptions opt;
    opt.buffers = buffers;
    RDNA2ComputeCompiler cc(opt);
    auto cr = cc.Compile(code, 3);
    assert(cr.success);

    // Descriptor bounds are enforced by the compiler and SW executor.
    // The descriptor is 8 dwords = 32 bytes. Loading a DWORDX2 (8 bytes) at
    // off=28 would span bytes [28,36) -- past the 32-byte bound. (off=16 is
    // NOT out of range here: [16,24) still fits inside [0,32).)
    const auto [badw0, badw1] = Smem(op, /*sbase=*/4u, /*sdst=*/2u, /*off=*/28u);
    const uint32_t bad[] = {badw0, badw1, end};
    auto badr = sw.Run(bad, 3, 1, {0}, 1, 2, out, &mem, &buffers);
    assert(!badr.ok);
    auto badc = cc.Compile(bad, 3);
    assert(!badc.success);

    std::cout << "GPU extended memory test passed\n";
    return 0;
}
