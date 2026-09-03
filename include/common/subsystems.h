#pragma once
#include "common/common.h"

#define KYTY_CLASS_NO_COPY(cls) cls(const cls&) = delete; cls& operator=(const cls&) = delete;
#define KYTY_SUBSYSTEM_DEFINE(name) inline void Subsystem_##name() {}
#define KYTY_SUBSYSTEM_INIT(name) inline void Subsystem_Init_##name()
#define KYTY_SUBSYSTEM_UNEXPECTED_SHUTDOWN(name) inline void Subsystem_Shutdown_##name()
#define KYTY_SUBSYSTEM_DESTROY(name) inline void Subsystem_Destroy_##name()

namespace Kyty {
    inline void RegisterSubsystem(const char*, void*) {}
}
