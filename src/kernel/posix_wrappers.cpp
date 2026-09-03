// ProsperoLayer PS5 emulator - POSIX compatibility layer implementation
//
// All functions forward to the LibKernel implementations (pthread.h /
// time.h) or provide a functional no-op where the guest API is unused.

#include "kernel/posix.h"
#include "kernel/pthread.h"
#include "kernel/time.h"

#include <cstring>

namespace Libs::Posix {

namespace K = Libs::LibKernel;

// ===========================================================================
// Threads
// ===========================================================================

int KYTY_SYSV_ABI pthread_create(void** thread, const void* attr,
                                 void* (*start_routine)(void*), void* arg) {
        return K::PthreadCreate(reinterpret_cast<K::Pthread*>(thread),
                                reinterpret_cast<const K::PthreadAttr*>(attr), start_routine, arg);
}

int KYTY_SYSV_ABI pthread_create_name_np(void** thread, const void* attr,
                                         void* (*start_routine)(void*), void* arg,
                                         const char* name) {
        return K::PthreadCreateNameNp(reinterpret_cast<K::Pthread*>(thread),
                                      reinterpret_cast<const K::PthreadAttr*>(attr), start_routine,
                                      arg, name);
}

int KYTY_SYSV_ABI pthread_join(void* thread, void** retval) {
        return K::PthreadJoin(thread, retval);
}

int KYTY_SYSV_ABI pthread_detach(void* thread) {
        return K::PthreadDetach(thread);
}

int KYTY_SYSV_ABI pthread_exit(void* retval) {
        return K::PthreadExit(retval);
}

void* KYTY_SYSV_ABI pthread_self() {
        return K::PthreadSelf();
}

int KYTY_SYSV_ABI pthread_yield() {
        return K::PthreadYield();
}

int KYTY_SYSV_ABI pthread_setcancelstate(int state, int* oldstate) {
        return K::PthreadSetcancelstate(state, oldstate);
}

int KYTY_SYSV_ABI pthread_getschedparam(void* thread, int* policy, void* param) {
        (void)thread;
        (void)policy;
        (void)param;
        return 0;
}

int KYTY_SYSV_ABI pthread_setschedparam(void* thread, int policy, const void* param) {
        (void)thread;
        (void)policy;
        (void)param;
        return 0;
}

int KYTY_SYSV_ABI pthread_setprio(void* thread, int prio) {
        return K::PthreadSetprio(thread, prio);
}

int KYTY_SYSV_ABI pthread_rename_np(void* thread, const char* name) {
        return K::PthreadRename(thread, name);
}

// ===========================================================================
// Keys / TLS
// ===========================================================================

int KYTY_SYSV_ABI pthread_key_create(int* key, void (*destructor)(void*)) {
        return K::PthreadKeyCreate(reinterpret_cast<K::PthreadKey*>(key), destructor);
}

int KYTY_SYSV_ABI pthread_key_delete(int key) {
        return K::PthreadKeyDelete(key);
}

int KYTY_SYSV_ABI pthread_setspecific(int key, const void* value) {
        return K::PthreadSetspecific(key, value);
}

void* KYTY_SYSV_ABI pthread_getspecific(int key) {
        return K::PthreadGetspecific(key);
}

// ===========================================================================
// Attributes
// ===========================================================================

int KYTY_SYSV_ABI pthread_attr_init(void** attr) {
        return K::PthreadAttrInit(reinterpret_cast<K::PthreadAttr*>(attr));
}

int KYTY_SYSV_ABI pthread_attr_destroy(void** attr) {
        return K::PthreadAttrDestroy(reinterpret_cast<K::PthreadAttr*>(attr));
}

int KYTY_SYSV_ABI pthread_attr_get_np(void* thread, void** attr) {
        (void)thread;
        return K::PthreadAttrInit(reinterpret_cast<K::PthreadAttr*>(attr));
}

int KYTY_SYSV_ABI pthread_attr_setdetachstate(void** attr, int detachstate) {
        return K::PthreadAttrSetdetachstate(reinterpret_cast<K::PthreadAttr*>(attr), detachstate);
}

int KYTY_SYSV_ABI pthread_attr_getdetachstate(const void** attr, int* detachstate) {
        auto a = const_cast<void**>(attr);
        return K::PthreadAttrGetdetachstate(reinterpret_cast<const K::PthreadAttr*>(a), detachstate);
}

int KYTY_SYSV_ABI pthread_attr_setstacksize(void** attr, size_t stacksize) {
        return K::PthreadAttrSetstacksize(reinterpret_cast<K::PthreadAttr*>(attr), stacksize);
}

int KYTY_SYSV_ABI pthread_attr_getstacksize(const void** attr, size_t* stacksize) {
        auto a = const_cast<void**>(attr);
        return K::PthreadAttrGetstacksize(reinterpret_cast<const K::PthreadAttr*>(a), stacksize);
}

int KYTY_SYSV_ABI pthread_attr_setstack(void** attr, void* stackaddr, size_t stacksize) {
        return K::PthreadAttrSetstack(reinterpret_cast<K::PthreadAttr*>(attr), stackaddr, stacksize);
}

int KYTY_SYSV_ABI pthread_attr_getstack(const void** attr, void** stackaddr, size_t* stacksize) {
        auto a = const_cast<void**>(attr);
        return K::PthreadAttrGetstack(reinterpret_cast<const K::PthreadAttr*>(a), stackaddr,
                                      stacksize);
}

int KYTY_SYSV_ABI pthread_attr_setguardsize(void** attr, size_t guardsize) {
        return K::PthreadAttrSetguardsize(reinterpret_cast<K::PthreadAttr*>(attr), guardsize);
}

int KYTY_SYSV_ABI pthread_attr_getguardsize(const void** attr, size_t* guardsize) {
        auto a = const_cast<void**>(attr);
        return K::PthreadAttrGetguardsize(reinterpret_cast<const K::PthreadAttr*>(a), guardsize);
}

int KYTY_SYSV_ABI pthread_attr_setschedparam(void** attr, const void* param) {
        return K::PthreadAttrSetschedparam(reinterpret_cast<K::PthreadAttr*>(attr), param);
}

int KYTY_SYSV_ABI pthread_attr_getschedparam(const void** attr, void* param) {
        auto a = const_cast<void**>(attr);
        return K::PthreadAttrGetschedparam(reinterpret_cast<const K::PthreadAttr*>(a), param);
}

int KYTY_SYSV_ABI pthread_attr_setschedpolicy(void** attr, int policy) {
        return K::PthreadAttrSetschedpolicy(reinterpret_cast<K::PthreadAttr*>(attr), policy);
}

int KYTY_SYSV_ABI pthread_attr_getschedpolicy(const void** attr, int* policy) {
        auto a = const_cast<void**>(attr);
        return K::PthreadAttrGetschedpolicy(reinterpret_cast<const K::PthreadAttr*>(a), policy);
}

int KYTY_SYSV_ABI pthread_attr_setinheritsched(void** attr, int inherit) {
        return K::PthreadAttrSetinheritsched(reinterpret_cast<K::PthreadAttr*>(attr), inherit);
}

int KYTY_SYSV_ABI pthread_getstack(void* thread, void** stackaddr, size_t* stacksize) {
        uint64_t addr = 0;
        uint64_t size = 0;
        if (!K::PthreadGetGuestStack(thread, &addr, &size)) {
                return 22; // EINVAL
        }
        if (stackaddr != nullptr) {
                *stackaddr = reinterpret_cast<void*>(addr);
        }
        if (stacksize != nullptr) {
                *stacksize = size;
        }
        return 0;
}

// ===========================================================================
// Mutexes
// ===========================================================================

int KYTY_SYSV_ABI pthread_mutex_init(void** mutex, const void** attr) {
        (void)attr;
        return K::PthreadMutexInit(reinterpret_cast<K::PthreadMutex*>(mutex),
                                   static_cast<const K::PthreadMutexattr*>(nullptr));
}

int KYTY_SYSV_ABI pthread_mutex_init(void** mutex, int /*type*/) {
        return K::PthreadMutexInit(reinterpret_cast<K::PthreadMutex*>(mutex),
                                   static_cast<const K::PthreadMutexattr*>(nullptr));
}

int KYTY_SYSV_ABI pthread_mutex_destroy(void** mutex) {
        return K::PthreadMutexDestroy(reinterpret_cast<K::PthreadMutex*>(mutex));
}

int KYTY_SYSV_ABI pthread_mutex_lock(void** mutex) {
        return K::PthreadMutexLock(reinterpret_cast<K::PthreadMutex*>(mutex));
}

int KYTY_SYSV_ABI pthread_mutex_trylock(void** mutex) {
        return K::PthreadMutexTrylock(reinterpret_cast<K::PthreadMutex*>(mutex));
}

int KYTY_SYSV_ABI pthread_mutex_unlock(void** mutex) {
        return K::PthreadMutexUnlock(reinterpret_cast<K::PthreadMutex*>(mutex));
}

int KYTY_SYSV_ABI pthread_mutex_timedlock(void** mutex, const void* abstime) {
        return K::PthreadMutexTimedlock(reinterpret_cast<K::PthreadMutex*>(mutex), abstime);
}

int KYTY_SYSV_ABI pthread_mutexattr_init(void** attr) {
        return K::PthreadMutexattrInit(reinterpret_cast<K::PthreadMutexattr*>(attr));
}

int KYTY_SYSV_ABI pthread_mutexattr_destroy(void** attr) {
        return K::PthreadMutexattrDestroy(reinterpret_cast<K::PthreadMutexattr*>(attr));
}

int KYTY_SYSV_ABI pthread_mutexattr_settype(void** attr, int type) {
        return K::PthreadMutexattrSettype(reinterpret_cast<K::PthreadMutexattr*>(attr), type);
}

int KYTY_SYSV_ABI pthread_mutexattr_setprotocol(void** attr, int protocol) {
        return K::PthreadMutexattrSetprotocol(reinterpret_cast<K::PthreadMutexattr*>(attr),
                                              protocol);
}

// ===========================================================================
// Condition variables
// ===========================================================================

int KYTY_SYSV_ABI pthread_cond_init(void** cond, const void** attr) {
        (void)attr;
        return K::PthreadCondInit(reinterpret_cast<K::PthreadCond*>(cond),
                                  static_cast<const K::PthreadCondattr*>(nullptr));
}

int KYTY_SYSV_ABI pthread_cond_destroy(void** cond) {
        return K::PthreadCondDestroy(reinterpret_cast<K::PthreadCond*>(cond));
}

int KYTY_SYSV_ABI pthread_cond_signal(void** cond) {
        return K::PthreadCondSignal(reinterpret_cast<K::PthreadCond*>(cond));
}

int KYTY_SYSV_ABI pthread_cond_broadcast(void** cond) {
        return K::PthreadCondBroadcast(reinterpret_cast<K::PthreadCond*>(cond));
}

int KYTY_SYSV_ABI pthread_cond_wait(void** cond, void** mutex) {
        return K::PthreadCondWait(reinterpret_cast<K::PthreadCond*>(cond),
                                  reinterpret_cast<K::PthreadMutex*>(mutex));
}

int KYTY_SYSV_ABI pthread_cond_timedwait(void** cond, void** mutex, const void* abstime) {
        return K::PthreadCondTimedwait(reinterpret_cast<K::PthreadCond*>(cond),
                                       reinterpret_cast<K::PthreadMutex*>(mutex), abstime);
}

int KYTY_SYSV_ABI pthread_condattr_init(void** attr) {
        return K::PthreadCondattrInit(reinterpret_cast<K::PthreadCondattr*>(attr));
}

int KYTY_SYSV_ABI pthread_condattr_destroy(void** attr) {
        return K::PthreadCondattrDestroy(reinterpret_cast<K::PthreadCondattr*>(attr));
}

int KYTY_SYSV_ABI pthread_condattr_setclock(void** attr, int clock_id) {
        (void)attr;
        (void)clock_id;
        return 0;
}

// ===========================================================================
// Read-write locks
// ===========================================================================

int KYTY_SYSV_ABI pthread_rwlock_init(void** rwlock, const void** attr) {
        (void)attr;
        return K::PthreadRwlockInit(reinterpret_cast<K::PthreadRwlock*>(rwlock),
                                    static_cast<const K::PthreadRwlockattr*>(nullptr));
}

int KYTY_SYSV_ABI pthread_rwlock_destroy(void** rwlock) {
        return K::PthreadRwlockDestroy(reinterpret_cast<K::PthreadRwlock*>(rwlock));
}

int KYTY_SYSV_ABI pthread_rwlock_rdlock(void** rwlock) {
        return K::PthreadRwlockRdlock(reinterpret_cast<K::PthreadRwlock*>(rwlock));
}

int KYTY_SYSV_ABI pthread_rwlock_wrlock(void** rwlock) {
        return K::PthreadRwlockWrlock(reinterpret_cast<K::PthreadRwlock*>(rwlock));
}

int KYTY_SYSV_ABI pthread_rwlock_unlock(void** rwlock) {
        return K::PthreadRwlockUnlock(reinterpret_cast<K::PthreadRwlock*>(rwlock));
}

// ===========================================================================
// Once / sched
// ===========================================================================

int KYTY_SYSV_ABI pthread_once(void* once_control, void (*init_routine)(void)) {
        (void)once_control;
        if (init_routine != nullptr) {
                init_routine();
        }
        return 0;
}

int KYTY_SYSV_ABI sched_get_priority_min(int policy) {
        (void)policy;
        return 1;
}

int KYTY_SYSV_ABI sched_get_priority_max(int policy) {
        (void)policy;
        return 99;
}

// ===========================================================================
// Semaphores
// ===========================================================================

int KYTY_SYSV_ABI sem_init(void* sem, int shared, unsigned int value) {
        (void)shared;
        if (sem != nullptr) {
                *static_cast<unsigned int*>(sem) = value;
        }
        return 0;
}

int KYTY_SYSV_ABI sem_destroy(void* sem) {
        (void)sem;
        return 0;
}

int KYTY_SYSV_ABI sem_wait(void* sem) {
        (void)sem;
        return 0;
}

int KYTY_SYSV_ABI sem_trywait(void* sem) {
        (void)sem;
        return 0;
}

int KYTY_SYSV_ABI sem_timedwait(void* sem, const void* abstime) {
        (void)sem;
        (void)abstime;
        return 0;
}

int KYTY_SYSV_ABI sem_reltimedwait_np(void* sem, const void* reltime) {
        (void)sem;
        (void)reltime;
        return 0;
}

int KYTY_SYSV_ABI sem_post(void* sem) {
        (void)sem;
        return 0;
}

int KYTY_SYSV_ABI sem_getvalue(void* sem, int* value) {
        (void)sem;
        if (value != nullptr) {
                *value = 1;
        }
        return 0;
}

} // namespace Libs::Posix