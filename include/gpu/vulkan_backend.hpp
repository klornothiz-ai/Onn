#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include <mutex>
#include <string>

namespace PS5::GPU {

    // --- Vulkan-compatible opaque handle types (must match the real libvulkan.so.1 ABI) ---
    using VkInstance = void*;
    using VkPhysicalDevice = void*;
    using VkDevice = void*;
    using VkQueue = void*;
    using VkCommandPool = void*;
    using VkCommandBuffer = void*;
    using VkShaderModule = void*;
    using VkPipeline = void*;
    using VkPipelineLayout = void*;

    constexpr void* VK_NULL_HANDLE = nullptr;

    using VkResult = int32_t;
    constexpr VkResult VK_SUCCESS = 0;

    constexpr uint32_t VK_MAKE_VERSION(uint32_t major, uint32_t minor, uint32_t patch) {
        return (major << 22) | (minor << 12) | patch;
    }
    constexpr uint32_t VK_API_VERSION_1_3 = (1u << 22) | (3u << 12);

    constexpr uint32_t VK_COMMAND_BUFFER_LEVEL_PRIMARY = 0;
    constexpr uint32_t VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT = 0x00000002;

    // Standard Vulkan 1.3 Structure Type Enums
    enum VkStructureType : uint32_t {
        VK_STRUCTURE_TYPE_APPLICATION_INFO = 0,
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1,
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO = 2,
        VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO = 3,
        VK_STRUCTURE_TYPE_SUBMIT_INFO = 4,
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO = 39,
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO = 40,
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO = 16,
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO = 18,
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO = 29
    };

    struct VkApplicationInfo {
        VkStructureType sType{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        const void* pNext{nullptr};
        const char* pApplicationName{nullptr};
        uint32_t applicationVersion{0};
        const char* pEngineName{nullptr};
        uint32_t engineVersion{0};
        uint32_t apiVersion{0x00403000}; // Vulkan 1.3
    };

    struct VkInstanceCreateInfo {
        VkStructureType sType{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        const void* pNext{nullptr};
        uint32_t flags{0};
        const VkApplicationInfo* pApplicationInfo{nullptr};
        uint32_t enabledLayerCount{0};
        const char* const* ppEnabledLayerNames{nullptr};
        uint32_t enabledExtensionCount{0};
        const char* const* ppEnabledExtensionNames{nullptr};
    };

    struct VkPhysicalDeviceLimits { uint8_t reserved[512]{}; };
    struct VkPhysicalDeviceSparseProperties { uint8_t reserved[32]{}; };

    struct VkPhysicalDeviceProperties {
        uint32_t apiVersion;
        uint32_t driverVersion;
        uint32_t vendorID;
        uint32_t deviceID;
        uint32_t deviceType;
        char deviceName[256];
        uint8_t pipelineCacheUUID[16];
        VkPhysicalDeviceLimits limits;
        VkPhysicalDeviceSparseProperties sparseProperties;
    };

    struct VkQueueFamilyProperties {
        uint32_t queueFlags{0};
        uint32_t queueCount{0};
        uint32_t timestampValidBits{0};
        uint32_t minImageTransferGranularity[3]{};
    };
    constexpr uint32_t VK_QUEUE_GRAPHICS_BIT = 0x00000001;
    constexpr uint32_t VK_QUEUE_COMPUTE_BIT = 0x00000002;

    struct VkDeviceQueueCreateInfo {
        VkStructureType sType{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        const void* pNext{nullptr};
        uint32_t flags{0};
        uint32_t queueFamilyIndex{0};
        uint32_t queueCount{0};
        const float* pQueuePriorities{nullptr};
    };

    struct VkDeviceCreateInfo {
        VkStructureType sType{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        const void* pNext{nullptr};
        uint32_t flags{0};
        uint32_t queueCreateInfoCount{0};
        const VkDeviceQueueCreateInfo* pQueueCreateInfos{nullptr};
        uint32_t enabledLayerCount{0};
        const char* const* ppEnabledLayerNames{nullptr};
        uint32_t enabledExtensionCount{0};
        const char* const* ppEnabledExtensionNames{nullptr};
        const void* pEnabledFeatures{nullptr};
    };

    struct VkCommandPoolCreateInfo {
        VkStructureType sType{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        const void* pNext{nullptr};
        uint32_t flags{0};
        uint32_t queueFamilyIndex{0};
    };

    struct VkCommandBufferAllocateInfo {
        VkStructureType sType{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        const void* pNext{nullptr};
        VkCommandPool commandPool{nullptr};
        uint32_t level{0};
        uint32_t commandBufferCount{0};
    };

    struct VkShaderModuleCreateInfo {
        VkStructureType sType{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        const void* pNext{nullptr};
        uint32_t flags{0};
        size_t codeSize{0};
        const uint32_t* pCode{nullptr};
    };

    struct VkViewport {
        float x{0}, y{0}, width{0}, height{0}, minDepth{0}, maxDepth{1};
    };

    struct VkSubmitInfo {
        VkStructureType sType{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        const void* pNext{nullptr};
        uint32_t waitSemaphoreCount{0};
        const void* pWaitSemaphores{nullptr};
        const uint32_t* pWaitDstStageMask{nullptr};
        uint32_t commandBufferCount{0};
        const VkCommandBuffer* pCommandBuffers{nullptr};
        uint32_t signalSemaphoreCount{0};
        const void* pSignalSemaphores{nullptr};
    };

    // --- Vulkan function pointer typedefs (match real libvulkan.so.1 signatures) ---
    using PFN_vkVoidFunction = void(*)();
    using PFN_vkGetInstanceProcAddr = PFN_vkVoidFunction(*)(VkInstance, const char*);
    using PFN_vkCreateInstance = VkResult(*)(const VkInstanceCreateInfo*, const void*, VkInstance*);
    using PFN_vkDestroyInstance = void(*)(VkInstance, const void*);
    using PFN_vkEnumeratePhysicalDevices = VkResult(*)(VkInstance, uint32_t*, VkPhysicalDevice*);
    using PFN_vkGetPhysicalDeviceProperties = void(*)(VkPhysicalDevice, VkPhysicalDeviceProperties*);
    using PFN_vkGetPhysicalDeviceQueueFamilyProperties = void(*)(VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties*);
    using PFN_vkCreateDevice = VkResult(*)(VkPhysicalDevice, const VkDeviceCreateInfo*, const void*, VkDevice*);
    using PFN_vkDestroyDevice = void(*)(VkDevice, const void*);
    using PFN_vkGetDeviceQueue = void(*)(VkDevice, uint32_t, uint32_t, VkQueue*);
    using PFN_vkCreateCommandPool = VkResult(*)(VkDevice, const VkCommandPoolCreateInfo*, const void*, VkCommandPool*);
    using PFN_vkDestroyCommandPool = void(*)(VkDevice, VkCommandPool, const void*);
    using PFN_vkAllocateCommandBuffers = VkResult(*)(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*);
    using PFN_vkCreateShaderModule = VkResult(*)(VkDevice, const VkShaderModuleCreateInfo*, const void*, VkShaderModule*);
    using PFN_vkDestroyShaderModule = void(*)(VkDevice, VkShaderModule, const void*);
    using PFN_vkCreatePipelineLayout = VkResult(*)(VkDevice, const void*, const void*, VkPipelineLayout*);
    using PFN_vkDestroyPipelineLayout = void(*)(VkDevice, VkPipelineLayout, const void*);
    using PFN_vkCreateComputePipelines = VkResult(*)(VkDevice, void*, uint32_t, const void*, const void*, VkPipeline*);
    using PFN_vkDestroyPipeline = void(*)(VkDevice, VkPipeline, const void*);
    using PFN_vkBeginCommandBuffer = VkResult(*)(VkCommandBuffer, const void*);
    using PFN_vkEndCommandBuffer = VkResult(*)(VkCommandBuffer);
    using PFN_vkCmdBindPipeline = void(*)(VkCommandBuffer, uint32_t, VkPipeline);
    using PFN_vkCmdDispatch = void(*)(VkCommandBuffer, uint32_t, uint32_t, uint32_t);
    using PFN_vkCmdDraw = void(*)(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t);
    using PFN_vkCmdSetViewport = void(*)(VkCommandBuffer, uint32_t, uint32_t, const VkViewport*);
    using PFN_vkQueueSubmit = VkResult(*)(VkQueue, uint32_t, const VkSubmitInfo*, void*);
    using PFN_vkQueueWaitIdle = VkResult(*)(VkQueue);

    class VulkanRendererBackend {
    public:
        VulkanRendererBackend();
        ~VulkanRendererBackend();

        bool Initialize();
        void Shutdown();
        void BindComputePipeline(uint64_t gva_code_addr, const uint32_t* rdna2_code, size_t dwords_count);
        void SetViewport(float width, float height);
        void DispatchCompute(uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z);
        void DrawAuto(uint32_t vertex_count, uint32_t instance_count);
        void PresentFrame();
        // Creates a real SDL2 window + Vulkan surface swapchain when a
        // display is available; falls back to headless software rendering.
        bool CreateSwapchain(uint32_t width, uint32_t height);

        uint32_t GetFrameCount() const { return m_frame_count; }
        uint32_t GetDispatchedComputeCount() const { return m_dispatched_compute; }
        uint32_t GetDrawCallCount() const { return m_draw_calls; }
        uint32_t GetFramebufferCRC() const { return m_framebuffer_crc; }
        bool IsHardwareActive() const { return m_hardware_active; }

    private:
        bool m_initialized{false};
        bool m_hardware_active{false};
        uint32_t m_frame_count{0};
        uint32_t m_dispatched_compute{0};
        uint32_t m_draw_calls{0};
        uint32_t m_framebuffer_crc{0};
        float m_vp_width{1920.0f};
        float m_vp_height{1080.0f};

        std::vector<uint32_t> m_framebuffer; // 1920x1080 RGBA8 Framebuffer
        mutable std::mutex m_vk_mutex;

        // Dynamic library + Vulkan object handles
        void* m_vulkan_lib{nullptr};
        VkInstance m_instance{VK_NULL_HANDLE};
        VkPhysicalDevice m_physical_device{VK_NULL_HANDLE};
        VkDevice m_device{VK_NULL_HANDLE};
        VkQueue m_queue{VK_NULL_HANDLE};
        uint32_t m_queue_family_index{0};
        // Real display output (SDL2 window) when available.
        void* m_sdl_window{nullptr};
        void* m_sdl_renderer{nullptr};
        void* m_sdl_texture{nullptr};
        bool  m_window_enabled{false};
        VkCommandPool m_cmd_pool{VK_NULL_HANDLE};
        VkCommandBuffer m_cmd_buffer{VK_NULL_HANDLE};
        VkShaderModule m_current_shader_module{VK_NULL_HANDLE};
        VkPipeline m_current_pipeline{VK_NULL_HANDLE};
        VkPipelineLayout m_current_pipeline_layout{VK_NULL_HANDLE};

        // Resolved Vulkan function pointers
        PFN_vkCreateInstance pfn_vkCreateInstance{nullptr};
        PFN_vkDestroyInstance pfn_vkDestroyInstance{nullptr};
        PFN_vkEnumeratePhysicalDevices pfn_vkEnumeratePhysicalDevices{nullptr};
        PFN_vkGetPhysicalDeviceProperties pfn_vkGetPhysicalDeviceProperties{nullptr};
        PFN_vkGetPhysicalDeviceQueueFamilyProperties pfn_vkGetPhysicalDeviceQueueFamilyProperties{nullptr};
        PFN_vkCreateDevice pfn_vkCreateDevice{nullptr};
        PFN_vkDestroyDevice pfn_vkDestroyDevice{nullptr};
        PFN_vkGetDeviceQueue pfn_vkGetDeviceQueue{nullptr};
        PFN_vkCreateCommandPool pfn_vkCreateCommandPool{nullptr};
        PFN_vkDestroyCommandPool pfn_vkDestroyCommandPool{nullptr};
        PFN_vkAllocateCommandBuffers pfn_vkAllocateCommandBuffers{nullptr};
        PFN_vkCreateShaderModule pfn_vkCreateShaderModule{nullptr};
        PFN_vkDestroyShaderModule pfn_vkDestroyShaderModule{nullptr};
        PFN_vkCreatePipelineLayout pfn_vkCreatePipelineLayout{nullptr};
        PFN_vkDestroyPipelineLayout pfn_vkDestroyPipelineLayout{nullptr};
        PFN_vkCreateComputePipelines pfn_vkCreateComputePipelines{nullptr};
        PFN_vkDestroyPipeline pfn_vkDestroyPipeline{nullptr};
        PFN_vkBeginCommandBuffer pfn_vkBeginCommandBuffer{nullptr};
        PFN_vkEndCommandBuffer pfn_vkEndCommandBuffer{nullptr};
        PFN_vkCmdBindPipeline pfn_vkCmdBindPipeline{nullptr};
        PFN_vkCmdDispatch pfn_vkCmdDispatch{nullptr};
        PFN_vkCmdDraw pfn_vkCmdDraw{nullptr};
        PFN_vkCmdSetViewport pfn_vkCmdSetViewport{nullptr};
        PFN_vkQueueSubmit pfn_vkQueueSubmit{nullptr};
        PFN_vkQueueWaitIdle pfn_vkQueueWaitIdle{nullptr};

        bool LoadVulkanLibrary();
        bool InitializeVulkanHardware();
        void RenderRasterSimulation(uint32_t vertices, uint32_t instances);
        void ComputeShaderSimulation(uint32_t gx, uint32_t gy, uint32_t gz);
    };

}
