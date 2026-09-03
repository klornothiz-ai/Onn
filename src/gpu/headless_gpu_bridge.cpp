// ProsperoLayer PS5 emulator - headless GPU bridge implementation.
// See include/graphics/host_gpu/headless_gpu_bridge.hpp for the rationale.

#include "graphics/host_gpu/headless_gpu_bridge.hpp"
#include "gpu/vulkan_compute_executor.hpp"

namespace Graphics {

HeadlessGpuBridge::HeadlessGpuBridge() : m_translator(m_backend) {}

HeadlessGpuBridge::~HeadlessGpuBridge() {
    if (m_initialized) {
        ShutdownGpu();
    }
}

void HeadlessGpuBridge::InitializeGpu(void* video_out) {
    (void)video_out;
    if (!m_initialized) {
        m_backend.Initialize();

        // Bring up the real GPU compute path when requested and a device is
        // available; otherwise the translator keeps the legacy backend call.
        if (m_real_compute_requested && m_guest_memory != nullptr) {
            m_executor = std::make_unique<PS5::GPU::VulkanComputeExecutor>();
            if (m_executor->Initialize()) {
                m_translator.BindComputeExecutor(m_executor.get(), m_guest_memory);
                m_real_compute_active = true;
                // Round 19: production wiring of the REAL VkGraphicsPipeline
                // raster path -- opt-in at the translator (unit tests control
                // it explicitly; the emulator uses every capability it has).
                m_translator.SetGraphicsRasterEnabled(true);
            } else {
                m_executor.reset();
            }
        }

        m_initialized = true;
    }
}

void HeadlessGpuBridge::ShutdownGpu() {
    if (m_initialized) {
        m_backend.Shutdown();
        m_initialized = false;
    }
}

void HeadlessGpuBridge::Submit(uint32_t* dcb, uint32_t size_in_dwords, const uint32_t* acb,
                               uint32_t acb_size_in_dwords, bool trigger_interrupt_on_done) {
    if (!m_initialized) {
        InitializeGpu(nullptr);
    }

    ++m_gfx_submits;

    // Translate the draw command buffer. The translator validates the entire
    // stream before applying any backend side effect, mirroring submit_dcb().
    if (dcb != nullptr && size_in_dwords > 0) {
        const auto result =
            m_translator.TranslateAndExecuteCommandRingChecked(dcb, size_in_dwords);
        m_last_submit_ok = static_cast<bool>(result);
    } else {
        m_last_submit_ok = true;
    }

    // A DCB may carry an inline ACB (compute) ring; process it on the same path.
    if (acb != nullptr && acb_size_in_dwords > 0) {
        const auto result =
            m_translator.TranslateAndExecuteCommandRingChecked(acb, acb_size_in_dwords);
        m_last_submit_ok = m_last_submit_ok && static_cast<bool>(result);
    }

    if (trigger_interrupt_on_done) {
        ++m_interrupts;
    }
}

void HeadlessGpuBridge::SubmitCompute(uint32_t queue, uint32_t* acb, uint32_t size_in_dwords,
                                      bool trigger_interrupt_on_done) {
    (void)queue;
    if (!m_initialized) {
        InitializeGpu(nullptr);
    }

    ++m_compute_submits;

    if (acb != nullptr && size_in_dwords > 0) {
        const auto result =
            m_translator.TranslateAndExecuteCommandRingChecked(acb, size_in_dwords);
        m_last_submit_ok = static_cast<bool>(result);
    } else {
        m_last_submit_ok = true;
    }

    if (trigger_interrupt_on_done) {
        ++m_interrupts;
    }
}

void HeadlessGpuBridge::Done() {
    m_backend.PresentFrame();
    ++m_frame_num;
}

uint64_t HeadlessGpuBridge::GetFrameNum() const {
    return m_frame_num;
}

} // namespace Graphics
