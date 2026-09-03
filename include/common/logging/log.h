#pragma once
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include "ps_errno.h"

// Default module name. LIB_NAME() in libs.h defines a more specific
// `static constexpr char g_module[]` inside its own namespace, which shadows
// this default within that scope. Translation units that use logging outside
// any LIB_NAME scope fall back to the empty name.
namespace {
inline const char g_module[] = "";
} // namespace

#ifndef EXIT_NAME
#define EXIT_NAME g_module
#endif
#define EXIT_IF(cond)                                                                                  \
        do {                                                                                           \
                if (cond) {                                                                            \
                        fprintf(stderr, "[%s][FATAL] %s\n", g_module, __func__);                      \
                        std::abort();                                                                  \
                }                                                                                      \
        } while (0)
#define EXIT_NOT_IMPLEMENTED(cond)                                                                    \
        do {                                                                                         \
                if (cond) {                                                                          \
                        fprintf(stderr, "[%s][NOT_IMPLEMENTED] %s\n", g_module, __func__);           \
                        std::abort();                                                                \
                }                                                                                    \
        } while (0)
#define EXIT_NOT_IMPLEMENTED_NAME(name) printf("NOT IMPLEMENTED: %s\n", name);
#define EXIT_IF_FAILED(cond) if (!(cond)) { return; }

// Kyty-style fatal-exit macro: prints the message and aborts.
#ifndef EXIT
#define EXIT(...)                                                                                  \
        do {                                                                                       \
                fprintf(stderr, "[%s][FATAL] ", g_module);                                         \
                fprintf(stderr, __VA_ARGS__);                                                      \
                fprintf(stderr, "\n");                                                             \
                std::abort();                                                                      \
        } while (0)
#endif

#define PRINT_NAME_ENABLED g_print_name
#define PRINT_NAME_ENABLE(flag) PRINT_NAME_ENABLED = flag;

#define PRINT_VERBOSE(fmt, ...) printf("[%s] " fmt "\n", g_module, ##__VA_ARGS__)
#define PRINT_WARN(fmt, ...) printf("[%s][WARN] " fmt "\n", g_module, ##__VA_ARGS__)
#define PRINT_ERROR(fmt, ...) printf("[%s][ERROR] " fmt "\n", g_module, ##__VA_ARGS__)
#define PRINT_INFO(fmt, ...) printf("[%s][INFO] " fmt "\n", g_module, ##__VA_ARGS__)

// ProsperoLayer LOG_* convenience aliases (used by system_services.cpp).
// These mirror the Kyty-style PRINT_* macros so call sites that use
// {}-style fmt placeholders still compile and run safely (the extra
// arguments are simply ignored when there is no % specifier).
#ifndef LOG_VERBOSE
#define LOG_VERBOSE(...) PRINT_VERBOSE(__VA_ARGS__)
#endif
#ifndef LOG_DEBUG
#define LOG_DEBUG(...) PRINT_VERBOSE(__VA_ARGS__)
#endif
#ifndef LOG_INFO
#define LOG_INFO(...) PRINT_INFO(__VA_ARGS__)
#endif
#ifndef LOG_WARN
#define LOG_WARN(...) PRINT_WARN(__VA_ARGS__)
#endif
#ifndef LOG_ERROR
#define LOG_ERROR(...) PRINT_ERROR(__VA_ARGS__)
#endif
