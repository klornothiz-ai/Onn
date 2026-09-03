// ProsperoLayer PS5 emulator - GPU <-> event queue synchronization
#include "graphics/host_gpu/renderer/sync.h"
#include "kernel/eventQueue.h"

#include <chrono>


namespace Graphics {

void SyncSignal(GpuFence* fence, uint64_t value) {
        if (fence == nullptr) return;
        uint64_t current = fence->value.load(std::memory_order_relaxed);
        while (current < value &&
               !fence->value.compare_exchange_weak(current, value,
                                                   std::memory_order_release,
                                                   std::memory_order_relaxed)) {
        }
        fence->cv.notify_all();
}

void SyncWait(GpuFence* fence, uint64_t value, uint64_t timeout_us) {
        if (fence == nullptr) return;
        std::unique_lock<std::mutex> lock(fence->mutex);
        if (timeout_us == 0) {
                fence->cv.wait(lock, [&] {
                        return fence->value.load(std::memory_order_acquire) >= value;
                });
                return;
        }
        (void)fence->cv.wait_for(lock, std::chrono::microseconds(timeout_us), [&] {
                return fence->value.load(std::memory_order_acquire) >= value;
        });
}

} // namespace Graphics

namespace Sync {

int AddEqEvent(::Graphics::RenderContext& renderer,
               Libs::LibKernel::EventQueue::KernelEqueue eq, int id, void* udata) {
        (void)renderer;
        // Post a graphics event into the kernel queue so the guest waiter
        // wakes up with the expected event id.
        Libs::LibKernel::EventQueue::KernelEvent ev{};
        ev.filter   = Libs::LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS;
        ev.ident    = static_cast<uint64_t>(id);
        ev.event_id = id;
        ev.udata    = reinterpret_cast<uint64_t>(udata);
        return Libs::LibKernel::EventQueue::KernelPostEvent(eq, ev);
}

int DeleteEqEvent(Libs::LibKernel::EventQueue::KernelEqueue eq, int id) {
        return Libs::LibKernel::EventQueue::KernelDeleteUserEvent(eq, id);
}

} // namespace Sync
