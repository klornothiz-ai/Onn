#pragma once
// ProsperoLayer PS5 emulator - platform debug helpers (Kyty-compatible)
#include "common/common.h"

// Placeholder platform debug header (Linux).
// Kyty's SysDbg provides debugger integration; in this emulator it is a stub.

namespace Common::Platform {

inline void DbgBreak() {
#if defined(__x86_64__) && !defined(NDEBUG)
        __builtin_trap();
#else
        // no-op in release
#endif
}

struct sys_dbg_stack_info_t {
        uintptr_t reserved_addr{0};
        size_t    reserved_size{0};
};

// Returns the current thread's stack reservation info (best effort).
inline void SysStackUsage(sys_dbg_stack_info_t& info) {
        info.reserved_addr = 0;
        info.reserved_size = 0;
        // The address of a local approximates the current stack position;
        // keep the query cheap and non-intrusive.
        uintptr_t local = 0;
        (void)local;
}

} // namespace Common::Platform
