// ProsperoLayer PS5 emulator - libkernel pthread subsystem implementation
//
// Implements the PS5 pthread API on top of host std::thread. Guest threads
// are tracked in a registry so signal dispatch and the scheduler can find
// them. Mutex/cond/rwlock are thin wrappers around the C++11 primitives.

#include "kernel/pthread.h"
#include "common/threads.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Libs::LibKernel {

namespace {

// ---------------------------------------------------------------------------
// Thread registry
// ---------------------------------------------------------------------------

struct ThreadRecord {
        std::thread               host_thread;
        uint64_t                  guest_stack_addr{0};
        uint64_t                  guest_stack_size{0};
        std::atomic<uint32_t>     pending_signals{0};
        std::mutex                wake_mutex;
        std::condition_variable   wake_cv;
        bool                      wake_flag{false};
        void*                     retval{nullptr};
        std::string               name;
        std::atomic<bool>         detached{false};
        // TLS
        std::unordered_map<int32_t, void*> tls_values;
};

Common::Mutex                          g_threads_mutex;
std::unordered_map<void*, std::unique_ptr<ThreadRecord>> g_threads;
std::atomic<uint32_t>                  g_next_unique_id{1};
thread_local void*                     g_current_thread{nullptr};
std::atomic<int32_t>                   g_next_key{1};
Common::Mutex                          g_tls_destructors_mutex;
std::unordered_map<int32_t, void (*)(void*)> g_tls_destructors;

void* CreateRecord() {
        auto rec          = std::make_unique<ThreadRecord>();
        void* handle      = rec.get();
        {
                std::lock_guard<Common::Mutex> lock(g_threads_mutex);
                g_threads[handle] = std::move(rec);
        }
        g_current_thread = handle;
        return handle;
}

ThreadRecord* FindRecord(void* handle) {
        if (handle == nullptr) {
                return nullptr;
        }
        std::lock_guard<Common::Mutex> lock(g_threads_mutex);
        auto it = g_threads.find(handle);
        return it != g_threads.end() ? it->second.get() : nullptr;
}

void RemoveRecord(void* handle) {
        std::lock_guard<Common::Mutex> lock(g_threads_mutex);
        g_threads.erase(handle);
}

// ---------------------------------------------------------------------------
// Mutex / cond / rwlock helpers
// ---------------------------------------------------------------------------

struct MutexImpl {
        std::mutex m;
};

struct CondImpl {
        std::condition_variable_any cv;
};

struct RwlockImpl {
        std::shared_mutex m;
};

} // namespace

// ===========================================================================
// Internal thread management API
// ===========================================================================

bool PthreadGetGuestStack(Pthread thread, uint64_t* addr, uint64_t* size) {
        auto* rec = FindRecord(thread);
        if (rec == nullptr) {
                return false;
        }
        if (addr != nullptr) {
                *addr = rec->guest_stack_addr;
        }
        if (size != nullptr) {
                *size = rec->guest_stack_size;
        }
        return rec->guest_stack_size != 0;
}

void PthreadQueuePendingSignal(Pthread thread, int signum) {
        auto* rec = FindRecord(thread);
        if (rec != nullptr && signum >= 0 && signum < 32) {
                rec->pending_signals.fetch_or(1u << static_cast<uint32_t>(signum),
                                              std::memory_order_relaxed);
                rec->wake_flag = true;
                rec->wake_cv.notify_all();
        }
}

bool PthreadHasPendingSignal(Pthread thread, int signum) {
        auto* rec = FindRecord(thread);
        if (rec == nullptr || signum < 0 || signum >= 32) {
                return false;
        }
        return (rec->pending_signals.load(std::memory_order_relaxed) &
                (1u << static_cast<uint32_t>(signum))) != 0;
}

bool PthreadTakePendingSignal(Pthread thread, int signum) {
        auto* rec = FindRecord(thread);
        if (rec == nullptr || signum < 0 || signum >= 32) {
                return false;
        }
        const uint32_t mask = 1u << static_cast<uint32_t>(signum);
        const uint32_t old  = rec->pending_signals.fetch_and(~mask, std::memory_order_relaxed);
        return (old & mask) != 0;
}

Pthread PthreadSelfOrNull() {
        return g_current_thread;
}

void PthreadWakeForSignal(Pthread thread) {
        auto* rec = FindRecord(thread);
        if (rec != nullptr) {
                rec->wake_flag = true;
                rec->wake_cv.notify_all();
        }
}

bool PthreadKillHost(Pthread thread, int signum) {
        // Host-side signal delivery is not supported; queuing is enough for
        // the guest signal-dispatch path used by the emulator.
        PthreadQueuePendingSignal(thread, signum);
        return true;
}

uint32_t PthreadGetUniqueId(Pthread thread) {
        auto* rec = FindRecord(thread);
        if (rec == nullptr) {
                return 0;
        }
        return reinterpret_cast<uintptr_t>(thread) & 0xFFFFFFFFu;
}

Pthread PthreadSwapSelfForSignal(Pthread thread) {
        auto* old = g_current_thread;
        g_current_thread = thread;
        return old;
}

uint64_t PthreadGetHostThreadId(Pthread thread) {
        auto* rec = FindRecord(thread);
        if (rec == nullptr) {
                return 0;
        }
        const auto id = rec->host_thread.get_id();
        std::hash<std::thread::id> hasher;
        return static_cast<uint64_t>(hasher(id));
}

// ===========================================================================
// Thread creation / control
// ===========================================================================

int KYTY_SYSV_ABI PthreadCreate(Pthread* thread, const PthreadAttr* attr,
                                void* (*start_routine)(void*), void* arg) {
        if (thread == nullptr || start_routine == nullptr) {
                return 22; // EINVAL
        }

        void* handle = CreateRecord();
        *thread      = handle;
        auto* rec    = FindRecord(handle);

        // Save guest stack info from the attribute if present.
        if (attr != nullptr && *attr != nullptr) {
                // Attribute layout is opaque; the emulator stores stack in a
                // side registry keyed by the attr handle in a full impl.
        }

        rec->host_thread = std::thread([start_routine, arg, handle]() {
                g_current_thread = handle;
                void* ret        = start_routine(arg);
                auto* rec2       = FindRecord(handle);
                if (rec2 != nullptr) {
                        rec2->retval = ret;
                        rec2->wake_flag = true;
                        rec2->wake_cv.notify_all();
                }
        });

        return 0;
}

int KYTY_SYSV_ABI PthreadCreateNameNp(Pthread* thread, const PthreadAttr* attr,
                                      void* (*start_routine)(void*), void* arg, const char* name) {
        const int result = PthreadCreate(thread, attr, start_routine, arg);
        if (result == 0) {
                auto* rec = FindRecord(*thread);
                if (rec != nullptr && name != nullptr) {
                        rec->name = name;
                }
        }
        return result;
}

int KYTY_SYSV_ABI PthreadJoin(Pthread thread, void** retval) {
        auto* rec = FindRecord(thread);
        if (rec == nullptr) {
                return 22; // EINVAL
        }
        if (rec->detached.load()) {
                return 22; // EINVAL
        }
        if (rec->host_thread.joinable()) {
                rec->host_thread.join();
        }
        if (retval != nullptr) {
                *retval = rec->retval;
        }
        return 0;
}

int KYTY_SYSV_ABI PthreadDetach(Pthread thread) {
        auto* rec = FindRecord(thread);
        if (rec == nullptr) {
                return 22; // EINVAL
        }
        rec->detached = true;
        if (rec->host_thread.joinable()) {
                rec->host_thread.detach();
        }
        return 0;
}

int KYTY_SYSV_ABI PthreadExit(void* retval) {
        if (g_current_thread != nullptr) {
                auto* rec = FindRecord(g_current_thread);
                if (rec != nullptr) {
                        rec->retval = retval;
                }
        }
        // We cannot exit a std::thread from within itself; the runnable
        // wrapper captures the return value, so just return.
        return 0;
}

Pthread KYTY_SYSV_ABI PthreadSelf() {
        if (g_current_thread == nullptr) {
                CreateRecord();
        }
        return g_current_thread;
}

int KYTY_SYSV_ABI PthreadEqual(Pthread t1, Pthread t2) {
        return t1 == t2 ? 0 : 1;
}

int KYTY_SYSV_ABI PthreadRename(Pthread thread, const char* name) {
        auto* rec = FindRecord(thread);
        if (rec == nullptr || name == nullptr) {
                return 22; // EINVAL
        }
        rec->name = name;
        return 0;
}

int KYTY_SYSV_ABI PthreadYield() {
        std::this_thread::yield();
        return 0;
}

int KYTY_SYSV_ABI PthreadGetthreadid(Pthread thread) {
        return static_cast<int>(PthreadGetUniqueId(thread));
}

int KYTY_SYSV_ABI PthreadSetcancelstate(int state, int* oldstate) {
        (void)state;
        if (oldstate != nullptr) {
                *oldstate = PTHREAD_CANCEL_ENABLE;
        }
        return 0;
}

int KYTY_SYSV_ABI PthreadSetcanceltype(int type, int* oldtype) {
        (void)type;
        if (oldtype != nullptr) {
                *oldtype = PTHREAD_CANCEL_DEFERRED;
        }
        return 0;
}

int KYTY_SYSV_ABI PthreadSetprio(Pthread /*thread*/, int /*prio*/) {
        return 0;
}

int KYTY_SYSV_ABI PthreadGetprio(Pthread /*thread*/, int* prio) {
        if (prio != nullptr) {
                *prio = 0;
        }
        return 0;
}

int KYTY_SYSV_ABI PthreadSetaffinity(Pthread /*thread*/, size_t /*cpusetsize*/,
                                     const void* /*cpuset*/) {
        return 0;
}

int KYTY_SYSV_ABI PthreadGetaffinity(Pthread /*thread*/, size_t cpusetsize, void* cpuset) {
        if (cpuset != nullptr) {
                std::memset(cpuset, 0, cpusetsize);
        }
        return 0;
}

int KYTY_SYSV_ABI PthreadGetname(Pthread thread, char* name, size_t namelen) {
        auto* rec = FindRecord(thread);
        if (rec == nullptr || name == nullptr || namelen == 0) {
                return 22; // EINVAL
        }
        std::strncpy(name, rec->name.c_str(), namelen - 1);
        name[namelen - 1] = '\0';
        return 0;
}

int KYTY_SYSV_ABI PthreadSetspecific(PthreadKey key, const void* value) {
        if (g_current_thread == nullptr) {
                CreateRecord();
        }
        auto* rec = FindRecord(g_current_thread);
        if (rec == nullptr) {
                return 22; // EINVAL
        }
        rec->tls_values[key] = const_cast<void*>(value);
        return 0;
}

void* KYTY_SYSV_ABI PthreadGetspecific(PthreadKey key) {
        if (g_current_thread == nullptr) {
                return nullptr;
        }
        auto* rec = FindRecord(g_current_thread);
        if (rec == nullptr) {
                return nullptr;
        }
        const auto it = rec->tls_values.find(key);
        return it != rec->tls_values.end() ? it->second : nullptr;
}

int KYTY_SYSV_ABI PthreadKeyCreate(PthreadKey* key, void (*destructor)(void*)) {
        if (key == nullptr) {
                return 22; // EINVAL
        }
        const int32_t k = g_next_key.fetch_add(1);
        *key            = k;
        if (destructor != nullptr) {
                std::lock_guard<Common::Mutex> lock(g_tls_destructors_mutex);
                g_tls_destructors[k] = destructor;
        }
        return 0;
}

int KYTY_SYSV_ABI PthreadKeyDelete(PthreadKey key) {
        std::lock_guard<Common::Mutex> lock(g_tls_destructors_mutex);
        g_tls_destructors.erase(key);
        return 0;
}

// ===========================================================================
// Attributes
// ===========================================================================

int KYTY_SYSV_ABI PthreadAttrInit(PthreadAttr* attr) {
        if (attr == nullptr) {
                return 22; // EINVAL
        }
        *attr = new (std::nothrow) int(0);
        return *attr != nullptr ? 0 : 12; // ENOMEM
}

int KYTY_SYSV_ABI PthreadAttrDestroy(PthreadAttr* attr) {
        if (attr == nullptr || *attr == nullptr) {
                return 22; // EINVAL
        }
        delete static_cast<int*>(*attr);
        *attr = nullptr;
        return 0;
}

int KYTY_SYSV_ABI PthreadAttrGet(const PthreadAttr* attr, int* detachstate, size_t* stacksize,
                                 void** stackaddr) {
        if (detachstate != nullptr) {
                *detachstate = PTHREAD_CREATE_JOINABLE;
        }
        if (stacksize != nullptr) {
                *stacksize = 0;
        }
        if (stackaddr != nullptr) {
                *stackaddr = nullptr;
        }
        (void)attr;
        return 0;
}

int KYTY_SYSV_ABI PthreadAttrSetdetachstate(PthreadAttr* /*attr*/, int /*detachstate*/) {
        return 0;
}

int KYTY_SYSV_ABI PthreadAttrGetdetachstate(const PthreadAttr* /*attr*/, int* detachstate) {
        if (detachstate != nullptr) {
                *detachstate = PTHREAD_CREATE_JOINABLE;
        }
        return 0;
}

int KYTY_SYSV_ABI PthreadAttrSetstacksize(PthreadAttr* /*attr*/, size_t /*stacksize*/) {
        return 0;
}

int KYTY_SYSV_ABI PthreadAttrGetstacksize(const PthreadAttr* /*attr*/, size_t* stacksize) {
        if (stacksize != nullptr) {
                *stacksize = 0;
        }
        return 0;
}

int KYTY_SYSV_ABI PthreadAttrSetstackaddr(PthreadAttr* /*attr*/, void* /*stackaddr*/) {
        return 0;
}

int KYTY_SYSV_ABI PthreadAttrGetstackaddr(const PthreadAttr* /*attr*/, void** stackaddr) {
        if (stackaddr != nullptr) {
                *stackaddr = nullptr;
        }
        return 0;
}

int KYTY_SYSV_ABI PthreadAttrSetstack(PthreadAttr* /*attr*/, void* /*stackaddr*/,
                                      size_t /*stacksize*/) {
        return 0;
}

int KYTY_SYSV_ABI PthreadAttrGetstack(const PthreadAttr* /*attr*/, void** stackaddr,
                                      size_t* stacksize) {
        if (stackaddr != nullptr) {
                *stackaddr = nullptr;
        }
        if (stacksize != nullptr) {
                *stacksize = 0;
        }
        return 0;
}

int KYTY_SYSV_ABI PthreadAttrSetguardsize(PthreadAttr* /*attr*/, size_t /*guardsize*/) {
        return 0;
}

int KYTY_SYSV_ABI PthreadAttrGetguardsize(const PthreadAttr* /*attr*/, size_t* guardsize) {
        if (guardsize != nullptr) {
                *guardsize = 0;
        }
        return 0;
}

int KYTY_SYSV_ABI PthreadAttrSetschedparam(PthreadAttr* /*attr*/, const void* /*param*/) {
        return 0;
}

int KYTY_SYSV_ABI PthreadAttrGetschedparam(const PthreadAttr* /*attr*/, void* /*param*/) {
        return 0;
}

int KYTY_SYSV_ABI PthreadAttrSetschedpolicy(PthreadAttr* /*attr*/, int /*policy*/) {
        return 0;
}

int KYTY_SYSV_ABI PthreadAttrGetschedpolicy(const PthreadAttr* /*attr*/, int* policy) {
        if (policy != nullptr) {
                *policy = 0;
        }
        return 0;
}

int KYTY_SYSV_ABI PthreadAttrSetinheritsched(PthreadAttr* /*attr*/, int /*inherit*/) {
        return 0;
}

int KYTY_SYSV_ABI PthreadAttrSetsolosched(PthreadAttr* /*attr*/, int /*solo*/) {
        return 0;
}

int KYTY_SYSV_ABI PthreadAttrGetsolosched(const PthreadAttr* /*attr*/, int* solo) {
        if (solo != nullptr) {
                *solo = 0;
        }
        return 0;
}

int KYTY_SYSV_ABI PthreadAttrSetaffinity(PthreadAttr* /*attr*/, size_t /*cpusetsize*/,
                                         const void* /*cpuset*/) {
        return 0;
}

int KYTY_SYSV_ABI PthreadAttrGetaffinity(const PthreadAttr* /*attr*/, size_t cpusetsize,
                                         void* cpuset) {
        if (cpuset != nullptr) {
                std::memset(cpuset, 0, cpusetsize);
        }
        return 0;
}

// ===========================================================================
// Mutexes
// ===========================================================================

int KYTY_SYSV_ABI PthreadMutexInit(PthreadMutex* mutex, const PthreadMutexattr* /*attr*/) {
        if (mutex == nullptr) {
                return 22; // EINVAL
        }
        *mutex = new (std::nothrow) MutexImpl;
        return *mutex != nullptr ? 0 : 12;
}

int KYTY_SYSV_ABI PthreadMutexDestroy(PthreadMutex* mutex) {
        if (mutex == nullptr || *mutex == nullptr) {
                return 22; // EINVAL
        }
        delete static_cast<MutexImpl*>(*mutex);
        *mutex = nullptr;
        return 0;
}

int KYTY_SYSV_ABI PthreadMutexLock(PthreadMutex* mutex) {
        if (mutex == nullptr || *mutex == nullptr) {
                return 22; // EINVAL
        }
        static_cast<MutexImpl*>(*mutex)->m.lock();
        return 0;
}

int KYTY_SYSV_ABI PthreadMutexTrylock(PthreadMutex* mutex) {
        if (mutex == nullptr || *mutex == nullptr) {
                return 22; // EINVAL
        }
        return static_cast<MutexImpl*>(*mutex)->m.try_lock() ? 0 : 16; // EBUSY
}

int KYTY_SYSV_ABI PthreadMutexUnlock(PthreadMutex* mutex) {
        if (mutex == nullptr || *mutex == nullptr) {
                return 22; // EINVAL
        }
        static_cast<MutexImpl*>(*mutex)->m.unlock();
        return 0;
}

int KYTY_SYSV_ABI PthreadMutexTimedlock(PthreadMutex* mutex, const void* /*abstime*/) {
        return PthreadMutexLock(mutex);
}

int KYTY_SYSV_ABI PthreadMutexattrInit(PthreadMutexattr* attr) {
        if (attr == nullptr) {
                return 22;
        }
        *attr = new (std::nothrow) int(0);
        return *attr != nullptr ? 0 : 12;
}

int KYTY_SYSV_ABI PthreadMutexattrDestroy(PthreadMutexattr* attr) {
        if (attr == nullptr || *attr == nullptr) {
                return 22;
        }
        delete static_cast<int*>(*attr);
        *attr = nullptr;
        return 0;
}

int KYTY_SYSV_ABI PthreadMutexattrSettype(PthreadMutexattr* /*attr*/, int /*type*/) {
        return 0;
}

int KYTY_SYSV_ABI PthreadMutexattrSetprotocol(PthreadMutexattr* /*attr*/, int /*protocol*/) {
        return 0;
}

// ===========================================================================
// Condition variables
// ===========================================================================

int KYTY_SYSV_ABI PthreadCondInit(PthreadCond* cond, const PthreadCondattr* /*attr*/) {
        if (cond == nullptr) {
                return 22;
        }
        *cond = new (std::nothrow) CondImpl;
        return *cond != nullptr ? 0 : 12;
}

int KYTY_SYSV_ABI PthreadCondDestroy(PthreadCond* cond) {
        if (cond == nullptr || *cond == nullptr) {
                return 22;
        }
        delete static_cast<CondImpl*>(*cond);
        *cond = nullptr;
        return 0;
}

int KYTY_SYSV_ABI PthreadCondSignal(PthreadCond* cond) {
        if (cond == nullptr || *cond == nullptr) {
                return 22;
        }
        static_cast<CondImpl*>(*cond)->cv.notify_one();
        return 0;
}

int KYTY_SYSV_ABI PthreadCondSignalto(PthreadCond* cond) {
        return PthreadCondSignal(cond);
}

int KYTY_SYSV_ABI PthreadCondBroadcast(PthreadCond* cond) {
        if (cond == nullptr || *cond == nullptr) {
                return 22;
        }
        static_cast<CondImpl*>(*cond)->cv.notify_all();
        return 0;
}

int KYTY_SYSV_ABI PthreadCondWait(PthreadCond* cond, PthreadMutex* mutex) {
        if (cond == nullptr || *cond == nullptr || mutex == nullptr || *mutex == nullptr) {
                return 22;
        }
        auto& cv   = static_cast<CondImpl*>(*cond)->cv;
        auto& m    = static_cast<MutexImpl*>(*mutex)->m;
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock);
        return 0;
}

int KYTY_SYSV_ABI PthreadCondTimedwait(PthreadCond* cond, PthreadMutex* mutex,
                                       const void* /*abstime*/) {
        return PthreadCondWait(cond, mutex);
}

int KYTY_SYSV_ABI PthreadCondattrInit(PthreadCondattr* attr) {
        if (attr == nullptr) {
                return 22;
        }
        *attr = new (std::nothrow) int(0);
        return *attr != nullptr ? 0 : 12;
}

int KYTY_SYSV_ABI PthreadCondattrDestroy(PthreadCondattr* attr) {
        if (attr == nullptr || *attr == nullptr) {
                return 22;
        }
        delete static_cast<int*>(*attr);
        *attr = nullptr;
        return 0;
}

// ===========================================================================
// Read-write locks
// ===========================================================================

int KYTY_SYSV_ABI PthreadRwlockInit(PthreadRwlock* rwlock, const PthreadRwlockattr* /*attr*/) {
        if (rwlock == nullptr) {
                return 22;
        }
        *rwlock = new (std::nothrow) RwlockImpl;
        return *rwlock != nullptr ? 0 : 12;
}

int KYTY_SYSV_ABI PthreadRwlockDestroy(PthreadRwlock* rwlock) {
        if (rwlock == nullptr || *rwlock == nullptr) {
                return 22;
        }
        delete static_cast<RwlockImpl*>(*rwlock);
        *rwlock = nullptr;
        return 0;
}

int KYTY_SYSV_ABI PthreadRwlockRdlock(PthreadRwlock* rwlock) {
        if (rwlock == nullptr || *rwlock == nullptr) {
                return 22;
        }
        static_cast<RwlockImpl*>(*rwlock)->m.lock_shared();
        return 0;
}

int KYTY_SYSV_ABI PthreadRwlockTryrdlock(PthreadRwlock* rwlock) {
        if (rwlock == nullptr || *rwlock == nullptr) {
                return 22;
        }
        return static_cast<RwlockImpl*>(*rwlock)->m.try_lock_shared() ? 0 : 16;
}

int KYTY_SYSV_ABI PthreadRwlockWrlock(PthreadRwlock* rwlock) {
        if (rwlock == nullptr || *rwlock == nullptr) {
                return 22;
        }
        static_cast<RwlockImpl*>(*rwlock)->m.lock();
        return 0;
}

int KYTY_SYSV_ABI PthreadRwlockTrywrlock(PthreadRwlock* rwlock) {
        if (rwlock == nullptr || *rwlock == nullptr) {
                return 22;
        }
        return static_cast<RwlockImpl*>(*rwlock)->m.try_lock() ? 0 : 16;
}

int KYTY_SYSV_ABI PthreadRwlockUnlock(PthreadRwlock* rwlock) {
        if (rwlock == nullptr || *rwlock == nullptr) {
                return 22;
        }
        static_cast<RwlockImpl*>(*rwlock)->m.unlock();
        return 0;
}

int KYTY_SYSV_ABI PthreadRwlockattrInit(PthreadRwlockattr* attr) {
        if (attr == nullptr) {
                return 22;
        }
        *attr = new (std::nothrow) int(0);
        return *attr != nullptr ? 0 : 12;
}

int KYTY_SYSV_ABI PthreadRwlockattrDestroy(PthreadRwlockattr* attr) {
        if (attr == nullptr || *attr == nullptr) {
                return 22;
        }
        delete static_cast<int*>(*attr);
        *attr = nullptr;
        return 0;
}

int KYTY_SYSV_ABI PthreadRwlockattrSettype(PthreadRwlockattr* /*attr*/, int /*type*/) {
        return 0;
}

void KYTY_SYSV_ABI KernelSetThreadDtors() {
        // No-op: guest thread destructor bookkeeping is not required in this
        // host-backed build. The NID is registered so guest code links cleanly.
}

} // namespace Libs::LibKernel
