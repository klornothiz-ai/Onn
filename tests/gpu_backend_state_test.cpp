#include "gpu/vulkan_backend.hpp"

#include <cassert>
#include <iostream>

int main() {
    PS5::GPU::VulkanRendererBackend backend;
    assert(backend.Initialize());
    backend.SetViewport(64.0f, 32.0f);
    backend.DispatchCompute(1, 1, 1);
    backend.DrawAuto(3, 1);
    backend.PresentFrame();
    assert(backend.GetDispatchedComputeCount() == 1);
    assert(backend.GetDrawCallCount() == 1);
    assert(backend.GetFrameCount() == 1);
    backend.Shutdown();
    std::cout << "GPU backend state test passed\n";
    return 0;
}
