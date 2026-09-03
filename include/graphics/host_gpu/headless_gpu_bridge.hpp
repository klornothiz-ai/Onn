#pragma once
// ProsperoLayer PS5 emulator - headless GPU bridge.
//
// The HLE graphics driver (libs/agc.cpp, Gen5Driver::GraphicsDriverSubmit*)
// submits guest command rings through the abstract Graphics::Gpu interface
// (include/graphics/host_gpu/renderer/renderContext.h). The full renderer that
// implements that interface is only built with SDL2 + a Vulkan ICD present.
//
// This bridge provides a dependency-free implementation of Graphics::Gpu that
// forwards guest DCB / ACB rings to the modern, already-headless PM4 translator
// (PS5::GPU::PM4VulkanTranslator) driving the software-fallback Vulkan backend
// (PS5::GPU::VulkanRendererBackend). It lets the HLE driver submit path run and
// be verified without a GPU: submitted rings are decoded and their draw,
// dispatch and viewport effects become observable through the backend counters.
//
// It is intentionally faithful to what submit_dcb()/submit_acb() do in
// libs/agc.cpp: validate the ring, then translate every packet. Malformed rings
// apply no side effects (the translator validates the whole stream first).

#include "graphics/host_gpu/renderer/renderContext.h"
#include "gpu/pm4_translator.hpp"
#include "gpu/vulkan_backend.hpp"
#include "gpu/vulkan_compute_executor.hpp"

#include <cstdint>
#include <memory>

namespace PS5 { namespace GPU { class GpuGuestMemory; } }

namespace Graphics {

class HeadlessGpuBridge final : public Gpu, public RenderContext {
public:
    HeadlessGpuBridge();
    ~HeadlessGpuBridge() override;

    // --- RenderContext ---
    void InitializeGpu(void* video_out) override;
    void ShutdownGpu() override;
    Gpu& GetGpu() override { return *this; }

    // --- Gpu ---
    void Submit(uint32_t* dcb, uint32_t size_in_dwords, const uint32_t* acb,
                uint32_t acb_size_in_dwords, bool trigger_interrupt_on_done) override;
    void SubmitCompute(uint32_t queue, uint32_t* acb, uint32_t size_in_dwords,
                       bool trigger_interrupt_on_done) override;
    void Done() override;
    uint64_t GetFrameNum() const override;

    // Enable the real GPU compute path: on the next InitializeGpu the bridge
    // brings up a VulkanComputeExecutor and binds it (with the given guest
    // memory accessor) to the translator, so DISPATCH_DIRECT packets execute
    // real compute on the device instead of the backend CPU-sim. Pass the
    // VMM-backed VmmGpuMemory in the real emulator. Safe to leave unset: the
    // legacy path is used and behaviour is unchanged.
    void EnableRealCompute(PS5::GPU::GpuGuestMemory* guest_memory) {
        m_guest_memory = guest_memory;
        m_real_compute_requested = true;
    }
    bool RealComputeActive() const { return m_real_compute_active; }

    // What guest-memory accessor the bridge is bound to for real compute
    // reads/writes (nullptr when none / legacy path only). Read-only
    // observability so tests can assert the boot path wired VMM-backed memory.
    PS5::GPU::GpuGuestMemory* GuestMemory() const { return m_guest_memory; }

    // --- Observability (for tests / diagnostics) ---
    PS5::GPU::VulkanRendererBackend&       Backend() { return m_backend; }
    const PS5::GPU::VulkanRendererBackend& Backend() const { return m_backend; }
    const PS5::GPU::ComputeDispatchRecord& LastComputeDispatch() const {
        return m_translator.GetLastComputeDispatch();
    }

    // Cumulative counters across all submissions.
    uint32_t GetGraphicsSubmitCount() const { return m_gfx_submits; }
    uint32_t GetComputeSubmitCount() const { return m_compute_submits; }
    uint32_t GetInterruptCount() const { return m_interrupts; }
    size_t   GetTranslatedPacketCount() const { return m_translator.GetProcessedPacketsCount(); }
    bool     LastSubmitOk() const { return m_last_submit_ok; }
    const PS5::GPU::PM4DecodeError& LastDecodeError() const {
        return m_translator.GetLastDecodeError();
    }

private:
    PS5::GPU::VulkanRendererBackend m_backend;
    PS5::GPU::PM4VulkanTranslator   m_translator;
    std::unique_ptr<PS5::GPU::VulkanComputeExecutor> m_executor;
    PS5::GPU::GpuGuestMemory*        m_guest_memory{nullptr};
    bool                            m_real_compute_requested{false};
    bool                            m_real_compute_active{false};
    bool                            m_initialized{false};
    uint64_t                        m_frame_num{0};
    uint32_t                        m_gfx_submits{0};
    uint32_t                        m_compute_submits{0};
    uint32_t                        m_interrupts{0};
    bool                            m_last_submit_ok{true};
};

} // namespace Graphics
