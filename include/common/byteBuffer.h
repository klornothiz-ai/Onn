#pragma once
// ProsperoLayer PS5 emulator - byte buffer helper (Kyty-compatible)
#include "common/common.h"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace Common {

class ByteBuffer {
public:
        ByteBuffer() = default;
        ByteBuffer(const uint8_t* data, size_t size): m_data(data, data + size) {}
        explicit ByteBuffer(size_t size): m_data(size, 0) {}
        explicit ByteBuffer(std::vector<uint8_t> data): m_data(std::move(data)) {}

        const uint8_t* data() const { return m_data.data(); }
        uint8_t* data() { return m_data.data(); }
        size_t size() const { return m_data.size(); }
        bool empty() const { return m_data.empty(); }

        void resize(size_t size) { m_data.resize(size); }
        void clear() { m_data.clear(); }
        void append(const uint8_t* data, size_t size) { m_data.insert(m_data.end(), data, data + size); }
        void append(const ByteBuffer& other) { append(other.data(), other.size()); }

        uint8_t operator[](size_t index) const { return m_data[index]; }
        uint8_t& operator[](size_t index) { return m_data[index]; }

private:
        std::vector<uint8_t> m_data;
};

inline std::string HexFromBin(const ByteBuffer& buffer) {
        static const char* hex_digits = "0123456789abcdef";
        std::string out;
        out.reserve(buffer.size() * 2);
        for (size_t i = 0; i < buffer.size(); i++) {
                const uint8_t b = buffer[i];
                out.push_back(hex_digits[(b >> 4) & 0x0f]);
                out.push_back(hex_digits[b & 0x0f]);
        }
        return out;
}

inline ByteBuffer BinFromHex(const std::string& hex) {
        ByteBuffer out;
        out.resize(hex.size() / 2);
        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
                auto nibble = [](char c) -> uint8_t {
                        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
                        if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
                        if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
                        return 0;
                };
                out[i / 2] = static_cast<uint8_t>((nibble(hex[i]) << 4) | nibble(hex[i + 1]));
        }
        return out;
}

} // namespace Common
