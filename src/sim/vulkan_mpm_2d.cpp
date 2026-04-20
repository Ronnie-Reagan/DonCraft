#include "sim/vulkan_mpm_2d.hpp"

#include "core/assert.hpp"
#include "core/log.hpp"
#include "sim/reference_mpm.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <thread>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(DF_ENABLE_VULKAN_MPM) && DF_ENABLE_VULKAN_MPM
#include <vulkan/vulkan.h>
#endif

namespace df::sim
{
namespace
{

auto EnvFlagEnabled(const char* const name) -> bool
{
    const char* const value = std::getenv(name);
    if (value == nullptr)
    {
        return false;
    }

    return value[0] == '1' || value[0] == 'T' || value[0] == 't' || value[0] == 'Y' || value[0] == 'y';
}

template <typename... Args>
void EmitImmediateTrace(const char* const scope, const Args&... args)
{
    return;
    static std::atomic<std::uint64_t> sequence{1u};

    std::ostringstream stream;
    (stream << ... << args);
    const std::string message = stream.str();
    const std::uint64_t sequenceId = sequence.fetch_add(1u, std::memory_order_relaxed);
    const unsigned long long threadId = static_cast<unsigned long long>(std::hash<std::thread::id>{}(std::this_thread::get_id()));

    std::fprintf(
        stderr,
        "[MPM-TRACE][%s][#%llu][tid=%llu] %s\n",
        scope,
        static_cast<unsigned long long>(sequenceId),
        threadId,
        message.c_str());
    std::fflush(stderr);

    try
    {
        std::ofstream traceFile("doncraft_mpm_trace.log", std::ios::app);
        if (traceFile)
        {
            traceFile
                << "[MPM-TRACE][" << scope << "][#" << sequenceId << "][tid=" << threadId << "] "
                << message
                << '\n';
            traceFile.flush();
        }
    }
    catch (...)
    {
    }

    try
    {
        LogInfo("[MPM-TRACE][", scope, "][#", sequenceId, "][tid=", threadId, "] ", message);
    }
    catch (...)
    {
    }
}

constexpr std::uint32_t kLocalSize = 64u;
constexpr float kMassScale = 10000.0f;
}

struct VulkanMpm2D::Impl
{
    explicit Impl(const int gridWidthIn, const int gridHeightIn, const float cellSizeIn, const float dtIn)
        : gridWidth(gridWidthIn)
        , gridHeight(gridHeightIn)
        , cellSize(cellSizeIn)
        , dt(dtIn)
    {
    }

    int gridWidth = 0;
    int gridHeight = 0;
    float cellSize = 1.0f;
    float dt = 1.0f / 60.0f;
    std::vector<Particle> particles;
    float totalGridMass = 0.0f;
    bool available = false;
    std::string failureMessage = "Vulkan MPM backend was not initialized.";
    std::string activeBackendName = "CPU Reference";
    std::string computeDeviceName;
    std::unique_ptr<ReferenceMpm2D> cpuFallback;
    std::size_t cpuFallbackWorkerCount = 1;

#if defined(DF_ENABLE_VULKAN_MPM) && DF_ENABLE_VULKAN_MPM
    struct GpuParticle
    {
        float positionX = 0.0f;
        float positionY = 0.0f;
        float velocityX = 0.0f;
        float velocityY = 0.0f;
        float mass = 1.0f;
        float padding0 = 0.0f;
        float padding1 = 0.0f;
        float padding2 = 0.0f;
    };

    struct GpuNode
    {
        float mass = 0.0f;
        float velocityX = 0.0f;
        float velocityY = 0.0f;
        float padding = 0.0f;
    };

    struct GpuNodeAccum
    {
        std::int32_t mass = 0;
        std::int32_t momentumX = 0;
        std::int32_t momentumY = 0;
        std::int32_t padding = 0;
    };

    struct GpuStats
    {
        std::int32_t totalMass = 0;
        std::int32_t padding0 = 0;
        std::int32_t padding1 = 0;
        std::int32_t padding2 = 0;
    };

    struct PushConstants
    {
        std::int32_t gridWidth = 0;
        std::int32_t gridHeight = 0;
        std::int32_t particleCount = 0;
        float cellSize = 1.0f;
        float dt = 1.0f / 60.0f;
        float gravityY = -9.81f;
    };

    struct BufferResource
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize capacity = 0;
        void* mapped = nullptr;
    };

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue computeQueue = VK_NULL_HANDLE;
    std::uint32_t computeQueueFamily = 0;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline clearPipeline = VK_NULL_HANDLE;
    VkPipeline scatterPipeline = VK_NULL_HANDLE;
    VkPipeline updatePipeline = VK_NULL_HANDLE;
    VkPipeline advectPipeline = VK_NULL_HANDLE;
    BufferResource particleBuffer{};
    BufferResource nodeAccumBuffer{};
    BufferResource nodeBuffer{};
    BufferResource statsBuffer{};

    ~Impl()
    {
        Shutdown();
    }

    void Shutdown()
    {
        if (device != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(device);
        }

        DestroyBuffer(particleBuffer);
        DestroyBuffer(nodeAccumBuffer);
        DestroyBuffer(nodeBuffer);
        DestroyBuffer(statsBuffer);

        if (clearPipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(device, clearPipeline, nullptr);
            clearPipeline = VK_NULL_HANDLE;
        }
        if (scatterPipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(device, scatterPipeline, nullptr);
            scatterPipeline = VK_NULL_HANDLE;
        }
        if (updatePipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(device, updatePipeline, nullptr);
            updatePipeline = VK_NULL_HANDLE;
        }
        if (advectPipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(device, advectPipeline, nullptr);
            advectPipeline = VK_NULL_HANDLE;
        }
        if (pipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            pipelineLayout = VK_NULL_HANDLE;
        }
        if (descriptorPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(device, descriptorPool, nullptr);
            descriptorPool = VK_NULL_HANDLE;
        }
        if (descriptorSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
            descriptorSetLayout = VK_NULL_HANDLE;
        }
        if (fence != VK_NULL_HANDLE)
        {
            vkDestroyFence(device, fence, nullptr);
            fence = VK_NULL_HANDLE;
        }
        if (commandPool != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(device, commandPool, nullptr);
            commandPool = VK_NULL_HANDLE;
        }
        if (device != VK_NULL_HANDLE)
        {
            vkDestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
        }
        if (instance != VK_NULL_HANDLE)
        {
            vkDestroyInstance(instance, nullptr);
            instance = VK_NULL_HANDLE;
        }

        computeQueue = VK_NULL_HANDLE;
        physicalDevice = VK_NULL_HANDLE;
        computeDeviceName.clear();
        available = false;
    }

    [[nodiscard]] static auto ReadBinaryFile(const std::filesystem::path& path) -> std::vector<std::uint32_t>
    {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input)
        {
            throw std::runtime_error("Failed to open shader file: " + path.string());
        }

        const std::streamsize size = input.tellg();
        if (size <= 0 || (size % 4) != 0)
        {
            throw std::runtime_error("Shader file had an invalid SPIR-V size: " + path.string());
        }

        input.seekg(0, std::ios::beg);
        std::vector<std::uint32_t> bytes(static_cast<std::size_t>(size) / sizeof(std::uint32_t));
        if (!input.read(reinterpret_cast<char*>(bytes.data()), size))
        {
            throw std::runtime_error("Failed to read shader file: " + path.string());
        }
        return bytes;
    }

    [[nodiscard]] static auto ResolveShaderPath(const std::string_view filename) -> std::filesystem::path
    {
        const std::array<std::filesystem::path, 10> candidates = {
            std::filesystem::current_path() / "shaders" / filename,
            std::filesystem::current_path() / "bin" / "Debug" / "shaders" / filename,
            std::filesystem::current_path() / "bin" / "Release" / "shaders" / filename,
            std::filesystem::current_path() / "generated" / "shaders" / filename,
            std::filesystem::current_path().parent_path() / "generated" / "shaders" / filename,
            std::filesystem::current_path().parent_path() / "shaders" / filename,
            std::filesystem::current_path() / "build" / "vs2022-debug" / "bin" / "Debug" / "shaders" / filename,
            std::filesystem::current_path() / "build" / "vs2022-release" / "bin" / "Release" / "shaders" / filename,
            std::filesystem::current_path() / "build" / "vs2022-debug" / "generated" / "shaders" / filename,
            std::filesystem::current_path() / "build" / "vs2022-release" / "generated" / "shaders" / filename,
        };

        for (const auto& candidate : candidates)
        {
            if (std::filesystem::exists(candidate))
            {
                return candidate;
            }
        }

        const std::filesystem::path buildRoot = std::filesystem::current_path() / "build";
        if (std::filesystem::exists(buildRoot))
        {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(buildRoot))
            {
                if (!entry.is_regular_file())
                {
                    continue;
                }
                if (entry.path().filename() == filename)
                {
                    return entry.path();
                }
            }
        }

        throw std::runtime_error("Failed to locate shader file: " + std::string(filename));
    }

    static void CheckVk(const VkResult result, const char* message)
    {
        if (result != VK_SUCCESS)
        {
            throw std::runtime_error(std::string(message) + " (VkResult=" + std::to_string(static_cast<int>(result)) + ")");
        }
    }

    [[nodiscard]] auto FindMemoryType(const std::uint32_t typeBits, const VkMemoryPropertyFlags properties) const -> std::uint32_t
    {
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
        for (std::uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index)
        {
            const bool typeSupported = (typeBits & (1u << index)) != 0u;
            const bool propertyMatch = (memoryProperties.memoryTypes[index].propertyFlags & properties) == properties;
            if (typeSupported && propertyMatch)
            {
                return index;
            }
        }

        throw std::runtime_error("Failed to find a matching Vulkan memory type for the MPM backend.");
    }

    void DestroyBuffer(BufferResource& buffer)
    {
        if (buffer.mapped != nullptr)
        {
            vkUnmapMemory(device, buffer.memory);
            buffer.mapped = nullptr;
        }
        if (buffer.buffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(device, buffer.buffer, nullptr);
            buffer.buffer = VK_NULL_HANDLE;
        }
        if (buffer.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(device, buffer.memory, nullptr);
            buffer.memory = VK_NULL_HANDLE;
        }
        buffer.capacity = 0;
    }

    void CreateBuffer(BufferResource& buffer, const VkDeviceSize minimumSize)
    {
        DestroyBuffer(buffer);

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = std::max<VkDeviceSize>(minimumSize, 16);
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        CheckVk(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer.buffer), "Failed to create a Vulkan MPM storage buffer.");

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, buffer.buffer, &requirements);

        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        CheckVk(vkAllocateMemory(device, &allocateInfo, nullptr, &buffer.memory), "Failed to allocate a Vulkan MPM buffer.");
        CheckVk(vkBindBufferMemory(device, buffer.buffer, buffer.memory, 0), "Failed to bind a Vulkan MPM buffer.");
        CheckVk(vkMapMemory(device, buffer.memory, 0, bufferInfo.size, 0, &buffer.mapped), "Failed to map a Vulkan MPM buffer.");
        buffer.capacity = bufferInfo.size;
    }

    void EnsureBufferCapacity(BufferResource& buffer, const VkDeviceSize minimumSize)
    {
        if (buffer.buffer != VK_NULL_HANDLE && buffer.capacity >= minimumSize)
        {
            return;
        }

        VkDeviceSize capacity = 256;
        while (capacity < minimumSize)
        {
            capacity *= 2;
        }
        CreateBuffer(buffer, capacity);
        UpdateDescriptorSet();
    }

    void CreateInstance()
    {
        VkApplicationInfo applicationInfo{};
        applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        applicationInfo.pApplicationName = "DonCraft Vulkan MPM";
        applicationInfo.apiVersion = VK_API_VERSION_1_1;

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &applicationInfo;
        CheckVk(vkCreateInstance(&createInfo, nullptr, &instance), "Failed to create the Vulkan instance for the MPM backend.");
    }

    void PickPhysicalDevice()
    {
        std::uint32_t deviceCount = 0;
        CheckVk(vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr), "Failed to enumerate Vulkan devices for the MPM backend.");
        if (deviceCount == 0)
        {
            throw std::runtime_error("No Vulkan physical device was available for the MPM backend.");
        }

        std::vector<VkPhysicalDevice> devices(deviceCount);
        CheckVk(vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data()), "Failed to enumerate Vulkan devices for the MPM backend.");
        int bestScore = std::numeric_limits<int>::min();
        for (const VkPhysicalDevice candidate : devices)
        {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);

            int deviceScore = 0;
            switch (properties.deviceType)
            {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                deviceScore += 10000;
                break;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                deviceScore += 5000;
                break;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
                deviceScore += 2500;
                break;
            case VK_PHYSICAL_DEVICE_TYPE_CPU:
                deviceScore += 100;
                break;
            default:
                deviceScore += 500;
                break;
            }
            deviceScore += static_cast<int>(properties.limits.maxComputeWorkGroupInvocations);
            deviceScore += static_cast<int>(properties.limits.maxComputeSharedMemorySize / 1024u);

            std::uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
            for (std::uint32_t familyIndex = 0; familyIndex < familyCount; ++familyIndex)
            {
                if ((families[familyIndex].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0)
                {
                    int candidateScore = deviceScore;
                    if ((families[familyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0)
                    {
                        candidateScore += 128;
                    }

                    if (candidateScore > bestScore)
                    {
                        bestScore = candidateScore;
                        physicalDevice = candidate;
                        computeQueueFamily = familyIndex;
                        computeDeviceName = properties.deviceName;
                    }
                }
            }
        }

        if (physicalDevice == VK_NULL_HANDLE)
        {
            throw std::runtime_error("No Vulkan compute queue family was found for the MPM backend.");
        }
    }

    void CreateDevice()
    {
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = computeQueueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;

        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.queueCreateInfoCount = 1;
        createInfo.pQueueCreateInfos = &queueInfo;

        CheckVk(vkCreateDevice(physicalDevice, &createInfo, nullptr, &device), "Failed to create the Vulkan device for the MPM backend.");
        vkGetDeviceQueue(device, computeQueueFamily, 0, &computeQueue);
    }

    void CreateCommandResources()
    {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = computeQueueFamily;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        CheckVk(vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool), "Failed to create the MPM command pool.");

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        CheckVk(vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer), "Failed to allocate the MPM command buffer.");

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        CheckVk(vkCreateFence(device, &fenceInfo, nullptr, &fence), "Failed to create the MPM fence.");
    }

    void CreateDescriptorResources()
    {
        const std::array<VkDescriptorSetLayoutBinding, 4> bindings = {{
            VkDescriptorSetLayoutBinding{
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
            VkDescriptorSetLayoutBinding{
                .binding = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
            VkDescriptorSetLayoutBinding{
                .binding = 2,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
            VkDescriptorSetLayoutBinding{
                .binding = 3,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        }};

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        CheckVk(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descriptorSetLayout), "Failed to create the MPM descriptor set layout.");

        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSize.descriptorCount = 4;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        CheckVk(vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool), "Failed to create the MPM descriptor pool.");

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = descriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &descriptorSetLayout;
        CheckVk(vkAllocateDescriptorSets(device, &allocInfo, &descriptorSet), "Failed to allocate the MPM descriptor set.");

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(PushConstants);

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;
        CheckVk(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout), "Failed to create the MPM pipeline layout.");
    }

    void UpdateDescriptorSet()
    {
        if (descriptorSet == VK_NULL_HANDLE ||
            particleBuffer.buffer == VK_NULL_HANDLE ||
            nodeAccumBuffer.buffer == VK_NULL_HANDLE ||
            nodeBuffer.buffer == VK_NULL_HANDLE ||
            statsBuffer.buffer == VK_NULL_HANDLE)
        {
            return;
        }

        const VkDescriptorBufferInfo particleInfo{
            particleBuffer.buffer,
            0,
            particleBuffer.capacity,
        };
        const VkDescriptorBufferInfo nodeAccumInfo{
            nodeAccumBuffer.buffer,
            0,
            nodeAccumBuffer.capacity,
        };
        const VkDescriptorBufferInfo nodeInfo{
            nodeBuffer.buffer,
            0,
            nodeBuffer.capacity,
        };
        const VkDescriptorBufferInfo statsInfo{
            statsBuffer.buffer,
            0,
            statsBuffer.capacity,
        };

        const std::array<VkWriteDescriptorSet, 4> writes = {{
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = descriptorSet,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &particleInfo,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = descriptorSet,
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &nodeAccumInfo,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = descriptorSet,
                .dstBinding = 2,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &nodeInfo,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = descriptorSet,
                .dstBinding = 3,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &statsInfo,
            },
        }};
        vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    [[nodiscard]] auto CreateShaderModule(const std::string_view filename) const -> VkShaderModule
    {
        const std::vector<std::uint32_t> spirv = ReadBinaryFile(ResolveShaderPath(filename));
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = spirv.size() * sizeof(std::uint32_t);
        createInfo.pCode = spirv.data();

        VkShaderModule module = VK_NULL_HANDLE;
        CheckVk(vkCreateShaderModule(device, &createInfo, nullptr, &module), "Failed to create an MPM compute shader module.");
        return module;
    }

    [[nodiscard]] auto CreateComputePipeline(const std::string_view filename) const -> VkPipeline
    {
        const VkShaderModule shaderModule = CreateShaderModule(filename);

        VkPipelineShaderStageCreateInfo stageInfo{};
        stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stageInfo.module = shaderModule;
        stageInfo.pName = "main";

        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage = stageInfo;
        pipelineInfo.layout = pipelineLayout;

        VkPipeline pipeline = VK_NULL_HANDLE;
        CheckVk(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline), "Failed to create an MPM compute pipeline.");
        vkDestroyShaderModule(device, shaderModule, nullptr);
        return pipeline;
    }

    void CreatePipelines()
    {
        clearPipeline = CreateComputePipeline("mpm_clear_nodes.comp.spv");
        scatterPipeline = CreateComputePipeline("mpm_scatter_nodes.comp.spv");
        updatePipeline = CreateComputePipeline("mpm_update_nodes.comp.spv");
        advectPipeline = CreateComputePipeline("mpm_advect_particles.comp.spv");
    }

    void Initialize()
    {
        try
        {
            CreateInstance();
            PickPhysicalDevice();
            CreateDevice();
            CreateCommandResources();
            CreateDescriptorResources();
            EnsureBufferCapacity(particleBuffer, sizeof(GpuParticle));
            EnsureBufferCapacity(nodeAccumBuffer, static_cast<VkDeviceSize>(std::max(gridWidth * gridHeight, 1)) * sizeof(GpuNodeAccum));
            EnsureBufferCapacity(nodeBuffer, static_cast<VkDeviceSize>(std::max(gridWidth * gridHeight, 1)) * sizeof(GpuNode));
            EnsureBufferCapacity(statsBuffer, sizeof(GpuStats));
            CreatePipelines();
            available = true;
            activeBackendName = "Vulkan Compute";
            failureMessage.clear();
            LogInfo("Vulkan MPM backend initialized on device '", computeDeviceName, "' for grid ", gridWidth, "x", gridHeight, ".");
        }
        catch (const std::exception& error)
        {
            failureMessage = error.what();
            activeBackendName = "CPU Reference";
            Shutdown();
            LogWarning("Vulkan MPM backend unavailable for grid ", gridWidth, "x", gridHeight, ": ", failureMessage);
        }
    }

    void UploadParticles()
    {
        const std::size_t particleCount = particles.size();
        EnsureBufferCapacity(particleBuffer, static_cast<VkDeviceSize>(std::max<std::size_t>(particleCount, 1u) * sizeof(GpuParticle)));
        auto* gpuParticles = static_cast<GpuParticle*>(particleBuffer.mapped);
        for (std::size_t particleIndex = 0; particleIndex < particleCount; ++particleIndex)
        {
            gpuParticles[particleIndex].positionX = particles[particleIndex].position.x;
            gpuParticles[particleIndex].positionY = particles[particleIndex].position.y;
            gpuParticles[particleIndex].velocityX = particles[particleIndex].velocity.x;
            gpuParticles[particleIndex].velocityY = particles[particleIndex].velocity.y;
            gpuParticles[particleIndex].mass = particles[particleIndex].mass;
        }
    }

    void DownloadParticles()
    {
        const auto* gpuParticles = static_cast<const GpuParticle*>(particleBuffer.mapped);
        for (std::size_t particleIndex = 0; particleIndex < particles.size(); ++particleIndex)
        {
            particles[particleIndex].position = {gpuParticles[particleIndex].positionX, gpuParticles[particleIndex].positionY};
            particles[particleIndex].velocity = {gpuParticles[particleIndex].velocityX, gpuParticles[particleIndex].velocityY};
            particles[particleIndex].mass = gpuParticles[particleIndex].mass;
        }
    }

    void DownloadStats()
    {
        const auto* gpuStats = static_cast<const GpuStats*>(statsBuffer.mapped);
        totalGridMass = static_cast<float>(gpuStats->totalMass) / kMassScale;
    }

    void DispatchPipeline(const VkCommandBuffer buffer, const VkPipeline pipeline, const PushConstants& pushConstants, const std::uint32_t invocationCount) const
    {
        if (invocationCount == 0)
        {
            return;
        }

        vkCmdBindPipeline(buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
        vkCmdPushConstants(buffer, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants), &pushConstants);
        vkCmdDispatch(buffer, (invocationCount + kLocalSize - 1u) / kLocalSize, 1, 1);
    }

    void AddStorageBarrier(const VkCommandBuffer buffer, const VkPipelineStageFlags srcStage, const VkPipelineStageFlags dstStage) const
    {
        std::array<VkBufferMemoryBarrier, 4> barriers = {{
            VkBufferMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = particleBuffer.buffer,
                .offset = 0,
                .size = particleBuffer.capacity,
            },
            VkBufferMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = nodeAccumBuffer.buffer,
                .offset = 0,
                .size = nodeAccumBuffer.capacity,
            },
            VkBufferMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = nodeBuffer.buffer,
                .offset = 0,
                .size = nodeBuffer.capacity,
            },
            VkBufferMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = statsBuffer.buffer,
                .offset = 0,
                .size = statsBuffer.capacity,
            },
        }};

        vkCmdPipelineBarrier(
            buffer,
            srcStage,
            dstStage,
            0,
            0,
            nullptr,
            static_cast<std::uint32_t>(barriers.size()),
            barriers.data(),
            0,
            nullptr);
    }

    void Step(const float gravityY)
    {
        EmitImmediateTrace(
            "VulkanMpm2D::Impl::Step",
            "enter available=", available,
            " backend_name=", activeBackendName,
            " grid=", gridWidth, "x", gridHeight,
            " particles=", particles.size(),
            " gravityY=", gravityY);

        if (!available)
        {
            EmitImmediateTrace("VulkanMpm2D::Impl::Step", "return_not_available");
            return;
        }

        const std::uint32_t nodeInvocationCount = static_cast<std::uint32_t>(std::max(gridWidth * gridHeight, 0));
        EmitImmediateTrace("VulkanMpm2D::Impl::Step", "before_ensure_buffers node_invocations=", nodeInvocationCount);
        EnsureBufferCapacity(nodeBuffer, static_cast<VkDeviceSize>(std::max(gridWidth * gridHeight, 1)) * sizeof(GpuNode));
        EnsureBufferCapacity(nodeAccumBuffer, static_cast<VkDeviceSize>(std::max(gridWidth * gridHeight, 1)) * sizeof(GpuNodeAccum));
        EnsureBufferCapacity(statsBuffer, sizeof(GpuStats));
        EmitImmediateTrace(
            "VulkanMpm2D::Impl::Step",
            "after_ensure_buffers particle_capacity=", particleBuffer.capacity,
            " node_capacity=", nodeBuffer.capacity,
            " node_accum_capacity=", nodeAccumBuffer.capacity,
            " stats_capacity=", statsBuffer.capacity);

        EmitImmediateTrace("VulkanMpm2D::Impl::Step", "before_upload_particles");
        UploadParticles();
        EmitImmediateTrace("VulkanMpm2D::Impl::Step", "after_upload_particles");

        const PushConstants pushConstants{
            .gridWidth = gridWidth,
            .gridHeight = gridHeight,
            .particleCount = static_cast<std::int32_t>(particles.size()),
            .cellSize = cellSize,
            .dt = dt,
            .gravityY = gravityY,
        };

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        EmitImmediateTrace("VulkanMpm2D::Impl::Step", "before_reset_command_buffer");
        CheckVk(vkResetCommandBuffer(commandBuffer, 0), "Failed to reset the MPM command buffer.");
        EmitImmediateTrace("VulkanMpm2D::Impl::Step", "before_begin_command_buffer");
        CheckVk(vkBeginCommandBuffer(commandBuffer, &beginInfo), "Failed to begin the MPM command buffer.");

        EmitImmediateTrace("VulkanMpm2D::Impl::Step", "dispatch_clear");
        DispatchPipeline(commandBuffer, clearPipeline, pushConstants, nodeInvocationCount);
        AddStorageBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        EmitImmediateTrace("VulkanMpm2D::Impl::Step", "dispatch_scatter particle_count=", particles.size());
        DispatchPipeline(commandBuffer, scatterPipeline, pushConstants, static_cast<std::uint32_t>(particles.size()));
        AddStorageBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        EmitImmediateTrace("VulkanMpm2D::Impl::Step", "dispatch_update");
        DispatchPipeline(commandBuffer, updatePipeline, pushConstants, nodeInvocationCount);
        AddStorageBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        EmitImmediateTrace("VulkanMpm2D::Impl::Step", "dispatch_advect particle_count=", particles.size());
        DispatchPipeline(commandBuffer, advectPipeline, pushConstants, static_cast<std::uint32_t>(particles.size()));

        std::array<VkBufferMemoryBarrier, 2> hostBarriers = {{
            VkBufferMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = particleBuffer.buffer,
                .offset = 0,
                .size = particleBuffer.capacity,
            },
            VkBufferMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = statsBuffer.buffer,
                .offset = 0,
                .size = statsBuffer.capacity,
            },
        }};
        vkCmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT,
            0,
            0,
            nullptr,
            static_cast<std::uint32_t>(hostBarriers.size()),
            hostBarriers.data(),
            0,
            nullptr);

        EmitImmediateTrace("VulkanMpm2D::Impl::Step", "before_end_command_buffer");
        CheckVk(vkEndCommandBuffer(commandBuffer), "Failed to end the MPM command buffer.");

        EmitImmediateTrace("VulkanMpm2D::Impl::Step", "before_queue_submit");
        CheckVk(vkResetFences(device, 1, &fence), "Failed to reset the MPM fence.");
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        CheckVk(vkQueueSubmit(computeQueue, 1, &submitInfo, fence), "Failed to submit the MPM compute work.");

        EmitImmediateTrace("VulkanMpm2D::Impl::Step", "before_wait_for_fence");
        CheckVk(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "Failed to wait for the MPM compute work.");

        EmitImmediateTrace("VulkanMpm2D::Impl::Step", "before_download_particles");
        DownloadParticles();
        EmitImmediateTrace("VulkanMpm2D::Impl::Step", "after_download_particles particles=", particles.size());

        EmitImmediateTrace("VulkanMpm2D::Impl::Step", "before_download_stats");
        DownloadStats();
        EmitImmediateTrace("VulkanMpm2D::Impl::Step", "exit total_grid_mass=", totalGridMass);
    }

#endif

    void SyncParticlesToCpuFallback()
    {
        if (cpuFallback == nullptr)
        {
            cpuFallback = std::make_unique<ReferenceMpm2D>(gridWidth, gridHeight, cellSize, dt);
            cpuFallback->SetWorkerCount(cpuFallbackWorkerCount);
        }

        cpuFallback->ClearParticles();
        for (const Particle& particle : particles)
        {
            cpuFallback->AddParticle({particle.position, particle.velocity, particle.mass});
        }
    }

    void SyncParticlesFromCpuFallback()
    {
        if (cpuFallback == nullptr)
        {
            return;
        }

        particles.clear();
        particles.reserve(cpuFallback->Particles().size());
        for (const auto& particle : cpuFallback->Particles())
        {
            particles.push_back({particle.position, particle.velocity, particle.mass});
        }
        totalGridMass = cpuFallback->TotalGridMass();
    }

    void StepWithFallback(const float gravityY)
    {
        EmitImmediateTrace(
            "VulkanMpm2D::Impl::StepWithFallback",
            "enter particles=", particles.size(),
            " worker_count=", cpuFallbackWorkerCount,
            " gravityY=", gravityY);

        SyncParticlesToCpuFallback();
        EmitImmediateTrace(
            "VulkanMpm2D::Impl::StepWithFallback",
            "after_sync_to_fallback fallback_particles=",
            cpuFallback != nullptr ? cpuFallback->Particles().size() : 0u);

        cpuFallback->Step(gravityY);
        EmitImmediateTrace(
            "VulkanMpm2D::Impl::StepWithFallback",
            "after_fallback_step fallback_particles=",
            cpuFallback != nullptr ? cpuFallback->Particles().size() : 0u,
            " fallback_grid_mass=",
            cpuFallback != nullptr ? cpuFallback->TotalGridMass() : 0.0f);

        SyncParticlesFromCpuFallback();
        activeBackendName = "CPU Reference";
        EmitImmediateTrace(
            "VulkanMpm2D::Impl::StepWithFallback",
            "exit particles=", particles.size(),
            " total_grid_mass=", totalGridMass);
    }
};

VulkanMpm2D::VulkanMpm2D(const int gridWidth, const int gridHeight, const float cellSize, const float dt, const bool allowGpuBackend)
    : impl_(std::make_unique<Impl>(gridWidth, gridHeight, cellSize, dt))
{
    DF_ASSERT(gridWidth > 1 && gridHeight > 1, "VulkanMpm2D requires at least a 2x2 node grid.");
    DF_ASSERT(cellSize > 0.0f, "VulkanMpm2D requires a positive cell size.");
    DF_ASSERT(dt > 0.0f, "VulkanMpm2D requires a positive timestep.");

    impl_->activeBackendName = "CPU Reference";

#if defined(DF_ENABLE_VULKAN_MPM) && DF_ENABLE_VULKAN_MPM
    if (allowGpuBackend)
    {
        impl_->Initialize();
    }
    else
    {
        impl_->failureMessage = "Vulkan MPM backend disabled for world simulation; using CPU fallback.";
    }
#else
    impl_->failureMessage = "DF_ENABLE_VULKAN_MPM is disabled for this build; using CPU fallback.";
    LogWarning(impl_->failureMessage);
#endif
}

VulkanMpm2D::~VulkanMpm2D() = default;

void VulkanMpm2D::ClearParticles()
{
    EmitImmediateTrace(
        "VulkanMpm2D::ClearParticles",
        "before size=", impl_->particles.size(),
        " total_grid_mass=", impl_->totalGridMass);
    impl_->particles.clear();
    impl_->totalGridMass = 0.0f;
    EmitImmediateTrace("VulkanMpm2D::ClearParticles", "after size=", impl_->particles.size());
}

void VulkanMpm2D::AddParticle(const Particle& particle)
{
    impl_->particles.push_back(particle);
}

int VulkanMpm2D::GridWidth() const
{
    return impl_->gridWidth;
}

int VulkanMpm2D::GridHeight() const
{
    return impl_->gridHeight;
}

void VulkanMpm2D::SetCpuFallbackWorkerCount(const std::size_t workerCount)
{
    impl_->cpuFallbackWorkerCount = std::max<std::size_t>(1, workerCount);
    if (impl_->cpuFallback != nullptr)
    {
        impl_->cpuFallback->SetWorkerCount(impl_->cpuFallbackWorkerCount);
    }
}

void VulkanMpm2D::Step(const float gravityY)
{
    EmitImmediateTrace(
        "VulkanMpm2D::Step",
        "enter available=", impl_->available,
        " backend_name=", impl_->activeBackendName,
        " particles=", impl_->particles.size(),
        " total_particle_mass=", TotalParticleMass(),
        " total_grid_mass=", impl_->totalGridMass,
        " gravityY=", gravityY,
        " force_cpu=", EnvFlagEnabled("DONCRAFT_MPM_FORCE_CPU_FALLBACK"));

#if defined(DF_ENABLE_VULKAN_MPM) && DF_ENABLE_VULKAN_MPM
    if (impl_->available && !EnvFlagEnabled("DONCRAFT_MPM_FORCE_CPU_FALLBACK"))
    {
        EmitImmediateTrace("VulkanMpm2D::Step", "dispatch_gpu");
        impl_->Step(gravityY);
        impl_->activeBackendName = "Vulkan Compute";
        EmitImmediateTrace(
            "VulkanMpm2D::Step",
            "return_gpu particles=", impl_->particles.size(),
            " total_grid_mass=", impl_->totalGridMass);
        return;
    }
#endif

    EmitImmediateTrace("VulkanMpm2D::Step", "dispatch_cpu_fallback");
    impl_->StepWithFallback(gravityY);
    EmitImmediateTrace(
        "VulkanMpm2D::Step",
        "return_cpu_fallback particles=", impl_->particles.size(),
        " total_grid_mass=", impl_->totalGridMass);
}

const std::vector<VulkanMpm2D::Particle>& VulkanMpm2D::Particles() const
{
    return impl_->particles;
}

float VulkanMpm2D::TotalParticleMass() const
{
    float totalMass = 0.0f;
    for (const Particle& particle : impl_->particles)
    {
        totalMass += particle.mass;
    }
    return totalMass;
}

float VulkanMpm2D::TotalGridMass() const
{
    return impl_->totalGridMass;
}

bool VulkanMpm2D::IsAvailable() const
{
    return impl_->available;
}

std::size_t VulkanMpm2D::CpuFallbackWorkerCount() const
{
    return impl_->cpuFallback != nullptr ? impl_->cpuFallback->WorkerCount() : impl_->cpuFallbackWorkerCount;
}

std::string_view VulkanMpm2D::ActiveBackendName() const
{
    return impl_->activeBackendName;
}

const std::string& VulkanMpm2D::FailureMessage() const
{
    return impl_->failureMessage;
}
}
