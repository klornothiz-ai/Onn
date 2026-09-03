#include "graphics/presentation/window.h"
#include "graphics/presentation/renderDoc.h"
#include "graphics/host_gpu/headless_gpu_bridge.hpp"

// Headless WindowInit / WindowShutdown.
//
// The HLE graphics driver (libs/agc.cpp) drives guest PM4 through the abstract
// Graphics::Gpu interface held by the global `g_renderer`. That pointer is only
// ever set by the Graphics subsystem init, which calls Graphics::WindowInit.
// In the full (SDL) build WindowInit comes from the windowed presenter; in this
// headless build it returns the headless GPU bridge so a real guest submission
// flows HLE -> PM4 translator -> GPU backend without aborting on a null
// g_renderer. This is what closes the CPU+HLE+PM4+GPU+VideoOut single-path gap.

namespace Graphics {

namespace {

HeadlessGpuBridge* g_headless_window = nullptr;

} // namespace

void RenderDocInit() {
    // Diagnostics-only stub in the headless build; RenderDoc is not loaded.
}

bool RenderDocIsCaptureInProgress() {
    return false;
}

RenderContext& WindowInit(uint32_t /*width*/, uint32_t /*height*/) {
    if (g_headless_window == nullptr) {
        static HeadlessGpuBridge bridge;
        g_headless_window = &bridge;
    }
    return *g_headless_window;
}

void WindowShutdown() {
    // The bridge is a process-lifetime singleton owned here; ShutdownGpu is
    // invoked via the RenderContext teardown in the Graphics subsystem destroy.
    g_headless_window = nullptr;
}

} // namespace Graphics
