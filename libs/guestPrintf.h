#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_GUEST_PRINTF_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_GUEST_PRINTF_H_

#include "common/abi.h"
#include "common/common.h"

#ifndef KYTY_FORMAT_PRINTF
#if defined(__GNUC__) || defined(__clang__)
#define KYTY_FORMAT_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))
#else
#define KYTY_FORMAT_PRINTF(fmt, args)
#endif
#endif

namespace Libs {

struct VaContext;
struct VaList;

using guest_printf_std_func_t = KYTY_FORMAT_PRINTF(1, 2) KYTY_SYSV_ABI
    int (*)(const char* str, ...);
using guest_printf_ctx_func_t   = int (*)(VaContext* c);
using guest_snprintf_ctx_func_t = int (*)(VaContext* c);
using guest_vprintf_func_t      = int (*)(const char* str, VaList* c);

guest_printf_std_func_t   GetGuestPrintfStdFunc();
guest_printf_ctx_func_t   GetGuestPrintfCtxFunc();
guest_snprintf_ctx_func_t GetGuestSnprintfCtxFunc();
guest_vprintf_func_t      GetGuestVprintfFunc();

} // namespace Libs

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_GUEST_PRINTF_H_ */
