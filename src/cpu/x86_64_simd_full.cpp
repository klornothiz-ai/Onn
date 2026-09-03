// ProsperoLayer PS5 emulator - the FULL systematic x86-64 SIMD engine.
//
// Round 28: one engine covering the complete legacy SSE1/SSE2/SSE3/SSSE3/
// SSE4.1/SSE4.2 maps and the VEX (AVX / AVX2 / FMA) maps at both 128- and
// 256-bit vector lengths. Every encoding this file handles is listed in
// tests/simd_forms_table.inc, a table PRODUCED by GNU as (scripts/
// gen_simd_forms.py) -- no opcode number in this file was transcribed from
// memory. Semantics are verified instruction-by-instruction against real
// hardware by tests/cpu_simd_diff_test.cpp, which executes the exact same
// byte sequences natively on the host CPU (wrappers in tests/
// simd_native_forms.S) and requires bit-identical results: register files,
// the 256-bit upper halves, written memory and RFLAGS must all agree.
//
// Only RCPSS/RCPPS/RSQRTSS/RSQRTPS are excluded from the differential
// comparison: those are approximation instructions whose bit patterns are
// microcode-defined and not reproducible in C++; they are implemented as
// IEEE 1/x and 1/sqrt(x) and documented as approximate.

#include "cpu/x86_64_interpreter.hpp"

#include <cmath>
#include <cstring>

namespace PS5::CPU {

namespace simdfull {

// ---------------------------------------------------------------------------
// A 256-bit vector value with typed lane views (little-endian, x86 layout).
// ---------------------------------------------------------------------------
union Vec {
    uint64_t q[4];
    uint32_t d[8];
    uint16_t w[16];
    uint8_t  b[32];
    int8_t   sb[32];
    int16_t  sw[16];
    int32_t  sd[8];
    int64_t  sq[4];
    float    f[8];
    double   df[4];

    static Vec FromLo(const CpuState::XmmReg& lo) {
        Vec v;
        v.q[0] = lo.lo;
        v.q[1] = lo.hi;
        return v;
    }
    static Vec FromFull(const CpuState::XmmReg& lo, const CpuState::XmmReg& hi) {
        Vec v;
        v.q[0] = lo.lo;
        v.q[1] = lo.hi;
        v.q[2] = hi.lo;
        v.q[3] = hi.hi;
        return v;
    }
    CpuState::XmmReg Lo() const {
        CpuState::XmmReg r{};
        r.lo = q[0];
        r.hi = q[1];
        return r;
    }
    CpuState::XmmReg Hi() const {
        CpuState::XmmReg r{};
        r.lo = q[2];
        r.hi = q[3];
        return r;
    }
};

// --- half-precision (F16C) --------------------------------------------------
inline uint16_t F16FromF32(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t exp = static_cast<int32_t>((x >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant = x & 0x7FFFFFu;
    if (((x >> 23) & 0xFFu) == 0xFFu) {
        if (mant == 0) return static_cast<uint16_t>(sign | 0x7C00u);
        mant >>= 13;
        if (mant == 0) mant = 1;
        return static_cast<uint16_t>(sign | 0x7C00u | mant);
    }
    if (exp >= 0x1F) return static_cast<uint16_t>(sign | 0x7C00u);
    if (exp <= 0) {
        if (exp < -10) return static_cast<uint16_t>(sign);
        mant |= 0x800000u;
        const int shift = 14 - exp + 13;
        uint32_t sub = mant >> shift;
        const uint32_t rem = mant & ((1u << shift) - 1u);
        const uint32_t half = 1u << (shift - 1);
        if (rem > half || (rem == half && (sub & 1u))) sub++;
        return static_cast<uint16_t>(sign | sub);
    }
    uint16_t h = static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | (mant >> 13));
    const uint32_t rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) h++;
    return h;
}

inline float F32FromF16(uint16_t h) {
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t out;
    if (exp == 0) {
        if (mant == 0) {
            out = sign;
        } else {
            // subnormal half: value = mant * 2^-24; normalize (exact -- the
            // 10-bit subnormal mantissa always fits the 24-bit float field)
            int shift = 0;
            uint32_t m = mant;
            while ((m & 0x400u) == 0) { m <<= 1; ++shift; }
            m &= 0x3FFu;
            out = sign | (static_cast<uint32_t>(113 - shift) << 23) | (m << 13);
        }
    } else if (exp == 0x1F) {
        out = sign | 0x7F800000u | (mant << 13);
    } else {
        out = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &out, 4);
    return f;
}

// --- x86 float rules --------------------------------------------------------
// x86 min/max NaN rule: any NaN -> return the SECOND source operand.
// NaN-safe negation: subtracting a NaN propagates the ORIGINAL NaN payload
// (hardware-verified); pre-negating the operand would flip its sign bit.
inline float NegN(float v) { return std::isnan(v) ? v : -v; }
inline double NegN(double v) { return std::isnan(v) ? v : -v; }

inline float XMin(float a, float b) { return (std::isnan(a) || std::isnan(b)) ? b : (a < b ? a : b); }
inline float XMax(float a, float b) { return (std::isnan(a) || std::isnan(b)) ? b : (a > b ? a : b); }
inline double XMin(double a, double b) { return (std::isnan(a) || std::isnan(b)) ? b : (a < b ? a : b); }
inline double XMax(double a, double b) { return (std::isnan(a) || std::isnan(b)) ? b : (a > b ? a : b); }

// CVTPS2DQ rounds to nearest even and yields 0x80000000 on NaN/overflow;
// CVTTPS2DQ truncates with the same indefinite value.
inline uint32_t CvtRne(float f) {
    if (std::isnan(f)) return 0x80000000u;
    const double d = std::nearbyint(static_cast<double>(f));
    if (d >= 2147483648.0 || d < -2147483648.0) return 0x80000000u;
    return static_cast<uint32_t>(static_cast<int32_t>(d));
}
inline uint32_t CvtTrunc(float f) {
    if (std::isnan(f)) return 0x80000000u;
    if (f >= 2147483648.0f || f < -2147483648.0f) return 0x80000000u;
    return static_cast<uint32_t>(static_cast<int32_t>(f));
}
inline uint32_t CvtRne(double d) {
    if (std::isnan(d)) return 0x80000000u;
    const double v = std::nearbyint(d);
    if (v >= 2147483648.0 || v < -2147483648.0) return 0x80000000u;
    return static_cast<uint32_t>(static_cast<int32_t>(v));
}
inline uint32_t CvtTrunc(double d) {
    if (std::isnan(d)) return 0x80000000u;
    if (d >= 2147483648.0 || d < -2147483648.0) return 0x80000000u;
    return static_cast<uint32_t>(static_cast<int32_t>(d));
}

// --- saturation -------------------------------------------------------------
inline uint8_t SatU8(int32_t v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }
inline uint16_t SatU16(int32_t v) { return static_cast<uint16_t>(v < 0 ? 0 : (v > 65535 ? 65535 : v)); }
inline int8_t SatS8(int32_t v) { return static_cast<int8_t>(v < -128 ? -128 : (v > 127 ? 127 : v)); }
inline int16_t SatS16(int32_t v) { return static_cast<int16_t>(v < -32768 ? -32768 : (v > 32767 ? 32767 : v)); }

// --- the full 32-entry vcmp predicate table ---------------------------------
template <typename F>
inline bool CmpPred(int pred, F a, F b) {
    // The exact 32-entry VCMPPS predicate table (Intel SDM). Bits [4:0]
    // select the predicate; bit 4 duplicates the low 16 entries (signalling
    // vs quiet exception behaviour is not modelled).
    const bool un = std::isnan(a) || std::isnan(b);
    switch (pred & 31) {
        case 0: case 16: return !un && a == b;    // EQ_OQ
        case 1: case 17: return !un && a <  b;    // LT_OS
        case 2: case 18: return !un && a <= b;    // LE_OS
        case 3: case 19: return un;               // UNORD_Q
        case 4: case 20: return un || a != b;     // NEQ_UQ
        case 5: case 21: return un || a >= b;     // NLT_US
        case 6: case 22: return un || a >  b;     // NLE_US
        case 7: case 23: return !un;              // ORD_Q
        case 8: case 24: return un || a == b;     // EQ_UQ
        case 9: case 25: return un || a <  b;     // NGE_US
        case 10: case 26: return un || a <= b;    // NGT_US
        case 11: case 27: return false;           // FALSE_O
        case 12: case 28: return !un && a != b;   // NEQ_OQ
        case 13: case 29: return !un && a >= b;   // GE_OS
        case 14: case 30: return !un && a >  b;   // GT_OS
        default: return true;                     // TRUE_U
    }
}

// --- CRC-32C (Castagnoli) ---------------------------------------------------
inline uint32_t Crc32c(uint32_t crc, const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u)));
    }
    return crc;
}

// --- AES (used by aesenc/aesdec/aesimc/aeskeygenassist) ---------------------
// MixColumns / InvMixColumns on one 16-byte state.
inline void AesMixColumns(uint8_t* s) {
    for (int c = 0; c < 4; c++) {
        uint8_t* col = s + c * 4;
        const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        const uint8_t x = a0 ^ a1 ^ a2 ^ a3;
        col[0] ^= x ^ (((a0 ^ a1) & 0x80) ? ((a0 ^ a1) << 1) ^ 0x1B : ((a0 ^ a1) << 1));
        col[1] ^= x ^ (((a1 ^ a2) & 0x80) ? ((a1 ^ a2) << 1) ^ 0x1B : ((a1 ^ a2) << 1));
        col[2] ^= x ^ (((a2 ^ a3) & 0x80) ? ((a2 ^ a3) << 1) ^ 0x1B : ((a2 ^ a3) << 1));
        col[3] ^= x ^ (((a3 ^ a0) & 0x80) ? ((a3 ^ a0) << 1) ^ 0x1B : ((a3 ^ a0) << 1));
    }
}
inline void AesInvMixColumns(uint8_t* s) {
    for (int c = 0; c < 4; c++) {
        uint8_t* col = s + c * 4;
        const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        auto mul = [](uint8_t v, int m) -> uint8_t {
            uint8_t r = 0;
            for (int i = 0; i < 8; i++) {
                if (m & (1 << i)) r ^= static_cast<uint8_t>(v << i);
                if (v & (0x80 >> (7 - i))) { /* handled by loop below */ }
            }
            // exact GF(2^8) multiply
            uint8_t out = 0;
            uint8_t x = v;
            int mm = m;
            while (mm) {
                if (mm & 1) out ^= x;
                const bool hi = x & 0x80;
                x <<= 1;
                if (hi) x ^= 0x1B;
                mm >>= 1;
            }
            return out;
        };
        col[0] = mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9);
        col[1] = mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13);
        col[2] = mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11);
        col[3] = mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14);
    }
}

} // namespace simdfull

using namespace simdfull;

namespace simdfull {

const uint8_t kAesSbox[256] = {
    0x63,0x7C,0x77,0x7B,0xF2,0x6B,0x6F,0xC5,0x30,0x01,0x67,0x2B,0xFE,0xD7,0xAB,0x76,
    0xCA,0x82,0xC9,0x7D,0xFA,0x59,0x47,0xF0,0xAD,0xD4,0xA2,0xAF,0x9C,0xA4,0x72,0xC0,
    0xB7,0xFD,0x93,0x26,0x36,0x3F,0xF7,0xCC,0x34,0xA5,0xE5,0xF1,0x71,0xD8,0x31,0x15,
    0x04,0xC7,0x23,0xC3,0x18,0x96,0x05,0x9A,0x07,0x12,0x80,0xE2,0xEB,0x27,0xB2,0x75,
    0x09,0x83,0x2C,0x1A,0x1B,0x6E,0x5A,0xA0,0x52,0x3B,0xD6,0xB3,0x29,0xE3,0x2F,0x84,
    0x53,0xD1,0x00,0xED,0x20,0xFC,0xB1,0x5B,0x6A,0xCB,0xBE,0x39,0x4A,0x4C,0x58,0xCF,
    0xD0,0xEF,0xAA,0xFB,0x43,0x4D,0x33,0x85,0x45,0xF9,0x02,0x7F,0x50,0x3C,0x9F,0xA8,
    0x51,0xA3,0x40,0x8F,0x92,0x9D,0x38,0xF5,0xBC,0xB6,0xDA,0x21,0x10,0xFF,0xF3,0xD2,
    0xCD,0x0C,0x13,0xEC,0x5F,0x97,0x44,0x17,0xC4,0xA7,0x7E,0x3D,0x64,0x5D,0x19,0x73,
    0x60,0x81,0x4F,0xDC,0x22,0x2A,0x90,0x88,0x46,0xEE,0xB8,0x14,0xDE,0x5E,0x0B,0xDB,
    0xE0,0x32,0x3A,0x0A,0x49,0x06,0x24,0x5C,0xC2,0xD3,0xAC,0x62,0x91,0x95,0xE4,0x79,
    0xE7,0xC8,0x37,0x6D,0x8D,0xD5,0x4E,0xA9,0x6C,0x56,0xF4,0xEA,0x65,0x7A,0xAE,0x08,
    0xBA,0x78,0x25,0x2E,0x1C,0xA6,0xB4,0xC6,0xE8,0xDD,0x74,0x1F,0x4B,0xBD,0x8B,0x8A,
    0x70,0x3E,0xB5,0x66,0x48,0x03,0xF6,0x0E,0x61,0x35,0x57,0xB9,0x86,0xC1,0x1D,0x9E,
    0xE1,0xF8,0x98,0x11,0x69,0xD9,0x8E,0x94,0x9B,0x1E,0x87,0xE9,0xCE,0x55,0x28,0xDF,
    0x8C,0xA1,0x89,0x0D,0xBF,0xE6,0x42,0x68,0x41,0x99,0x2D,0x0F,0xB0,0x54,0xBB,0x16,
};
const uint8_t kAesInvSbox[256] = {
    0x52,0x09,0x6A,0xD5,0x30,0x36,0xA5,0x38,0xBF,0x40,0xA3,0x9E,0x81,0xF3,0xD7,0xFB,
    0x7C,0xE3,0x39,0x82,0x9B,0x2F,0xFF,0x87,0x34,0x8E,0x43,0x44,0xC4,0xDE,0xE9,0xCB,
    0x54,0x7B,0x94,0x32,0xA6,0xC2,0x23,0x3D,0xEE,0x4C,0x95,0x0B,0x42,0xFA,0xC3,0x4E,
    0x08,0x2E,0xA1,0x66,0x28,0xD9,0x24,0xB2,0x76,0x5B,0xA2,0x49,0x6D,0x8B,0xD1,0x25,
    0x72,0xF8,0xF6,0x64,0x86,0x68,0x98,0x16,0xD4,0xA4,0x5C,0xCC,0x5D,0x65,0xB6,0x92,
    0x6C,0x70,0x48,0x50,0xFD,0xED,0xB9,0xDA,0x5E,0x15,0x46,0x57,0xA7,0x8D,0x9D,0x84,
    0x90,0xD8,0xAB,0x00,0x8C,0xBC,0xD3,0x0A,0xF7,0xE4,0x58,0x05,0xB8,0xB3,0x45,0x06,
    0xD0,0x2C,0x1E,0x8F,0xCA,0x3F,0x0F,0x02,0xC1,0xAF,0xBD,0x03,0x01,0x13,0x8A,0x6B,
    0x3A,0x91,0x11,0x41,0x4F,0x67,0xDC,0xEA,0x97,0xF2,0xCF,0xCE,0xF0,0xB4,0xE6,0x73,
    0x96,0xAC,0x74,0x22,0xE7,0xAD,0x35,0x85,0xE2,0xF9,0x37,0xE8,0x1C,0x75,0xDF,0x6E,
    0x47,0xF1,0x1A,0x71,0x1D,0x29,0xC5,0x89,0x6F,0xB7,0x62,0x0E,0xAA,0x18,0xBE,0x1B,
    0xFC,0x56,0x3E,0x4B,0xC6,0xD2,0x79,0x20,0x9A,0xDB,0xC0,0xFE,0x78,0xCD,0x5A,0xF4,
    0x1F,0xDD,0xA8,0x33,0x88,0x07,0xC7,0x31,0xB1,0x12,0x10,0x59,0x27,0x80,0xEC,0x5F,
    0x60,0x51,0x7F,0xA9,0x19,0xB5,0x4A,0x0D,0x2D,0xE5,0x7A,0x9F,0x93,0xC9,0x9C,0xEF,
    0xA0,0xE0,0x3B,0x4D,0xAE,0x2A,0xF5,0xB0,0xC8,0xEB,0xBB,0x3C,0x83,0x53,0x99,0x61,
    0x17,0x2B,0x04,0x7E,0xBA,0x77,0xD6,0x26,0xE1,0x69,0x14,0x63,0x55,0x21,0x0C,0x7D,
};

inline void AesShiftRows(uint8_t* s) {
    uint8_t t;
    // row 1: rotate left 1
    t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
    // row 2: rotate left 2
    t = s[2]; s[2] = s[10]; s[10] = t; t = s[6]; s[6] = s[14]; s[14] = t;
    // row 3: rotate left 3
    t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
}
inline void AesInvShiftRows(uint8_t* s) {
    uint8_t t;
    t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
    t = s[2]; s[2] = s[10]; s[10] = t; t = s[6]; s[6] = s[14]; s[14] = t;
    t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;
}
inline void AesRound(const uint8_t* in, const uint8_t* rk, uint8_t* out) {
    uint8_t t[16];
    for (int i = 0; i < 16; i++) t[i] = kAesSbox[in[i]];
    AesShiftRows(t);
    AesMixColumns(t);
    for (int i = 0; i < 16; i++) out[i] = t[i] ^ rk[i];
}
inline void AesRoundLast(const uint8_t* in, const uint8_t* rk, uint8_t* out) {
    uint8_t t[16];
    for (int i = 0; i < 16; i++) t[i] = kAesSbox[in[i]];
    AesShiftRows(t);
    for (int i = 0; i < 16; i++) out[i] = t[i] ^ rk[i];
}
// hardware-verified AESDEC order: InvShiftRows -> InvSubBytes ->
// InvMixColumns, with the round key XORed at the END (not first).
inline void AesInvRound(const uint8_t* in, const uint8_t* rk, uint8_t* out) {
    uint8_t t[16], u[16];
    std::memcpy(t, in, 16);
    AesInvShiftRows(t);
    for (int i = 0; i < 16; i++) t[i] = kAesInvSbox[t[i]];
    AesInvMixColumns(t);
    for (int i = 0; i < 16; i++) u[i] = t[i] ^ rk[i];
    std::memcpy(out, u, 16);
}
inline void AesInvRoundLast(const uint8_t* in, const uint8_t* rk, uint8_t* out) {
    uint8_t t[16];
    std::memcpy(t, in, 16);
    AesInvShiftRows(t);
    for (int i = 0; i < 16; i++) t[i] = kAesInvSbox[t[i]];
    for (int i = 0; i < 16; i++) out[i] = t[i] ^ rk[i];
}

// carry-less 64x64 -> 128 multiply
inline void ClMul64(uint64_t a, uint64_t b, uint64_t& lo, uint64_t& hi) {
    lo = 0; hi = 0;
    for (int i = 0; i < 64; i++) {
        if ((a >> i) & 1) {
            lo ^= b << i;
            if (i > 0) hi ^= b >> (64 - i);
        }
    }
}

} // namespace simdfull

// ---------------------------------------------------------------------------
// Coverage predicate: pure function of (map, op, pp, W, is_vex, L). No
// stream bytes are consumed -- callers may safely fall through to the legacy
// handlers when this returns false.
// ---------------------------------------------------------------------------
static bool SimdFullCovered(int map, uint8_t op, bool is_vex) {
    if (map == 1) {
        switch (op) {
            case 0x10: case 0x11: case 0x12: case 0x13:
            case 0x14: case 0x15: case 0x16: case 0x17:
            case 0x28: case 0x29: case 0x2A: case 0x2B:
            case 0x2C: case 0x2D: case 0x2E: case 0x2F:
            case 0x50: case 0x51: case 0x52: case 0x53:
            case 0x54: case 0x55: case 0x56: case 0x57:
            case 0x58: case 0x59: case 0x5A: case 0x5B:
            case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            case 0x60: case 0x61: case 0x62: case 0x63:
            case 0x64: case 0x65: case 0x66: case 0x67:
            case 0x68: case 0x69: case 0x6A: case 0x6B:
            case 0x6C: case 0x6D: case 0x6E: case 0x6F:
            case 0x70: case 0x71: case 0x72: case 0x73:
            case 0x74: case 0x75: case 0x76: case 0x77:
            case 0x7C: case 0x7D: case 0x7E: case 0x7F:
            case 0xC2: case 0xC4: case 0xC5: case 0xC6:
            case 0xD0: case 0xD1: case 0xD2: case 0xD3:
            case 0xD4: case 0xD5: case 0xD6: case 0xD7:
            case 0xD8: case 0xD9: case 0xDA: case 0xDB:
            case 0xDC: case 0xDD: case 0xDE: case 0xDF:
            case 0xE0: case 0xE1: case 0xE2: case 0xE3:
            case 0xE4: case 0xE5: case 0xE6: case 0xE7:
            case 0xE8: case 0xE9: case 0xEA: case 0xEB:
            case 0xEC: case 0xED: case 0xEE: case 0xEF:
            case 0xF1: case 0xF2: case 0xF3: case 0xF4:
            case 0xF5: case 0xF6: case 0xF7:
            case 0xF8: case 0xF9: case 0xFA: case 0xFB:
            case 0xFC: case 0xFD: case 0xFE:
                return true;
            default:
                return false;
        }
    }
    if (map == 2) {
        switch (op) {
            case 0x00: case 0x01: case 0x02: case 0x03: case 0x04:
            case 0x05: case 0x06: case 0x07: case 0x08: case 0x09:
            case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E:
            case 0x0F:
            case 0x10: case 0x13: case 0x14: case 0x15: case 0x16:
            case 0x17: case 0x18: case 0x19: case 0x1A: case 0x1C:
            case 0x1D: case 0x1E:
            case 0x20: case 0x21: case 0x22: case 0x23: case 0x24:
            case 0x25: case 0x28: case 0x29: case 0x2A: case 0x2B:
            case 0x2C: case 0x2D: case 0x2E: case 0x2F:
            case 0x30: case 0x31: case 0x32: case 0x33: case 0x34:
            case 0x35: case 0x36: case 0x37: case 0x38: case 0x39:
            case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E:
            case 0x3F: case 0x40: case 0x41:
            case 0x45: case 0x46: case 0x47:
            case 0x58: case 0x59: case 0x78: case 0x79:
            case 0x8C: case 0x8E: case 0x90: case 0x92:
            case 0x96: case 0x97: case 0x98: case 0x99: case 0x9A:
            case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F:
            case 0xA6: case 0xA7: case 0xA8: case 0xA9: case 0xAA:
            case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
            case 0xB6: case 0xB7: case 0xB8: case 0xB9: case 0xBA:
            case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
            case 0xF0: case 0xF1:
                // FMA / AVX2 / broadcast / gather / maskmov blocks are VEX-only
                if ((op >= 0x8C && op <= 0x9F) || (op >= 0xA6 && op <= 0xAF) ||
                    (op >= 0xB6 && op <= 0xBF) || op == 0x0C || op == 0x0D ||
                    op == 0x0E || op == 0x0F || op == 0x13 || op == 0x16 ||
                    op == 0x18 || op == 0x19 || op == 0x1A || op == 0x36 ||
                    op == 0x45 || op == 0x46 || op == 0x47 || op == 0x90 ||
                    op == 0x92) {
                    return is_vex;
                }
                return true;
            default:
                return false;
        }
    }
    if (map == 3) {
        switch (op) {
            case 0x00: case 0x01: case 0x04: case 0x05: case 0x06:
            case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C:
            case 0x0D: case 0x0E: case 0x0F:
            case 0x14: case 0x15: case 0x16: case 0x17:
            case 0x18: case 0x19: case 0x1D:
            case 0x20: case 0x21: case 0x22:
            case 0x38: case 0x39: case 0x40: case 0x41: case 0x42:
            case 0x44: case 0x46: case 0x4A: case 0x4B: case 0x4C:
            case 0x60: case 0x61: case 0x62: case 0x63: case 0xDF:
                if (op == 0x00 || op == 0x01 || op == 0x04 || op == 0x05 ||
                    op == 0x06 || op == 0x18 || op == 0x19 || op == 0x38 ||
                    op == 0x39 || op == 0x46 || op == 0x4A || op == 0x4B ||
                    op == 0x4C) {
                    return is_vex;
                }
                return true;
            default:
                return false;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Engine entry
// ---------------------------------------------------------------------------
bool X86Interpreter::ExecSimdFull(uint64_t& rip, const Prefixes& p, uint8_t op2,
                                  int map, const SimdVexInfo* vex, RunResult& r) {
    const bool is_vex = vex != nullptr;
    const bool L = is_vex && vex->L;
    const bool W = is_vex && vex->W;
    const int pp = p.opsize ? 1 : (p.rep == 0xF3 ? 2 : (p.rep == 0xF2 ? 3 : 0));

    if (!SimdFullCovered(map, op2, is_vex)) {
        return false;
    }

    // Scalar VEX forms with L=1 (256-bit) are invalid encodings on real
    // hardware. Reject exactly the scalar-only opcodes (round 28): packed
    // instructions that legitimately pair 66/F2/F3 with L=1 -- vhaddps,
    // vaddsubps, vcvttps2dq, vcvtdq2pd, vcvtph2ps, ... -- still run.
    if (is_vex && L) {
        bool scalar_form = false;
        if (map == 1) {
            const bool p23 = (pp == 2 || pp == 3);
            scalar_form =
                ((op2 == 0x10 || op2 == 0x11) && p23) ||
                (op2 == 0x51 && p23) || (op2 == 0x52 && pp == 2) ||
                (op2 == 0x53 && pp == 2) ||
                ((op2 == 0x58 || op2 == 0x59 ||
                  (op2 >= 0x5C && op2 <= 0x5F)) && p23) ||
                (op2 == 0x5A && p23) || op2 == 0x2A ||
                op2 == 0x2C || op2 == 0x2D || op2 == 0x2E || op2 == 0x2F ||
                (op2 == 0xC2 && p23);
        } else if (map == 2 && op2 >= 0x96) {
            const int col = op2 & 0x0F;               // FMA scalar columns
            scalar_form = (col == 9 || col == 0xB || col == 0xD || col == 0xF);
        } else if (map == 3) {
            scalar_form = (op2 == 0x0A || op2 == 0x0B);  // vroundss/vroundsd
        }
        if (scalar_form) {
            r.status = ExecStatus::UnsupportedOpcode;
            r.fault_addr = rip;
            return true;
        }
    }

    auto fault = [&](ExecStatus s, uint64_t addr) { r.status = s; r.fault_addr = addr; };
    const int vw = L ? 32 : 16;   // vector width in bytes

    // --- register file helpers ---------------------------------------------
    auto read_reg_vec = [&](uint8_t idx) -> Vec {
        const uint8_t i = idx & 0xF;
        if (L) return Vec::FromFull(m_state.xmm[i], m_state.ymm_hi[i]);
        return Vec::FromLo(m_state.xmm[i]);
    };
    // VEX.128 writes zero the upper half (hardware rule, verified by the
    // differential test); LEGACY SSE writes PRESERVE the upper half
    // (also hardware-verified -- this is the actual Zen 2 behaviour).
    auto write_reg_vec = [&](uint8_t idx, const Vec& v) {
        const uint8_t i = idx & 0xF;
        m_state.xmm[i] = v.Lo();
        if (L) m_state.ymm_hi[i] = v.Hi();
        else if (is_vex) m_state.ymm_hi[i] = CpuState::XmmReg{};
    };
    auto read_rm_vec = [&](const ModRMOperand& op, int width, Vec& out) -> bool {
        if (op.is_reg) {
            const uint8_t i = static_cast<uint8_t>(op.reg | (p.rex_b ? 0x08 : 0x00));
            if (width == 32) out = Vec::FromFull(m_state.xmm[i], m_state.ymm_hi[i]);
            else out = Vec::FromLo(m_state.xmm[i]);
            return true;
        }
        out = Vec{};
        if (width <= 16) return m_mem.Read(op.addr, &out, width);
        if (!m_mem.Read(op.addr, &out, 16)) return false;
        return m_mem.Read(op.addr + 16, reinterpret_cast<uint8_t*>(&out) + 16, 16);
    };
    auto write_rm_vec = [&](const ModRMOperand& op, int width, const Vec& v) -> bool {
        if (op.is_reg) {
            const uint8_t i = static_cast<uint8_t>(op.reg | (p.rex_b ? 0x08 : 0x00));
            m_state.xmm[i] = v.Lo();
            if (width == 32) m_state.ymm_hi[i] = v.Hi();
            else if (is_vex) m_state.ymm_hi[i] = CpuState::XmmReg{};
            return true;
        }
        if (width <= 16) return m_mem.Write(op.addr, &v, width);
        if (!m_mem.Write(op.addr, &v, 16)) return false;
        return m_mem.Write(op.addr + 16, reinterpret_cast<const uint8_t*>(&v) + 16, 16);
    };
    auto read_rm_int = [&](const ModRMOperand& op, int size, uint64_t& out) -> bool {
        if (op.is_reg) {
            out = ReadReg(static_cast<uint8_t>(op.reg | (p.rex_b ? 0x08 : 0x00)), size);
            return true;
        }
        return m_mem.Read(op.addr, &out, size);
    };
    auto write_rm_int = [&](const ModRMOperand& op, int size, uint64_t v) -> bool {
        if (op.is_reg) {
            WriteReg(static_cast<uint8_t>(op.reg | (p.rex_b ? 0x08 : 0x00)), size, v);
            return true;
        }
        return m_mem.Write(op.addr, &v, size);
    };
    uint8_t modrm = 0;
    ModRMOperand rm{};
    uint8_t reg = 0;

    auto src_a = [&]() -> Vec {   // legacy dst / VEX vvvv source
        return is_vex ? read_reg_vec(vex->vreg) : read_reg_vec(reg);
    };

    auto modrm_fetch = [&]() -> bool {
        if (!Fetch8(rip, modrm)) { fault(ExecStatus::DecodeFault, rip); return false; }
        if (!DecodeModRM(rip, p, modrm, rm, reg)) { fault(ExecStatus::DecodeFault, rip); return false; }
        return true;
    };
    auto fetch_imm8 = [&](uint8_t& imm) -> bool {
        if (!Fetch8(rip, imm)) { fault(ExecStatus::DecodeFault, rip); return false; }
        return true;
    };
    auto commit = [&]() { m_state.rip = rip; };

    // The map execution bodies live in sibling .inc files (kept separate so
    // each execution map stays reviewable; they are plain code blocks inside
    // this function and share the helpers above by reference).
#include "cpu/simd_full_map1.inc"
#include "cpu/simd_full_map23.inc"

    return false;
}

extern "C" void engine_aesround(const uint8_t* in, const uint8_t* rk, uint8_t* out) {
    simdfull::AesRound(in, rk, out);
}
extern "C" void engine_aesimc(const uint8_t* in, uint8_t* out) {
    uint8_t t[16];
    std::memcpy(t, in, 16);
    simdfull::AesInvMixColumns(t);
    std::memcpy(out, t, 16);
}

} // namespace PS5::CPU
