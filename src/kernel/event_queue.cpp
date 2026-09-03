// ProsperoLayer PS5 emulator - libkernel event queue implementation
//
// Implements KernelEqueue (kqueue-like) on top of host primitives:
// user events, HR timer events and AMPr events are queued and delivered
// through KernelWaitEqueue.

#include "kernel/eventQueue.h"
#include "common/threads.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace Libs::LibKernel::EventQueue {

namespace {

struct EventEntry {
        KernelEvent ev{};
};

struct EqueueImpl {
        std::string                          name;
        Common::Mutex                        mutex;
        std::condition_variable_any          cv;
        std::deque<KernelEvent>              queue;
        int32_t                              next_id{1};
        std::unordered_map<int32_t, uint64_t> user_events;   // id -> udata
        std::unordered_map<int32_t, int64_t>  hr_timer_events; // id -> period
        bool                                 deleted{false};
        // Round 9 UAF fix: HR-timer delivery threads hold a pointer to this
        // impl. KernelDeleteEqueue waits until every in-flight timer thread
        // has retired before freeing the object.
        uint32_t                             pending_timers{0};
};

struct AmprEventEntry {
        int32_t id;
        void*   ampr;
        uint64_t udata;
};

Common::Mutex                       g_ampr_events_mutex;
std::vector<AmprEventEntry>         g_ampr_events;

// ---------------------------------------------------------------------------
// Round 9 handle-registry fix. KernelEqueue is a 32-bit guest handle, but the
// original implementation cast the raw EqueueImpl* through it -- truncating
// the 64-bit pointer on every create (found by the new dependency-free test:
// the subsystem had never been exercised before). Handles are now small
// monotonic ids resolved through a registry, mirroring the syscall layer's
// kqueue-fd discipline.
// ---------------------------------------------------------------------------
Common::Mutex g_equeue_registry_mutex;
std::unordered_map<KernelEqueue, std::unique_ptr<EqueueImpl>> g_equeues;
KernelEqueue g_next_equeue_handle = 1;   // 0 / negative stay invalid

EqueueImpl* GetImpl(KernelEqueue eq) {
        if (eq <= 0) {
                return nullptr;
        }
        std::lock_guard<Common::Mutex> lock(g_equeue_registry_mutex);
        const auto it = g_equeues.find(eq);
        return it != g_equeues.end() ? it->second.get() : nullptr;
}

} // namespace

int KYTY_SYSV_ABI KernelCreateEqueue(KernelEqueue* eq, const char* name, uint32_t /*attr*/) {
        if (eq == nullptr) {
                return 22; // EINVAL
        }
        auto impl = std::make_unique<EqueueImpl>();
        if (impl == nullptr) {
                return 12; // ENOMEM
        }
        if (name != nullptr) {
                impl->name = name;
        }
        std::lock_guard<Common::Mutex> lock(g_equeue_registry_mutex);
        const KernelEqueue handle = g_next_equeue_handle++;
        g_equeues[handle] = std::move(impl);
        *eq = handle;
        return 0;
}

int KYTY_SYSV_ABI KernelDeleteEqueue(KernelEqueue eq) {
        auto* impl = GetImpl(eq);
        if (impl == nullptr) {
                return 22; // EINVAL
        }
        {
                std::unique_lock<Common::Mutex> lock(impl->mutex);
                impl->deleted = true;
                // Round 9 UAF fix: an in-flight HR-timer delivery thread
                // sleeps with a raw pointer to this impl; wait for every
                // timer thread to retire before the memory is freed. (A
                // concurrent KernelWaitEqueue waiter is still a guest contract
                // violation -- deleting an equeue someone is blocked on is
                // undefined on the real kernel too.)
                impl->cv.wait(lock, [impl] { return impl->pending_timers == 0; });
        }
        {
                std::lock_guard<Common::Mutex> lock(g_equeue_registry_mutex);
                g_equeues.erase(eq);   // unique_ptr frees the retired impl
        }
        return 0;
}

int KYTY_SYSV_ABI KernelWaitEqueue(KernelEqueue eq, KernelEvent* ev, int32_t num, int32_t* out_num,
                                   uint32_t* timeout) {
        auto* impl = GetImpl(eq);
        if (impl == nullptr || ev == nullptr || num <= 0) {
                return 22; // EINVAL
        }

        std::unique_lock<Common::Mutex> lock(impl->mutex);

        auto has_event = [impl]() { return !impl->queue.empty(); };

        bool ok = true;
        if (timeout == nullptr || *timeout == 0) {
                impl->cv.wait(lock, has_event);
        } else {
                ok = impl->cv.wait_for(lock, std::chrono::microseconds(*timeout), has_event);
        }

        if (!ok) {
                if (out_num != nullptr) {
                        *out_num = 0;
                }
                return 35; // ETIMEDOUT
        }

        int32_t count = 0;
        while (count < num && !impl->queue.empty()) {
                ev[count] = impl->queue.front();
                impl->queue.pop_front();
                count++;
        }

        if (out_num != nullptr) {
                *out_num = count;
        }
        return 0;
}

int KYTY_SYSV_ABI KernelGetEventUserData(const KernelEvent* ev, uint64_t* udata) {
        if (ev == nullptr || udata == nullptr) {
                return 22; // EINVAL
        }
        *udata = ev->udata;
        return 0;
}

int KYTY_SYSV_ABI KernelGetEventId(const KernelEvent* ev) {
        return ev != nullptr ? ev->event_id : -1;
}

int KYTY_SYSV_ABI KernelGetEventFilter(const KernelEvent* ev) {
        return ev != nullptr ? static_cast<int>(ev->filter) : -1;
}

int KYTY_SYSV_ABI KernelGetEventData(const KernelEvent* ev, uint32_t* data) {
        if (ev == nullptr || data == nullptr) {
                return 22; // EINVAL
        }
        *data = static_cast<uint32_t>(ev->data);
        return 0;
}

int KYTY_SYSV_ABI KernelGetEventFflags(const KernelEvent* ev, int32_t* fflags) {
        if (ev == nullptr || fflags == nullptr) {
                return 22; // EINVAL
        }
        *fflags = ev->fflags;
        return 0;
}

int KYTY_SYSV_ABI KernelGetEventError(const KernelEvent* ev) {
        return ev != nullptr ? ev->error : 0;
}

namespace {

int AddUserEventInternal(KernelEqueue eq, int32_t* id, uint64_t udata, bool edge) {
        auto* impl = GetImpl(eq);
        if (impl == nullptr || id == nullptr) {
                return 22; // EINVAL
        }
        std::lock_guard<Common::Mutex> lock(impl->mutex);
        const int32_t new_id = impl->next_id++;
        impl->user_events[new_id] = udata;

        KernelEvent ev{};
        ev.filter   = edge ? KERNEL_EVFILT_USER : KERNEL_EVFILT_USER;
        ev.ident    = static_cast<uint64_t>(new_id);
        ev.event_id = new_id;
        ev.udata    = udata;
        *id         = new_id;
        // Pre-queue a registration event so waiters observe the new id.
        impl->queue.push_back(ev);
        impl->cv.notify_all();
        return 0;
}

} // namespace

int KYTY_SYSV_ABI KernelAddUserEvent(KernelEqueue eq, int32_t* id, uint64_t udata) {
        return AddUserEventInternal(eq, id, udata, false);
}

int KYTY_SYSV_ABI KernelAddUserEventEdge(KernelEqueue eq, int32_t* id, uint64_t udata) {
        return AddUserEventInternal(eq, id, udata, true);
}

int KYTY_SYSV_ABI KernelTriggerUserEvent(KernelEqueue eq, int32_t id, uint32_t data) {
        auto* impl = GetImpl(eq);
        if (impl == nullptr) {
                return 22; // EINVAL
        }
        std::lock_guard<Common::Mutex> lock(impl->mutex);
        const auto it = impl->user_events.find(id);
        if (it == impl->user_events.end()) {
                return 2; // ENOENT
        }
        KernelEvent ev{};
        ev.filter   = KERNEL_EVFILT_USER;
        ev.ident    = static_cast<uint64_t>(id);
        ev.data     = data;
        ev.udata    = it->second;
        ev.event_id = id;
        impl->queue.push_back(ev);
        impl->cv.notify_all();
        return 0;
}

int KYTY_SYSV_ABI KernelDeleteUserEvent(KernelEqueue eq, int32_t id) {
        auto* impl = GetImpl(eq);
        if (impl == nullptr) {
                return 22; // EINVAL
        }
        std::lock_guard<Common::Mutex> lock(impl->mutex);
        impl->user_events.erase(id);
        return 0;
}

int KYTY_SYSV_ABI KernelPostEvent(KernelEqueue eq, const KernelEvent& ev) {
        auto* impl = GetImpl(eq);
        if (impl == nullptr) {
                return 22; // EINVAL
        }
        std::lock_guard<Common::Mutex> lock(impl->mutex);
        impl->queue.push_back(ev);
        impl->cv.notify_all();
        return 0;
}

int KYTY_SYSV_ABI KernelAddHRTimerEvent(KernelEqueue eq, int32_t* id, uint64_t udata,
                                        uint32_t /*type*/, int64_t time) {
        auto* impl = GetImpl(eq);
        if (impl == nullptr || id == nullptr) {
                return 22; // EINVAL
        }
        std::lock_guard<Common::Mutex> lock(impl->mutex);
        if (impl->deleted) {
                // A concurrent KernelDeleteEqueue has already retired this
                // queue: refuse instead of arming a timer the deleter does
                // not know about.
                return 22; // EINVAL
        }
        const int32_t new_id = impl->next_id++;
        impl->hr_timer_events[new_id] = time;
        *id = new_id;

        // Schedule a delivery thread that fires after `time` microseconds.
        // The impl tracks the thread through pending_timers so a concurrent
        // KernelDeleteEqueue waits for it instead of freeing the impl under
        // it (round 9). Cancelling the timer (KernelDeleteHRTimerEvent)
        // suppresses delivery.
        ++impl->pending_timers;
        std::thread([impl, new_id, udata, time]() {
                std::this_thread::sleep_for(std::chrono::microseconds(time));
                std::lock_guard<Common::Mutex> l(impl->mutex);
                const bool cancelled = impl->deleted ||
                                       impl->hr_timer_events.count(new_id) == 0;
                if (!cancelled) {
                        KernelEvent ev{};
                        ev.filter   = KERNEL_EVFILT_HRTIMER;
                        ev.ident    = static_cast<uint64_t>(new_id);
                        ev.event_id = new_id;
                        ev.udata    = udata;
                        impl->queue.push_back(ev);
                }
                // Retire this delivery thread and unblock a waiting deleter.
                --impl->pending_timers;
                impl->cv.notify_all();
        }).detach();
        return 0;
}

int KYTY_SYSV_ABI KernelDeleteHRTimerEvent(KernelEqueue eq, int32_t id) {
        auto* impl = GetImpl(eq);
        if (impl == nullptr) {
                return 22; // EINVAL
        }
        std::lock_guard<Common::Mutex> lock(impl->mutex);
        impl->hr_timer_events.erase(id);
        return 0;
}

int KYTY_SYSV_ABI KernelAddAmprEvent(KernelEqueue eq, int32_t* id, uint64_t udata, void* ampr) {
        auto* impl = GetImpl(eq);
        if (impl == nullptr || id == nullptr) {
                return 22; // EINVAL
        }
        std::lock_guard<Common::Mutex> lock(impl->mutex);
        const int32_t new_id = impl->next_id++;
        *id = new_id;
        {
                std::lock_guard<Common::Mutex> l(g_ampr_events_mutex);
                g_ampr_events.push_back({new_id, ampr, udata});
        }
        return 0;
}

int KYTY_SYSV_ABI KernelAddAmprSystemEvent(KernelEqueue eq, int32_t* id, uint64_t udata,
                                           void* ampr) {
        return KernelAddAmprEvent(eq, id, udata, ampr);
}

int KYTY_SYSV_ABI KernelDeleteAmprEvent(KernelEqueue eq, int32_t id) {
        auto* impl = GetImpl(eq);
        if (impl == nullptr) {
                return 22; // EINVAL
        }
        {
                std::lock_guard<Common::Mutex> l(g_ampr_events_mutex);
                g_ampr_events.erase(std::remove_if(g_ampr_events.begin(), g_ampr_events.end(),
                                                   [id](const AmprEventEntry& e) { return e.id == id; }),
                                    g_ampr_events.end());
        }
        return 0;
}

int KYTY_SYSV_ABI KernelDeleteAmprSystemEvent(KernelEqueue eq, int32_t id) {
        return KernelDeleteAmprEvent(eq, id);
}

} // namespace Libs::LibKernel::EventQueue
