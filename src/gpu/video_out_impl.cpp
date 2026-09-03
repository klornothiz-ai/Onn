// ProsperoLayer PS5 emulator - VideoOut subsystem implementation.
// Bridges the guest libVideoOut NIDs to the host renderer/presenter and
// maintains per-handle flip/vblank event state.
//
// Round 10 fidelity fix: Add*Event used to POST the event immediately at
// registration time (a registration is not a flip!). Events are now
// REGISTERED per handle and delivered when they actually happen --
// VideoOutSubmitFlip fires the flip events, each vblank status poll fires
// the vblank events (the headless vblank tick model, documented in
// CHANGES.md round 10). Delivery fans out to BOTH queue systems, exactly as
// the round 10 item requires: a libkernel Equeue handle (KernelPostEvent)
// and a raw syscall kqueue fd (the dispatcher's EVFILT_GRAPHICS bridge). A
// handle is at most one of the two, so exactly one path lands.
#include "graphics/presentation/videoOut.h"

#include "common/logging/log.h"
#include "cpu/prospero_syscalls.hpp"
#include "kernel/eventQueue.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics::VideoOut {

namespace {

struct EventRegistration {
        int32_t  equeue{0};   // libkernel Equeue handle OR syscall kqueue fd
        int32_t  id{0};       // guest event id
        uint64_t udata{0};
};

struct VideoOutHandle {
        int32_t                     user_id{0};
        int32_t                     type{0};
        int32_t                     index{0};
        uint32_t                    flip_rate{1};
        uint32_t                    flip_counter{0};
        uint32_t                    vblank_counter{0};
        std::vector<const void*>    buffers;
        VideoOutBufferAttribute     attr{};
        bool                        attr_valid{false};
        std::vector<EventRegistration> flip_events;
        std::vector<EventRegistration> vblank_events;
};

std::mutex                          g_mutex;
std::unordered_map<int32_t, VideoOutHandle> g_handles;
std::atomic<int32_t>                g_next_handle{1};

VideoOutHandle* FindHandle(int32_t handle) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_handles.find(handle);
        return it != g_handles.end() ? &it->second : nullptr;
}

void PostKernelEvent(int32_t equeue, int32_t id, uint64_t udata) {
        if (equeue <= 0) {
                return;
        }
        LibKernel::EventQueue::KernelEvent ev{};
        ev.filter    = LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS;
        ev.event_id  = id;
        ev.udata     = udata;
        LibKernel::EventQueue::KernelPostEvent(
            static_cast<LibKernel::EventQueue::KernelEqueue>(equeue), ev);
}

// Deliver one event to every registered queue for this video-out handle:
// the libkernel Equeue path and the syscall kqueue (EVFILT_GRAPHICS) path.
void FireEvent(int32_t video_handle, const EventRegistration& reg, uint32_t kind) {
        // libkernel Equeue path (the classic game path: a KernelEqueue handle).
        PostKernelEvent(reg.equeue, reg.id, reg.udata);
        // Raw syscall kqueue path: queue an EVFILT_GRAPHICS trigger for every
        // kqueue registered for this video-out handle. (No-op when the handle
        // is an equeue or nothing listens.)
        (void)PS5::CPU::ProsperoSyscallDispatcher::Instance().PostGraphicsEvent(
            video_handle, kind, static_cast<uint32_t>(reg.id), reg.udata);
}

// Fire a whole registration list (under the caller's g_mutex hold the
// registrations vector is stable; delivery itself takes no g_mutex).
void FireEvents(int32_t video_handle, const std::vector<EventRegistration>& regs,
                uint32_t kind) {
        for (const EventRegistration& reg : regs) {
                FireEvent(video_handle, reg, kind);
        }
}

} // namespace

::Graphics::RenderContext& VideoOutInit(uint32_t width, uint32_t height,
                                        ::Graphics::RenderContext& presenter) {
        (void)width;
        (void)height;
        return presenter;
}

void VideoOutShutdown() {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_handles.clear();
}

int KYTY_SYSV_ABI VideoOutOpen(int32_t user_id, int32_t type, int32_t index, int32_t* handle) {
        if (handle == nullptr) {
                return static_cast<int>(0x80a00002); // KERNEL_ERROR_EINVAL-ish
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        VideoOutHandle h;
        h.user_id = user_id;
        h.type    = type;
        h.index   = index;
        const int32_t id = g_next_handle.fetch_add(1);
        g_handles.emplace(id, h);
        *handle = id;
        return 0;
}

int KYTY_SYSV_ABI VideoOutClose(int32_t handle) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_handles.erase(handle);
        return 0;
}

int KYTY_SYSV_ABI VideoOutSetBufferAttribute2(int32_t handle, VideoOutBufferAttribute* attr,
                                              int32_t num) {
        auto* h = FindHandle(handle);
        if (h == nullptr || attr == nullptr || num <= 0) {
                return static_cast<int>(0x80a00002);
        }
        h->attr       = *attr;
        h->attr_valid = true;
        return 0;
}

int KYTY_SYSV_ABI VideoOutRegisterBuffers2(int32_t handle, int32_t start_index,
                                           const void* const* addresses, int32_t num_buffers,
                                           const void* attribs, int32_t* out_label_addr) {
        (void)start_index;   // buffer registration is a flat list in this model
        auto* h = FindHandle(handle);
        if (h == nullptr || addresses == nullptr || num_buffers <= 0) {
                return static_cast<int>(0x80a00002);
        }
        h->buffers.assign(addresses, addresses + num_buffers);
        if (attribs != nullptr && num_buffers > 0) {
                h->attr = *static_cast<const VideoOutBufferAttribute*>(attribs);
        }
        if (out_label_addr != nullptr) {
                *out_label_addr = 0;
        }
        return 0;
}

int KYTY_SYSV_ABI VideoOutSubmitChangeBufferAttribute2(int32_t handle, int32_t index,
                                                       const void* attribs) {
        auto* h = FindHandle(handle);
        if (h == nullptr || attribs == nullptr) {
                return static_cast<int>(0x80a00002);
        }
        if (index >= 0 && index < static_cast<int32_t>(h->buffers.size())) {
                h->attr = *static_cast<const VideoOutBufferAttribute*>(attribs);
        }
        return 0;
}

int KYTY_SYSV_ABI VideoOutUnregisterBuffers(int32_t handle, int32_t start_index,
                                            int32_t num_buffers) {
        (void)start_index; (void)num_buffers;  // clears the whole flat list
        auto* h = FindHandle(handle);
        if (h == nullptr) {
                return static_cast<int>(0x80a00002);
        }
        h->buffers.clear();
        return 0;
}

int KYTY_SYSV_ABI VideoOutSetFlipRate(int32_t handle, int32_t rate) {
        auto* h = FindHandle(handle);
        if (h == nullptr) {
                return static_cast<int>(0x80a00002);
        }
        h->flip_rate = rate > 0 ? static_cast<uint32_t>(rate) : 1;
        return 0;
}

int KYTY_SYSV_ABI VideoOutAddFlipEvent(int32_t handle, int32_t equeue, int32_t id, uint64_t udata) {
        // Round 10: REGISTRATION only -- the event fires when it actually
        // happens (a flip / a vblank tick), not at registration time.
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_handles.find(handle);
        if (it == g_handles.end()) {
                return static_cast<int>(0x80a00002);
        }
        it->second.flip_events.push_back({equeue, id, udata});
        return 0;
}

int KYTY_SYSV_ABI VideoOutAddVblankEvent(int32_t handle, int32_t equeue, int32_t id, uint64_t udata) {
        // Round 10: REGISTRATION only -- the event fires when it actually
        // happens (a flip / a vblank tick), not at registration time.
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_handles.find(handle);
        if (it == g_handles.end()) {
                return static_cast<int>(0x80a00002);
        }
        it->second.vblank_events.push_back({equeue, id, udata});
        return 0;
}

int KYTY_SYSV_ABI VideoOutAddPreVblankStartEvent(int32_t handle, int32_t equeue, int32_t id,
                                                 uint64_t udata) {
        return VideoOutAddVblankEvent(handle, equeue, id, udata);
}

int KYTY_SYSV_ABI VideoOutDeleteFlipEvent(int32_t handle, int32_t equeue, int32_t id) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_handles.find(handle);
        if (it == g_handles.end()) {
                return static_cast<int>(0x80a00002);
        }
        auto& v = it->second.flip_events;
        v.erase(std::remove_if(v.begin(), v.end(),
                               [equeue, id](const EventRegistration& r) {
                                       return r.equeue == equeue && r.id == id;
                               }),
                v.end());
        return 0;
}

int KYTY_SYSV_ABI VideoOutDeleteVblankEvent(int32_t handle, int32_t equeue, int32_t id) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_handles.find(handle);
        if (it == g_handles.end()) {
                return static_cast<int>(0x80a00002);
        }
        auto& v = it->second.vblank_events;
        v.erase(std::remove_if(v.begin(), v.end(),
                               [equeue, id](const EventRegistration& r) {
                                       return r.equeue == equeue && r.id == id;
                               }),
                v.end());
        return 0;
}

int KYTY_SYSV_ABI VideoOutDeletePreVblankStartEvent(int32_t handle, int32_t equeue, int32_t id) {
        return VideoOutDeleteVblankEvent(handle, equeue, id);
}

int KYTY_SYSV_ABI VideoOutSubmitFlip(int32_t handle, int32_t buffer_index, int32_t flip_mode,
                                     int64_t flip_arg) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_handles.find(handle);
        if (it == g_handles.end()) {
                return static_cast<int>(0x80a00002);
        }
        VideoOutHandle& h = it->second;
        ++h.flip_counter;
        // Round 10: the flip fires every registered flip event, to BOTH the
        // libkernel Equeue path and the syscall-kqueue EVFILT_GRAPHICS path.
        FireEvents(handle, h.flip_events,
                   PS5::CPU::ProsperoSyscallDispatcher::GRAPHICS_EVENT_FLIP);
        (void)buffer_index;
        (void)flip_mode;
        (void)flip_arg;
        return 0;
}

int KYTY_SYSV_ABI VideoOutGetFlipStatus(int32_t handle, int32_t* out_flip_arg,
                                        int32_t* out_counter) {
        auto* h = FindHandle(handle);
        if (h == nullptr) {
                return static_cast<int>(0x80a00002);
        }
        if (out_flip_arg != nullptr) {
                *out_flip_arg = 0;
        }
        if (out_counter != nullptr) {
                *out_counter = static_cast<int32_t>(h->flip_counter);
        }
        return 0;
}

int KYTY_SYSV_ABI VideoOutIsFlipPending(int32_t handle, int32_t* out_pending) {
        auto* h = FindHandle(handle);
        if (h == nullptr) {
                return static_cast<int>(0x80a00002);
        }
        if (out_pending != nullptr) {
                *out_pending = 0;
        }
        return 0;
}

int KYTY_SYSV_ABI VideoOutGetVblankStatus(int32_t handle, int32_t* out_counter) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_handles.find(handle);
        if (it == g_handles.end()) {
                return static_cast<int>(0x80a00002);
        }
        VideoOutHandle& h = it->second;
        const int32_t counter = static_cast<int32_t>(++h.vblank_counter);
        // Round 10 headless vblank tick model: each status poll IS one vblank
        // period, so the registered vblank events fire with it.
        FireEvents(handle, h.vblank_events,
                   PS5::CPU::ProsperoSyscallDispatcher::GRAPHICS_EVENT_VBLANK);
        if (out_counter != nullptr) {
                *out_counter = counter;
        }
        return 0;
}

int KYTY_SYSV_ABI VideoOutSetWindowModeMargins(int32_t handle, int32_t top, int32_t bottom) {
        (void)handle;
        (void)top;
        (void)bottom;
        return 0;
}

int KYTY_SYSV_ABI VideoOutAddOutputModeEvent(int32_t handle, int32_t equeue, int32_t id,
                                             uint64_t udata) {
        (void)handle;
        PostKernelEvent(equeue, id, udata);
        return 0;
}

int KYTY_SYSV_ABI VideoOutGetEventId(const void* event) {
        return event != nullptr ? 0 : -1;
}

int KYTY_SYSV_ABI VideoOutGetEventData(const void* event) {
        return event != nullptr ? 0 : -1;
}

int KYTY_SYSV_ABI VideoOutGetEventCount(const void* event) {
        return event != nullptr ? 1 : 0;
}

int KYTY_SYSV_ABI VideoOutWaitVblank(int32_t handle) {
        auto* h = FindHandle(handle);
        if (h == nullptr) {
                return static_cast<int>(0x80a00002);
        }
        ++h->vblank_counter;
        return 0;
}

int KYTY_SYSV_ABI VideoOutGetOutputStatus(int32_t handle, int32_t* out_status) {
        auto* h = FindHandle(handle);
        if (h == nullptr) {
                return static_cast<int>(0x80a00002);
        }
        if (out_status != nullptr) {
                *out_status = 0;
        }
        return 0;
}

int KYTY_SYSV_ABI VideoOutInitializeOutputOptions(int32_t handle, void* options) {
        (void)handle;
        if (options != nullptr) {
                std::memset(options, 0, 128);
        }
        return 0;
}

int KYTY_SYSV_ABI VideoOutIsOutputSupported(int32_t handle, int32_t option) {
        (void)handle;
        (void)option;
        return 0;
}

int KYTY_SYSV_ABI VideoOutConfigureOutput(int32_t handle, const void* options) {
        (void)handle;
        (void)options;
        return 0;
}

int KYTY_SYSV_ABI VideoOutLatencyControlWaitBeforeInput(int32_t handle, int32_t option) {
        (void)handle;
        (void)option;
        return 0;
}

int KYTY_SYSV_ABI VideoOutLatencyMeasureSetStartPoint(int32_t handle, int32_t option) {
        (void)handle;
        (void)option;
        return 0;
}

int KYTY_SYSV_ABI VideoOutColorSettingsSetGamma(int32_t handle, float gamma) {
        (void)handle;
        (void)gamma;
        return 0;
}

int KYTY_SYSV_ABI VideoOutAdjustColor(int32_t handle, const void* settings) {
        (void)handle;
        (void)settings;
        return 0;
}

} // namespace Libs::Graphics::VideoOut
