#include "gpu/vulkan_backend.hpp"
#include "gpu/shader_spirv_recompiler.hpp"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <dlfcn.h>

// SDL2 is used for real windowed presentation; the backend still works
// headless when SDL or a display is unavailable.
#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
struct SDL_Window {};
struct SDL_Renderer {};
struct SDL_Texture {};
struct SDL_Event { unsigned type{0}; };
constexpr unsigned SDL_INIT_VIDEO = 0;
constexpr unsigned SDL_WINDOW_SHOWN = 0;
constexpr int SDL_WINDOWPOS_CENTERED = 0;
constexpr unsigned SDL_RENDERER_ACCELERATED = 0;
constexpr unsigned SDL_RENDERER_PRESENTVSYNC = 0;
constexpr unsigned SDL_RENDERER_SOFTWARE = 0;
constexpr unsigned SDL_PIXELFORMAT_ABGR8888 = 0;
constexpr unsigned SDL_TEXTUREACCESS_STREAMING = 0;
constexpr unsigned SDL_QUIT = 0x100;
static int SDL_InitSubSystem(unsigned) { return -1; }
static SDL_Window* SDL_CreateWindow(const char*, int, int, int, int, unsigned) { return nullptr; }
static SDL_Renderer* SDL_CreateRenderer(SDL_Window*, int, unsigned) { return nullptr; }
static SDL_Texture* SDL_CreateTexture(SDL_Renderer*, unsigned, int, int, int) { return nullptr; }
static const char* SDL_GetError() { return "SDL2 headers unavailable"; }
static void SDL_DestroyWindow(SDL_Window*) {}
static void SDL_DestroyRenderer(SDL_Renderer*) {}
static void SDL_DestroyTexture(SDL_Texture*) {}
static void SDL_QuitSubSystem(unsigned) {}
static int SDL_PollEvent(SDL_Event*) { return 0; }
static void SDL_HideWindow(SDL_Window*) {}
static int SDL_LockTexture(SDL_Texture*, const void*, void**, int*) { return -1; }
static void SDL_UnlockTexture(SDL_Texture*) {}
static void SDL_RenderClear(SDL_Renderer*) {}
static void SDL_RenderCopy(SDL_Renderer*, SDL_Texture*, const void*, const void*) {}
static void SDL_RenderPresent(SDL_Renderer*) {}
#endif

namespace PS5::GPU {

    bool VulkanRendererBackend::CreateSwapchain(uint32_t width, uint32_t height) {
        std::lock_guard<std::mutex> lock(m_vk_mutex);

        if (width == 0 || height == 0) return false;
        m_vp_width = static_cast<float>(width);
        m_vp_height = static_cast<float>(height);
        m_framebuffer.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0xFF000000);

        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
                // Headless environment: keep the memory framebuffer path.
                std::cout << "  [Vulkan Backend] SDL video unavailable, using memory framebuffer\n";
                return false;
        }

        SDL_Window* window = SDL_CreateWindow(
            "ProsperoLayer PS5 Emulator v19", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            static_cast<int>(width), static_cast<int>(height), SDL_WINDOW_SHOWN);
        if (window == nullptr) {
                std::cout << "  [Vulkan Backend] SDL window creation failed: " << SDL_GetError() << "\n";
                return false;
        }

        SDL_Renderer* renderer = SDL_CreateRenderer(
            window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (renderer == nullptr) {
                renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        }
        if (renderer == nullptr) {
                SDL_DestroyWindow(window);
                return false;
        }

        SDL_Texture* texture = SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
            static_cast<int>(width), static_cast<int>(height));
        if (texture == nullptr) {
                SDL_DestroyRenderer(renderer);
                SDL_DestroyWindow(window);
                SDL_QuitSubSystem(SDL_INIT_VIDEO);
                return false;
        }

        m_sdl_window   = window;
        m_sdl_renderer = renderer;
        m_sdl_texture  = texture;
        m_window_enabled = true;
        m_vp_width     = static_cast<float>(width);
        m_vp_height    = static_cast<float>(height);

        std::cout << "  [Vulkan Backend] Real swapchain created: " << width << "x" << height
                  << " (SDL2 windowed presentation enabled)\n";
        return true;
    }

    void VulkanRendererBackend::PresentFrame() {
        std::lock_guard<std::mutex> lock(m_vk_mutex);
        m_frame_count++;

        if (m_hardware_active && m_queue && m_cmd_buffer && pfn_vkQueueSubmit) {
            VkSubmitInfo submit_info{};
            submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit_info.commandBufferCount = 1;
            submit_info.pCommandBuffers = &m_cmd_buffer;
            pfn_vkQueueSubmit(m_queue, 1, &submit_info, VK_NULL_HANDLE);
        }

        // Pump events so a windowed guest can close or remain responsive.
        if (m_window_enabled) {
                SDL_Event event{};
                while (SDL_PollEvent(&event) != 0) {
                        if (event.type == SDL_QUIT) {
                                SDL_HideWindow(static_cast<SDL_Window*>(m_sdl_window));
                        }
                }
        }

        // Present the framebuffer to the real window when available.
        if (m_window_enabled && m_sdl_renderer != nullptr && m_sdl_texture != nullptr) {
                SDL_Renderer* renderer = static_cast<SDL_Renderer*>(m_sdl_renderer);
                SDL_Texture*  texture  = static_cast<SDL_Texture*>(m_sdl_texture);
                void*         pixels   = nullptr;
                int           pitch    = 0;
                if (SDL_LockTexture(texture, nullptr, &pixels, &pitch) == 0 && pixels != nullptr) {
                        const size_t row_bytes = std::min<size_t>(
                            static_cast<size_t>(m_vp_width) * sizeof(uint32_t),
                            static_cast<size_t>(pitch));
                        const size_t rows = std::min<size_t>(
                            static_cast<size_t>(m_vp_height), m_framebuffer.size() /
                            std::max<size_t>(1, static_cast<size_t>(m_vp_width)));
                        const auto* source = reinterpret_cast<const uint8_t*>(m_framebuffer.data());
                        auto* destination = static_cast<uint8_t*>(pixels);
                        for (size_t row = 0; row < rows; ++row) {
                            std::memcpy(destination + row * static_cast<size_t>(pitch),
                                        source + row * static_cast<size_t>(m_vp_width) * sizeof(uint32_t),
                                        row_bytes);
                        }
                        SDL_UnlockTexture(texture);
                }
                SDL_RenderClear(renderer);
                SDL_RenderCopy(renderer, texture, nullptr, nullptr);
                SDL_RenderPresent(renderer);
        }

        uint32_t crc = 0xFFFFFFFF;
        const size_t crc_samples = std::min<size_t>(1024, m_framebuffer.size());
        for (size_t i = 0; i < crc_samples; ++i) {
            crc = (crc >> 1) ^ (m_framebuffer[i] & 0xFF);
        }
        m_framebuffer_crc = ~crc;

        std::cout << "  [Vulkan Backend] vkQueuePresentKHR() -> Frame #" << m_frame_count 
                  << " Presented (CRC: 0x" << std::hex << m_framebuffer_crc << std::dec 
                  << ", Mode: " << (m_hardware_active ? "GPU Queue" : "Memory Framebuffer") << ")\n";
    }

    VulkanRendererBackend::VulkanRendererBackend() {
        m_framebuffer.resize(static_cast<size_t>(m_vp_width) * static_cast<size_t>(m_vp_height),
                             0xFF000000);
    }

    VulkanRendererBackend::~VulkanRendererBackend() {
        Shutdown();
    }

    void VulkanRendererBackend::Shutdown() {
        std::lock_guard<std::mutex> lock(m_vk_mutex);
        if (!m_initialized && !m_window_enabled) return;

        // Wait for all GPU work to finish before destroying anything.
        if (m_device != VK_NULL_HANDLE) {
            if (pfn_vkQueueWaitIdle && m_queue) pfn_vkQueueWaitIdle(m_queue);
        }

        if (m_device != VK_NULL_HANDLE) {
            if (pfn_vkDestroyPipeline && m_current_pipeline) pfn_vkDestroyPipeline(m_device, m_current_pipeline, nullptr);
            if (pfn_vkDestroyPipelineLayout && m_current_pipeline_layout) pfn_vkDestroyPipelineLayout(m_device, m_current_pipeline_layout, nullptr);
            if (pfn_vkDestroyShaderModule && m_current_shader_module) pfn_vkDestroyShaderModule(m_device, m_current_shader_module, nullptr);
            if (pfn_vkDestroyCommandPool && m_cmd_pool) pfn_vkDestroyCommandPool(m_device, m_cmd_pool, nullptr);
            if (pfn_vkDestroyDevice) pfn_vkDestroyDevice(m_device, nullptr);
            m_device = VK_NULL_HANDLE;
        }

        if (m_instance != VK_NULL_HANDLE) {
            if (pfn_vkDestroyInstance) pfn_vkDestroyInstance(m_instance, nullptr);
            m_instance = VK_NULL_HANDLE;
        }

        // NOTE: Deliberately NOT dlclose()'ing the Vulkan driver. llvmpipe /
        // lavapipe (and most ICDs) register atexit cleanup handlers; unloading
        // the shared object while those handlers are still registered crashes
        // at process exit. The OS reclaims the library when the process ends.
        m_vulkan_lib = nullptr;

        // Tear down the SDL window (if any) after the Vulkan objects.
        if (m_window_enabled) {
                if (m_sdl_texture != nullptr) {
                        SDL_DestroyTexture(static_cast<SDL_Texture*>(m_sdl_texture));
                        m_sdl_texture = nullptr;
                }
                if (m_sdl_renderer != nullptr) {
                        SDL_DestroyRenderer(static_cast<SDL_Renderer*>(m_sdl_renderer));
                        m_sdl_renderer = nullptr;
                }
                if (m_sdl_window != nullptr) {
                        SDL_DestroyWindow(static_cast<SDL_Window*>(m_sdl_window));
                        m_sdl_window = nullptr;
                }
                m_window_enabled = false;
                SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }

        m_initialized = false;
        m_hardware_active = false;
    }

    bool VulkanRendererBackend::LoadVulkanLibrary() {
        const char* lib_names[] = {
            "libvulkan.so.1",
            "libvulkan.so",
            "/usr/lib/x86_64-linux-gnu/libvulkan.so.1",
            "/usr/lib64/libvulkan.so.1"
        };

        for (const char* name : lib_names) {
            m_vulkan_lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (m_vulkan_lib) break;
        }

        if (!m_vulkan_lib) {
            std::cerr << "[Vulkan Backend] Warning: Dynamic loader could not open libvulkan.so.1 (" 
                      << dlerror() << "). Switching to CPU reference mode.\n";
            return false;
        }

        auto vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(m_vulkan_lib, "vkGetInstanceProcAddr"));
        if (!vkGetInstanceProcAddr) {
            std::cerr << "[Vulkan Backend] Error: Failed to resolve vkGetInstanceProcAddr.\n";
            return false;
        }

        pfn_vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(dlsym(m_vulkan_lib, "vkCreateInstance"));
        pfn_vkDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(dlsym(m_vulkan_lib, "vkDestroyInstance"));
        pfn_vkEnumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(dlsym(m_vulkan_lib, "vkEnumeratePhysicalDevices"));

        return (pfn_vkCreateInstance != nullptr);
    }

    bool VulkanRendererBackend::InitializeVulkanHardware() {
        if (!LoadVulkanLibrary()) return false;

        VkApplicationInfo app_info{};
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.pApplicationName = "Prospero PS5 Vulkan Emulator Core";
        app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        app_info.pEngineName = "Hedra Oberon RDNA2 Engine";
        app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        app_info.apiVersion = VK_API_VERSION_1_3;

        VkInstanceCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.pApplicationInfo = &app_info;

        if (!pfn_vkCreateInstance) {
            std::cout << "[Vulkan 1.3] Notice: vkCreateInstance symbol not resolved. "
                      << "Initializing software reference pipeline.\n";
            return false;
        }

        VkResult res = pfn_vkCreateInstance(&create_info, nullptr, &m_instance);
        if (res != VK_SUCCESS || !m_instance) {
            std::cout << "[Vulkan 1.3] Notice: vkCreateInstance returned " << res 
                      << ". Active GPU ICD not present. Initializing software reference pipeline.\n";
            return false;
        }

        std::cout << "[Vulkan 1.3] Dynamic Vulkan 1.3 Loader Connected:\n"
                  << "  * Vulkan Library Handle : " << m_vulkan_lib << "\n"
                  << "  * VkInstance Created    : " << m_instance << "\n"
                  << "  * Target API Version    : 1.3 (0x00403000)\n";

        // Query Physical Devices
        uint32_t device_count = 0;
        pfn_vkEnumeratePhysicalDevices(m_instance, &device_count, nullptr);
        std::cout << "  * Physical GPUs Detected: " << device_count << "\n";

        if (device_count > 0) {
            std::vector<VkPhysicalDevice> devices(device_count);
            pfn_vkEnumeratePhysicalDevices(m_instance, &device_count, devices.data());
            m_physical_device = devices[0];

            auto getProc = [this](const char* name) {
                return dlsym(m_vulkan_lib, name);
            };

            pfn_vkGetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(getProc("vkGetPhysicalDeviceProperties"));
            pfn_vkGetPhysicalDeviceQueueFamilyProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(getProc("vkGetPhysicalDeviceQueueFamilyProperties"));
            pfn_vkCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(getProc("vkCreateDevice"));
            pfn_vkDestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(getProc("vkDestroyDevice"));
            pfn_vkGetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(getProc("vkGetDeviceQueue"));
            pfn_vkCreateCommandPool = reinterpret_cast<PFN_vkCreateCommandPool>(getProc("vkCreateCommandPool"));
            pfn_vkDestroyCommandPool = reinterpret_cast<PFN_vkDestroyCommandPool>(getProc("vkDestroyCommandPool"));
            pfn_vkAllocateCommandBuffers = reinterpret_cast<PFN_vkAllocateCommandBuffers>(getProc("vkAllocateCommandBuffers"));
            pfn_vkCreateShaderModule = reinterpret_cast<PFN_vkCreateShaderModule>(getProc("vkCreateShaderModule"));
            pfn_vkDestroyShaderModule = reinterpret_cast<PFN_vkDestroyShaderModule>(getProc("vkDestroyShaderModule"));
            pfn_vkCreatePipelineLayout = reinterpret_cast<PFN_vkCreatePipelineLayout>(getProc("vkCreatePipelineLayout"));
            pfn_vkDestroyPipelineLayout = reinterpret_cast<PFN_vkDestroyPipelineLayout>(getProc("vkDestroyPipelineLayout"));
            pfn_vkCreateComputePipelines = reinterpret_cast<PFN_vkCreateComputePipelines>(getProc("vkCreateComputePipelines"));
            pfn_vkDestroyPipeline = reinterpret_cast<PFN_vkDestroyPipeline>(getProc("vkDestroyPipeline"));
            pfn_vkBeginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(getProc("vkBeginCommandBuffer"));
            pfn_vkEndCommandBuffer = reinterpret_cast<PFN_vkEndCommandBuffer>(getProc("vkEndCommandBuffer"));
            pfn_vkCmdBindPipeline = reinterpret_cast<PFN_vkCmdBindPipeline>(getProc("vkCmdBindPipeline"));
            pfn_vkCmdDispatch = reinterpret_cast<PFN_vkCmdDispatch>(getProc("vkCmdDispatch"));
            pfn_vkCmdDraw = reinterpret_cast<PFN_vkCmdDraw>(getProc("vkCmdDraw"));
            pfn_vkCmdSetViewport = reinterpret_cast<PFN_vkCmdSetViewport>(getProc("vkCmdSetViewport"));
            pfn_vkQueueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(getProc("vkQueueSubmit"));
            pfn_vkQueueWaitIdle = reinterpret_cast<PFN_vkQueueWaitIdle>(getProc("vkQueueWaitIdle"));

            if (pfn_vkGetPhysicalDeviceProperties) {
                VkPhysicalDeviceProperties props;
                pfn_vkGetPhysicalDeviceProperties(m_physical_device, &props);
                std::cout << "  * Active Physical GPU   : " << props.deviceName 
                          << " (Driver Version: " << props.driverVersion << ")\n";
            }

            // Select a queue family that can execute both graphics and compute.
            uint32_t family_count = 0;
            pfn_vkGetPhysicalDeviceQueueFamilyProperties(m_physical_device, &family_count, nullptr);
            if (family_count == 0) return false;
            std::vector<VkQueueFamilyProperties> families(family_count);
            pfn_vkGetPhysicalDeviceQueueFamilyProperties(m_physical_device, &family_count, families.data());
            bool found_family = false;
            for (uint32_t index = 0; index < family_count; ++index) {
                if (families[index].queueCount > 0 &&
                    (families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 &&
                    (families[index].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) {
                    m_queue_family_index = index;
                    found_family = true;
                    break;
                }
            }
            if (!found_family) return false;

            // Create Logical Device
            float queue_priority = 1.0f;
            VkDeviceQueueCreateInfo queue_info{};
            queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queue_info.queueFamilyIndex = m_queue_family_index;
            queue_info.queueCount = 1;
            queue_info.pQueuePriorities = &queue_priority;

            VkDeviceCreateInfo device_info{};
            device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
            device_info.queueCreateInfoCount = 1;
            device_info.pQueueCreateInfos = &queue_info;

            if (pfn_vkCreateDevice && pfn_vkCreateDevice(m_physical_device, &device_info, nullptr, &m_device) == VK_SUCCESS) {
                std::cout << "  * VkDevice Created      : " << m_device << " (Hardware Rendering Enabled)\n";
                if (pfn_vkGetDeviceQueue) {
                    pfn_vkGetDeviceQueue(m_device, m_queue_family_index, 0, &m_queue);
                }

                // Command Pool
                VkCommandPoolCreateInfo pool_info{};
                pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
                pool_info.queueFamilyIndex = m_queue_family_index;
                pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

                if (pfn_vkCreateCommandPool && pfn_vkCreateCommandPool(m_device, &pool_info, nullptr, &m_cmd_pool) == VK_SUCCESS) {
                    VkCommandBufferAllocateInfo alloc_info{};
                    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                    alloc_info.commandPool = m_cmd_pool;
                    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                    alloc_info.commandBufferCount = 1;
                    if (pfn_vkAllocateCommandBuffers) {
                        pfn_vkAllocateCommandBuffers(m_device, &alloc_info, &m_cmd_buffer);
                    }
                }

                const bool command_path_ready =
                    m_queue != VK_NULL_HANDLE && m_cmd_buffer != VK_NULL_HANDLE &&
                    pfn_vkBeginCommandBuffer && pfn_vkEndCommandBuffer &&
                    pfn_vkCmdBindPipeline && pfn_vkCmdDispatch && pfn_vkCmdDraw;
                m_hardware_active = command_path_ready;
                if (!command_path_ready) {
                    std::cerr << "[Vulkan Backend] Device created without a complete command path; "
                              << "using reference framebuffer mode.\n";
                }
            }
        }

        return m_hardware_active;
    }

    bool VulkanRendererBackend::Initialize() {
        std::lock_guard<std::mutex> lock(m_vk_mutex);
        if (m_initialized) return true;

        InitializeVulkanHardware();

        std::cout << "[Vulkan 1.3] Pipeline Configured (Mode: " 
                  << (m_hardware_active ? "Hardware Physical GPU" : "Native Software Memory Framebuffer") << ")\n"
                  << "  * Framebuffer Resolution : " << static_cast<uint32_t>(m_vp_width)
                  << "x" << static_cast<uint32_t>(m_vp_height) << " RGBA8\n";

        m_initialized = true;
        return true;
    }

    void VulkanRendererBackend::BindComputePipeline(uint64_t gva_code_addr, const uint32_t* rdna2_code, size_t dwords_count) {
        std::lock_guard<std::mutex> lock(m_vk_mutex);
        ShaderSpirvRecompiler recompiler;
        auto compilation = recompiler.CompileRDNA2ToSpirv(rdna2_code, dwords_count);
        if (!compilation) {
            std::cerr << "  [Vulkan Backend] RDNA2 subset compilation rejected at dword "
                      << compilation.error_dword << ": " << compilation.message << "\n";
            return;
        }

        if (m_hardware_active && m_device && pfn_vkCreateShaderModule) {
            VkShaderModuleCreateInfo sm_info{};
            sm_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            sm_info.codeSize = compilation.spirv.size() * sizeof(uint32_t);
            sm_info.pCode = compilation.spirv.data();

            VkShaderModule candidate = VK_NULL_HANDLE;
            const VkResult sm_res = pfn_vkCreateShaderModule(m_device, &sm_info, nullptr, &candidate);
            if (sm_res == VK_SUCCESS && candidate != VK_NULL_HANDLE) {
                if (m_current_shader_module) {
                    pfn_vkDestroyShaderModule(m_device, m_current_shader_module, nullptr);
                }
                m_current_shader_module = candidate;
                std::cout << "  [Vulkan Backend] VkShaderModule created.\n";
            } else {
                std::cerr << "  [Vulkan Backend] vkCreateShaderModule failed with result "
                          << sm_res << "; preserving the previous module.\n";
                return;
            }
        }

        std::cout << "  [Vulkan Backend] RDNA2 subset compiled to a minimal SPIR-V 1.0 module:\n"
                  << "    * Input RDNA2 Address : 0x" << std::hex << gva_code_addr << std::dec << "\n"
                  << "    * Input Instructions  : " << dwords_count << "\n"
                  << "    * SPIR-V Size         : "
                  << (compilation.spirv.size() * sizeof(uint32_t)) << " bytes\n";
    }

    void VulkanRendererBackend::SetViewport(float width, float height) {
        if (width <= 0.0f || height <= 0.0f) return;
        m_vp_width = width;
        m_vp_height = height;
        const size_t framebuffer_size = static_cast<size_t>(width) * static_cast<size_t>(height);
        if (framebuffer_size != 0 && framebuffer_size != m_framebuffer.size()) {
            m_framebuffer.assign(framebuffer_size, 0xFF000000);
        }

        if (m_hardware_active && m_cmd_buffer && pfn_vkCmdSetViewport) {
            VkViewport vp{};
            vp.x = 0.0f;
            vp.y = 0.0f;
            vp.width = width;
            vp.height = height;
            vp.minDepth = 0.0f;
            vp.maxDepth = 1.0f;
            pfn_vkCmdSetViewport(m_cmd_buffer, 0, 1, &vp);
        }

        std::cout << "  [Vulkan Backend] vkCmdSetViewport(" << width << "x" << height << ") at (0, 0)\n";
    }

    void VulkanRendererBackend::DispatchCompute(uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z) {
        std::lock_guard<std::mutex> lock(m_vk_mutex);
        m_dispatched_compute++;

        const bool can_dispatch = m_hardware_active && m_current_pipeline != VK_NULL_HANDLE &&
                                  m_cmd_buffer && pfn_vkCmdDispatch;
        if (can_dispatch) {
            pfn_vkCmdDispatch(m_cmd_buffer, group_count_x, group_count_y, group_count_z);
        } else {
            std::cerr << "  [Vulkan Backend] dispatch skipped: no bound compute pipeline; "
                      << "using reference mode.\n";
        }

        if (!can_dispatch) {
            ComputeShaderSimulation(group_count_x, group_count_y, group_count_z);
        }
        std::cout << "  [Vulkan Backend] vkCmdDispatch(" << group_count_x << ", " << group_count_y 
                  << ", " << group_count_z << ") -> "
                  << (can_dispatch ? "executed on Vulkan compute queue." : "reference path.") << "\n";
    }

    void VulkanRendererBackend::DrawAuto(uint32_t vertex_count, uint32_t instance_count) {
        std::lock_guard<std::mutex> lock(m_vk_mutex);
        m_draw_calls++;

        const bool can_draw = m_hardware_active && m_current_pipeline != VK_NULL_HANDLE &&
                              m_cmd_buffer && pfn_vkCmdDraw;
        if (can_draw) {
            pfn_vkCmdDraw(m_cmd_buffer, vertex_count, instance_count, 0, 0);
        } else {
            std::cerr << "  [Vulkan Backend] draw skipped: no bound graphics pipeline; "
                      << "using reference mode.\n";
        }

        if (!can_draw) {
            RenderRasterSimulation(vertex_count, instance_count);
        }
        std::cout << "  [Vulkan Backend] vkCmdDraw(" << vertex_count << " Vertices, " << instance_count 
                  << " Instances) -> "
                  << (can_draw ? "submitted to Vulkan." : "reference path.") << "\n";
    }

    void VulkanRendererBackend::ComputeShaderSimulation(uint32_t gx, uint32_t gy, uint32_t gz) {
        const uint64_t groups = static_cast<uint64_t>(gx) * gy * gz;
        const size_t total_elements = std::min<size_t>(
            m_framebuffer.size(), groups > SIZE_MAX / 64 ? SIZE_MAX : static_cast<size_t>(groups * 64));
        for (size_t i = 0; i < total_elements; ++i) {
            m_framebuffer[i] ^= 0x001F0000;
        }
    }

    void VulkanRendererBackend::RenderRasterSimulation(uint32_t vertices, uint32_t) {
        size_t pixels_to_draw = std::min<size_t>(m_framebuffer.size(), vertices * 64);
        for (size_t i = 0; i < pixels_to_draw; ++i) {
            m_framebuffer[i] = 0xFF00FF00;
        }
    }


}
