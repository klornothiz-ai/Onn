#pragma once
// ProsperoLayer PS5 emulator - POSIX compatibility layer (Kyty-compatible)
//
// These functions are registered into the guest "Posix" library and forward
// to the LibKernel implementations. They are declared in namespace Libs::Posix
// and defined in src/kernel/posix_wrappers.cpp.
#include "common/abi.h"
#include <cstddef>
#include <cstdint>

namespace Libs::Posix {

// --- threads ---
int  KYTY_SYSV_ABI pthread_create(void** thread, const void* attr,
                                  void* (*start_routine)(void*), void* arg);
int  KYTY_SYSV_ABI pthread_create_name_np(void** thread, const void* attr,
                                          void* (*start_routine)(void*), void* arg,
                                          const char* name);
int  KYTY_SYSV_ABI pthread_join(void* thread, void** retval);
int  KYTY_SYSV_ABI pthread_detach(void* thread);
int  KYTY_SYSV_ABI pthread_exit(void* retval);
void* KYTY_SYSV_ABI pthread_self();
int  KYTY_SYSV_ABI pthread_yield();
int  KYTY_SYSV_ABI pthread_setcancelstate(int state, int* oldstate);
int  KYTY_SYSV_ABI pthread_getschedparam(void* thread, int* policy, void* param);
int  KYTY_SYSV_ABI pthread_setschedparam(void* thread, int policy, const void* param);
int  KYTY_SYSV_ABI pthread_setprio(void* thread, int prio);
int  KYTY_SYSV_ABI pthread_rename_np(void* thread, const char* name);

// --- keys / TLS ---
int  KYTY_SYSV_ABI pthread_key_create(int* key, void (*destructor)(void*));
int  KYTY_SYSV_ABI pthread_key_delete(int key);
int  KYTY_SYSV_ABI pthread_setspecific(int key, const void* value);
void* KYTY_SYSV_ABI pthread_getspecific(int key);

// --- attributes ---
int KYTY_SYSV_ABI pthread_attr_init(void** attr);
int KYTY_SYSV_ABI pthread_attr_destroy(void** attr);
int KYTY_SYSV_ABI pthread_attr_get_np(void* thread, void** attr);
int KYTY_SYSV_ABI pthread_attr_setdetachstate(void** attr, int detachstate);
int KYTY_SYSV_ABI pthread_attr_getdetachstate(const void** attr, int* detachstate);
int KYTY_SYSV_ABI pthread_attr_setstacksize(void** attr, size_t stacksize);
int KYTY_SYSV_ABI pthread_attr_getstacksize(const void** attr, size_t* stacksize);
int KYTY_SYSV_ABI pthread_attr_setstack(void** attr, void* stackaddr, size_t stacksize);
int KYTY_SYSV_ABI pthread_attr_getstack(const void** attr, void** stackaddr, size_t* stacksize);
int KYTY_SYSV_ABI pthread_attr_setguardsize(void** attr, size_t guardsize);
int KYTY_SYSV_ABI pthread_attr_getguardsize(const void** attr, size_t* guardsize);
int KYTY_SYSV_ABI pthread_attr_setschedparam(void** attr, const void* param);
int KYTY_SYSV_ABI pthread_attr_getschedparam(const void** attr, void* param);
int KYTY_SYSV_ABI pthread_attr_setschedpolicy(void** attr, int policy);
int KYTY_SYSV_ABI pthread_attr_getschedpolicy(const void** attr, int* policy);
int KYTY_SYSV_ABI pthread_attr_setinheritsched(void** attr, int inherit);
int KYTY_SYSV_ABI pthread_getstack(void* thread, void** stackaddr, size_t* stacksize);

// --- mutexes ---
int KYTY_SYSV_ABI pthread_mutex_init(void** mutex, const void** attr);
int KYTY_SYSV_ABI pthread_mutex_init(void** mutex, int type);
// Explicit non-template pointer helper for the overloaded symbol so
// LIB_FUNC("...", Posix::pthread_mutex_init) resolves unambiguously.
inline int (*pthread_mutex_init_fn(void** mutex, const void** attr))(void**, const void**) {
        (void)mutex;
        (void)attr;
        return &pthread_mutex_init;
}
int KYTY_SYSV_ABI pthread_mutex_destroy(void** mutex);
int KYTY_SYSV_ABI pthread_mutex_lock(void** mutex);
int KYTY_SYSV_ABI pthread_mutex_trylock(void** mutex);
int KYTY_SYSV_ABI pthread_mutex_unlock(void** mutex);
int KYTY_SYSV_ABI pthread_mutex_timedlock(void** mutex, const void* abstime);
int KYTY_SYSV_ABI pthread_mutexattr_init(void** attr);
int KYTY_SYSV_ABI pthread_mutexattr_destroy(void** attr);
int KYTY_SYSV_ABI pthread_mutexattr_settype(void** attr, int type);
int KYTY_SYSV_ABI pthread_mutexattr_setprotocol(void** attr, int protocol);

// --- condition variables ---
int KYTY_SYSV_ABI pthread_cond_init(void** cond, const void** attr);
int KYTY_SYSV_ABI pthread_cond_destroy(void** cond);
int KYTY_SYSV_ABI pthread_cond_signal(void** cond);
int KYTY_SYSV_ABI pthread_cond_broadcast(void** cond);
int KYTY_SYSV_ABI pthread_cond_wait(void** cond, void** mutex);
int KYTY_SYSV_ABI pthread_cond_timedwait(void** cond, void** mutex, const void* abstime);
int KYTY_SYSV_ABI pthread_condattr_init(void** attr);
int KYTY_SYSV_ABI pthread_condattr_destroy(void** attr);
int KYTY_SYSV_ABI pthread_condattr_setclock(void** attr, int clock_id);

// --- rwlocks ---
int KYTY_SYSV_ABI pthread_rwlock_init(void** rwlock, const void** attr);
int KYTY_SYSV_ABI pthread_rwlock_destroy(void** rwlock);
int KYTY_SYSV_ABI pthread_rwlock_rdlock(void** rwlock);
int KYTY_SYSV_ABI pthread_rwlock_wrlock(void** rwlock);
int KYTY_SYSV_ABI pthread_rwlock_unlock(void** rwlock);

// --- once / sched ---
int KYTY_SYSV_ABI pthread_once(void* once_control, void (*init_routine)(void));
int KYTY_SYSV_ABI sched_get_priority_min(int policy);
int KYTY_SYSV_ABI sched_get_priority_max(int policy);

// --- semaphores ---
int KYTY_SYSV_ABI sem_init(void* sem, int shared, unsigned int value);
int KYTY_SYSV_ABI sem_destroy(void* sem);
int KYTY_SYSV_ABI sem_wait(void* sem);
int KYTY_SYSV_ABI sem_trywait(void* sem);
int KYTY_SYSV_ABI sem_timedwait(void* sem, const void* abstime);
int KYTY_SYSV_ABI sem_reltimedwait_np(void* sem, const void* reltime);
int KYTY_SYSV_ABI sem_post(void* sem);
int KYTY_SYSV_ABI sem_getvalue(void* sem, int* value);

} // namespace Libs::Posix
