#pragma once
// ProsperoLayer PS5 emulator - libkernel event queue subsystem (Kyty-compatible interface)
#include "common/common.h"
#include <cstdint>

namespace Libs::LibKernel {

namespace EventQueue {

using KernelEqueue = int32_t;
using KernelEventId = int32_t;

constexpr KernelEqueue KERNEL_EQUEUE_INVALID = -1;

constexpr uint16_t KERNEL_EVFILT_USER     = 0x100u;
constexpr uint16_t KERNEL_EVFILT_HRTIMER  = 0x101u;
constexpr uint16_t KERNEL_EVFILT_SIGNAL   = 0x103u;
constexpr uint16_t KERNEL_EVFILT_GRAPHICS = 0x200u;

struct KernelEvent {
        uint16_t filter;
        uint16_t flags;
        uint32_t fflags;
        uint64_t data;
        uint64_t udata;
        int32_t  event_id;
        int32_t  error;
        uint64_t ident;
};

int KYTY_SYSV_ABI KernelCreateEqueue(KernelEqueue* eq, const char* name, uint32_t attr);
int KYTY_SYSV_ABI KernelDeleteEqueue(KernelEqueue eq);
int KYTY_SYSV_ABI KernelWaitEqueue(KernelEqueue eq, KernelEvent* ev, int32_t num, int32_t* out_num,
                                   uint32_t* timeout);
int KYTY_SYSV_ABI KernelGetEventUserData(const KernelEvent* ev, uint64_t* udata);
int KYTY_SYSV_ABI KernelGetEventId(const KernelEvent* ev);
int KYTY_SYSV_ABI KernelGetEventFilter(const KernelEvent* ev);
int KYTY_SYSV_ABI KernelGetEventData(const KernelEvent* ev, uint32_t* data);
int KYTY_SYSV_ABI KernelGetEventFflags(const KernelEvent* ev, int32_t* fflags);
int KYTY_SYSV_ABI KernelGetEventError(const KernelEvent* ev);
int KYTY_SYSV_ABI KernelAddUserEvent(KernelEqueue eq, int32_t* id, uint64_t udata);
int KYTY_SYSV_ABI KernelAddUserEventEdge(KernelEqueue eq, int32_t* id, uint64_t udata);
int KYTY_SYSV_ABI KernelTriggerUserEvent(KernelEqueue eq, int32_t id, uint32_t data);
int KYTY_SYSV_ABI KernelDeleteUserEvent(KernelEqueue eq, int32_t id);
int KYTY_SYSV_ABI KernelPostEvent(KernelEqueue eq, const KernelEvent& ev);
int KYTY_SYSV_ABI KernelAddHRTimerEvent(KernelEqueue eq, int32_t* id, uint64_t udata,
                                        uint32_t type, int64_t time);
int KYTY_SYSV_ABI KernelDeleteHRTimerEvent(KernelEqueue eq, int32_t id);
int KYTY_SYSV_ABI KernelAddAmprEvent(KernelEqueue eq, int32_t* id, uint64_t udata, void* ampr);
int KYTY_SYSV_ABI KernelAddAmprSystemEvent(KernelEqueue eq, int32_t* id, uint64_t udata,
                                           void* ampr);
int KYTY_SYSV_ABI KernelDeleteAmprEvent(KernelEqueue eq, int32_t id);
int KYTY_SYSV_ABI KernelDeleteAmprSystemEvent(KernelEqueue eq, int32_t id);

} // namespace EventQueue

} // namespace Libs::LibKernel
