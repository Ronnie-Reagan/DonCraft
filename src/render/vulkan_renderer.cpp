#include "render/vulkan_renderer.hpp"

#include "core/assert.hpp"
#include "core/config.hpp"
#include "core/log.hpp"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace df::render
{
namespace
{
constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";
constexpr std::uint32_t kShadowMapSize = 2048u;
constexpr Vec3 kWorldUp{0.0f, 1.0f, 0.0f};
const Vec3 kShadowLightDirection = Normalize(Vec3{0.28f, 0.88f, 0.36f});
constexpr VkFormatFeatureFlags kRequiredShadowDepthFormatFeatures =
    VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;

struct SwapchainSupportDetails
{
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

struct ScenePushConstants
{
    Mat4 worldToClip{};
    Mat4 worldToShadowClip{};
};

[[nodiscard]] auto VulkanError(const char* message, const VkResult result) -> std::runtime_error
{
    std::ostringstream stream;
    stream << message << " (VkResult=" << static_cast<int>(result) << ")";
    return std::runtime_error(stream.str());
}

void CheckVk(const VkResult result, const char* message)
{
    if (result != VK_SUCCESS)
    {
        throw VulkanError(message, result);
    }
}

[[nodiscard]] auto QuerySwapchainSupport(const VkPhysicalDevice device, const VkSurfaceKHR surface) -> SwapchainSupportDetails
{
    SwapchainSupportDetails details{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);

    std::uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
    details.formats.resize(formatCount);
    if (formatCount > 0)
    {
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, details.formats.data());
    }

    std::uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);
    details.presentModes.resize(presentModeCount);
    if (presentModeCount > 0)
    {
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, details.presentModes.data());
    }

    return details;
}

[[nodiscard]] auto ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) -> VkSurfaceFormatKHR
{
    const auto preferred = std::find_if(formats.begin(), formats.end(), [](const VkSurfaceFormatKHR& format) {
        return format.format == VK_FORMAT_B8G8R8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });

    return preferred != formats.end() ? *preferred : formats.front();
}

[[nodiscard]] auto ChoosePresentMode(const std::vector<VkPresentModeKHR>& presentModes) -> VkPresentModeKHR
{
    const auto preferred = std::find(presentModes.begin(), presentModes.end(), VK_PRESENT_MODE_MAILBOX_KHR);
    return preferred != presentModes.end() ? *preferred : VK_PRESENT_MODE_FIFO_KHR;
}

[[nodiscard]] auto ChooseExtent(SDL_Window* window, const VkSurfaceCapabilitiesKHR& capabilities) -> VkExtent2D
{
    if (capabilities.currentExtent.width != UINT32_MAX)
    {
        return capabilities.currentExtent;
    }

    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window, &width, &height);

    VkExtent2D extent{};
    extent.width = std::clamp(static_cast<std::uint32_t>(std::max(width, 0)), capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
    extent.height = std::clamp(static_cast<std::uint32_t>(std::max(height, 0)), capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    return extent;
}

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void*)
{
    if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    {
        LogError("Vulkan validation: ", callbackData->pMessage);
    }
    else if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
    {
        LogWarning("Vulkan validation: ", callbackData->pMessage);
    }
    else
    {
        LogTrace("Vulkan validation: ", callbackData->pMessage);
    }

    return VK_FALSE;
}

[[nodiscard]] auto ReadBinaryFile(const std::filesystem::path& path) -> std::vector<std::uint32_t>
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

    std::vector<char> rawBytes(static_cast<std::size_t>(size));
    if (!input.read(rawBytes.data(), size))
    {
        throw std::runtime_error("Failed to read shader file: " + path.string());
    }

    std::vector<std::uint32_t> spirv(rawBytes.size() / sizeof(std::uint32_t));
    std::memcpy(spirv.data(), rawBytes.data(), rawBytes.size());
    return spirv;
}

[[nodiscard]] auto CreateShaderModule(const VkDevice device, const std::vector<std::uint32_t>& spirv) -> VkShaderModule
{
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = spirv.size() * sizeof(std::uint32_t);
    createInfo.pCode = spirv.data();

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    CheckVk(vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule), "Failed to create a Vulkan shader module.");
    return shaderModule;
}

[[nodiscard]] auto RoundUpCapacity(const VkDeviceSize minimumSize) -> VkDeviceSize
{
    VkDeviceSize capacity = 4096;
    while (capacity < minimumSize)
    {
        capacity *= 2;
    }
    return capacity;
}

void ExpandBounds(const Vec3& point, Vec3& minimum, Vec3& maximum, bool& hasBounds)
{
    if (!hasBounds)
    {
        minimum = point;
        maximum = point;
        hasBounds = true;
        return;
    }

    minimum.x = std::min(minimum.x, point.x);
    minimum.y = std::min(minimum.y, point.y);
    minimum.z = std::min(minimum.z, point.z);
    maximum.x = std::max(maximum.x, point.x);
    maximum.y = std::max(maximum.y, point.y);
    maximum.z = std::max(maximum.z, point.z);
}

void ExpandVertexBounds(
    const std::span<const ColorVertex3D> vertices,
    Vec3& minimum,
    Vec3& maximum,
    bool& hasBounds)
{
    for (const ColorVertex3D& vertex : vertices)
    {
        ExpandBounds(vertex.position, minimum, maximum, hasBounds);
    }
}

auto BuildShadowProjection(const FrameRenderData& frameData) -> Mat4
{
    Vec3 sceneMinimum{};
    Vec3 sceneMaximum{};
    bool hasSceneBounds = false;
    ExpandVertexBounds(frameData.terrainTriangles, sceneMinimum, sceneMaximum, hasSceneBounds);
    ExpandVertexBounds(frameData.translucentTerrainTriangles, sceneMinimum, sceneMaximum, hasSceneBounds);
    ExpandVertexBounds(frameData.dynamicTriangles, sceneMinimum, sceneMaximum, hasSceneBounds);
    ExpandVertexBounds(frameData.dynamicTranslucentTriangles, sceneMinimum, sceneMaximum, hasSceneBounds);

    if (!hasSceneBounds)
    {
        return IdentityMatrix();
    }

    const Vec3 sceneCenter = (sceneMinimum + sceneMaximum) * 0.5f;
    const float sceneExtent = std::max({
        sceneMaximum.x - sceneMinimum.x,
        sceneMaximum.y - sceneMinimum.y,
        sceneMaximum.z - sceneMinimum.z,
        1.0f,
    });
    const Vec3 upHint = std::abs(Dot(kShadowLightDirection, kWorldUp)) > 0.95f ? Vec3{1.0f, 0.0f, 0.0f} : kWorldUp;
    const Vec3 lightPosition = sceneCenter + kShadowLightDirection * (sceneExtent + 48.0f);
    const Mat4 lightView = LookAtMatrix(lightPosition, sceneCenter, upHint);

    Vec3 lightMinimum{};
    Vec3 lightMaximum{};
    bool hasLightBounds = false;
    const auto expandLightBounds = [&](const std::span<const ColorVertex3D> vertices)
    {
        for (const ColorVertex3D& vertex : vertices)
        {
            ExpandBounds(TransformPoint(lightView, vertex.position), lightMinimum, lightMaximum, hasLightBounds);
        }
    };

    expandLightBounds(frameData.terrainTriangles);
    expandLightBounds(frameData.translucentTerrainTriangles);
    expandLightBounds(frameData.dynamicTriangles);
    expandLightBounds(frameData.dynamicTranslucentTriangles);

    if (!hasLightBounds)
    {
        return IdentityMatrix();
    }

    constexpr float xyPadding = 3.0f;
    constexpr float zPadding = 8.0f;

    float left = lightMinimum.x - xyPadding;
    float right = lightMaximum.x + xyPadding;
    float bottom = lightMinimum.y - xyPadding;
    float top = lightMaximum.y + xyPadding;
    if ((right - left) < 1.0f)
    {
        const float center = (left + right) * 0.5f;
        left = center - 0.5f;
        right = center + 0.5f;
    }
    if ((top - bottom) < 1.0f)
    {
        const float center = (bottom + top) * 0.5f;
        bottom = center - 0.5f;
        top = center + 0.5f;
    }

    const float nearPlane = std::max(0.1f, -lightMaximum.z - zPadding);
    const float farPlane = std::max(nearPlane + 1.0f, -lightMinimum.z + zPadding);
    return OrthographicMatrix(left, right, bottom, top, nearPlane, farPlane) * lightView;
}
}

VulkanRenderer::~VulkanRenderer()
{
    Shutdown();
}

void VulkanRenderer::Initialize(SDL_Window* window)
{
    if (initialized_)
    {
        return;
    }

    window_ = window;

#if defined(NDEBUG)
    validationEnabled_ = false;
#else
    validationEnabled_ = SupportsValidationLayer();
#endif

    CreateInstance();
    CreateDebugMessenger();
    CreateSurface();
    PickPhysicalDevice();
    CreateDevice();
    CreateCommandPool();
    CreateDescriptorResources();
    CreatePipelineLayout();
    CreateFrameResources();
    CreateShadowResources();
    CreateSwapchain();

    initialized_ = true;
    LogInfo("Vulkan renderer initialized on adapter ", adapterName_);
}

void VulkanRenderer::Shutdown()
{
    if (device_ != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(device_);
    }

    DestroySwapchain();

    for (FrameResources& frame : frames_)
    {
        DestroyBuffer(frame.terrainVertexBuffer);
        DestroyBuffer(frame.translucentTerrainVertexBuffer);
        DestroyBuffer(frame.dynamicVertexBuffer);
        DestroyBuffer(frame.dynamicTranslucentVertexBuffer);
        DestroyBuffer(frame.effectVertexBuffer);
        DestroyBuffer(frame.lineVertexBuffer);
        DestroyBuffer(frame.overlayVertexBuffer);
        frame.uploadedTerrainMeshVersion = 0;

        if (frame.imageAvailable != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(device_, frame.imageAvailable, nullptr);
            frame.imageAvailable = VK_NULL_HANDLE;
        }

        if (frame.inFlight != VK_NULL_HANDLE)
        {
            vkDestroyFence(device_, frame.inFlight, nullptr);
            frame.inFlight = VK_NULL_HANDLE;
        }
    }

    if (pipelineLayout_ != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }

    DestroyShadowResources();
    DestroyDescriptorResources();

    if (commandPool_ != VK_NULL_HANDLE)
    {
        vkDestroyCommandPool(device_, commandPool_, nullptr);
        commandPool_ = VK_NULL_HANDLE;
    }

    if (device_ != VK_NULL_HANDLE)
    {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }

    if (surface_ != VK_NULL_HANDLE)
    {
        SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }

    DestroyDebugMessenger();

    if (instance_ != VK_NULL_HANDLE)
    {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }

    initialized_ = false;
    framebufferResized_ = false;
    validationEnabled_ = false;
    frameIndex_ = 0;
    graphicsQueue_ = VK_NULL_HANDLE;
    presentQueue_ = VK_NULL_HANDLE;
    physicalDevice_ = VK_NULL_HANDLE;
    window_ = nullptr;
    depthFormat_ = VK_FORMAT_UNDEFINED;
    adapterName_.clear();
}

void VulkanRenderer::Draw(const FrameRenderData& frameData)
{
    const ScopedProfileSection drawScope(profiler_, "Render Draw Internal");
    if (!initialized_)
    {
        return;
    }

    if (framebufferResized_ && !TryRecreateSwapchain())
    {
        return;
    }

    {
        const ScopedProfileSection scope(profiler_, "Render Wait Shared Images");
        std::array<VkFence, static_cast<std::size_t>(config::kMaxFramesInFlight)> frameFences{};
        for (std::size_t frameIndex = 0; frameIndex < frames_.size(); ++frameIndex)
        {
            frameFences[frameIndex] = frames_[frameIndex].inFlight;
        }
        CheckVk(
            vkWaitForFences(device_, static_cast<std::uint32_t>(frameFences.size()), frameFences.data(), VK_TRUE, UINT64_MAX),
            "Failed to wait for shared depth and shadow resources.");
    }

    FrameResources& frame = frames_[frameIndex_];
    {
        const ScopedProfileSection scope(profiler_, "Render Wait Fence");
        CheckVk(vkWaitForFences(device_, 1, &frame.inFlight, VK_TRUE, UINT64_MAX), "Failed to wait for an in-flight frame fence.");
    }

    std::uint32_t imageIndex = 0;
    VkResult acquireResult = VK_SUCCESS;
    {
        const ScopedProfileSection scope(profiler_, "Render Acquire");
        acquireResult = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, frame.imageAvailable, VK_NULL_HANDLE, &imageIndex);
    }
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
    {
        (void)TryRecreateSwapchain();
        return;
    }
    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR)
    {
        throw VulkanError("Failed to acquire the next swapchain image.", acquireResult);
    }

    if (imageIndex < swapchainImageFences_.size() &&
        swapchainImageFences_[imageIndex] != VK_NULL_HANDLE &&
        swapchainImageFences_[imageIndex] != frame.inFlight)
    {
        CheckVk(vkWaitForFences(device_, 1, &swapchainImageFences_[imageIndex], VK_TRUE, UINT64_MAX), "Failed to wait for the swapchain image fence.");
    }

    if (imageIndex < swapchainImageFences_.size())
    {
        swapchainImageFences_[imageIndex] = frame.inFlight;
    }

    auto uploadVertices = [this](BufferResource& buffer, const void* vertexData, const std::size_t vertexCount, const std::size_t vertexStride) {
        if (vertexCount == 0)
        {
            return;
        }

        const VkDeviceSize byteCount = static_cast<VkDeviceSize>(vertexCount * vertexStride);
        EnsureBufferCapacity(buffer, byteCount);
        std::memcpy(buffer.mapped, vertexData, static_cast<std::size_t>(byteCount));
    };

    {
        const ScopedProfileSection scope(profiler_, "Render Upload");
        if (frame.uploadedTerrainMeshVersion != frameData.terrainMeshVersion)
        {
            frame.uploadedTerrainMeshVersion = frameData.terrainMeshVersion;
            uploadVertices(frame.terrainVertexBuffer, frameData.terrainTriangles.data(), frameData.terrainTriangles.size(), sizeof(ColorVertex3D));
        }
        uploadVertices(frame.translucentTerrainVertexBuffer, frameData.translucentTerrainTriangles.data(), frameData.translucentTerrainTriangles.size(), sizeof(ColorVertex3D));
        uploadVertices(frame.dynamicVertexBuffer, frameData.dynamicTriangles.data(), frameData.dynamicTriangles.size(), sizeof(ColorVertex3D));
        uploadVertices(frame.dynamicTranslucentVertexBuffer, frameData.dynamicTranslucentTriangles.data(), frameData.dynamicTranslucentTriangles.size(), sizeof(ColorVertex3D));
        uploadVertices(frame.effectVertexBuffer, frameData.effectTriangles.data(), frameData.effectTriangles.size(), sizeof(ColorVertex3D));
        uploadVertices(frame.lineVertexBuffer, frameData.debugLines.data(), frameData.debugLines.size(), sizeof(ColorVertex3D));
        uploadVertices(frame.overlayVertexBuffer, frameData.overlayTriangles.data(), frameData.overlayTriangles.size(), sizeof(ColorVertex2D));
    }

    {
        const ScopedProfileSection scope(profiler_, "Render Record");
        CheckVk(vkResetFences(device_, 1, &frame.inFlight), "Failed to reset the frame fence.");
        CheckVk(vkResetCommandBuffer(frame.commandBuffer, 0), "Failed to reset the frame command buffer.");

        RecordFrame(frame.commandBuffer, imageIndex, frameData, frame);
    }
    VkSemaphore renderCompleteSemaphore = swapchainRenderCompleteSemaphores_[imageIndex];

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &frame.imageAvailable;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &frame.commandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &renderCompleteSemaphore;

    {
        const ScopedProfileSection scope(profiler_, "Render Submit");
        CheckVk(vkQueueSubmit(graphicsQueue_, 1, &submitInfo, frame.inFlight), "Failed to submit the graphics queue.");
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &renderCompleteSemaphore;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &imageIndex;

    VkResult presentResult = VK_SUCCESS;
    {
        const ScopedProfileSection scope(profiler_, "Render Present");
        presentResult = vkQueuePresentKHR(presentQueue_, &presentInfo);
    }
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR || framebufferResized_)
    {
        (void)TryRecreateSwapchain();
    }
    else if (presentResult != VK_SUCCESS)
    {
        throw VulkanError("Failed to present the swapchain image.", presentResult);
    }

    frameIndex_ = (frameIndex_ + 1) % config::kMaxFramesInFlight;
}

void VulkanRenderer::RequestResize()
{
    framebufferResized_ = true;
}

void VulkanRenderer::CreateInstance()
{
    const std::vector<const char*> extensions = GetRequiredInstanceExtensions();

    VkApplicationInfo applicationInfo{};
    applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    applicationInfo.pApplicationName = config::kApplicationName;
    applicationInfo.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    applicationInfo.pEngineName = config::kApplicationName;
    applicationInfo.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    applicationInfo.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &applicationInfo;
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    if (validationEnabled_)
    {
        createInfo.enabledLayerCount = 1;
        createInfo.ppEnabledLayerNames = &kValidationLayerName;
    }

    CheckVk(vkCreateInstance(&createInfo, nullptr, &instance_), "Failed to create the Vulkan instance.");
}

void VulkanRenderer::CreateDebugMessenger()
{
    if (!validationEnabled_)
    {
        return;
    }

    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = &DebugCallback;

    const auto createDebugUtilsMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
    if (createDebugUtilsMessenger == nullptr)
    {
        LogWarning("vkCreateDebugUtilsMessengerEXT was unavailable even though validation is enabled.");
        return;
    }

    CheckVk(createDebugUtilsMessenger(instance_, &createInfo, nullptr, &debugMessenger_), "Failed to create the Vulkan debug messenger.");
}

void VulkanRenderer::DestroyDebugMessenger()
{
    if (debugMessenger_ == VK_NULL_HANDLE || instance_ == VK_NULL_HANDLE)
    {
        return;
    }

    const auto destroyDebugUtilsMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
    if (destroyDebugUtilsMessenger != nullptr)
    {
        destroyDebugUtilsMessenger(instance_, debugMessenger_, nullptr);
    }

    debugMessenger_ = VK_NULL_HANDLE;
}

void VulkanRenderer::CreateSurface()
{
    if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_))
    {
        throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError());
    }
}

void VulkanRenderer::PickPhysicalDevice()
{
    std::uint32_t deviceCount = 0;
    CheckVk(vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr), "Failed to enumerate Vulkan physical devices.");
    if (deviceCount == 0)
    {
        throw std::runtime_error("No Vulkan physical devices were found.");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    CheckVk(vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data()), "Failed to enumerate Vulkan physical devices.");

    for (const VkPhysicalDevice device : devices)
    {
        const QueueFamilySelection queueFamilies = FindQueueFamilies(device);
        if (!queueFamilies.Complete())
        {
            continue;
        }

        const SwapchainSupportDetails swapchainSupport = QuerySwapchainSupport(device, surface_);
        if (swapchainSupport.formats.empty() || swapchainSupport.presentModes.empty())
        {
            continue;
        }

        std::uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data());

        const auto hasSwapchainExtension = std::find_if(extensions.begin(), extensions.end(), [](const VkExtensionProperties& extension) {
            return std::string_view(extension.extensionName) == VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        });
        if (hasSwapchainExtension == extensions.end())
        {
            continue;
        }

        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = &features13;
        vkGetPhysicalDeviceFeatures2(device, &features2);

        if (!features13.dynamicRendering)
        {
            continue;
        }

        physicalDevice_ = device;
        graphicsQueueFamily_ = queueFamilies.graphics.value();
        presentQueueFamily_ = queueFamilies.present.value();

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        adapterName_ = properties.deviceName;
        depthFormat_ = ChooseDepthFormat();
        return;
    }

    throw std::runtime_error("No Vulkan device matched the renderer requirements.");
}

void VulkanRenderer::CreateDevice()
{
    const float queuePriority = 1.0f;

    std::vector<std::uint32_t> queueFamilies{graphicsQueueFamily_};
    if (presentQueueFamily_ != graphicsQueueFamily_)
    {
        queueFamilies.push_back(presentQueueFamily_);
    }

    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    queueCreateInfos.reserve(queueFamilies.size());

    for (const std::uint32_t queueFamily : queueFamilies)
    {
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = queueFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }

    VkPhysicalDeviceFeatures deviceFeatures{};
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;

    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext = &features13;
    createInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &deviceFeatures;
    createInfo.enabledExtensionCount = 1;
    createInfo.ppEnabledExtensionNames = deviceExtensions;

    CheckVk(vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_), "Failed to create the Vulkan device.");
    vkGetDeviceQueue(device_, graphicsQueueFamily_, 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, presentQueueFamily_, 0, &presentQueue_);
}

void VulkanRenderer::CreateCommandPool()
{
    VkCommandPoolCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    createInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    createInfo.queueFamilyIndex = graphicsQueueFamily_;

    CheckVk(vkCreateCommandPool(device_, &createInfo, nullptr, &commandPool_), "Failed to create the Vulkan command pool.");
}

void VulkanRenderer::CreateDescriptorResources()
{
    VkDescriptorSetLayoutBinding shadowBinding{};
    shadowBinding.binding = 0;
    shadowBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    shadowBinding.descriptorCount = 1;
    shadowBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &shadowBinding;
    CheckVk(vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &descriptorSetLayout_), "Failed to create the shadow descriptor set layout.");

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    CheckVk(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_), "Failed to create the shadow descriptor pool.");

    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = descriptorPool_;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &descriptorSetLayout_;
    CheckVk(vkAllocateDescriptorSets(device_, &allocateInfo, &shadowDescriptorSet_), "Failed to allocate the shadow descriptor set.");

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    samplerInfo.maxAnisotropy = 1.0f;
    CheckVk(vkCreateSampler(device_, &samplerInfo, nullptr, &shadowSampler_), "Failed to create the shadow sampler.");
}

void VulkanRenderer::DestroyDescriptorResources()
{
    if (shadowSampler_ != VK_NULL_HANDLE)
    {
        vkDestroySampler(device_, shadowSampler_, nullptr);
        shadowSampler_ = VK_NULL_HANDLE;
    }

    shadowDescriptorSet_ = VK_NULL_HANDLE;

    if (descriptorPool_ != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
    }

    if (descriptorSetLayout_ != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
        descriptorSetLayout_ = VK_NULL_HANDLE;
    }
}

void VulkanRenderer::CreateShadowResources()
{
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = kShadowMapSize;
    imageInfo.extent.height = kShadowMapSize;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = depthFormat_;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CheckVk(vkCreateImage(device_, &imageInfo, nullptr, &shadowImage_.image), "Failed to create the shadow depth image.");

    VkMemoryRequirements memoryRequirements{};
    vkGetImageMemoryRequirements(device_, shadowImage_.image, &memoryRequirements);

    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = memoryRequirements.size;
    allocateInfo.memoryTypeIndex = FindMemoryType(memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CheckVk(vkAllocateMemory(device_, &allocateInfo, nullptr, &shadowImage_.memory), "Failed to allocate shadow depth image memory.");
    CheckVk(vkBindImageMemory(device_, shadowImage_.image, shadowImage_.memory, 0), "Failed to bind the shadow depth image memory.");

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = shadowImage_.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = depthFormat_;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    CheckVk(vkCreateImageView(device_, &viewInfo, nullptr, &shadowImage_.view), "Failed to create the shadow depth image view.");

    shadowImage_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    UpdateShadowDescriptorSet();
}

void VulkanRenderer::DestroyShadowResources()
{
    if (shadowImage_.view != VK_NULL_HANDLE)
    {
        vkDestroyImageView(device_, shadowImage_.view, nullptr);
        shadowImage_.view = VK_NULL_HANDLE;
    }

    if (shadowImage_.image != VK_NULL_HANDLE)
    {
        vkDestroyImage(device_, shadowImage_.image, nullptr);
        shadowImage_.image = VK_NULL_HANDLE;
    }

    if (shadowImage_.memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(device_, shadowImage_.memory, nullptr);
        shadowImage_.memory = VK_NULL_HANDLE;
    }

    shadowImage_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

void VulkanRenderer::UpdateShadowDescriptorSet()
{
    if (shadowDescriptorSet_ == VK_NULL_HANDLE || shadowSampler_ == VK_NULL_HANDLE || shadowImage_.view == VK_NULL_HANDLE)
    {
        return;
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = shadowSampler_;
    imageInfo.imageView = shadowImage_.view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = shadowDescriptorSet_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

void VulkanRenderer::CreatePipelineLayout()
{
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(ScenePushConstants);

    VkPipelineLayoutCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    createInfo.setLayoutCount = 1;
    createInfo.pSetLayouts = &descriptorSetLayout_;
    createInfo.pushConstantRangeCount = 1;
    createInfo.pPushConstantRanges = &pushConstantRange;

    CheckVk(vkCreatePipelineLayout(device_, &createInfo, nullptr, &pipelineLayout_), "Failed to create the Vulkan pipeline layout.");
}

void VulkanRenderer::CreateFrameResources()
{
    for (FrameResources& frame : frames_)
    {
        VkCommandBufferAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateInfo.commandPool = commandPool_;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        CheckVk(vkAllocateCommandBuffers(device_, &allocateInfo, &frame.commandBuffer), "Failed to allocate a frame command buffer.");

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        CheckVk(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &frame.imageAvailable), "Failed to create the image-available semaphore.");

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        CheckVk(vkCreateFence(device_, &fenceInfo, nullptr, &frame.inFlight), "Failed to create the frame fence.");
    }
}

void VulkanRenderer::CreateSwapchain()
{
    const SwapchainSupportDetails support = QuerySwapchainSupport(physicalDevice_, surface_);
    if (support.formats.empty() || support.presentModes.empty())
    {
        throw std::runtime_error("The selected Vulkan device does not expose swapchain support for this window.");
    }

    const VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat(support.formats);
    const VkPresentModeKHR presentMode = ChoosePresentMode(support.presentModes);
    const VkExtent2D extent = ChooseExtent(window_, support.capabilities);

    std::uint32_t imageCount = support.capabilities.minImageCount + 1;
    if (support.capabilities.maxImageCount > 0)
    {
        imageCount = std::min(imageCount, support.capabilities.maxImageCount);
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface_;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.preTransform = support.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;

    const std::uint32_t queueFamilyIndices[] = {graphicsQueueFamily_, presentQueueFamily_};
    if (graphicsQueueFamily_ != presentQueueFamily_)
    {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndices;
    }
    else
    {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    CheckVk(vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_), "Failed to create the Vulkan swapchain.");

    std::uint32_t actualImageCount = 0;
    CheckVk(vkGetSwapchainImagesKHR(device_, swapchain_, &actualImageCount, nullptr), "Failed to query swapchain image count.");
    swapchainImages_.resize(actualImageCount);
    CheckVk(vkGetSwapchainImagesKHR(device_, swapchain_, &actualImageCount, swapchainImages_.data()), "Failed to fetch swapchain images.");

    swapchainFormat_ = surfaceFormat.format;
    swapchainExtent_ = extent;

    swapchainImageViews_.resize(swapchainImages_.size());
    for (std::size_t imageIndex = 0; imageIndex < swapchainImages_.size(); ++imageIndex)
    {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = swapchainImages_[imageIndex];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = swapchainFormat_;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        CheckVk(vkCreateImageView(device_, &viewInfo, nullptr, &swapchainImageViews_[imageIndex]), "Failed to create a swapchain image view.");
    }

    swapchainRenderCompleteSemaphores_.resize(swapchainImages_.size(), VK_NULL_HANDLE);
    swapchainImageFences_.resize(swapchainImages_.size(), VK_NULL_HANDLE);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (VkSemaphore& semaphore : swapchainRenderCompleteSemaphores_)
    {
        CheckVk(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &semaphore), "Failed to create a per-image render-complete semaphore.");
    }

    CreateDepthResources();
    CreatePipelines();
}

void VulkanRenderer::CreateDepthResources()
{
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = swapchainExtent_.width;
    imageInfo.extent.height = swapchainExtent_.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = depthFormat_;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    CheckVk(vkCreateImage(device_, &imageInfo, nullptr, &depthImage_.image), "Failed to create the depth image.");

    VkMemoryRequirements memoryRequirements{};
    vkGetImageMemoryRequirements(device_, depthImage_.image, &memoryRequirements);

    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = memoryRequirements.size;
    allocateInfo.memoryTypeIndex = FindMemoryType(memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CheckVk(vkAllocateMemory(device_, &allocateInfo, nullptr, &depthImage_.memory), "Failed to allocate depth image memory.");
    CheckVk(vkBindImageMemory(device_, depthImage_.image, depthImage_.memory, 0), "Failed to bind depth image memory.");

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = depthImage_.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = depthFormat_;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    CheckVk(vkCreateImageView(device_, &viewInfo, nullptr, &depthImage_.view), "Failed to create the depth image view.");

    depthImage_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

void VulkanRenderer::DestroyDepthResources()
{
    if (depthImage_.view != VK_NULL_HANDLE)
    {
        vkDestroyImageView(device_, depthImage_.view, nullptr);
        depthImage_.view = VK_NULL_HANDLE;
    }

    if (depthImage_.image != VK_NULL_HANDLE)
    {
        vkDestroyImage(device_, depthImage_.image, nullptr);
        depthImage_.image = VK_NULL_HANDLE;
    }

    if (depthImage_.memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(device_, depthImage_.memory, nullptr);
        depthImage_.memory = VK_NULL_HANDLE;
    }

    depthImage_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

void VulkanRenderer::DestroySwapchain()
{
    DestroyPipelines();
    DestroyDepthResources();

    for (VkSemaphore semaphore : swapchainRenderCompleteSemaphores_)
    {
        if (semaphore != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(device_, semaphore, nullptr);
        }
    }

    swapchainRenderCompleteSemaphores_.clear();
    swapchainImageFences_.clear();

    for (VkImageView imageView : swapchainImageViews_)
    {
        if (imageView != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device_, imageView, nullptr);
        }
    }

    swapchainImageViews_.clear();
    swapchainImages_.clear();
    swapchainExtent_ = {};
    swapchainFormat_ = VK_FORMAT_UNDEFINED;

    if (swapchain_ != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

bool VulkanRenderer::TryRecreateSwapchain()
{
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window_, &width, &height);
    if (width <= 0 || height <= 0)
    {
        return false;
    }

    vkDeviceWaitIdle(device_);
    DestroySwapchain();
    CreateSwapchain();
    framebufferResized_ = false;
    return true;
}

void VulkanRenderer::CreatePipelines()
{
    const VkShaderModule worldVertexShader = LoadShaderModule("world_3d.vert.spv");
    const VkShaderModule worldFragmentShader = LoadShaderModule("world_3d.frag.spv");
    const VkShaderModule unshadowedWorldFragmentShader = LoadShaderModule("world_unshadowed.frag.spv");
    const VkShaderModule shadowVertexShader = LoadShaderModule("shadow_depth.vert.spv");
    const VkShaderModule overlayVertexShader = LoadShaderModule("overlay_2d.vert.spv");
    const VkShaderModule overlayFragmentShader = LoadShaderModule("overlay_2d.frag.spv");

    const std::array<VkDynamicState, 2> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizationState{};
    rasterizationState.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizationState.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizationState.cullMode = VK_CULL_MODE_NONE;
    rasterizationState.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizationState.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampleState{};
    multisampleState.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampleState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState opaqueColorBlendAttachment{};
    opaqueColorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT |
        VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendAttachmentState alphaBlendAttachment = opaqueColorBlendAttachment;
    alphaBlendAttachment.blendEnable = VK_TRUE;
    alphaBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    alphaBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    alphaBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    alphaBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    alphaBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    alphaBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo opaqueBlendState{};
    opaqueBlendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    opaqueBlendState.attachmentCount = 1;
    opaqueBlendState.pAttachments = &opaqueColorBlendAttachment;

    VkPipelineColorBlendStateCreateInfo alphaBlendState{};
    alphaBlendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    alphaBlendState.attachmentCount = 1;
    alphaBlendState.pAttachments = &alphaBlendAttachment;

    VkPipelineColorBlendStateCreateInfo shadowBlendState{};
    shadowBlendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;

    VkPipelineDepthStencilStateCreateInfo terrainDepthState{};
    terrainDepthState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    terrainDepthState.depthTestEnable = VK_TRUE;
    terrainDepthState.depthWriteEnable = VK_TRUE;
    terrainDepthState.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineDepthStencilStateCreateInfo lineDepthState = terrainDepthState;
    lineDepthState.depthWriteEnable = VK_FALSE;

    VkPipelineDepthStencilStateCreateInfo translucentTerrainDepthState = terrainDepthState;
    translucentTerrainDepthState.depthWriteEnable = VK_FALSE;

    VkPipelineDepthStencilStateCreateInfo overlayDepthState{};
    overlayDepthState.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

    VkPipelineDepthStencilStateCreateInfo shadowDepthState = terrainDepthState;

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPipelineRenderingCreateInfo sceneRenderingInfo{};
    sceneRenderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    sceneRenderingInfo.colorAttachmentCount = 1;
    sceneRenderingInfo.pColorAttachmentFormats = &swapchainFormat_;
    sceneRenderingInfo.depthAttachmentFormat = depthFormat_;

    VkPipelineRenderingCreateInfo shadowRenderingInfo{};
    shadowRenderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    shadowRenderingInfo.colorAttachmentCount = 0;
    shadowRenderingInfo.pColorAttachmentFormats = nullptr;
    shadowRenderingInfo.depthAttachmentFormat = depthFormat_;

    auto createPipeline = [this, &viewportState, &rasterizationState, &multisampleState, &dynamicState](
                              const VkShaderModule vertexShader,
                              const VkShaderModule fragmentShader,
                              const VkPrimitiveTopology topology,
                              const VkVertexInputBindingDescription& bindingDescription,
                              const VkVertexInputAttributeDescription* attributes,
                              const std::uint32_t attributeCount,
                              const VkPipelineColorBlendStateCreateInfo& blendState,
                              const VkPipelineDepthStencilStateCreateInfo& depthState,
                              const VkPipelineRenderingCreateInfo& renderingInfo) -> VkPipeline {
        std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages = {{
            VkPipelineShaderStageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_VERTEX_BIT,
                .module = vertexShader,
                .pName = "main",
            },
            VkPipelineShaderStageCreateInfo{},
        }};
        std::uint32_t shaderStageCount = 1;
        if (fragmentShader != VK_NULL_HANDLE)
        {
            shaderStages[1] = VkPipelineShaderStageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = fragmentShader,
                .pName = "main",
            };
            shaderStageCount = 2;
        }

        VkPipelineVertexInputStateCreateInfo vertexInputState{};
        vertexInputState.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInputState.vertexBindingDescriptionCount = 1;
        vertexInputState.pVertexBindingDescriptions = &bindingDescription;
        vertexInputState.vertexAttributeDescriptionCount = attributeCount;
        vertexInputState.pVertexAttributeDescriptions = attributes;

        VkPipelineInputAssemblyStateCreateInfo inputAssemblyState{};
        inputAssemblyState.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssemblyState.topology = topology;

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.pNext = &renderingInfo;
        pipelineInfo.stageCount = shaderStageCount;
        pipelineInfo.pStages = shaderStages.data();
        pipelineInfo.pVertexInputState = &vertexInputState;
        pipelineInfo.pInputAssemblyState = &inputAssemblyState;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizationState;
        pipelineInfo.pMultisampleState = &multisampleState;
        pipelineInfo.pDepthStencilState = &depthState;
        pipelineInfo.pColorBlendState = &blendState;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = pipelineLayout_;

        VkPipeline pipeline = VK_NULL_HANDLE;
        CheckVk(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline), "Failed to create a graphics pipeline.");
        return pipeline;
    };

    const VkVertexInputBindingDescription worldBindingDescription{0, sizeof(ColorVertex3D), VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array<VkVertexInputAttributeDescription, 2> worldAttributes = {{
        VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<std::uint32_t>(offsetof(ColorVertex3D, position))},
        VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, static_cast<std::uint32_t>(offsetof(ColorVertex3D, color))},
    }};

    const VkVertexInputBindingDescription overlayBindingDescription{0, sizeof(ColorVertex2D), VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array<VkVertexInputAttributeDescription, 2> overlayAttributes = {{
        VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<std::uint32_t>(offsetof(ColorVertex2D, clipPosition))},
        VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, static_cast<std::uint32_t>(offsetof(ColorVertex2D, color))},
    }};

    shadowPipeline_ = createPipeline(shadowVertexShader, VK_NULL_HANDLE, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, worldBindingDescription, worldAttributes.data(), static_cast<std::uint32_t>(worldAttributes.size()), shadowBlendState, shadowDepthState, shadowRenderingInfo);
    terrainPipeline_ = createPipeline(worldVertexShader, worldFragmentShader, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, worldBindingDescription, worldAttributes.data(), static_cast<std::uint32_t>(worldAttributes.size()), opaqueBlendState, terrainDepthState, sceneRenderingInfo);
    translucentTerrainPipeline_ = createPipeline(worldVertexShader, worldFragmentShader, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, worldBindingDescription, worldAttributes.data(), static_cast<std::uint32_t>(worldAttributes.size()), alphaBlendState, translucentTerrainDepthState, sceneRenderingInfo);
    effectPipeline_ = createPipeline(worldVertexShader, unshadowedWorldFragmentShader, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, worldBindingDescription, worldAttributes.data(), static_cast<std::uint32_t>(worldAttributes.size()), opaqueBlendState, terrainDepthState, sceneRenderingInfo);
    linePipeline_ = createPipeline(worldVertexShader, unshadowedWorldFragmentShader, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, worldBindingDescription, worldAttributes.data(), static_cast<std::uint32_t>(worldAttributes.size()), alphaBlendState, lineDepthState, sceneRenderingInfo);
    overlayPipeline_ = createPipeline(overlayVertexShader, overlayFragmentShader, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, overlayBindingDescription, overlayAttributes.data(), static_cast<std::uint32_t>(overlayAttributes.size()), alphaBlendState, overlayDepthState, sceneRenderingInfo);

    vkDestroyShaderModule(device_, shadowVertexShader, nullptr);
    vkDestroyShaderModule(device_, worldVertexShader, nullptr);
    vkDestroyShaderModule(device_, worldFragmentShader, nullptr);
    vkDestroyShaderModule(device_, unshadowedWorldFragmentShader, nullptr);
    vkDestroyShaderModule(device_, overlayVertexShader, nullptr);
    vkDestroyShaderModule(device_, overlayFragmentShader, nullptr);
}

void VulkanRenderer::DestroyPipelines()
{
    if (shadowPipeline_ != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(device_, shadowPipeline_, nullptr);
        shadowPipeline_ = VK_NULL_HANDLE;
    }

    if (terrainPipeline_ != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(device_, terrainPipeline_, nullptr);
        terrainPipeline_ = VK_NULL_HANDLE;
    }

    if (translucentTerrainPipeline_ != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(device_, translucentTerrainPipeline_, nullptr);
        translucentTerrainPipeline_ = VK_NULL_HANDLE;
    }

    if (effectPipeline_ != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(device_, effectPipeline_, nullptr);
        effectPipeline_ = VK_NULL_HANDLE;
    }

    if (linePipeline_ != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(device_, linePipeline_, nullptr);
        linePipeline_ = VK_NULL_HANDLE;
    }

    if (overlayPipeline_ != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(device_, overlayPipeline_, nullptr);
        overlayPipeline_ = VK_NULL_HANDLE;
    }
}

void VulkanRenderer::RecordFrame(
    VkCommandBuffer commandBuffer,
    const std::uint32_t imageIndex,
    const FrameRenderData& frameData,
    const FrameResources& frame)
{
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CheckVk(vkBeginCommandBuffer(commandBuffer, &beginInfo), "Failed to begin recording the frame command buffer.");

    const Mat4 worldToShadowClip = BuildShadowProjection(frameData);
    const ScenePushConstants pushConstants{
        frameData.worldToClip,
        worldToShadowClip,
    };

    if (shadowImage_.layout == VK_IMAGE_LAYOUT_UNDEFINED)
    {
        TransitionImage(commandBuffer, shadowImage_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    }
    else
    {
        TransitionImage(commandBuffer, shadowImage_.image, VK_IMAGE_ASPECT_DEPTH_BIT, shadowImage_.layout, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    }
    shadowImage_.layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;

    VkClearValue shadowDepthClearValue{};
    shadowDepthClearValue.depthStencil.depth = 1.0f;

    VkRenderingAttachmentInfo shadowDepthAttachment{};
    shadowDepthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    shadowDepthAttachment.imageView = shadowImage_.view;
    shadowDepthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    shadowDepthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    shadowDepthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    shadowDepthAttachment.clearValue = shadowDepthClearValue;

    VkRenderingInfo shadowRenderingInfo{};
    shadowRenderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    shadowRenderingInfo.renderArea.extent = VkExtent2D{kShadowMapSize, kShadowMapSize};
    shadowRenderingInfo.layerCount = 1;
    shadowRenderingInfo.colorAttachmentCount = 0;
    shadowRenderingInfo.pDepthAttachment = &shadowDepthAttachment;

    vkCmdBeginRendering(commandBuffer, &shadowRenderingInfo);

    const VkViewport shadowViewport{
        0.0f,
        0.0f,
        static_cast<float>(kShadowMapSize),
        static_cast<float>(kShadowMapSize),
        0.0f,
        1.0f,
    };
    const VkRect2D shadowScissor{{0, 0}, {kShadowMapSize, kShadowMapSize}};
    vkCmdSetViewport(commandBuffer, 0, 1, &shadowViewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &shadowScissor);

    constexpr VkDeviceSize bufferOffset = 0;
    if (!frameData.terrainTriangles.empty())
    {
        const VkBuffer vertexBuffer = frame.terrainVertexBuffer.buffer;
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);
        vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4), &worldToShadowClip);
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &bufferOffset);
        vkCmdDraw(commandBuffer, static_cast<std::uint32_t>(frameData.terrainTriangles.size()), 1, 0, 0);
    }

    if (!frameData.dynamicTriangles.empty())
    {
        const VkBuffer vertexBuffer = frame.dynamicVertexBuffer.buffer;
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);
        vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4), &worldToShadowClip);
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &bufferOffset);
        vkCmdDraw(commandBuffer, static_cast<std::uint32_t>(frameData.dynamicTriangles.size()), 1, 0, 0);
    }

    vkCmdEndRendering(commandBuffer);

    TransitionImage(commandBuffer, shadowImage_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    shadowImage_.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    TransitionSwapchainImage(commandBuffer, swapchainImages_[imageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    if (depthImage_.layout == VK_IMAGE_LAYOUT_UNDEFINED)
    {
        TransitionImage(commandBuffer, depthImage_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
        depthImage_.layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    }

    VkClearValue clearColorValue{};
    clearColorValue.color.float32[0] = frameData.clearColor.x;
    clearColorValue.color.float32[1] = frameData.clearColor.y;
    clearColorValue.color.float32[2] = frameData.clearColor.z;
    clearColorValue.color.float32[3] = frameData.clearColor.w;

    VkClearValue depthClearValue{};
    depthClearValue.depthStencil.depth = 1.0f;

    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = swapchainImageViews_[imageIndex];
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue = clearColorValue;

    VkRenderingAttachmentInfo depthAttachment{};
    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttachment.imageView = depthImage_.view;
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.clearValue = depthClearValue;

    VkRenderingInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderingInfo.renderArea.extent = swapchainExtent_;
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;
    renderingInfo.pDepthAttachment = &depthAttachment;

    vkCmdBeginRendering(commandBuffer, &renderingInfo);

    const VkViewport viewport{
        0.0f,
        0.0f,
        static_cast<float>(swapchainExtent_.width),
        static_cast<float>(swapchainExtent_.height),
        0.0f,
        1.0f,
    };
    const VkRect2D scissor{{0, 0}, swapchainExtent_};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &shadowDescriptorSet_, 0, nullptr);

    if (!frameData.terrainTriangles.empty())
    {
        const VkBuffer vertexBuffer = frame.terrainVertexBuffer.buffer;
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, terrainPipeline_);
        vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ScenePushConstants), &pushConstants);
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &bufferOffset);
        vkCmdDraw(commandBuffer, static_cast<std::uint32_t>(frameData.terrainTriangles.size()), 1, 0, 0);
    }

    if (!frameData.dynamicTriangles.empty())
    {
        const VkBuffer vertexBuffer = frame.dynamicVertexBuffer.buffer;
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, terrainPipeline_);
        vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ScenePushConstants), &pushConstants);
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &bufferOffset);
        vkCmdDraw(commandBuffer, static_cast<std::uint32_t>(frameData.dynamicTriangles.size()), 1, 0, 0);
    }

    if (!frameData.translucentTerrainTriangles.empty())
    {
        const VkBuffer vertexBuffer = frame.translucentTerrainVertexBuffer.buffer;
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, translucentTerrainPipeline_);
        vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ScenePushConstants), &pushConstants);
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &bufferOffset);
        vkCmdDraw(commandBuffer, static_cast<std::uint32_t>(frameData.translucentTerrainTriangles.size()), 1, 0, 0);
    }

    if (!frameData.dynamicTranslucentTriangles.empty())
    {
        const VkBuffer vertexBuffer = frame.dynamicTranslucentVertexBuffer.buffer;
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, translucentTerrainPipeline_);
        vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ScenePushConstants), &pushConstants);
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &bufferOffset);
        vkCmdDraw(commandBuffer, static_cast<std::uint32_t>(frameData.dynamicTranslucentTriangles.size()), 1, 0, 0);
    }

    if (!frameData.effectTriangles.empty())
    {
        const VkBuffer vertexBuffer = frame.effectVertexBuffer.buffer;
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, effectPipeline_);
        vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ScenePushConstants), &pushConstants);
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &bufferOffset);
        vkCmdDraw(commandBuffer, static_cast<std::uint32_t>(frameData.effectTriangles.size()), 1, 0, 0);
    }

    if (!frameData.debugLines.empty())
    {
        const VkBuffer vertexBuffer = frame.lineVertexBuffer.buffer;
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, linePipeline_);
        vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ScenePushConstants), &pushConstants);
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &bufferOffset);
        vkCmdDraw(commandBuffer, static_cast<std::uint32_t>(frameData.debugLines.size()), 1, 0, 0);
    }

    if (!frameData.overlayTriangles.empty())
    {
        const VkBuffer vertexBuffer = frame.overlayVertexBuffer.buffer;
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, overlayPipeline_);
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &bufferOffset);
        vkCmdDraw(commandBuffer, static_cast<std::uint32_t>(frameData.overlayTriangles.size()), 1, 0, 0);
    }

    vkCmdEndRendering(commandBuffer);

    TransitionSwapchainImage(commandBuffer, swapchainImages_[imageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    CheckVk(vkEndCommandBuffer(commandBuffer), "Failed to finish recording the frame command buffer.");
}

void VulkanRenderer::TransitionSwapchainImage(
    VkCommandBuffer commandBuffer,
    VkImage image,
    const VkImageLayout oldLayout,
    const VkImageLayout newLayout) const
{
    TransitionImage(commandBuffer, image, VK_IMAGE_ASPECT_COLOR_BIT, oldLayout, newLayout);
}

void VulkanRenderer::TransitionImage(
    VkCommandBuffer commandBuffer,
    VkImage image,
    const VkImageAspectFlags aspectMask,
    const VkImageLayout oldLayout,
    const VkImageLayout newLayout) const
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspectMask;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags destinationStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
    {
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        destinationStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    else if (oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
    {
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        destinationStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    }
    else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL)
    {
        barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        destinationStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    }
    else if (oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL)
    {
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        destinationStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    }
    else if (oldLayout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)
    {
        barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    else
    {
        DF_UNREACHABLE("Unsupported image transition.");
    }

    vkCmdPipelineBarrier(
        commandBuffer,
        sourceStage,
        destinationStage,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barrier);
}

std::uint32_t VulkanRenderer::FindMemoryType(const std::uint32_t typeFilter, const VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties);

    for (std::uint32_t memoryTypeIndex = 0; memoryTypeIndex < memoryProperties.memoryTypeCount; ++memoryTypeIndex)
    {
        const bool typeSupported = (typeFilter & (1u << memoryTypeIndex)) != 0u;
        const bool propertiesMatch = (memoryProperties.memoryTypes[memoryTypeIndex].propertyFlags & properties) == properties;
        if (typeSupported && propertiesMatch)
        {
            return memoryTypeIndex;
        }
    }

    throw std::runtime_error("Failed to find a compatible Vulkan memory type.");
}

VkFormat VulkanRenderer::ChooseDepthFormat() const
{
    constexpr std::array<VkFormat, 2> candidates = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D16_UNORM,
    };

    for (const VkFormat format : candidates)
    {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice_, format, &properties);
        if ((properties.optimalTilingFeatures & kRequiredShadowDepthFormatFeatures) == kRequiredShadowDepthFormatFeatures)
        {
            return format;
        }
    }

    throw std::runtime_error("The selected Vulkan device did not expose a depth format that can be both rendered to and sampled for shadows.");
}

VkShaderModule VulkanRenderer::LoadShaderModule(const std::string_view filename) const
{
    std::filesystem::path shaderPath;

    if (const char* basePath = SDL_GetBasePath())
    {
        shaderPath = std::filesystem::path(basePath) / "shaders" / filename;
    }
    else
    {
        shaderPath = std::filesystem::current_path() / "shaders" / filename;
    }

    if (!std::filesystem::exists(shaderPath))
    {
        const std::filesystem::path fallbackPath = std::filesystem::current_path() / "shaders" / filename;
        if (std::filesystem::exists(fallbackPath))
        {
            shaderPath = fallbackPath;
        }
    }

    return CreateShaderModule(device_, ReadBinaryFile(shaderPath));
}

void VulkanRenderer::EnsureBufferCapacity(BufferResource& buffer, const VkDeviceSize minimumSize)
{
    if (minimumSize == 0)
    {
        return;
    }

    if (buffer.buffer != VK_NULL_HANDLE && buffer.capacity >= minimumSize)
    {
        return;
    }

    DestroyBuffer(buffer);

    const VkDeviceSize capacity = RoundUpCapacity(minimumSize);

    VkBufferCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    createInfo.size = capacity;
    createInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    CheckVk(vkCreateBuffer(device_, &createInfo, nullptr, &buffer.buffer), "Failed to create a vertex buffer.");

    VkMemoryRequirements memoryRequirements{};
    vkGetBufferMemoryRequirements(device_, buffer.buffer, &memoryRequirements);

    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = memoryRequirements.size;
    allocateInfo.memoryTypeIndex = FindMemoryType(
        memoryRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    CheckVk(vkAllocateMemory(device_, &allocateInfo, nullptr, &buffer.memory), "Failed to allocate vertex buffer memory.");
    CheckVk(vkBindBufferMemory(device_, buffer.buffer, buffer.memory, 0), "Failed to bind vertex buffer memory.");
    CheckVk(vkMapMemory(device_, buffer.memory, 0, capacity, 0, &buffer.mapped), "Failed to map vertex buffer memory.");

    buffer.capacity = capacity;
    buffer.hostCoherent = true;
}

void VulkanRenderer::DestroyBuffer(BufferResource& buffer)
{
    if (buffer.mapped != nullptr)
    {
        vkUnmapMemory(device_, buffer.memory);
        buffer.mapped = nullptr;
    }

    if (buffer.buffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(device_, buffer.buffer, nullptr);
        buffer.buffer = VK_NULL_HANDLE;
    }

    if (buffer.memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(device_, buffer.memory, nullptr);
        buffer.memory = VK_NULL_HANDLE;
    }

    buffer.capacity = 0;
    buffer.hostCoherent = false;
}

std::vector<const char*> VulkanRenderer::GetRequiredInstanceExtensions() const
{
    std::uint32_t extensionCount = 0;
    const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
    if (sdlExtensions == nullptr)
    {
        throw std::runtime_error(std::string("SDL_Vulkan_GetInstanceExtensions failed: ") + SDL_GetError());
    }

    std::vector<const char*> extensions(sdlExtensions, sdlExtensions + extensionCount);
    if (validationEnabled_)
    {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    return extensions;
}

VulkanRenderer::QueueFamilySelection VulkanRenderer::FindQueueFamilies(const VkPhysicalDevice device) const
{
    QueueFamilySelection selection{};

    std::uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

    for (std::uint32_t familyIndex = 0; familyIndex < queueFamilyCount; ++familyIndex)
    {
        const VkQueueFamilyProperties& queueFamily = queueFamilies[familyIndex];
        if (queueFamily.queueCount > 0 && (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
        {
            selection.graphics = familyIndex;
        }

        VkBool32 supportsPresent = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, familyIndex, surface_, &supportsPresent);
        if (queueFamily.queueCount > 0 && supportsPresent == VK_TRUE)
        {
            selection.present = familyIndex;
        }

        if (selection.Complete())
        {
            break;
        }
    }

    return selection;
}

bool VulkanRenderer::SupportsValidationLayer() const
{
    std::uint32_t layerCount = 0;
    CheckVk(vkEnumerateInstanceLayerProperties(&layerCount, nullptr), "Failed to enumerate Vulkan instance layers.");
    std::vector<VkLayerProperties> layers(layerCount);
    CheckVk(vkEnumerateInstanceLayerProperties(&layerCount, layers.data()), "Failed to enumerate Vulkan instance layers.");

    const auto found = std::find_if(layers.begin(), layers.end(), [](const VkLayerProperties& layer) {
        return std::string_view(layer.layerName) == kValidationLayerName;
    });

    if (found == layers.end())
    {
        LogWarning("Vulkan validation layer ", kValidationLayerName, " is not installed. Continuing without validation.");
        return false;
    }

    return true;
}
}
