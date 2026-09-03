#pragma once
// ProsperoLayer PS5 emulator - render context / renderer interface (Kyty-compatible)
#include "common/common.h"
#include <cstdint>

namespace Graphics {

class Gpu {
public:
        virtual ~Gpu() = default;

        virtual void Submit(uint32_t* dcb, uint32_t size_in_dwords, const uint32_t* acb,
                            uint32_t acb_size_in_dwords, bool trigger_interrupt_on_done) = 0;
        virtual void SubmitCompute(uint32_t queue, uint32_t* acb, uint32_t size_in_dwords,
                                   bool trigger_interrupt_on_done) = 0;
        virtual void Done() = 0;
        virtual uint64_t GetFrameNum() const = 0;
};

class RenderContext {
public:
        virtual ~RenderContext() = default;

        virtual void InitializeGpu(void* video_out) = 0;
        virtual void ShutdownGpu() = 0;
        virtual Gpu& GetGpu() = 0;
        virtual RenderContext& Renderer() { return *this; }
};

} // namespace Graphics
