#pragma once

#include "core/profiler.hpp"
#include "core/config.hpp"
#include "render/frame_data.hpp"

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace df::render
{
class VulkanRenderer
{
public:
    VulkanRenderer() = default;
    ~VulkanRenderer();

    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    void Initialize(SDL_Window* window);
    void Shutdown();
    void Draw(const FrameRenderData& frameData);
    void RequestResize();
    void SetFrameProfiler(FrameProfiler* profiler)
    {
        profiler_ = profiler;
    }

    [[nodiscard]] bool IsReady() const
    {
        return initialized_;
    }

    [[nodiscard]] const std::string& AdapterName() const
    {
        return adapterName_;
    }

private:
    struct BufferResource
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize capacity = 0;
        void* mapped = nullptr;
        bool hostCoherent = false;
    };

    struct ImageResource
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    struct FrameResources
    {
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
        BufferResource terrainVertexBuffer{};
        BufferResource translucentTerrainVertexBuffer{};
        BufferResource dynamicVertexBuffer{};
        BufferResource dynamicTranslucentVertexBuffer{};
        BufferResource viewModelVertexBuffer{};
        BufferResource viewModelPostScopeVertexBuffer{};
        BufferResource effectVertexBuffer{};
        BufferResource lineVertexBuffer{};
        BufferResource overlayVertexBuffer{};
        std::uint64_t uploadedTerrainMeshVersion = 0;
    };

    struct QueueFamilySelection
    {
        std::optional<std::uint32_t> graphics;
        std::optional<std::uint32_t> present;

        [[nodiscard]] bool Complete() const
        {
            return graphics.has_value() && present.has_value();
        }
    };

    void CreateInstance();
    void CreateDebugMessenger();
    void DestroyDebugMessenger();
    void CreateSurface();
    void PickPhysicalDevice();
    void CreateDevice();
    void CreateCommandPool();
    void CreateDescriptorResources();
    void DestroyDescriptorResources();
    void CreateShadowResources();
    void DestroyShadowResources();
    void UpdateShadowDescriptorSet();
    void CreatePipelineLayout();
    void CreateFrameResources();
    void CreateSwapchain();
    void CreateDepthResources();
    void DestroyDepthResources();
    void CreatePipelines();
    void DestroyPipelines();
    void DestroySwapchain();
    [[nodiscard]] bool TryRecreateSwapchain();
    void RecordFrame(VkCommandBuffer commandBuffer, std::uint32_t imageIndex, const FrameRenderData& frameData, const FrameResources& frame);
    void TransitionSwapchainImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout) const;
    void TransitionImage(VkCommandBuffer commandBuffer, VkImage image, VkImageAspectFlags aspectMask, VkImageLayout oldLayout, VkImageLayout newLayout) const;
    [[nodiscard]] std::uint32_t FindMemoryType(std::uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
    [[nodiscard]] VkFormat ChooseDepthFormat() const;
    [[nodiscard]] VkShaderModule LoadShaderModule(std::string_view filename) const;

    void EnsureBufferCapacity(BufferResource& buffer, VkDeviceSize minimumSize);
    void DestroyBuffer(BufferResource& buffer);

    [[nodiscard]] std::vector<const char*> GetRequiredInstanceExtensions() const;
    [[nodiscard]] QueueFamilySelection FindQueueFamilies(VkPhysicalDevice device) const;
    [[nodiscard]] bool SupportsValidationLayer() const;

    SDL_Window* window_ = nullptr;
    bool initialized_ = false;
    bool framebufferResized_ = false;
    bool validationEnabled_ = false;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;
    std::uint32_t graphicsQueueFamily_ = 0;
    std::uint32_t presentQueueFamily_ = 0;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::array<FrameResources, static_cast<std::size_t>(config::kMaxFramesInFlight)> frames_{};
    std::uint32_t frameIndex_ = 0;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet shadowDescriptorSet_ = VK_NULL_HANDLE;
    VkSampler shadowSampler_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent_{};
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainImageViews_;
    std::vector<VkSemaphore> swapchainRenderCompleteSemaphores_;
    std::vector<VkFence> swapchainImageFences_;
    VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
    ImageResource depthImage_{};
    ImageResource shadowImage_{};
    VkPipeline shadowPipeline_ = VK_NULL_HANDLE;
    VkPipeline terrainPipeline_ = VK_NULL_HANDLE;
    VkPipeline translucentTerrainPipeline_ = VK_NULL_HANDLE;
    VkPipeline effectPipeline_ = VK_NULL_HANDLE;
    VkPipeline linePipeline_ = VK_NULL_HANDLE;
    VkPipeline overlayPipeline_ = VK_NULL_HANDLE;

    std::string adapterName_;
    FrameProfiler* profiler_ = nullptr;
};
}
