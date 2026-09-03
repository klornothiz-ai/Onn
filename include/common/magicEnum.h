#pragma once
// ProsperoLayer PS5 emulator - minimal magic enum helper (Kyty-compatible)
// Provides compile-time enum <-> string conversion without external deps.
#include "common/common.h"
#include <string>
#include <type_traits>

namespace Common {

template <class T>
inline std::string MagicEnumToString(T value) {
        using U = std::underlying_type_t<T>;
        return std::to_string(static_cast<U>(value));
}

template <class T>
inline T MagicEnumFromString(const std::string& str) {
        using U = std::underlying_type_t<T>;
        return static_cast<T>(static_cast<U>(std::stoull(str)));
}

// magic_enum-style name lookup (limited to the common enum types used).
template <class T>
inline std::string EnumName(T value) {
        return MagicEnumToString(value);
}

} // namespace Common

// Minimal magic_enum-style API used by some libs.
#ifndef MAGIC_ENUM_RANGE_MAX
#define MAGIC_ENUM_RANGE_MAX 256
#endif
