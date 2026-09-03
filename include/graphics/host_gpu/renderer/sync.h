#pragma once
// ProsperoLayer PS5 emulator - renderer sync primitives (Kyty-compatible)
#include "common/common.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "kernel/eventQueue.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace Graphics {

// Fence / semaphore helpers used by the guest GPU submission path.
struct GpuFence {
        std::atomic<uint64_t> value{0};
        mutable std::mutex mutex;
        mutable std::condition_variable cv;
};

void SyncSignal(GpuFence* fence, uint64_t value);
void SyncWait(GpuFence* fence, uint64_t value, uint64_t timeout_us);

} // namespace Graphics

namespace Sync {

// Posts an event into the given kernel equeue from the renderer context.
int AddEqEvent(::Graphics::RenderContext& renderer,
               Libs::LibKernel::EventQueue::KernelEqueue eq, int id, void* udata);

// Deletes a previously posted event.
int DeleteEqEvent(Libs::LibKernel::EventQueue::KernelEqueue eq, int id);

} // namespace Sync
