#pragma once
// ProsperoLayer PS5 emulator - libkernel pthread subsystem (Kyty-compatible interface)
#include "common/common.h"
#include <cstdint>
#include <cstddef>

namespace Libs::LibKernel {

using Pthread           = void*;
using PthreadMutex      = void*;
using PthreadCond       = void*;
using PthreadRwlock     = void*;
using PthreadKey        = int32_t;

// CPU affinity mask (bitmask of logical CPUs).
using KernelCpumask = uint64_t;
using PthreadAttr       = void*;
using PthreadMutexattr  = void*;
using PthreadCondattr   = void*;
using PthreadRwlockattr = void*;
using PthreadTls        = void*;

constexpr int PTHREAD_CREATE_JOINABLE     = 0;
constexpr int PTHREAD_CREATE_DETACHED     = 1;
constexpr int PTHREAD_PROCESS_PRIVATE     = 0;
constexpr int PTHREAD_PROCESS_SHARED      = 1;
constexpr int PTHREAD_SCOPE_SYSTEM        = 0;
constexpr int PTHREAD_SCOPE_PROCESS       = 1;
constexpr int PTHREAD_INHERIT_SCHED       = 0;
constexpr int PTHREAD_EXPLICIT_SCHED      = 1;
constexpr int PTHREAD_CANCEL_ENABLE       = 0;
constexpr int PTHREAD_CANCEL_DISABLE      = 1;
constexpr int PTHREAD_CANCEL_DEFERRED     = 0;
constexpr int PTHREAD_CANCEL_ASYNCHRONOUS = 1;

// --- Internal thread management (used by signal dispatch & scheduler) ---

bool PthreadGetGuestStack(Pthread thread, uint64_t* addr, uint64_t* size);
void PthreadQueuePendingSignal(Pthread thread, int signum);
bool PthreadHasPendingSignal(Pthread thread, int signum);
bool PthreadTakePendingSignal(Pthread thread, int signum);
Pthread PthreadSelfOrNull();
void PthreadWakeForSignal(Pthread thread);
bool PthreadKillHost(Pthread thread, int signum);
uint32_t PthreadGetUniqueId(Pthread thread);
Pthread PthreadSwapSelfForSignal(Pthread thread);
uint64_t PthreadGetHostThreadId(Pthread thread);

// --- Public POSIX-like API (registered into the guest symbol table) ---

int KYTY_SYSV_ABI PthreadCreate(Pthread* thread, const PthreadAttr* attr,
                                void* (*start_routine)(void*), void* arg);
int KYTY_SYSV_ABI PthreadCreateNameNp(Pthread* thread, const PthreadAttr* attr,
                                      void* (*start_routine)(void*), void* arg, const char* name);
int KYTY_SYSV_ABI PthreadJoin(Pthread thread, void** retval);
int KYTY_SYSV_ABI PthreadDetach(Pthread thread);
int KYTY_SYSV_ABI PthreadExit(void* retval);
Pthread KYTY_SYSV_ABI PthreadSelf();
int KYTY_SYSV_ABI PthreadEqual(Pthread t1, Pthread t2);
int KYTY_SYSV_ABI PthreadRename(Pthread thread, const char* name);
int KYTY_SYSV_ABI PthreadYield();
int KYTY_SYSV_ABI PthreadGetthreadid(Pthread thread);
int KYTY_SYSV_ABI PthreadSetcancelstate(int state, int* oldstate);
int KYTY_SYSV_ABI PthreadSetcanceltype(int type, int* oldtype);
int KYTY_SYSV_ABI PthreadSetprio(Pthread thread, int prio);
int KYTY_SYSV_ABI PthreadGetprio(Pthread thread, int* prio);
int KYTY_SYSV_ABI PthreadSetaffinity(Pthread thread, size_t cpusetsize, const void* cpuset);
int KYTY_SYSV_ABI PthreadGetaffinity(Pthread thread, size_t cpusetsize, void* cpuset);
int KYTY_SYSV_ABI PthreadGetname(Pthread thread, char* name, size_t namelen);
int KYTY_SYSV_ABI PthreadSetspecific(PthreadKey key, const void* value);
void* KYTY_SYSV_ABI PthreadGetspecific(PthreadKey key);
int KYTY_SYSV_ABI PthreadKeyCreate(PthreadKey* key, void (*destructor)(void*));
int KYTY_SYSV_ABI PthreadKeyDelete(PthreadKey key);

int KYTY_SYSV_ABI PthreadAttrInit(PthreadAttr* attr);
int KYTY_SYSV_ABI PthreadAttrDestroy(PthreadAttr* attr);
int KYTY_SYSV_ABI PthreadAttrGet(const PthreadAttr* attr, int* detachstate, size_t* stacksize,
                                 void** stackaddr);
int KYTY_SYSV_ABI PthreadAttrSetdetachstate(PthreadAttr* attr, int detachstate);
int KYTY_SYSV_ABI PthreadAttrGetdetachstate(const PthreadAttr* attr, int* detachstate);
int KYTY_SYSV_ABI PthreadAttrSetstacksize(PthreadAttr* attr, size_t stacksize);
int KYTY_SYSV_ABI PthreadAttrGetstacksize(const PthreadAttr* attr, size_t* stacksize);
int KYTY_SYSV_ABI PthreadAttrSetstackaddr(PthreadAttr* attr, void* stackaddr);
int KYTY_SYSV_ABI PthreadAttrGetstackaddr(const PthreadAttr* attr, void** stackaddr);
int KYTY_SYSV_ABI PthreadAttrSetstack(PthreadAttr* attr, void* stackaddr, size_t stacksize);
int KYTY_SYSV_ABI PthreadAttrGetstack(const PthreadAttr* attr, void** stackaddr, size_t* stacksize);
int KYTY_SYSV_ABI PthreadAttrSetguardsize(PthreadAttr* attr, size_t guardsize);
int KYTY_SYSV_ABI PthreadAttrGetguardsize(const PthreadAttr* attr, size_t* guardsize);
int KYTY_SYSV_ABI PthreadAttrSetschedparam(PthreadAttr* attr, const void* param);
int KYTY_SYSV_ABI PthreadAttrGetschedparam(const PthreadAttr* attr, void* param);
int KYTY_SYSV_ABI PthreadAttrSetschedpolicy(PthreadAttr* attr, int policy);
int KYTY_SYSV_ABI PthreadAttrGetschedpolicy(const PthreadAttr* attr, int* policy);
int KYTY_SYSV_ABI PthreadAttrSetinheritsched(PthreadAttr* attr, int inherit);
int KYTY_SYSV_ABI PthreadAttrSetsolosched(PthreadAttr* attr, int solo);
int KYTY_SYSV_ABI PthreadAttrGetsolosched(const PthreadAttr* attr, int* solo);
int KYTY_SYSV_ABI PthreadAttrSetaffinity(PthreadAttr* attr, size_t cpusetsize, const void* cpuset);
int KYTY_SYSV_ABI PthreadAttrGetaffinity(const PthreadAttr* attr, size_t cpusetsize, void* cpuset);

int KYTY_SYSV_ABI PthreadMutexInit(PthreadMutex* mutex, const PthreadMutexattr* attr);
int KYTY_SYSV_ABI PthreadMutexInit(void** mutex, int type);
int KYTY_SYSV_ABI PthreadMutexDestroy(PthreadMutex* mutex);
int KYTY_SYSV_ABI PthreadMutexLock(PthreadMutex* mutex);
int KYTY_SYSV_ABI PthreadMutexTrylock(PthreadMutex* mutex);
int KYTY_SYSV_ABI PthreadMutexUnlock(PthreadMutex* mutex);
int KYTY_SYSV_ABI PthreadMutexTimedlock(PthreadMutex* mutex, const void* abstime);
int KYTY_SYSV_ABI PthreadMutexattrInit(PthreadMutexattr* attr);
int KYTY_SYSV_ABI PthreadMutexattrDestroy(PthreadMutexattr* attr);
int KYTY_SYSV_ABI PthreadMutexattrSettype(PthreadMutexattr* attr, int type);
int KYTY_SYSV_ABI PthreadMutexattrSetprotocol(PthreadMutexattr* attr, int protocol);

int KYTY_SYSV_ABI PthreadCondInit(PthreadCond* cond, const PthreadCondattr* attr);
int KYTY_SYSV_ABI PthreadCondDestroy(PthreadCond* cond);
int KYTY_SYSV_ABI PthreadCondSignal(PthreadCond* cond);
int KYTY_SYSV_ABI PthreadCondSignalto(PthreadCond* cond);
int KYTY_SYSV_ABI PthreadCondBroadcast(PthreadCond* cond);
int KYTY_SYSV_ABI PthreadCondWait(PthreadCond* cond, PthreadMutex* mutex);
int KYTY_SYSV_ABI PthreadCondTimedwait(PthreadCond* cond, PthreadMutex* mutex,
                                       const void* abstime);
int KYTY_SYSV_ABI PthreadCondattrInit(PthreadCondattr* attr);
int KYTY_SYSV_ABI PthreadCondattrDestroy(PthreadCondattr* attr);

int KYTY_SYSV_ABI PthreadRwlockInit(PthreadRwlock* rwlock, const PthreadRwlockattr* attr);
int KYTY_SYSV_ABI PthreadRwlockDestroy(PthreadRwlock* rwlock);
int KYTY_SYSV_ABI PthreadRwlockRdlock(PthreadRwlock* rwlock);
int KYTY_SYSV_ABI PthreadRwlockTryrdlock(PthreadRwlock* rwlock);
int KYTY_SYSV_ABI PthreadRwlockWrlock(PthreadRwlock* rwlock);
int KYTY_SYSV_ABI PthreadRwlockTrywrlock(PthreadRwlock* rwlock);
int KYTY_SYSV_ABI PthreadRwlockUnlock(PthreadRwlock* rwlock);
int KYTY_SYSV_ABI PthreadRwlockattrInit(PthreadRwlockattr* attr);
int KYTY_SYSV_ABI PthreadRwlockattrDestroy(PthreadRwlockattr* attr);
int KYTY_SYSV_ABI PthreadRwlockattrSettype(PthreadRwlockattr* attr, int type);

// Called by sceKernelSetThreadDtors: no-op bookkeeping in this build.
void KYTY_SYSV_ABI KernelSetThreadDtors();

} // namespace Libs::LibKernel
