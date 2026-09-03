#pragma once
#include "common/common.h"
#include <cstdarg>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>

namespace Kyty::Core {
    class StringUtils {
    public:
        static std::string Format(const char* fmt, ...) {
            char buf[1024];
            va_list args;
            va_start(args, fmt);
            vsnprintf(buf, sizeof(buf), fmt, args);
            va_end(args);
            return std::string(buf);
        }
    };
}

namespace Common {
    inline std::string PathToString(const std::filesystem::path& path) {
        return path.string();
    }

    inline std::string ToLower(std::string str) {
        for (char& c : str) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return str;
    }

    inline std::string ToUpper(std::string str) {
        for (char& c : str) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return str;
    }

    template <class T>
    inline bool IndexValid(const std::vector<T>& vec, size_t index) {
        return index < vec.size();
    }

    inline bool IndexValid(const std::string& str, size_t index) {
        return index < str.size();
    }

    inline bool StartsWith(const std::string& str, const std::string& prefix) {
        return str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix) == 0;
    }

    inline bool EndsWith(const std::string& str, const std::string& suffix) {
        return str.size() >= suffix.size() &&
               str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    // Normalizes backslashes to forward slashes (guest path convention).
    inline std::string FixDirectorySlash(std::string path) {
        for (auto& c : path) {
                if (c == '\\') {
                        c = '/';
                }
        }
        return path;
    }

    template <class T, class V>
    inline int FindIndex(const std::vector<T>& vec, const V& value) {
        const auto it = std::find(vec.begin(), vec.end(), value);
        return it != vec.end() ? static_cast<int>(std::distance(vec.begin(), it)) : -1;
    }

    inline int FindIndex(const std::string& str, char value, size_t from = 0) {
        const size_t pos = str.find(value, from);
        return pos != std::string::npos ? static_cast<int>(pos) : -1;
    }

    inline std::string Mid(const std::string& str, size_t start, size_t count) {
        if (start >= str.size()) {
            return "";
        }
        return str.substr(start, count);
    }

    inline std::string Utf16ToUtf8(const char16_t* utf16, size_t len) {
        std::string out;
        for (size_t i = 0; i < len; i++) {
                char32_t cp = utf16[i];
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len) {
                        const char32_t lo = utf16[i + 1];
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                i++;
                        }
                }
                if (cp < 0x80) {
                        out += static_cast<char>(cp);
                } else if (cp < 0x800) {
                        out += static_cast<char>(0xC0 | (cp >> 6));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                        out += static_cast<char>(0xE0 | (cp >> 12));
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                        out += static_cast<char>(0xF0 | (cp >> 18));
                        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                }
        }
        return out;
    }

    inline std::string Utf16ToUtf8(const std::u16string& utf16) {
        return Utf16ToUtf8(utf16.data(), utf16.size());
    }
}
