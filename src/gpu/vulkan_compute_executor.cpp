#include "gpu/vulkan_compute_executor.hpp"
#include "gpu/rdna2_compute_compiler.hpp"
#include "gpu/vulkan_pipeline_cache.hpp"

#include <cstring>

// Real Vulkan path is compiled only when the loader headers are present. On a
// host without <vulkan/vulkan.h> the executor still links and honestly reports
// Unavailable instead of faking results.
#if __has_include(<vulkan/vulkan.h>)
#define PROSPERO_HAVE_VULKAN 1
#include <vulkan/vulkan.h>
#else
#define PROSPERO_HAVE_VULKAN 0
#endif

namespace PS5::GPU {

#if PROSPERO_HAVE_VULKAN

struct VulkanComputeExecutor::Impl {
    VkInstance instance{VK_NULL_HANDLE};
    VkPhysicalDevice phys{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    VkQueue queue{VK_NULL_HANDLE};
    uint32_t queue_family{0};
    VkCommandPool cmd_pool{VK_NULL_HANDLE};
    VkPhysicalDeviceMemoryProperties mem_props{};

    uint32_t FindMemoryType(uint32_t bits, VkMemoryPropertyFlags want) const {
        for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) &&
                (mem_props.memoryTypes[i].propertyFlags & want) == want) {
                return i;
            }
        }
        return UINT32_MAX;
    }

    // ---- round 20: cross-dispatch pipeline caches ---------------------------
    // The objects a dispatch used to rebuild every time. They live with the
    // DEVICE (destroyed in the Impl destructor), indexed by the process-wide
    // pipeline cache key. The production bridge keeps one executor for the
    // process, so every re-dispatch of the same program is a driver-free
    // reuse of these handles.
    struct CachedComputeObjects {
        VkDescriptorSetLayout dsl{VK_NULL_HANDLE};
        VkPipelineLayout playout{VK_NULL_HANDLE};
        VkShaderModule module{VK_NULL_HANDLE};
        VkPipeline pipeline{VK_NULL_HANDLE};
    };
    struct CachedGraphicsObjects {
        VkDescriptorSetLayout dsl{VK_NULL_HANDLE};
        VkPipelineLayout playout{VK_NULL_HANDLE};
        VkShaderModule vs_module{VK_NULL_HANDLE};
        VkShaderModule fs_module{VK_NULL_HANDLE};
        VkRenderPass render_pass{VK_NULL_HANDLE};
        VkPipeline pipeline{VK_NULL_HANDLE};
    };
    std::unordered_map<PipelineCacheKey, CachedComputeObjects, PipelineCacheKeyHash> compute_cache;
    std::unordered_map<PipelineCacheKey, CachedGraphicsObjects, PipelineCacheKeyHash> graphics_cache;

    void DestroyCaches(VkDevice dev) {
        for (auto& [k, o] : compute_cache) {
            if (o.pipeline) vkDestroyPipeline(dev, o.pipeline, nullptr);
            if (o.module) vkDestroyShaderModule(dev, o.module, nullptr);
            if (o.playout) vkDestroyPipelineLayout(dev, o.playout, nullptr);
            if (o.dsl) vkDestroyDescriptorSetLayout(dev, o.dsl, nullptr);
        }
        compute_cache.clear();
        for (auto& [k, o] : graphics_cache) {
            if (o.pipeline) vkDestroyPipeline(dev, o.pipeline, nullptr);
            if (o.render_pass) vkDestroyRenderPass(dev, o.render_pass, nullptr);
            if (o.vs_module) vkDestroyShaderModule(dev, o.vs_module, nullptr);
            if (o.fs_module) vkDestroyShaderModule(dev, o.fs_module, nullptr);
            if (o.playout) vkDestroyPipelineLayout(dev, o.playout, nullptr);
            if (o.dsl) vkDestroyDescriptorSetLayout(dev, o.dsl, nullptr);
        }
        graphics_cache.clear();
    }
};

VulkanComputeExecutor::VulkanComputeExecutor() : m_impl(new Impl()) {}

VulkanComputeExecutor::~VulkanComputeExecutor() {
    if (m_impl) {
        if (m_impl->device) m_impl->DestroyCaches(m_impl->device);
        if (m_impl->cmd_pool) vkDestroyCommandPool(m_impl->device, m_impl->cmd_pool, nullptr);
        if (m_impl->device)   vkDestroyDevice(m_impl->device, nullptr);
        if (m_impl->instance) vkDestroyInstance(m_impl->instance, nullptr);
        delete m_impl;
        m_impl = nullptr;
    }
}

bool VulkanComputeExecutor::Initialize() {
    if (m_ready) return true;
    if (!m_impl) return false;

    VkApplicationInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "ProsperoLayer";
    ai.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    if (vkCreateInstance(&ici, nullptr, &m_impl->instance) != VK_SUCCESS) {
        return false;
    }

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_impl->instance, &count, nullptr);
    if (count == 0) return false;
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_impl->instance, &count, devices.data());
    m_impl->phys = devices[0];

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(m_impl->phys, &props);
    m_device_name = props.deviceName;

    vkGetPhysicalDeviceMemoryProperties(m_impl->phys, &m_impl->mem_props);

    uint32_t qcount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_impl->phys, &qcount, nullptr);
    if (qcount == 0) return false;
    std::vector<VkQueueFamilyProperties> families(qcount);
    vkGetPhysicalDeviceQueueFamilyProperties(m_impl->phys, &qcount, families.data());
    // Round 19: prefer a family with BOTH compute and graphics (family 0 on
    // virtually every driver) so the graphics raster path shares the same
    // queue; fall back to the first compute-only family (graphics then
    // reports unavailable -- fail-closed, exactly like before).
    long best = -1;
    for (uint32_t i = 0; i < qcount; ++i) {
        if (families[i].queueCount == 0) continue;
        const bool compute = (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
        const bool graphics = (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        if (compute && graphics) { best = static_cast<long>(i); break; }
        if (compute && best < 0) best = static_cast<long>(i);
    }
    if (best < 0) return false;
    m_impl->queue_family = static_cast<uint32_t>(best);
    m_graphics_queue =
        (families[m_impl->queue_family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = m_impl->queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    if (vkCreateDevice(m_impl->phys, &dci, nullptr, &m_impl->device) != VK_SUCCESS) {
        return false;
    }
    vkGetDeviceQueue(m_impl->device, m_impl->queue_family, 0, &m_impl->queue);

    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.queueFamilyIndex = m_impl->queue_family;
    if (vkCreateCommandPool(m_impl->device, &cpi, nullptr, &m_impl->cmd_pool) != VK_SUCCESS) {
        return false;
    }

    m_ready = true;
    return true;
}

ComputeDispatchResult VulkanComputeExecutor::RunSpirv(
        const std::vector<uint32_t>& spirv,
        const std::vector<uint32_t>& input,
        uint32_t local_size_x,
        uint32_t threads,
        uint32_t out_elements,
        std::vector<SpirvExtraSsbo>* extra,
        const uint32_t* push_constants,
        uint32_t push_constant_dwords,
        std::vector<SpirvExtraImage>* extra_images) {
    ComputeDispatchResult out;
    out.device_name = m_device_name;
    out.spirv_dwords = spirv.size();
    if (!m_ready) {
        out.status = ComputeExecStatus::Unavailable;
        out.message = "Vulkan device not initialized";
        return out;
    }
    if (spirv.empty() || input.empty()) {
        out.status = ComputeExecStatus::ResourceFailed;
        out.message = "empty SPIR-V or input";
        return out;
    }

    // Lane geometry. Defaults (0) preserve the compute model: one lane per
    // input element, one output element per lane. The VGT vertex model passes
    // an explicit (smaller) lane count and output count.
    const uint32_t in_elements = static_cast<uint32_t>(input.size());
    const uint32_t lane_count = threads != 0U ? threads : in_elements;
    const uint32_t out_count = out_elements != 0U ? out_elements : in_elements;
    if (lane_count == 0U || lane_count > in_elements || out_count == 0U) {
        out.status = ComputeExecStatus::ResourceFailed;
        out.message = "invalid lane geometry for strided dispatch";
        return out;
    }
    const size_t extra_count = extra != nullptr ? extra->size() : 0u;
    if (push_constant_dwords != 0U && push_constants == nullptr) {
        out.status = ComputeExecStatus::ResourceFailed;
        out.message = "push-constant size without data";
        return out;
    }

    Impl& d = *m_impl;
    const VkDeviceSize in_bytes =
        static_cast<VkDeviceSize>(in_elements) * sizeof(uint32_t);
    const VkDeviceSize out_bytes =
        static_cast<VkDeviceSize>(out_count) * sizeof(uint32_t);

    // --- buffers + device memory --------------------------------------------
    // Round 19: the in/out SSBO pair plus one SSBO per extra binding (the
    // SMEM scalar mirror and the per-descriptor MUBUF buffers) -- the same
    // make_buffer pattern the round-18 path established.
    const size_t total_buffers = 2u + extra_count;
    std::vector<VkBuffer> bufs(total_buffers, VK_NULL_HANDLE);
    std::vector<VkDeviceMemory> mems(total_buffers, VK_NULL_HANDLE);
    auto make_buffer = [&](VkBuffer& b, VkDeviceMemory& m, VkDeviceSize size) -> bool {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = size;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(d.device, &bci, nullptr, &b) != VK_SUCCESS) return false;
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(d.device, b, &mr);
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = d.FindMemoryType(
            mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mai.memoryTypeIndex == UINT32_MAX) return false;
        if (vkAllocateMemory(d.device, &mai, nullptr, &m) != VK_SUCCESS) return false;
        return vkBindBufferMemory(d.device, b, m, 0) == VK_SUCCESS;
    };

    // --- Round 28: image objects -------------------------------------------
    // Per image resource: a 2D RGBA32UI VkImage (SAMPLED|STORAGE usage, the
    // GENERAL layout so both access patterns coexist), one VkImageView used
    // by BOTH descriptor slots, a NEAREST+CLAMP_TO_EDGE sampler (bit-exact
    // with the software executor's sample model) and a staging buffer for
    // upload/download.
    struct ImageState {
        VkImage image{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
        VkSampler sampler{VK_NULL_HANDLE};
        VkBuffer stage_buf{VK_NULL_HANDLE};
        VkDeviceMemory stage_mem{VK_NULL_HANDLE};
        VkDeviceSize stage_bytes{0};
    };
    const size_t image_count = extra_images != nullptr ? extra_images->size() : 0u;
    std::vector<ImageState> img_states;

    auto cleanup = [&]() {
        for (size_t i = 0; i < total_buffers; ++i) {
            if (bufs[i]) vkDestroyBuffer(d.device, bufs[i], nullptr);
            if (mems[i]) vkFreeMemory(d.device, mems[i], nullptr);
        }
        // Round 28: per-image objects (image, memory, view, sampler, staging).
        for (size_t g = 0; g < img_states.size(); ++g) {
            auto& s = img_states[g];
            if (s.view) vkDestroyImageView(d.device, s.view, nullptr);
            if (s.image) vkDestroyImage(d.device, s.image, nullptr);
            if (s.memory) vkFreeMemory(d.device, s.memory, nullptr);
            if (s.sampler) vkDestroySampler(d.device, s.sampler, nullptr);
            if (s.stage_buf) vkDestroyBuffer(d.device, s.stage_buf, nullptr);
            if (s.stage_mem) vkFreeMemory(d.device, s.stage_mem, nullptr);
        }
    };

    auto make_staging = [&](VkBuffer& b, VkDeviceMemory& m,
                            VkDeviceSize size) -> bool {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = size;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(d.device, &bci, nullptr, &b) != VK_SUCCESS) return false;
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(d.device, b, &mr);
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = d.FindMemoryType(
            mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mai.memoryTypeIndex == UINT32_MAX) return false;
        if (vkAllocateMemory(d.device, &mai, nullptr, &m) != VK_SUCCESS) return false;
        return vkBindBufferMemory(d.device, b, m, 0) == VK_SUCCESS;
    };
    for (size_t g = 0; g < image_count; ++g) {
        const SpirvExtraImage& im = (*extra_images)[g];
        const VkDeviceSize texel_bytes =
            static_cast<VkDeviceSize>(im.contents.size()) * sizeof(uint32_t);
        if (im.width == 0u || im.height == 0u || texel_bytes == 0) {
            cleanup();
            out.status = ComputeExecStatus::ResourceFailed;
            out.message = "image resource with empty contents/dims";
            return out;
        }
        ImageState st{};
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R32G32B32A32_UINT;
        ici.extent = {im.width, im.height, 1u};
        ici.mipLevels = im.mips;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_STORAGE_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(d.device, &ici, nullptr, &st.image) != VK_SUCCESS ||
            !make_staging(st.stage_buf, st.stage_mem, texel_bytes)) {
            if (st.image) vkDestroyImage(d.device, st.image, nullptr);
            if (st.stage_buf) vkDestroyBuffer(d.device, st.stage_buf, nullptr);
            if (st.stage_mem) vkFreeMemory(d.device, st.stage_mem, nullptr);
            cleanup();
            out.status = ComputeExecStatus::ResourceFailed;
            out.message = "image/staging allocation failed";
            return out;
        }
        VkMemoryRequirements mr{};
        vkGetImageMemoryRequirements(d.device, st.image, &mr);
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = d.FindMemoryType(mr.memoryTypeBits, 0);
        if (mai.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(d.device, &mai, nullptr, &st.memory) != VK_SUCCESS ||
            vkBindImageMemory(d.device, st.image, st.memory, 0) != VK_SUCCESS) {
            if (st.memory) vkFreeMemory(d.device, st.memory, nullptr);
            vkDestroyImage(d.device, st.image, nullptr);
            vkDestroyBuffer(d.device, st.stage_buf, nullptr);
            vkFreeMemory(d.device, st.stage_mem, nullptr);
            cleanup();
            out.status = ComputeExecStatus::ResourceFailed;
            out.message = "image memory allocation failed";
            return out;
        }
        VkImageViewCreateInfo ivci{};
        ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivci.image = st.image;
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = VK_FORMAT_R32G32B32A32_UINT;
        ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, im.mips, 0, 1};
        if (vkCreateImageView(d.device, &ivci, nullptr, &st.view) !=
            VK_SUCCESS) {
            cleanup();
            out.status = ComputeExecStatus::ResourceFailed;
            out.message = "image view creation failed";
            return out;
        }
        // NEAREST + CLAMP_TO_EDGE: the exact sampler the software model uses.
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_NEAREST;
        sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(d.device, &sci, nullptr, &st.sampler) !=
            VK_SUCCESS) {
            cleanup();
            out.status = ComputeExecStatus::ResourceFailed;
            out.message = "sampler creation failed";
            return out;
        }
        st.stage_bytes = texel_bytes;
        img_states.push_back(st);
    }

    if (!make_buffer(bufs[0], mems[0], in_bytes) ||
        !make_buffer(bufs[1], mems[1], out_bytes)) {
        cleanup();
        out.status = ComputeExecStatus::ResourceFailed;
        out.message = "buffer/memory allocation failed";
        return out;
    }
    for (size_t e = 0; e < extra_count; ++e) {
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(
            (*extra)[e].contents.size()) * sizeof(uint32_t);
        if (bytes == 0 ||
            !make_buffer(bufs[2 + e], mems[2 + e], bytes)) {
            cleanup();
            out.status = ComputeExecStatus::ResourceFailed;
            out.message = "resource SSBO allocation failed";
            return out;
        }
    }

    // upload input, zero output, upload extras
    void* mapped = nullptr;
    vkMapMemory(d.device, mems[0], 0, in_bytes, 0, &mapped);
    std::memcpy(mapped, input.data(), static_cast<size_t>(in_bytes));
    vkUnmapMemory(d.device, mems[0]);
    vkMapMemory(d.device, mems[1], 0, out_bytes, 0, &mapped);
    std::memset(mapped, 0, static_cast<size_t>(out_bytes));
    vkUnmapMemory(d.device, mems[1]);
    for (size_t e = 0; e < extra_count; ++e) {
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(
            (*extra)[e].contents.size()) * sizeof(uint32_t);
        vkMapMemory(d.device, mems[2 + e], 0, bytes, 0, &mapped);
        std::memcpy(mapped, (*extra)[e].contents.data(),
                    static_cast<size_t>(bytes));
        vkUnmapMemory(d.device, mems[2 + e]);
    }
    // Round 28: stage the image texel data (the copy happens in the command
    // buffer below, between the layout transitions).
    for (size_t g = 0; g < img_states.size(); ++g) {
        const VkDeviceSize bytes = img_states[g].stage_bytes;
        vkMapMemory(d.device, img_states[g].stage_mem, 0, bytes, 0, &mapped);
        std::memcpy(mapped, (*extra_images)[g].contents.data(),
                    static_cast<size_t>(bytes));
        vkUnmapMemory(d.device, img_states[g].stage_mem);
    }

    // --- descriptor set layout + pipeline layout ----------------------------
    std::vector<VkDescriptorSetLayoutBinding> bindings(total_buffers);
    for (size_t i = 0; i < total_buffers; ++i) {
        bindings[i] = {static_cast<uint32_t>(i),
                       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    // Round 28: three slots per image -- sampled image, storage image,
    // sampler (the exact layout the compiler's BuildSkeleton decorates).
    for (size_t g = 0; g < img_states.size(); ++g) {
        const uint32_t base = static_cast<uint32_t>(total_buffers + g * 3u);
        bindings.push_back({base + 0u, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                            VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        bindings.push_back({base + 1u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                            VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        bindings.push_back({base + 2u, VK_DESCRIPTOR_TYPE_SAMPLER, 1,
                            VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    }
    // --- round 20: the cross-dispatch pipeline cache ------------------------
    // The key covers every driver-visible input: the SPIR-V word stream, the
    // binding count (the descriptor set layout shape) and the push-constant
    // size. A second dispatch of the same program reuses the objects and
    // skips the entire vkCreate* chain (dsl included -- nothing leaks).
    const PipelineCacheKey cache_key = ComputePipelineKey(
        PipelineKind::Compute, spirv.data(), spirv.size(),
        static_cast<uint32_t>(bindings.size()), push_constant_dwords);
    const bool cache_hit =
        PipelineKeyRegistry::Instance().NoteDispatch(cache_key);
    Impl::CachedComputeObjects cached{};
    bool from_cache = false;
    if (cache_hit) {
        const auto it = d.compute_cache.find(cache_key);
        if (it != d.compute_cache.end()) {
            cached = it->second;
            from_cache = true;
        }
    }

    // Round 19: the push-constant range (the SMEM mirror base block) rides
    // the same pipeline layout when the caller supplies values.
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = push_constant_dwords * sizeof(uint32_t);

    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout playout = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (from_cache) {
        dsl = cached.dsl;
        playout = cached.playout;
        shader = cached.module;
        pipeline = cached.pipeline;
        out.pipeline_cache_hit = true;
    } else {
        VkDescriptorSetLayoutCreateInfo dlci{};
        dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dlci.bindingCount = static_cast<uint32_t>(bindings.size());
        dlci.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(d.device, &dlci, nullptr, &dsl) != VK_SUCCESS) {
            cleanup();
            out.status = ComputeExecStatus::PipelineFailed;
            out.message = "descriptor set layout creation failed";
            return out;
        }
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &dsl;
        plci.pushConstantRangeCount = push_constant_dwords != 0U ? 1u : 0u;
        plci.pPushConstantRanges =
            push_constant_dwords != 0U ? &push_range : nullptr;
        vkCreatePipelineLayout(d.device, &plci, nullptr, &playout);

        // --- shader module + compute pipeline -------------------------------
        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = spirv.size() * sizeof(uint32_t);
        smci.pCode = spirv.data();
        if (vkCreateShaderModule(d.device, &smci, nullptr, &shader) != VK_SUCCESS) {
            vkDestroyPipelineLayout(d.device, playout, nullptr);
            vkDestroyDescriptorSetLayout(d.device, dsl, nullptr);
            cleanup();
            out.status = ComputeExecStatus::PipelineFailed;
            out.message = "vkCreateShaderModule failed";
            return out;
        }
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = shader;
        stage.pName = "main";
        VkComputePipelineCreateInfo cpci{};
        cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpci.stage = stage;
        cpci.layout = playout;
        if (vkCreateComputePipelines(d.device, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline) != VK_SUCCESS) {
            vkDestroyShaderModule(d.device, shader, nullptr);
            vkDestroyPipelineLayout(d.device, playout, nullptr);
            vkDestroyDescriptorSetLayout(d.device, dsl, nullptr);
            cleanup();
            out.status = ComputeExecStatus::PipelineFailed;
            out.message = "vkCreateComputePipelines failed";
            return out;
        }
        // Store the fresh objects in the device cache (they are destroyed
        // with the Impl, not per dispatch).
        Impl::CachedComputeObjects keep{};
        keep.dsl = dsl;
        keep.playout = playout;
        keep.module = shader;
        keep.pipeline = pipeline;
        d.compute_cache.emplace(cache_key, keep);
    }

    // --- descriptor pool + set ----------------------------------------------
    // Round 28: the pool grows the three image descriptor types.
    std::vector<VkDescriptorPoolSize> psizes;
    psizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                      static_cast<uint32_t>(total_buffers)});
    if (!img_states.empty()) {
        psizes.push_back({VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                          static_cast<uint32_t>(img_states.size())});
        psizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                          static_cast<uint32_t>(img_states.size())});
        psizes.push_back({VK_DESCRIPTOR_TYPE_SAMPLER,
                          static_cast<uint32_t>(img_states.size())});
    }
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = static_cast<uint32_t>(psizes.size());
    dpci.pPoolSizes = psizes.data();
    VkDescriptorPool pool = VK_NULL_HANDLE;
    vkCreateDescriptorPool(d.device, &dpci, nullptr, &pool);
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &dsl;
    VkDescriptorSet dset = VK_NULL_HANDLE;
    vkAllocateDescriptorSets(d.device, &dsai, &dset);
    std::vector<VkDescriptorBufferInfo> binfos(total_buffers);
    std::vector<VkWriteDescriptorSet> writes(total_buffers);
    for (size_t i = 0; i < total_buffers; ++i) {
        const VkDeviceSize bytes = i == 0u ? in_bytes
            : i == 1u ? out_bytes
            : static_cast<VkDeviceSize>((*extra)[i - 2u].contents.size()) *
                  sizeof(uint32_t);
        binfos[i] = {bufs[i], 0, bytes};
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = dset;
        writes[i].dstBinding = static_cast<uint32_t>(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &binfos[i];
    }
    // Round 28: the image descriptor writes (sampled view + storage view +
    // sampler, layout GENERAL for both image slots).
    std::vector<VkDescriptorImageInfo> iinfos;
    iinfos.reserve(img_states.size() * 3u);
    for (size_t g = 0; g < img_states.size(); ++g) {
        auto& s = img_states[g];
        const uint32_t base = static_cast<uint32_t>(total_buffers + g * 3u);
        iinfos.push_back({s.sampler, s.view,
                          VK_IMAGE_LAYOUT_GENERAL});
        VkWriteDescriptorSet w_sampled{};
        w_sampled.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w_sampled.dstSet = dset;
        w_sampled.dstBinding = base + 0u;
        w_sampled.descriptorCount = 1;
        w_sampled.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w_sampled.pImageInfo = &iinfos[iinfos.size() - 1u];
        writes.push_back(w_sampled);
        iinfos.push_back({VK_NULL_HANDLE, s.view,
                          VK_IMAGE_LAYOUT_GENERAL});
        VkWriteDescriptorSet w_storage{};
        w_storage.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w_storage.dstSet = dset;
        w_storage.dstBinding = base + 1u;
        w_storage.descriptorCount = 1;
        w_storage.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w_storage.pImageInfo = &iinfos[iinfos.size() - 1u];
        writes.push_back(w_storage);
        iinfos.push_back({s.sampler, VK_NULL_HANDLE,
                          VK_IMAGE_LAYOUT_UNDEFINED});
        VkWriteDescriptorSet w_sampler{};
        w_sampler.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w_sampler.dstSet = dset;
        w_sampler.dstBinding = base + 2u;
        w_sampler.descriptorCount = 1;
        w_sampler.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w_sampler.pImageInfo = &iinfos[iinfos.size() - 1u];
        writes.push_back(w_sampler);
    }
    vkUpdateDescriptorSets(d.device,
                           static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);

    // --- command buffer: bind, push, dispatch, submit ------------------------
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = d.cmd_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(d.device, &cbai, &cmd);
    VkCommandBufferBeginInfo cbbi{};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cbbi);
    // Round 28: upload the staged texel data into every image
    // (UNDEFINED -> TRANSFER_DST -> GENERAL).
    for (size_t g = 0; g < img_states.size(); ++g) {
        auto& s = img_states[g];
        const SpirvExtraImage& im = (*extra_images)[g];
        VkImageMemoryBarrier to_dst{};
        to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_dst.srcAccessMask = 0;
        to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.image = s.image;
        to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &to_dst);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {im.width, im.height, 1u};
        vkCmdCopyBufferToImage(cmd, s.stage_buf, s.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        VkImageMemoryBarrier to_general{};
        to_general.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_general.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_general.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                   VK_ACCESS_SHADER_WRITE_BIT;
        to_general.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_general.image = s.image;
        to_general.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &to_general);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, playout, 0, 1, &dset, 0, nullptr);
    if (push_constant_dwords != 0U) {
        vkCmdPushConstants(cmd, playout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           push_range.size, push_constants);
    }
    const uint32_t groups = (lane_count + local_size_x - 1) / local_size_x;
    vkCmdDispatch(cmd, groups, 1, 1);
    // Round 28: download the (possibly modified) texel data
    // (GENERAL -> TRANSFER_SRC, copy, host barrier) in the SAME command
    // buffer so MIMG stores/atomics land in guest memory.
    for (size_t g = 0; g < img_states.size(); ++g) {
        auto& s = img_states[g];
        const SpirvExtraImage& im = (*extra_images)[g];
        VkImageMemoryBarrier to_src{};
        to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_src.srcAccessMask = VK_ACCESS_SHADER_READ_BIT |
                               VK_ACCESS_SHADER_WRITE_BIT;
        to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        to_src.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_src.image = s.image;
        to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &to_src);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {im.width, im.height, 1u};
        vkCmdCopyImageToBuffer(cmd, s.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.stage_buf,
                               1, &copy);
        VkMemoryBarrier to_host{};
        to_host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0,
                             nullptr, 0, nullptr);
    }
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VkResult submit = vkQueueSubmit(d.queue, 1, &si, VK_NULL_HANDLE);
    if (submit == VK_SUCCESS) submit = vkQueueWaitIdle(d.queue);

    if (submit == VK_SUCCESS) {
        out.output.resize(out_count);
        vkMapMemory(d.device, mems[1], 0, out_bytes, 0, &mapped);
        std::memcpy(out.output.data(), mapped, static_cast<size_t>(out_bytes));
        vkUnmapMemory(d.device, mems[1]);
        // Round 19: read back the resource SSBOs the caller wants (MUBUF
        // stores land in the per-descriptor buffers).
        for (size_t e = 0; e < extra_count; ++e) {
            if (!(*extra)[e].read_back) continue;
            const VkDeviceSize bytes = static_cast<VkDeviceSize>(
                (*extra)[e].contents.size()) * sizeof(uint32_t);
            vkMapMemory(d.device, mems[2 + e], 0, bytes, 0, &mapped);
            std::memcpy((*extra)[e].contents.data(), mapped,
                        static_cast<size_t>(bytes));
            vkUnmapMemory(d.device, mems[2 + e]);
        }
        // Round 28: read the staged texel data back into the image structs.
        for (size_t g = 0; g < img_states.size(); ++g) {
            if (!(*extra_images)[g].read_back) continue;
            const VkDeviceSize bytes = img_states[g].stage_bytes;
            vkMapMemory(d.device, img_states[g].stage_mem, 0, bytes, 0,
                        &mapped);
            std::memcpy((*extra_images)[g].contents.data(), mapped,
                        static_cast<size_t>(bytes));
            vkUnmapMemory(d.device, img_states[g].stage_mem);
        }
        out.status = ComputeExecStatus::Ok;
        out.hardware = true;
        out.message = "compute dispatch executed on " + m_device_name;
    } else {
        out.status = ComputeExecStatus::DispatchFailed;
        out.message = "queue submit/wait failed";
    }

    // --- teardown of per-dispatch objects -----------------------------------
    // Round 20: the pipeline objects now LIVE IN THE CACHE (destroyed with
    // the device) -- only the per-dispatch resources go away here.
    vkFreeCommandBuffers(d.device, d.cmd_pool, 1, &cmd);
    vkDestroyDescriptorPool(d.device, pool, nullptr);
    cleanup();
    return out;
}

ImageOpResult VulkanComputeExecutor::ClearImage(uint32_t width, uint32_t height,
                                                float r, float g, float b, float a) {
    ImageOpResult out;
    out.width = width;
    out.height = height;
    if (!m_ready) {
        out.status = ComputeExecStatus::Unavailable;
        out.message = "Vulkan device not initialized";
        return out;
    }
    if (width == 0 || height == 0) {
        out.status = ComputeExecStatus::ResourceFailed;
        out.message = "zero image extent";
        return out;
    }

    Impl& d = *m_impl;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4;

    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_mem = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;

    auto teardown = [&]() {
        if (cmd)         vkFreeCommandBuffers(d.device, d.cmd_pool, 1, &cmd);
        if (staging)     vkDestroyBuffer(d.device, staging, nullptr);
        if (staging_mem) vkFreeMemory(d.device, staging_mem, nullptr);
        if (image)       vkDestroyImage(d.device, image, nullptr);
        if (image_mem)   vkFreeMemory(d.device, image_mem, nullptr);
    };

    // --- device-local 2D image ----------------------------------------------
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(d.device, &ici, nullptr, &image) != VK_SUCCESS) {
        out.status = ComputeExecStatus::ResourceFailed;
        out.message = "vkCreateImage failed";
        return out;
    }
    VkMemoryRequirements imr{};
    vkGetImageMemoryRequirements(d.device, image, &imr);
    VkMemoryAllocateInfo imai{};
    imai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    imai.allocationSize = imr.size;
    imai.memoryTypeIndex = d.FindMemoryType(imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (imai.memoryTypeIndex == UINT32_MAX) {
        imai.memoryTypeIndex = d.FindMemoryType(imr.memoryTypeBits, 0);
    }
    if (imai.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(d.device, &imai, nullptr, &image_mem) != VK_SUCCESS ||
        vkBindImageMemory(d.device, image, image_mem, 0) != VK_SUCCESS) {
        teardown();
        out.status = ComputeExecStatus::ResourceFailed;
        out.message = "image memory allocation failed";
        return out;
    }

    // --- host-visible staging buffer ----------------------------------------
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d.device, &bci, nullptr, &staging) != VK_SUCCESS) {
        teardown();
        out.status = ComputeExecStatus::ResourceFailed;
        out.message = "staging buffer creation failed";
        return out;
    }
    VkMemoryRequirements bmr{};
    vkGetBufferMemoryRequirements(d.device, staging, &bmr);
    VkMemoryAllocateInfo bmai{};
    bmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bmai.allocationSize = bmr.size;
    bmai.memoryTypeIndex = d.FindMemoryType(
        bmr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (bmai.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(d.device, &bmai, nullptr, &staging_mem) != VK_SUCCESS ||
        vkBindBufferMemory(d.device, staging, staging_mem, 0) != VK_SUCCESS) {
        teardown();
        out.status = ComputeExecStatus::ResourceFailed;
        out.message = "staging memory allocation failed";
        return out;
    }

    // --- record: barrier -> clear -> barrier -> copy ------------------------
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = d.cmd_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    vkAllocateCommandBuffers(d.device, &cbai, &cmd);
    VkCommandBufferBeginInfo cbbi{};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cbbi);

    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier to_dst{};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = image;
    to_dst.subresourceRange = range;
    to_dst.srcAccessMask = 0;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_dst);

    VkClearColorValue color{};
    color.float32[0] = r; color.float32[1] = g; color.float32[2] = b; color.float32[3] = a;
    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);

    VkImageMemoryBarrier to_src = to_dst;
    to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_src);

    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &copy);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VkResult submit = vkQueueSubmit(d.queue, 1, &si, VK_NULL_HANDLE);
    if (submit == VK_SUCCESS) submit = vkQueueWaitIdle(d.queue);

    if (submit == VK_SUCCESS) {
        out.pixels.resize(static_cast<size_t>(bytes));
        void* mapped = nullptr;
        vkMapMemory(d.device, staging_mem, 0, bytes, 0, &mapped);
        std::memcpy(out.pixels.data(), mapped, static_cast<size_t>(bytes));
        vkUnmapMemory(d.device, staging_mem);
        out.status = ComputeExecStatus::Ok;
        out.hardware = true;
        out.message = "image cleared and read back on " + m_device_name;
    } else {
        out.status = ComputeExecStatus::DispatchFailed;
        out.message = "queue submit/wait failed";
    }

    teardown();
    return out;
}

// ---------------------------------------------------------------------------
// Round 19 (phase 2): the REAL VkGraphicsPipeline raster path.
// ---------------------------------------------------------------------------
VulkanComputeExecutor::GraphicsRasterResult VulkanComputeExecutor::DrawVerticesToGuest(
        const uint32_t* rdna2_code, size_t rdna2_dwords,
        const std::vector<uint32_t>& input,
        uint32_t in_dwords_per_lane, uint32_t out_dwords_per_lane,
        uint32_t draw_vertex_count,
        const GcnDispatchResources& resources,
        const GraphicsTargetDesc& target,
        uint64_t color_gva, uint64_t depth_gva,
        GpuGuestMemory* mem) {
    GraphicsRasterResult out;
    out.device_name = m_device_name;

    // ---- fail-closed gates (every one keeps the software rasterizer) ------
    if (!m_ready || !m_graphics_queue) {
        out.status = ComputeExecStatus::Unavailable;
        out.message = m_ready
            ? "device has no graphics-capable queue"
            : "Vulkan device not initialized";
        return out;
    }
    if (mem == nullptr || rdna2_code == nullptr || rdna2_dwords == 0 ||
        input.empty() || draw_vertex_count == 0U ||
        in_dwords_per_lane == 0U || out_dwords_per_lane == 0U ||
        input.size() / in_dwords_per_lane < draw_vertex_count) {
        out.status = ComputeExecStatus::ResourceFailed;
        out.message = "invalid draw parameters for the graphics path";
        return out;
    }
    if (target.width == 0U || target.height == 0U ||
        target.color_format == Pm4::GuestColorFormat::Invalid ||
        !target.color_write || color_gva == 0) {
        out.status = ComputeExecStatus::ResourceFailed;
        out.message = "unsupported render-target binding for the graphics path";
        return out;
    }
    if (target.depth_enabled && depth_gva == 0) {
        out.status = ComputeExecStatus::ResourceFailed;
        out.message = "depth enabled without a depth plane";
        return out;
    }

    Impl& d = *m_impl;
    const uint32_t w = target.width;
    const uint32_t h = target.height;
    const VkFormat color_format =
        static_cast<VkFormat>(static_cast<uint32_t>(target.color_format));
    // Round 20: the colour plane stride follows the CB format (8 bytes per
    // pixel for the 16-bit-per-channel layouts); the depth plane is always
    // one float32 per pixel.
    const uint32_t color_bpp =
        Pm4::GuestColorFormatBytesPerPixel(target.color_format);
    const VkDeviceSize plane_bytes =
        static_cast<VkDeviceSize>(w) * h * color_bpp;
    const VkDeviceSize depth_plane_bytes =
        static_cast<VkDeviceSize>(w) * h * 4;

    // ---- guest plane upload sources (merge semantics: LOAD, not CLEAR) ----
    std::vector<uint8_t> color_plane(static_cast<size_t>(plane_bytes), 0u);
    std::vector<uint32_t> depth_plane;
    if (!mem->ReadDwords(color_gva,
                         reinterpret_cast<uint32_t*>(color_plane.data()),
                         color_plane.size() / 4)) {
        out.status = ComputeExecStatus::ResourceFailed;
        out.message = "guest colour plane unreadable";
        return out;
    }
    if (target.depth_enabled) {
        depth_plane.resize(static_cast<size_t>(w) * h, 0u);
        if (!mem->ReadDwords(depth_gva, depth_plane.data(), depth_plane.size())) {
            out.status = ComputeExecStatus::ResourceFailed;
            out.message = "guest depth plane unreadable";
            return out;
        }
    }

    // ---- resource staging (same as the compute path) -----------------------
    std::vector<std::vector<uint32_t>> buffer_contents;
    std::vector<uint32_t> mirror_contents;
    if (!LoadResourceContents(resources, mem, buffer_contents,
                              mirror_contents)) {
        out.status = ComputeExecStatus::ResourceFailed;
        out.message = "resource table staging failed";
        return out;
    }

    // ---- compile the guest VS into a VERTEX-stage module -------------------
    ComputeCompilerOptions copt;
    copt.in_dwords_per_lane = in_dwords_per_lane;
    copt.out_dwords_per_lane = out_dwords_per_lane;
    copt.buffers = resources.buffers;
    copt.scalar_mirror_base_gva = resources.scalar_mirror_base_gva;
    copt.emit_vertex_stage = true;
    RDNA2ComputeCompiler compiler(copt);
    auto vs = compiler.Compile(rdna2_code, rdna2_dwords);
    if (!vs) {
        out.status = ComputeExecStatus::CompileFailed;
        out.message = "vertex-stage compile failed at dword " +
                      std::to_string(vs.error_dword) + ": " + vs.message;
        return out;
    }
    const std::vector<uint32_t> fs =
        RDNA2ComputeCompiler::BuildPassthroughFragmentShader();

    // ---- object handles + teardown -----------------------------------------
    VkImage color_image = VK_NULL_HANDLE, depth_image = VK_NULL_HANDLE;
    VkDeviceMemory color_mem = VK_NULL_HANDLE, depth_mem = VK_NULL_HANDLE;
    VkImageView color_view = VK_NULL_HANDLE, depth_view = VK_NULL_HANDLE;
    VkBuffer color_stage = VK_NULL_HANDLE, depth_stage = VK_NULL_HANDLE;
    VkDeviceMemory color_stage_mem = VK_NULL_HANDLE, depth_stage_mem = VK_NULL_HANDLE;
    std::vector<VkBuffer> bufs;        // 0 in / 1 out / 2 mirror / 3+ desc
    std::vector<VkDeviceMemory> mems;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout playout = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkShaderModule vs_module = VK_NULL_HANDLE, fs_module = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet dset = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;

    // Round 20: the cross-dispatch pipeline cache. The graphics key covers
    // the VS SPIR-V, the FS SPIR-V, the binding count, the colour format,
    // ZFUNC and the depth binding -- everything the cached objects depend
    // on. The per-target objects (image/view/framebuffer/staging/pool/SSBOs)
    // stay per-dispatch.
    PipelineCacheKey gfx_key{};
    bool gfx_from_cache = false;
    VkPushConstantRange push_range{};   // { base_lo, base_hi } -- vertex stage

    auto teardown = [&]() {
        if (cmd) vkFreeCommandBuffers(d.device, d.cmd_pool, 1, &cmd);
        if (pool) vkDestroyDescriptorPool(d.device, pool, nullptr);
        if (!gfx_from_cache) {
            if (pipeline) vkDestroyPipeline(d.device, pipeline, nullptr);
            if (vs_module) vkDestroyShaderModule(d.device, vs_module, nullptr);
            if (fs_module) vkDestroyShaderModule(d.device, fs_module, nullptr);
            if (pass) vkDestroyRenderPass(d.device, pass, nullptr);
            if (playout) vkDestroyPipelineLayout(d.device, playout, nullptr);
            if (dsl) vkDestroyDescriptorSetLayout(d.device, dsl, nullptr);
        }
        for (size_t i = 0; i < bufs.size(); ++i) {
            if (bufs[i]) vkDestroyBuffer(d.device, bufs[i], nullptr);
            if (mems[i]) vkFreeMemory(d.device, mems[i], nullptr);
        }
        if (depth_stage) vkDestroyBuffer(d.device, depth_stage, nullptr);
        if (depth_stage_mem) vkFreeMemory(d.device, depth_stage_mem, nullptr);
        if (color_stage) vkDestroyBuffer(d.device, color_stage, nullptr);
        if (color_stage_mem) vkFreeMemory(d.device, color_stage_mem, nullptr);
        if (depth_view) vkDestroyImageView(d.device, depth_view, nullptr);
        if (color_view) vkDestroyImageView(d.device, color_view, nullptr);
        if (depth_image) vkDestroyImage(d.device, depth_image, nullptr);
        if (depth_mem) vkFreeMemory(d.device, depth_mem, nullptr);
        if (color_image) vkDestroyImage(d.device, color_image, nullptr);
        if (color_mem) vkFreeMemory(d.device, color_mem, nullptr);
    };
    auto fail = [&](ComputeExecStatus status, const char* message) {
        teardown();
        out.status = status;
        out.message = message;
        return out;
    };

    auto make_image = [&](VkImage& img, VkDeviceMemory& img_mem,
                          VkFormat format, VkImageUsageFlags usage) -> bool {
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = format;
        ici.extent = {w, h, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = usage;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(d.device, &ici, nullptr, &img) != VK_SUCCESS) {
            return false;
        }
        VkMemoryRequirements imr{};
        vkGetImageMemoryRequirements(d.device, img, &imr);
        VkMemoryAllocateInfo imai{};
        imai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        imai.allocationSize = imr.size;
        imai.memoryTypeIndex = d.FindMemoryType(imr.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (imai.memoryTypeIndex == UINT32_MAX) {
            imai.memoryTypeIndex = d.FindMemoryType(imr.memoryTypeBits, 0);
        }
        if (imai.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(d.device, &imai, nullptr, &img_mem) != VK_SUCCESS ||
            vkBindImageMemory(d.device, img, img_mem, 0) != VK_SUCCESS) {
            return false;
        }
        return true;
    };
    auto make_stage = [&](VkBuffer& b, VkDeviceMemory& m,
                          VkDeviceSize bytes) -> bool {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(d.device, &bci, nullptr, &b) != VK_SUCCESS) {
            return false;
        }
        VkMemoryRequirements bmr{};
        vkGetBufferMemoryRequirements(d.device, b, &bmr);
        VkMemoryAllocateInfo bmai{};
        bmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        bmai.allocationSize = bmr.size;
        bmai.memoryTypeIndex = d.FindMemoryType(
            bmr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (bmai.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(d.device, &bmai, nullptr, &m) != VK_SUCCESS ||
            vkBindBufferMemory(d.device, b, m, 0) != VK_SUCCESS) {
            return false;
        }
        return true;
    };
    auto make_ssbo = [&](VkDeviceSize bytes) -> size_t {
        const size_t idx = bufs.size();
        bufs.push_back(VK_NULL_HANDLE);
        mems.push_back(VK_NULL_HANDLE);
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(d.device, &bci, nullptr, &bufs[idx]) != VK_SUCCESS) {
            return SIZE_MAX;
        }
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(d.device, bufs[idx], &mr);
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = d.FindMemoryType(
            mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mai.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(d.device, &mai, nullptr, &mems[idx]) != VK_SUCCESS ||
            vkBindBufferMemory(d.device, bufs[idx], mems[idx], 0) != VK_SUCCESS) {
            return SIZE_MAX;
        }
        return idx;
    };

    // ---- images + views + staging -------------------------------------------
    const VkImageUsageFlags color_usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!make_image(color_image, color_mem, color_format, color_usage)) {
        return fail(ComputeExecStatus::ResourceFailed, "colour image creation failed");
    }
    VkImageViewCreateInfo cvci{};
    cvci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    cvci.image = color_image;
    cvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    cvci.format = color_format;
    cvci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(d.device, &cvci, nullptr, &color_view) != VK_SUCCESS) {
        return fail(ComputeExecStatus::ResourceFailed, "colour image view failed");
    }
    if (target.depth_enabled) {
        const VkImageUsageFlags depth_usage =
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!make_image(depth_image, depth_mem, VK_FORMAT_D32_SFLOAT,
                        depth_usage)) {
            return fail(ComputeExecStatus::ResourceFailed, "depth image creation failed");
        }
        VkImageViewCreateInfo dvci{};
        dvci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        dvci.image = depth_image;
        dvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        dvci.format = VK_FORMAT_D32_SFLOAT;
        dvci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(d.device, &dvci, nullptr, &depth_view) != VK_SUCCESS) {
            return fail(ComputeExecStatus::ResourceFailed, "depth image view failed");
        }
    }
    if (!make_stage(color_stage, color_stage_mem, plane_bytes)) {
        return fail(ComputeExecStatus::ResourceFailed, "colour staging failed");
    }
    if (target.depth_enabled &&
        !make_stage(depth_stage, depth_stage_mem, depth_plane_bytes)) {
        return fail(ComputeExecStatus::ResourceFailed, "depth staging failed");
    }

    // ---- SSBOs: in / out / (mirror) / (per-descriptor) ----------------------
    const size_t in_idx = make_ssbo(static_cast<VkDeviceSize>(input.size()) * 4);
    const size_t out_idx = make_ssbo(static_cast<VkDeviceSize>(draw_vertex_count) *
                                     out_dwords_per_lane * 4);
    const size_t mirror_idx = vs.used_scalar_mirror
        ? make_ssbo(static_cast<VkDeviceSize>(mirror_contents.size()) * 4)
        : SIZE_MAX;
    std::vector<size_t> buf_idx(buffer_contents.size(), SIZE_MAX);
    for (size_t b = 0; b < buffer_contents.size(); ++b) {
        buf_idx[b] = make_ssbo(static_cast<VkDeviceSize>(
            buffer_contents[b].size()) * 4);
    }
    if (in_idx == SIZE_MAX || out_idx == SIZE_MAX ||
        (vs.used_scalar_mirror && mirror_idx == SIZE_MAX)) {
        return fail(ComputeExecStatus::ResourceFailed, "vertex SSBO allocation failed");
    }
    for (size_t b = 0; b < buf_idx.size(); ++b) {
        if (buf_idx[b] == SIZE_MAX) {
            return fail(ComputeExecStatus::ResourceFailed, "resource SSBO allocation failed");
        }
    }

    // upload SSBO contents
    {
        void* mapped = nullptr;
        vkMapMemory(d.device, mems[in_idx], 0, VK_WHOLE_SIZE, 0, &mapped);
        std::memcpy(mapped, input.data(), input.size() * 4);
        vkUnmapMemory(d.device, mems[in_idx]);
        const VkDeviceSize out_bytes = static_cast<VkDeviceSize>(
            draw_vertex_count) * out_dwords_per_lane * 4;
        vkMapMemory(d.device, mems[out_idx], 0, out_bytes, 0, &mapped);
        std::memset(mapped, 0, static_cast<size_t>(out_bytes));
        vkUnmapMemory(d.device, mems[out_idx]);
        if (mirror_idx != SIZE_MAX && !mirror_contents.empty()) {
            vkMapMemory(d.device, mems[mirror_idx], 0, VK_WHOLE_SIZE, 0, &mapped);
            std::memcpy(mapped, mirror_contents.data(),
                        mirror_contents.size() * 4);
            vkUnmapMemory(d.device, mems[mirror_idx]);
        }
        for (size_t b = 0; b < buffer_contents.size(); ++b) {
            if (buffer_contents[b].empty()) continue;
            vkMapMemory(d.device, mems[buf_idx[b]], 0, VK_WHOLE_SIZE, 0, &mapped);
            std::memcpy(mapped, buffer_contents[b].data(),
                        buffer_contents[b].size() * 4);
            vkUnmapMemory(d.device, mems[buf_idx[b]]);
        }
    }

    // ---- descriptor set layout + pipeline layout (stage = VERTEX) ----------
    // Binding order matches BuildSkeleton exactly: in=0, out=1, mirror=2
    // (when used), descriptor SSBOs after it.
    const size_t total_bindings = 2u + (mirror_idx != SIZE_MAX ? 1u : 0u) +
                                  buffer_contents.size();

    // Round 20: cache key + lookup BEFORE any object creation.
    {
        const uint32_t aux =
            static_cast<uint32_t>(target.color_format) |
            (static_cast<uint32_t>(target.zfunc) << 8) |
            (target.depth_enabled ? 0x1000u : 0u) |
            (target.depth_write ? 0x2000u : 0u);
        gfx_key = ComputePipelineKey(PipelineKind::Graphics,
                                     vs.spirv.data(), vs.spirv.size(),
                                     static_cast<uint32_t>(total_bindings),
                                     2u, aux);
        // The FS words ride the key too (folded through the same hash by a
        // second pass; the key already mixes the VS stream).
        {
            PipelineCacheKey fs_key = ComputePipelineKey(
                PipelineKind::Graphics, fs.data(), fs.size(),
                static_cast<uint32_t>(total_bindings), 2u, aux);
            gfx_key.lo ^= fs_key.lo * 0x9E3779B97F4A7C15ull;
            gfx_key.hi ^= fs_key.hi * 0xC2B2AE3D27D4EB4Full;
        }
        const bool hit = PipelineKeyRegistry::Instance().NoteDispatch(gfx_key);
        if (hit) {
            const auto it = d.graphics_cache.find(gfx_key);
            if (it != d.graphics_cache.end()) {
                dsl = it->second.dsl;
                playout = it->second.playout;
                pass = it->second.render_pass;
                vs_module = it->second.vs_module;
                fs_module = it->second.fs_module;
                pipeline = it->second.pipeline;
                gfx_from_cache = true;
                out.pipeline_cache_hit = true;
            }
        }
    }
    if (!gfx_from_cache) {
    std::vector<VkDescriptorSetLayoutBinding> dslb(total_bindings);
    uint32_t next_binding = 0;
    auto next_slot = [&]() -> VkDescriptorSetLayoutBinding& {
        return dslb[next_binding++];
    };
    {
        auto& b = next_slot();
        b = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
        auto& b2 = next_slot();
        b2 = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
        if (mirror_idx != SIZE_MAX) {
            auto& bm = next_slot();
            bm = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
        }
        for (size_t b = 0; b < buffer_contents.size(); ++b) {
            auto& bd = next_slot();
            bd = {static_cast<uint32_t>(2u + (mirror_idx != SIZE_MAX ? 1u : 0u) + b),
                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                  VK_SHADER_STAGE_VERTEX_BIT, nullptr};
        }
    }
    VkDescriptorSetLayoutCreateInfo dlci{};
    dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlci.bindingCount = static_cast<uint32_t>(dslb.size());
    dlci.pBindings = dslb.data();
    if (vkCreateDescriptorSetLayout(d.device, &dlci, nullptr, &dsl) != VK_SUCCESS) {
        return fail(ComputeExecStatus::PipelineFailed, "descriptor set layout failed");
    }
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.offset = 0;
    push_range.size = 2 * sizeof(uint32_t);
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;
    plci.pushConstantRangeCount = mirror_idx != SIZE_MAX ? 1u : 0u;
    plci.pPushConstantRanges = mirror_idx != SIZE_MAX ? &push_range : nullptr;
    if (vkCreatePipelineLayout(d.device, &plci, nullptr, &playout) != VK_SUCCESS) {
        return fail(ComputeExecStatus::PipelineFailed, "pipeline layout failed");
    }

    // ---- render pass + framebuffer ------------------------------------------
    VkAttachmentDescription attachments[2]{};
    uint32_t attachment_count = 0;
    attachments[attachment_count].format = color_format;
    attachments[attachment_count].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[attachment_count].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[attachment_count].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[attachment_count].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[attachment_count].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[attachment_count].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[attachment_count].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ++attachment_count;
    if (target.depth_enabled) {
        attachments[attachment_count].format = VK_FORMAT_D32_SFLOAT;
        attachments[attachment_count].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[attachment_count].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[attachment_count].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[attachment_count].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[attachment_count].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[attachment_count].initialLayout =
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments[attachment_count].finalLayout =
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        ++attachment_count;
    }
    VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    subpass.pDepthStencilAttachment =
        target.depth_enabled ? &depth_ref : nullptr;
    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = attachment_count;
    rpci.pAttachments = attachments;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    if (vkCreateRenderPass(d.device, &rpci, nullptr, &pass) != VK_SUCCESS) {
        return fail(ComputeExecStatus::PipelineFailed, "render pass creation failed");
    }
    // (framebuffer: created per dispatch -- it binds THIS dispatch's image
    // views; on the cache-miss path right here, on the cache-hit path in the
    // else branch above/below with the cached render pass)
    if (!gfx_from_cache) {
        VkImageView views[2] = {color_view, depth_view};
        VkFramebufferCreateInfo fbci{};
        fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbci.renderPass = pass;
        fbci.attachmentCount = attachment_count;
        fbci.pAttachments = views;
        fbci.width = w;
        fbci.height = h;
        fbci.layers = 1;
        if (vkCreateFramebuffer(d.device, &fbci, nullptr, &fb) != VK_SUCCESS) {
            return fail(ComputeExecStatus::PipelineFailed,
                        "framebuffer creation failed");
        }
    }

    // ---- graphics pipeline ----------------------------------------------------
    VkShaderModuleCreateInfo vsmci{};
    vsmci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vsmci.codeSize = vs.spirv.size() * sizeof(uint32_t);
    vsmci.pCode = vs.spirv.data();
    VkShaderModuleCreateInfo fsmci{};
    fsmci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fsmci.codeSize = fs.size() * sizeof(uint32_t);
    fsmci.pCode = fs.data();
    if (vkCreateShaderModule(d.device, &vsmci, nullptr, &vs_module) != VK_SUCCESS ||
        vkCreateShaderModule(d.device, &fsmci, nullptr, &fs_module) != VK_SUCCESS) {
        return fail(ComputeExecStatus::PipelineFailed, "shader module creation failed");
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs_module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs_module;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    // No vertex bindings: the guest VS fetches its attributes from the in
    // SSBO by gl_VertexIndex (the same lane model as the compute dispatch).
    VkPipelineInputAssemblyStateCreateInfo input_asm{};
    input_asm.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_asm.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;        // the SW rasterizer never culls
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = target.depth_enabled ? VK_TRUE : VK_FALSE;
    depth_stencil.depthWriteEnable =
        (target.depth_enabled && target.depth_write) ? VK_TRUE : VK_FALSE;
    depth_stencil.depthCompareOp =
        static_cast<VkCompareOp>(Pm4::ZFuncToVkCompareOp(target.zfunc));
    depth_stencil.minDepthBounds = 0.0f;
    depth_stencil.maxDepthBounds = 1.0f;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.blendEnable = VK_FALSE;    // the SW rasterizer has no blend
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                      VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT |
                                      VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                          VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dyn_states;

    VkGraphicsPipelineCreateInfo gpci{};
    gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vertex_input;
    gpci.pInputAssemblyState = &input_asm;
    gpci.pViewportState = &viewport_state;
    gpci.pRasterizationState = &raster;
    gpci.pMultisampleState = &multisample;
    gpci.pDepthStencilState = &depth_stencil;
    gpci.pColorBlendState = &blend;
    gpci.pDynamicState = &dynamic;
    gpci.layout = playout;
    gpci.renderPass = pass;
    gpci.subpass = 0;
        if (vkCreateGraphicsPipelines(d.device, VK_NULL_HANDLE, 1, &gpci, nullptr,
                                      &pipeline) != VK_SUCCESS) {
            return fail(ComputeExecStatus::PipelineFailed,
                        "vkCreateGraphicsPipelines failed");
        }
        // Everything the pipeline depends on is now built: store the whole
        // chain in the device cache.
        Impl::CachedGraphicsObjects keep{};
        keep.dsl = dsl;
        keep.playout = playout;
        keep.vs_module = vs_module;
        keep.fs_module = fs_module;
        keep.render_pass = pass;
        keep.pipeline = pipeline;
        d.graphics_cache.emplace(gfx_key, keep);
    } else {
        // Cached: only the FRAMEBUFFER is rebuilt (it binds THIS dispatch's
        // image views to the cached render pass).
        VkImageView views[2] = {color_view, depth_view};
        VkFramebufferCreateInfo fbci{};
        fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbci.renderPass = pass;
        fbci.attachmentCount = target.depth_enabled ? 2u : 1u;
        fbci.pAttachments = views;
        fbci.width = w;
        fbci.height = h;
        fbci.layers = 1;
        if (vkCreateFramebuffer(d.device, &fbci, nullptr, &fb) != VK_SUCCESS) {
            return fail(ComputeExecStatus::PipelineFailed,
                        "framebuffer creation failed");
        }
    }

    // ---- descriptor set ------------------------------------------------------
    VkDescriptorPoolSize psize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                               static_cast<uint32_t>(total_bindings)};
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &psize;
    if (vkCreateDescriptorPool(d.device, &dpci, nullptr, &pool) != VK_SUCCESS) {
        return fail(ComputeExecStatus::ResourceFailed, "descriptor pool failed");
    }
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &dsl;
    if (vkAllocateDescriptorSets(d.device, &dsai, &dset) != VK_SUCCESS) {
        return fail(ComputeExecStatus::ResourceFailed, "descriptor set failed");
    }
    std::vector<VkDescriptorBufferInfo> binfos;
    std::vector<VkWriteDescriptorSet> dwrites;
    auto push_write = [&](uint32_t binding, VkBuffer buffer, VkDeviceSize bytes) {
        binfos.push_back({buffer, 0, bytes});
        VkWriteDescriptorSet wr{};
        wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr.dstSet = dset;
        wr.dstBinding = binding;
        wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        wr.pBufferInfo = &binfos.back();
        dwrites.push_back(wr);
    };
    push_write(0, bufs[in_idx],
               static_cast<VkDeviceSize>(input.size()) * 4);
    push_write(1, bufs[out_idx],
               static_cast<VkDeviceSize>(draw_vertex_count) *
                   out_dwords_per_lane * 4);
    if (mirror_idx != SIZE_MAX) {
        push_write(2, bufs[mirror_idx],
                   static_cast<VkDeviceSize>(mirror_contents.size()) * 4);
    }
    for (size_t b = 0; b < buffer_contents.size(); ++b) {
        push_write(static_cast<uint32_t>(2u + (mirror_idx != SIZE_MAX ? 1u : 0u) + b),
                   bufs[buf_idx[b]],
                   static_cast<VkDeviceSize>(buffer_contents[b].size()) * 4);
    }
    vkUpdateDescriptorSets(d.device, static_cast<uint32_t>(dwrites.size()),
                           dwrites.data(), 0, nullptr);

    // ---- record ---------------------------------------------------------------
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = d.cmd_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(d.device, &cbai, &cmd) != VK_SUCCESS) {
        return fail(ComputeExecStatus::DispatchFailed, "command buffer failed");
    }
    VkCommandBufferBeginInfo cbbi{};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cbbi);

    // Upload the guest planes (LOAD semantics): buffer -> image.
    {
        void* mapped = nullptr;
        vkMapMemory(d.device, color_stage_mem, 0, plane_bytes, 0, &mapped);
        std::memcpy(mapped, color_plane.data(), static_cast<size_t>(plane_bytes));
        vkUnmapMemory(d.device, color_stage_mem);
        if (target.depth_enabled) {
            vkMapMemory(d.device, depth_stage_mem, 0, depth_plane_bytes, 0,
                        &mapped);
            std::memcpy(mapped, depth_plane.data(),
                        static_cast<size_t>(depth_plane_bytes));
            vkUnmapMemory(d.device, depth_stage_mem);
        }
    }
    auto image_barrier = [&](VkImage image, VkImageAspectFlags aspect,
                             VkImageLayout old_layout,
                             VkImageLayout new_layout,
                             VkAccessFlags src_access,
                             VkAccessFlags dst_access) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.oldLayout = old_layout;
        barrier.newLayout = new_layout;
        barrier.image = image;
        barrier.subresourceRange = {aspect, 0, 1, 0, 1};
        barrier.srcAccessMask = src_access;
        barrier.dstAccessMask = dst_access;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
    };
    {
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {w, h, 1};
        // UNDEFINED -> TRANSFER_DST (special barrier with TOP_OF_PIPE src).
        VkImageMemoryBarrier to_dst{};
        to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_dst.image = color_image;
        to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &to_dst);
        vkCmdCopyBufferToImage(cmd, color_stage, color_image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        image_barrier(color_image, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        if (target.depth_enabled) {
            VkImageMemoryBarrier to_dst_d = to_dst;
            to_dst_d.image = depth_image;
            to_dst_d.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                 0, nullptr, 1, &to_dst_d);
            VkBufferImageCopy dcopy{};
            dcopy.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
            dcopy.imageExtent = {w, h, 1};
            vkCmdCopyBufferToImage(cmd, depth_stage, depth_image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &dcopy);
            image_barrier(depth_image, VK_IMAGE_ASPECT_DEPTH_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                          VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        }
    }

    // Render pass: the guest VS as the VERTEX stage + passthrough FS, the
    // dynamic viewport mirroring the PA_CL_VPORT transform exactly:
    //   screen = ndc * scale + offset  <=>  x = off - scale, size = 2*scale
    // (a negative guest YSCALE becomes a negative viewport height -- the
    // Vulkan 1.1 y-flip idiom, matching the top-left-origin guest).
    VkRenderPassBeginInfo rpbi{};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = pass;
    rpbi.framebuffer = fb;
    rpbi.renderArea = {{0, 0}, {w, h}};
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, 1,
                            &dset, 0, nullptr);
    if (mirror_idx != SIZE_MAX) {
        const uint32_t mirror_base[2] = {
            static_cast<uint32_t>(resources.scalar_mirror_base_gva),
            static_cast<uint32_t>(resources.scalar_mirror_base_gva >> 32)};
        vkCmdPushConstants(cmd, playout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           push_range.size, mirror_base);
    }
    VkViewport viewport{};
    viewport.x = target.vport_off_x - target.vport_scale_x;
    viewport.y = target.vport_off_y - target.vport_scale_y;
    viewport.width = 2.0f * target.vport_scale_x;
    viewport.height = 2.0f * target.vport_scale_y;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {w, h};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdDraw(cmd, draw_vertex_count, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    // Read the planes back: attachment -> TRANSFER_SRC -> staging buffer.
    {
        image_barrier(color_image, VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cmd, color_image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               color_stage, 1, &copy);
        if (target.depth_enabled) {
            image_barrier(depth_image, VK_IMAGE_ASPECT_DEPTH_BIT,
                          VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferImageCopy dcopy{};
            dcopy.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
            dcopy.imageExtent = {w, h, 1};
            vkCmdCopyImageToBuffer(cmd, depth_image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   depth_stage, 1, &dcopy);
        }
    }
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VkResult submit = vkQueueSubmit(d.queue, 1, &si, VK_NULL_HANDLE);
    if (submit == VK_SUCCESS) submit = vkQueueWaitIdle(d.queue);
    if (submit != VK_SUCCESS) {
        return fail(ComputeExecStatus::DispatchFailed, "queue submit/wait failed");
    }

    // ---- write everything back to guest memory ------------------------------
    {
        void* mapped = nullptr;
        vkMapMemory(d.device, color_stage_mem, 0, plane_bytes, 0, &mapped);
        std::memcpy(color_plane.data(), mapped, static_cast<size_t>(plane_bytes));
        vkUnmapMemory(d.device, color_stage_mem);
        if (!mem->WriteDwords(
                color_gva,
                reinterpret_cast<const uint32_t*>(color_plane.data()),
                color_plane.size() / 4)) {
            return fail(ComputeExecStatus::DispatchFailed,
                        "guest colour plane write-back failed");
        }
        if (target.depth_enabled) {
            vkMapMemory(d.device, depth_stage_mem, 0, depth_plane_bytes, 0,
                        &mapped);
            std::memcpy(depth_plane.data(), mapped,
                        static_cast<size_t>(depth_plane_bytes));
            vkUnmapMemory(d.device, depth_stage_mem);
            if (!mem->WriteDwords(depth_gva, depth_plane.data(),
                                  depth_plane.size())) {
                return fail(ComputeExecStatus::DispatchFailed,
                            "guest depth plane write-back failed");
            }
        }
        // The transformed-vertex dump (the round-9 draw ABI): read the out
        // SSBO the vertex stage wrote.
        const VkDeviceSize out_bytes = static_cast<VkDeviceSize>(
            draw_vertex_count) * out_dwords_per_lane * 4;
        out.transformed_vertices.assign(draw_vertex_count * out_dwords_per_lane,
                                        0u);
        vkMapMemory(d.device, mems[out_idx], 0, out_bytes, 0, &mapped);
        std::memcpy(out.transformed_vertices.data(), mapped,
                    static_cast<size_t>(out_bytes));
        vkUnmapMemory(d.device, mems[out_idx]);
        // Modified MUBUF descriptor contents go back too.
        for (size_t b = 0; b < buffer_contents.size(); ++b) {
            if (buffer_contents[b].empty()) continue;
            vkMapMemory(d.device, mems[buf_idx[b]], 0, VK_WHOLE_SIZE, 0, &mapped);
            std::memcpy(buffer_contents[b].data(), mapped,
                        buffer_contents[b].size() * 4);
            vkUnmapMemory(d.device, mems[buf_idx[b]]);
        }
    }
    StoreResourceContents(resources, mem, buffer_contents);

    out.status = ComputeExecStatus::Ok;
    out.executed = true;
    out.message = "rasterized on " + m_device_name;
    teardown();
    return out;
}

#else // !PROSPERO_HAVE_VULKAN

struct VulkanComputeExecutor::Impl {};
VulkanComputeExecutor::VulkanComputeExecutor() : m_impl(nullptr) {}
VulkanComputeExecutor::~VulkanComputeExecutor() {}
bool VulkanComputeExecutor::Initialize() { return false; }

ComputeDispatchResult VulkanComputeExecutor::RunSpirv(
        const std::vector<uint32_t>& spirv,
        const std::vector<uint32_t>&, uint32_t, uint32_t, uint32_t,
        std::vector<SpirvExtraSsbo>*, const uint32_t*, uint32_t,
        std::vector<SpirvExtraImage>*) {
    ComputeDispatchResult out;
    out.spirv_dwords = spirv.size();
    out.status = ComputeExecStatus::Unavailable;
    out.message = "built without <vulkan/vulkan.h>";
    return out;
}

VulkanComputeExecutor::GraphicsRasterResult VulkanComputeExecutor::DrawVerticesToGuest(
        const uint32_t*, size_t,
        const std::vector<uint32_t>&,
        uint32_t, uint32_t, uint32_t,
        const GcnDispatchResources&,
        const GraphicsTargetDesc&,
        uint64_t, uint64_t, GpuGuestMemory*) {
    GraphicsRasterResult out;
    out.status = ComputeExecStatus::Unavailable;
    out.message = "built without <vulkan/vulkan.h>";
    return out;
}

ImageOpResult VulkanComputeExecutor::ClearImage(uint32_t width, uint32_t height,
                                                float, float, float, float) {
    ImageOpResult out;
    out.width = width;
    out.height = height;
    out.status = ComputeExecStatus::Unavailable;
    out.message = "built without <vulkan/vulkan.h>";
    return out;
}

#endif // PROSPERO_HAVE_VULKAN

namespace {
// Shared software-fallback driver: runs the raw GFX10 bytecode on the real
// GCN software interpreter when the hardware path could not. Returns true
// and fills `out` on success; false leaves the caller's original result.
bool TryGcnSoftwareFallback(const uint32_t* code, size_t dwords,
                             const std::vector<uint32_t>& input,
                             uint32_t k_in, uint32_t m_out,
                             GpuGuestMemory* mem,
                             const GcnDispatchResources* resources,
                             ComputeDispatchResult& out) {
    if (k_in == 0U || m_out == 0U || input.empty() ||
        input.size() % k_in != 0U) {
        return false;
    }
    const size_t lanes = input.size() / k_in;
    if (lanes > GcnSwExecutor::kMaxLanes) {
        return false;
    }
    GcnSwExecutor sw;
    // Round 28: seed the image pool + the SGPR descriptor quads from the
    // resource table. The compiler derives image i from srsrc = 4*(i+1), and
    // the software executor reads the quad's first dword -- seeding quad
    // [i, width, height, 0] at that exact SGPR base makes both paths agree
    // on every program (the round-19 buffer descriptors don't use SGPRs at
    // all in this executor, so the quads never collide).
    if (resources != nullptr && mem != nullptr) {
        for (size_t i = 0; i < resources->images.size(); ++i) {
            const auto& res = resources->images[i];
            if (i >= GcnSwExecutor::kMaxImages) break;
            GcnSwExecutor::SwImage img{};
            img.width  = res.width;
            img.height = res.height;
            img.mips   = res.mips;
            img.rgba.assign(static_cast<size_t>(res.width) * res.height * 4u,
                            0u);
            if (!mem->ReadDwords(res.base_gva, img.rgba.data(),
                                 img.rgba.size())) {
                return false;
            }
            sw.SetImage(i, img);
            const uint32_t base = static_cast<uint32_t>(4u * (i + 1u));
            if (base + 3u < GcnSwExecutor::kSgprCount) {
                sw.SetSgpr(base + 0u, static_cast<uint32_t>(i));
                sw.SetSgpr(base + 1u, res.width);
                sw.SetSgpr(base + 2u, res.height);
                sw.SetSgpr(base + 3u, 0u);
            }
        }
    }
    GcnSwExecResult r = sw.Run(code, dwords, lanes, input, k_in, m_out,
                               out.output, mem,
                               resources != nullptr ? &resources->buffers
                                                    : nullptr);
    if (!r.ok) {
        return false;
    }
    out.status = ComputeExecStatus::Ok;
    out.hardware = false;
    out.device_name = "GCN software interpreter (GFX10)";
    out.message = "executed on the GCN software interpreter; " +
                  std::to_string(r.instructions_executed) + " instructions, " +
                  std::to_string(r.lanes_run) + " lanes";
    out.spirv_dwords = 0;
    // Round 28: the software executor writes MIMG stores/atomics straight
    // into its image pool -- persist it back to guest memory.
    if (resources != nullptr && mem != nullptr) {
        for (size_t i = 0; i < resources->images.size() &&
                           i < GcnSwExecutor::kMaxImages; ++i) {
            const auto& img = sw.GetImage(i);
            if (img.rgba.empty()) continue;
            mem->WriteDwords(resources->images[i].base_gva, img.rgba.data(),
                             img.rgba.size());
        }
    }
    return true;
}
} // namespace

// ---- shared, driver-independent front-ends ---------------------------------

// Round 19: guest-memory staging for the resource tables. Reads every MUBUF
// descriptor's buffer and the SMEM mirror window in full (fail-closed: any
// unreadable range aborts with nothing staged). These run on every host --
// the same code path prepares the hardware dispatch and the unit tests.
bool VulkanComputeExecutor::LoadResourceContents(
        const GcnDispatchResources& resources,
        GpuGuestMemory* mem,
        std::vector<std::vector<uint32_t>>& buffer_contents,
        std::vector<uint32_t>& mirror_contents) {
    buffer_contents.clear();
    mirror_contents.clear();
    if (mem == nullptr) return false;
    for (const auto& buf : resources.buffers) {
        if (buf.base_gva == 0 || buf.size_dwords == 0) return false;
        std::vector<uint32_t> contents(buf.size_dwords, 0u);
        if (!mem->ReadDwords(buf.base_gva, contents.data(),
                             buf.size_dwords)) {
            buffer_contents.clear();
            return false;
        }
        buffer_contents.push_back(std::move(contents));
    }
    if (resources.scalar_mirror_base_gva != 0) {
        if (resources.scalar_mirror_dwords == 0) return false;
        mirror_contents.assign(resources.scalar_mirror_dwords, 0u);
        if (!mem->ReadDwords(resources.scalar_mirror_base_gva,
                             mirror_contents.data(),
                             resources.scalar_mirror_dwords)) {
            buffer_contents.clear();
            mirror_contents.clear();
            return false;
        }
    }
    return true;
}

// Round 19: writes the (possibly modified by MUBUF stores) buffer contents
// back to guest memory. The mirror is read-only in the round-18/19 lowering
// (S_LOAD_* only), so it never writes back.
bool VulkanComputeExecutor::StoreResourceContents(
        const GcnDispatchResources& resources,
        GpuGuestMemory* mem,
        const std::vector<std::vector<uint32_t>>& buffer_contents) {
    if (mem == nullptr) return false;
    if (buffer_contents.size() != resources.buffers.size()) return false;
    for (size_t i = 0; i < buffer_contents.size(); ++i) {
        if (buffer_contents[i].empty()) continue;
        if (buffer_contents[i].size() != resources.buffers[i].size_dwords) {
            return false;
        }
        if (!mem->WriteDwords(resources.buffers[i].base_gva,
                              buffer_contents[i].data(),
                              buffer_contents[i].size())) {
            return false;
        }
    }
    return true;
}

// Round 28: stages every image's texel array from guest memory (raw
// RGBA32UI dwords, width*height*4). Fail-closed like the buffer loader.
bool VulkanComputeExecutor::LoadImageContents(
        const GcnDispatchResources& resources,
        GpuGuestMemory* mem,
        std::vector<SpirvExtraImage>& image_contents) {
    image_contents.clear();
    if (mem == nullptr) return false;
    for (const auto& img : resources.images) {
        if (img.base_gva == 0 || img.width == 0 || img.height == 0) return false;
        SpirvExtraImage staged{};
        staged.width = img.width;
        staged.height = img.height;
        staged.mips = img.mips;
        staged.contents.assign(static_cast<size_t>(img.width) * img.height * 4u,
                               0u);
        if (!mem->ReadDwords(img.base_gva, staged.contents.data(),
                             staged.contents.size())) {
            image_contents.clear();
            return false;
        }
        image_contents.push_back(std::move(staged));
    }
    return true;
}

// Round 28: writes the (possibly MIMG-modified) texel arrays back to guest
// memory.
bool VulkanComputeExecutor::StoreImageContents(
        const GcnDispatchResources& resources,
        GpuGuestMemory* mem,
        const std::vector<SpirvExtraImage>& image_contents) {
    if (mem == nullptr) return false;
    if (image_contents.size() != resources.images.size()) return false;
    for (size_t i = 0; i < image_contents.size(); ++i) {
        const auto& staged = image_contents[i];
        if (staged.contents.empty()) continue;
        if (!mem->WriteDwords(resources.images[i].base_gva,
                              staged.contents.data(),
                              staged.contents.size())) {
            return false;
        }
    }
    return true;
}

ComputeDispatchResult VulkanComputeExecutor::RunRDNA2WithResources(
        const uint32_t* rdna2_code, size_t dwords,
        const std::vector<uint32_t>& input,
        const GcnDispatchResources& resources,
        GpuGuestMemory* mem,
        uint32_t in_dwords_per_lane,
        uint32_t out_dwords_per_lane) {
    ComputeDispatchResult out;
    out.device_name = m_device_name;

    if (in_dwords_per_lane == 0U || out_dwords_per_lane == 0U ||
        input.empty() || input.size() % in_dwords_per_lane != 0U) {
        out.status = ComputeExecStatus::ResourceFailed;
        out.message = "input is not a whole number of strided lanes";
        return out;
    }

    // Stage the resource tables from guest memory (fail-closed: a broken
    // binding never dispatches anything, hardware or software).
    std::vector<std::vector<uint32_t>> buffer_contents;
    std::vector<uint32_t> mirror_contents;
    std::vector<SpirvExtraImage> image_contents;
    if (!LoadResourceContents(resources, mem, buffer_contents,
                              mirror_contents) ||
        !LoadImageContents(resources, mem, image_contents)) {
        out.status = ComputeExecStatus::ResourceFailed;
        out.message = "resource-table staging failed (guest memory unreadable)";
        return out;
    }

    // Compile with the tables (the round-19 push-constant mirror base: the
    // VALUE is pushed at dispatch time, not baked into the module).
    ComputeCompilerOptions options;
    options.in_dwords_per_lane = in_dwords_per_lane;
    options.out_dwords_per_lane = out_dwords_per_lane;
    options.buffers = resources.buffers;
    options.images = resources.images;          // Round 28 (MIMG)
    options.scalar_mirror_base_gva = resources.scalar_mirror_base_gva;
    RDNA2ComputeCompiler compiler(options);
    auto compiled = compiler.Compile(rdna2_code, dwords);
    if (!compiled) {
        out.status = ComputeExecStatus::CompileFailed;
        out.message = "RDNA2->SPIR-V failed at dword " +
                      std::to_string(compiled.error_dword) + ": " +
                      compiled.message;
        if (m_sw_fallback &&
            TryGcnSoftwareFallback(rdna2_code, dwords, input,
                                   in_dwords_per_lane, out_dwords_per_lane,
                                   mem, &resources, out)) {
            return out;
        }
        return out;
    }

    // Hardware dispatch: bind the extras in EXACTLY the module's binding
    // order (mirror first at 2, then the per-descriptor SSBOs).
    std::vector<SpirvExtraSsbo> extra;
    if (compiled.used_scalar_mirror) {
        extra.push_back({std::move(mirror_contents), /*read_back=*/false});
    }
    for (auto& contents : buffer_contents) {
        extra.push_back({std::move(contents), /*read_back=*/true});
    }
    uint32_t push[2] = {
        static_cast<uint32_t>(resources.scalar_mirror_base_gva),
        static_cast<uint32_t>(resources.scalar_mirror_base_gva >> 32)};
    const uint32_t lanes =
        static_cast<uint32_t>(input.size() / in_dwords_per_lane);
    const uint32_t out_elems = lanes * out_dwords_per_lane;
    auto hw = RunSpirv(compiled.spirv, input, /*local_size_x=*/64, lanes,
                       out_elems, &extra,
                       compiled.used_scalar_mirror ? push : nullptr,
                       compiled.used_scalar_mirror ? 2u : 0u,
                       &image_contents);
    if (hw.status == ComputeExecStatus::Ok) {
        // The staged vectors were moved into `extra`; the downloaded results
        // live there now -- write them back to guest memory.
        std::vector<std::vector<uint32_t>> modified;
        size_t idx = compiled.used_scalar_mirror ? 1u : 0u;
        for (size_t b = 0; b < resources.buffers.size(); ++b, ++idx) {
            modified.push_back(std::move(extra[idx].contents));
        }
        StoreResourceContents(resources, mem, modified);
        StoreImageContents(resources, mem, image_contents);
        return hw;
    }
    // Hardware could not run: the honest GCN software interpreter with the
    // SAME tables (it writes guest memory live through the bridge).
    if (m_sw_fallback &&
        TryGcnSoftwareFallback(rdna2_code, dwords, input,
                               in_dwords_per_lane, out_dwords_per_lane,
                               mem, &resources, hw)) {
        return hw;
    }
    return hw;
}

void VulkanComputeExecutor::SetSoftwareFallback(bool enabled, GpuGuestMemory* mem,
                                                 const std::vector<GcnBufferResource>* buffers) {
    m_sw_fallback = enabled;
    m_sw_mem = mem;
    m_sw_buffers = buffers;
    // Round 28: mirror the legacy buffer-only table into the full resource
    // struct the fallback consumes.
    m_sw_resources = GcnDispatchResources{};
    if (buffers != nullptr) {
        m_sw_resources.buffers = *buffers;
    }
    m_sw_res_ptr = &m_sw_resources;
}


ComputeDispatchResult VulkanComputeExecutor::RunRDNA2(
        const uint32_t* rdna2_code, size_t dwords,
        const std::vector<uint32_t>& input) {
    ComputeDispatchResult out;
    out.device_name = m_device_name;

    RDNA2ComputeCompiler compiler;
    auto compiled = compiler.Compile(rdna2_code, dwords);
    if (!compiled) {
        out.status = ComputeExecStatus::CompileFailed;
        out.message = "RDNA2->SPIR-V failed at dword " +
                      std::to_string(compiled.error_dword) + ": " + compiled.message;
        if (m_sw_fallback &&
            TryGcnSoftwareFallback(rdna2_code, dwords, input, 1, 1,
                                   m_sw_mem, m_sw_res_ptr, out)) {
            return out;
        }
        return out;
    }
    auto hw = RunSpirv(compiled.spirv, input, /*local_size_x=*/64);
    if (hw.status != ComputeExecStatus::Ok && m_sw_fallback &&
        TryGcnSoftwareFallback(rdna2_code, dwords, input, 1, 1,
                               m_sw_mem, m_sw_res_ptr, hw)) {
        return hw;
    }
    return hw;
}

ComputeDispatchResult VulkanComputeExecutor::RunRDNA2Strided(
        const uint32_t* rdna2_code, size_t dwords,
        const std::vector<uint32_t>& input,
        uint32_t in_dwords_per_lane, uint32_t out_dwords_per_lane) {
    ComputeDispatchResult out;
    out.device_name = m_device_name;

    if (in_dwords_per_lane == 0U || out_dwords_per_lane == 0U || input.empty() ||
        input.size() % in_dwords_per_lane != 0U) {
        out.status = ComputeExecStatus::ResourceFailed;
        out.message = "input is not a whole number of strided lanes";
        return out;
    }

    ComputeCompilerOptions options;
    options.in_dwords_per_lane = in_dwords_per_lane;
    options.out_dwords_per_lane = out_dwords_per_lane;
    RDNA2ComputeCompiler compiler(options);
    auto compiled = compiler.Compile(rdna2_code, dwords);
    if (!compiled) {
        out.status = ComputeExecStatus::CompileFailed;
        out.message = "RDNA2->SPIR-V failed at dword " +
                      std::to_string(compiled.error_dword) + ": " + compiled.message;
        if (m_sw_fallback &&
            TryGcnSoftwareFallback(rdna2_code, dwords, input,
                                   in_dwords_per_lane, out_dwords_per_lane,
                                   m_sw_mem, m_sw_res_ptr, out)) {
            return out;
        }
        return out;
    }
    const uint32_t lanes =
        static_cast<uint32_t>(input.size() / in_dwords_per_lane);
    const uint32_t out_elems = lanes * out_dwords_per_lane;
    auto hw = RunSpirv(compiled.spirv, input, /*local_size_x=*/64, lanes, out_elems);
    if (hw.status != ComputeExecStatus::Ok && m_sw_fallback &&
        TryGcnSoftwareFallback(rdna2_code, dwords, input,
                               in_dwords_per_lane, out_dwords_per_lane,
                               m_sw_mem, m_sw_res_ptr, hw)) {
        return hw;
    }
    return hw;
}

ComputeDispatchResult VulkanComputeExecutor::RunRDNA2Float(
        const uint32_t* rdna2_code, size_t dwords,
        const std::vector<float>& input) {
    std::vector<uint32_t> bits(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        std::memcpy(&bits[i], &input[i], sizeof(uint32_t));
    }
    return RunRDNA2(rdna2_code, dwords, bits);
}

} // namespace PS5::GPU
