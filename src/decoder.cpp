#include "decoder.h"
#include <cstdint>
#include <fstream>
#include <vector>
#include "pch.h"
#include "graphics.h"
#include <cmath>
#include <filesystem>
#include <REX/W32/DXGI_2.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/log.h>
}

#ifdef ERROR
#undef ERROR
#endif

namespace f4ffmpeg
{
struct producedFrameVulkanState
{
    AVBufferRef* deviceRef = nullptr;

    VkDevice device =
        VK_NULL_HANDLE;

    const VkAllocationCallbacks* allocator =
        nullptr;

    VkImage image =
        VK_NULL_HANDLE;

    VkDeviceMemory memory =
        VK_NULL_HANDLE;

    VkImageView view =
        VK_NULL_HANDLE;

    VkImage rgbaImage =
        VK_NULL_HANDLE;

    VkDeviceMemory rgbaMemory =
        VK_NULL_HANDLE;

    VkImageView rgbaView =
        VK_NULL_HANDLE;

    std::uint32_t rgbaWidth = 0;
    std::uint32_t rgbaHeight = 0;

    VkCommandPool commandPool =
        VK_NULL_HANDLE;

    VkCommandBuffer commandBuffer =
        VK_NULL_HANDLE;

    VkQueue queue =
        VK_NULL_HANDLE;

    std::uint32_t queueFamilyIndex =
        static_cast<std::uint32_t>(-1);

    HANDLE sharedHandle =
        nullptr;

    PFN_vkDestroyImage destroyImage =
        nullptr;

    PFN_vkDestroyImageView destroyImageView =
        nullptr;

    PFN_vkFreeMemory freeMemory =
        nullptr;

    PFN_vkDestroyCommandPool destroyCommandPool =
        nullptr;

    ~producedFrameVulkanState()
    {
        if (
            commandPool != VK_NULL_HANDLE &&
            destroyCommandPool != nullptr)
        {
            destroyCommandPool(
                device,
                commandPool,
                allocator
            );
        }

        if (
            rgbaView != VK_NULL_HANDLE &&
            destroyImageView != nullptr)
        {
            destroyImageView(
                device,
                rgbaView,
                allocator
            );
        }

        if (
            rgbaImage != VK_NULL_HANDLE &&
            destroyImage != nullptr)
        {
            destroyImage(
                device,
                rgbaImage,
                allocator
            );
        }

        if (
            rgbaMemory != VK_NULL_HANDLE &&
            freeMemory != nullptr)
        {
            freeMemory(
                device,
                rgbaMemory,
                allocator
            );
        }

        if (
            view != VK_NULL_HANDLE &&
            destroyImageView != nullptr)
        {
            destroyImageView(
                device,
                view,
                allocator
            );
        }

        if (
            image != VK_NULL_HANDLE &&
            destroyImage != nullptr)
        {
            destroyImage(
                device,
                image,
                allocator
            );
        }

        if (
            memory != VK_NULL_HANDLE &&
            freeMemory != nullptr)
        {
            freeMemory(
                device,
                memory,
                allocator
            );
        }

        if (sharedHandle != nullptr)
        {
            ::CloseHandle(
                sharedHandle
            );

            sharedHandle = nullptr;
        }

        av_buffer_unref(
            &deviceRef
        );
    }
};


namespace
{
    template <class T>
    T loadVulkanInstanceProc(
        const AVVulkanDeviceContext& context,
        const char* name)
    {
        if (
            context.get_proc_addr == nullptr ||
            context.inst == VK_NULL_HANDLE ||
            name == nullptr)
        {
            return nullptr;
        }

        return reinterpret_cast<T>(
            context.get_proc_addr(
                context.inst,
                name
            )
        );
    }

    template <class T>
    T loadVulkanDeviceProc(
        const AVVulkanDeviceContext& context,
        const char* name)
    {
        if (
            context.act_dev == VK_NULL_HANDLE ||
            name == nullptr)
        {
            return nullptr;
        }

        const auto getDeviceProcAddr =
            loadVulkanInstanceProc<PFN_vkGetDeviceProcAddr>(
                context,
                "vkGetDeviceProcAddr"
            );

        if (getDeviceProcAddr == nullptr)
        {
            return nullptr;
        }

        return reinterpret_cast<T>(
            getDeviceProcAddr(
                context.act_dev,
                name
            )
        );
    }

    bool chooseMemoryType(
        std::uint32_t compatibleTypes,
        const VkPhysicalDeviceMemoryProperties& properties,
        VkMemoryPropertyFlags preferredFlags,
        std::uint32_t& memoryTypeIndex)
    {
        for (
            std::uint32_t index = 0;
            index < properties.memoryTypeCount;
            ++index)
        {
            const std::uint32_t bit =
                1u << index;

            if (
                (compatibleTypes & bit) != 0 &&
                (properties.memoryTypes[index].propertyFlags & preferredFlags) ==
                    preferredFlags)
            {
                memoryTypeIndex = index;
                return true;
            }
        }

        for (
            std::uint32_t index = 0;
            index < properties.memoryTypeCount;
            ++index)
        {
            if (
                compatibleTypes &
                    (1u << index))
            {
                memoryTypeIndex = index;
                return true;
            }
        }

        return false;
    }

    bool chooseVulkanProducerQueueFamily(
        const AVVulkanDeviceContext& context,
        std::uint32_t& queueFamilyIndex)
    {
        for (int index = 0; index < context.nb_qf; ++index)
        {
            const auto& family =
                context.qf[index];

            if (
                family.idx >= 0 &&
                family.num > 0 &&
                (family.flags & VK_QUEUE_COMPUTE_BIT) != 0)
            {
                queueFamilyIndex =
                    static_cast<std::uint32_t>(
                        family.idx
                    );

                return true;
            }
        }

        for (int index = 0; index < context.nb_qf; ++index)
        {
            const auto& family =
                context.qf[index];

            if (
                family.idx >= 0 &&
                family.num > 0 &&
                (family.flags & VK_QUEUE_TRANSFER_BIT) != 0)
            {
                queueFamilyIndex =
                    static_cast<std::uint32_t>(
                        family.idx
                    );

                return true;
            }
        }

        return false;
    }

    bool ensureProducedFrameVulkanState(
        producedFrame& output,
        AVHWFramesContext& framesContext,
        AVVulkanDeviceContext& vkDeviceContext)
    {
        if (output.vulkanState)
        {
            return true;
        }

        if (
            output.texture == nullptr ||
            output.width == 0 ||
            output.height == 0 ||
            framesContext.device_ref == nullptr ||
            vkDeviceContext.act_dev == VK_NULL_HANDLE ||
            vkDeviceContext.phys_dev == VK_NULL_HANDLE)
        {
            return false;
        }

        auto state =
            std::make_unique<producedFrameVulkanState>();

        state->device =
            vkDeviceContext.act_dev;

        state->allocator =
            vkDeviceContext.alloc;

        state->deviceRef =
            av_buffer_ref(
                framesContext.device_ref
            );

        if (state->deviceRef == nullptr)
        {
            return false;
        }

        const auto createImage =
            loadVulkanDeviceProc<PFN_vkCreateImage>(
                vkDeviceContext,
                "vkCreateImage"
            );

        const auto getImageMemoryRequirements =
            loadVulkanDeviceProc<PFN_vkGetImageMemoryRequirements>(
                vkDeviceContext,
                "vkGetImageMemoryRequirements"
            );

        const auto getMemoryWin32HandleProperties =
            loadVulkanDeviceProc<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
                vkDeviceContext,
                "vkGetMemoryWin32HandlePropertiesKHR"
            );

        const auto allocateMemory =
            loadVulkanDeviceProc<PFN_vkAllocateMemory>(
                vkDeviceContext,
                "vkAllocateMemory"
            );

        const auto bindImageMemory =
            loadVulkanDeviceProc<PFN_vkBindImageMemory>(
                vkDeviceContext,
                "vkBindImageMemory"
            );

        const auto createImageView =
            loadVulkanDeviceProc<PFN_vkCreateImageView>(
                vkDeviceContext,
                "vkCreateImageView"
            );

        state->destroyImage =
            loadVulkanDeviceProc<PFN_vkDestroyImage>(
                vkDeviceContext,
                "vkDestroyImage"
            );

        state->destroyImageView =
            loadVulkanDeviceProc<PFN_vkDestroyImageView>(
                vkDeviceContext,
                "vkDestroyImageView"
            );

        state->freeMemory =
            loadVulkanDeviceProc<PFN_vkFreeMemory>(
                vkDeviceContext,
                "vkFreeMemory"
            );

        state->destroyCommandPool =
            loadVulkanDeviceProc<PFN_vkDestroyCommandPool>(
                vkDeviceContext,
                "vkDestroyCommandPool"
            );

        if (
            createImage == nullptr ||
            getImageMemoryRequirements == nullptr ||
            getMemoryWin32HandleProperties == nullptr ||
            allocateMemory == nullptr ||
            bindImageMemory == nullptr ||
            createImageView == nullptr ||
            state->destroyImage == nullptr ||
            state->destroyImageView == nullptr ||
            state->freeMemory == nullptr ||
            state->destroyCommandPool == nullptr)
        {
            REX::ERROR(
                "Failed to load Vulkan external-memory functions."
            );

            return false;
        }

        REX::W32::IDXGIResource1* dxgiResource =
            nullptr;

        const auto queryResult =
            output.texture->QueryInterface(
                REX::W32::IID_IDXGIResource1,
                reinterpret_cast<void**>(
                    &dxgiResource
                )
            );

        if (
            queryResult < 0 ||
            dxgiResource == nullptr)
        {
            REX::ERROR(
                "Failed to query produced texture for IDXGIResource1: 0x{:08X}",
                static_cast<std::uint32_t>(
                    queryResult
                )
            );

            return false;
        }

        constexpr std::uint32_t sharedAccess =
            0x80000000u | // DXGI_SHARED_RESOURCE_READ
            0x00000001u;  // DXGI_SHARED_RESOURCE_WRITE

        const auto sharedHandleResult =
            dxgiResource->CreateSharedHandle(
                nullptr,
                sharedAccess,
                nullptr,
                &state->sharedHandle
            );

        dxgiResource->Release();

        if (
            sharedHandleResult < 0 ||
            state->sharedHandle == nullptr)
        {
            REX::ERROR(
                "Failed to create produced texture NT handle: 0x{:08X}",
                static_cast<std::uint32_t>(
                    sharedHandleResult
                )
            );

            return false;
        }

        VkExternalMemoryImageCreateInfo externalImageInfo{};
        externalImageInfo.sType =
            VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        externalImageInfo.handleTypes =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;

        VkImageCreateInfo imageInfo{};
        imageInfo.sType =
            VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.pNext =
            &externalImageInfo;
        imageInfo.imageType =
            VK_IMAGE_TYPE_2D;
        imageInfo.format =
            VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = {
            output.width,
            output.height,
            1
        };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples =
            VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling =
            VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage =
            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        imageInfo.sharingMode =
            VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout =
            VK_IMAGE_LAYOUT_UNDEFINED;

        const VkResult createImageResult =
            createImage(
                state->device,
                &imageInfo,
                state->allocator,
                &state->image
            );

        if (createImageResult != VK_SUCCESS)
        {
            REX::ERROR(
                "Failed to create Vulkan alias image: {}",
                static_cast<int>(createImageResult)
            );

            return false;
        }

        VkMemoryRequirements requirements{};
        getImageMemoryRequirements(
            state->device,
            state->image,
            &requirements
        );

        VkMemoryWin32HandlePropertiesKHR handleProperties{};
        handleProperties.sType =
            VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR;

        const VkResult handlePropertiesResult =
            getMemoryWin32HandleProperties(
                state->device,
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
                state->sharedHandle,
                &handleProperties
            );

        if (handlePropertiesResult != VK_SUCCESS)
        {
            REX::ERROR(
                "Failed to query Vulkan properties for D3D11 handle: {}",
                static_cast<int>(handlePropertiesResult)
            );

            return false;
        }

        const std::uint32_t compatibleTypes =
            requirements.memoryTypeBits &
            handleProperties.memoryTypeBits;

        VkPhysicalDeviceMemoryProperties memoryProperties{};

        const auto getPhysicalDeviceMemoryProperties =
            loadVulkanInstanceProc<PFN_vkGetPhysicalDeviceMemoryProperties>(
                vkDeviceContext,
                "vkGetPhysicalDeviceMemoryProperties"
            );

        if (getPhysicalDeviceMemoryProperties == nullptr)
        {
            REX::ERROR(
                "Failed to load vkGetPhysicalDeviceMemoryProperties."
            );

            return false;
        }

        getPhysicalDeviceMemoryProperties(
            vkDeviceContext.phys_dev,
            &memoryProperties
        );

        std::uint32_t memoryTypeIndex = 0;

        if (!chooseMemoryType(
                compatibleTypes,
                memoryProperties,
                0,
                memoryTypeIndex))
        {
            REX::ERROR(
                "No compatible Vulkan memory type exists for the D3D11 texture import."
            );

            return false;
        }

        VkMemoryDedicatedAllocateInfo dedicatedInfo{};
        dedicatedInfo.sType =
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicatedInfo.image =
            state->image;

        VkImportMemoryWin32HandleInfoKHR importInfo{};
        importInfo.sType =
            VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
        importInfo.pNext =
            &dedicatedInfo;
        importInfo.handleType =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        importInfo.handle =
            state->sharedHandle;

        VkMemoryAllocateInfo allocationInfo{};
        allocationInfo.sType =
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocationInfo.pNext =
            &importInfo;
        allocationInfo.allocationSize =
            requirements.size;
        allocationInfo.memoryTypeIndex =
            memoryTypeIndex;

        const VkResult allocateResult =
            allocateMemory(
                state->device,
                &allocationInfo,
                state->allocator,
                &state->memory
            );

        if (allocateResult != VK_SUCCESS)
        {
            REX::ERROR(
                "Failed to import D3D11 texture memory into Vulkan: {}",
                static_cast<int>(allocateResult)
            );

            return false;
        }

        const VkResult bindResult =
            bindImageMemory(
                state->device,
                state->image,
                state->memory,
                0
            );

        if (bindResult != VK_SUCCESS)
        {
            REX::ERROR(
                "Failed to bind imported D3D11 memory to Vulkan image: {}",
                static_cast<int>(bindResult)
            );

            return false;
        }

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType =
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image =
            state->image;
        viewInfo.viewType =
            VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format =
            VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        const VkResult viewResult =
            createImageView(
                state->device,
                &viewInfo,
                state->allocator,
                &state->view
            );

        if (viewResult != VK_SUCCESS)
        {
            REX::ERROR(
                "Failed to create Vulkan view for produced texture: {}",
                static_cast<int>(viewResult)
            );

            return false;
        }

        REX::TRACE(
            "Imported produced D3D11 texture into Vulkan: {}x{}, VkImage={}",
            output.width,
            output.height,
            reinterpret_cast<void*>(
                state->image
            )
        );

        output.vulkanState =
            std::move(state);

        return true;
    }

    bool ensureVulkanRgbaSurface(
        producedFrameVulkanState& state,
        AVHWFramesContext& framesContext,
        AVVulkanFramesContext& vkFramesContext,
        AVVulkanDeviceContext& vkDeviceContext,
        int width,
        int height)
    {
        (void)framesContext;
        (void)vkFramesContext;

        if (
            width <= 0 ||
            height <= 0 ||
            state.device == VK_NULL_HANDLE ||
            state.device != vkDeviceContext.act_dev)
        {
            return false;
        }

        if (
            state.rgbaImage != VK_NULL_HANDLE &&
            state.rgbaWidth ==
                static_cast<std::uint32_t>(width) &&
            state.rgbaHeight ==
                static_cast<std::uint32_t>(height))
        {
            return true;
        }

        if (
            state.rgbaImage != VK_NULL_HANDLE ||
            state.rgbaMemory != VK_NULL_HANDLE ||
            state.rgbaView != VK_NULL_HANDLE)
        {
            REX::ERROR(
                "RGBA surface dimensions changed on an existing produced-frame Vulkan state."
            );

            return false;
        }

        const auto createImage =
            loadVulkanDeviceProc<PFN_vkCreateImage>(
                vkDeviceContext,
                "vkCreateImage"
            );

        const auto getImageMemoryRequirements =
            loadVulkanDeviceProc<PFN_vkGetImageMemoryRequirements>(
                vkDeviceContext,
                "vkGetImageMemoryRequirements"
            );

        const auto allocateMemory =
            loadVulkanDeviceProc<PFN_vkAllocateMemory>(
                vkDeviceContext,
                "vkAllocateMemory"
            );

        const auto bindImageMemory =
            loadVulkanDeviceProc<PFN_vkBindImageMemory>(
                vkDeviceContext,
                "vkBindImageMemory"
            );

        const auto createImageView =
            loadVulkanDeviceProc<PFN_vkCreateImageView>(
                vkDeviceContext,
                "vkCreateImageView"
            );

        const auto getPhysicalDeviceMemoryProperties =
            loadVulkanInstanceProc<PFN_vkGetPhysicalDeviceMemoryProperties>(
                vkDeviceContext,
                "vkGetPhysicalDeviceMemoryProperties"
            );

        if (
            createImage == nullptr ||
            getImageMemoryRequirements == nullptr ||
            allocateMemory == nullptr ||
            bindImageMemory == nullptr ||
            createImageView == nullptr ||
            getPhysicalDeviceMemoryProperties == nullptr)
        {
            REX::ERROR(
                "Failed to load Vulkan RGBA-surface functions."
            );

            return false;
        }

        VkImageCreateInfo imageInfo{};
        imageInfo.sType =
            VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType =
            VK_IMAGE_TYPE_2D;
        imageInfo.format =
            VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = {
            static_cast<std::uint32_t>(width),
            static_cast<std::uint32_t>(height),
            1
        };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples =
            VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling =
            VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage =
            VK_IMAGE_USAGE_STORAGE_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.sharingMode =
            VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout =
            VK_IMAGE_LAYOUT_UNDEFINED;

        const VkResult imageResult =
            createImage(
                state.device,
                &imageInfo,
                state.allocator,
                &state.rgbaImage
            );

        if (imageResult != VK_SUCCESS)
        {
            REX::ERROR(
                "Failed to create Vulkan RGBA scratch image: {}",
                static_cast<int>(imageResult)
            );

            return false;
        }

        VkMemoryRequirements requirements{};
        getImageMemoryRequirements(
            state.device,
            state.rgbaImage,
            &requirements
        );

        VkPhysicalDeviceMemoryProperties memoryProperties{};
        getPhysicalDeviceMemoryProperties(
            vkDeviceContext.phys_dev,
            &memoryProperties
        );

        std::uint32_t memoryTypeIndex = 0;

        if (!chooseMemoryType(
                requirements.memoryTypeBits,
                memoryProperties,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                memoryTypeIndex))
        {
            REX::ERROR(
                "No compatible Vulkan memory type exists for the RGBA scratch image."
            );

            return false;
        }

        VkMemoryAllocateInfo allocationInfo{};
        allocationInfo.sType =
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocationInfo.allocationSize =
            requirements.size;
        allocationInfo.memoryTypeIndex =
            memoryTypeIndex;

        const VkResult allocationResult =
            allocateMemory(
                state.device,
                &allocationInfo,
                state.allocator,
                &state.rgbaMemory
            );

        if (allocationResult != VK_SUCCESS)
        {
            REX::ERROR(
                "Failed to allocate Vulkan RGBA scratch memory: {}",
                static_cast<int>(allocationResult)
            );

            return false;
        }

        const VkResult bindResult =
            bindImageMemory(
                state.device,
                state.rgbaImage,
                state.rgbaMemory,
                0
            );

        if (bindResult != VK_SUCCESS)
        {
            REX::ERROR(
                "Failed to bind Vulkan RGBA scratch memory: {}",
                static_cast<int>(bindResult)
            );

            return false;
        }

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType =
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image =
            state.rgbaImage;
        viewInfo.viewType =
            VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format =
            VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        const VkResult viewResult =
            createImageView(
                state.device,
                &viewInfo,
                state.allocator,
                &state.rgbaView
            );

        if (viewResult != VK_SUCCESS)
        {
            REX::ERROR(
                "Failed to create Vulkan RGBA scratch view: {}",
                static_cast<int>(viewResult)
            );

            return false;
        }

        state.rgbaWidth =
            static_cast<std::uint32_t>(width);
        state.rgbaHeight =
            static_cast<std::uint32_t>(height);

        REX::TRACE(
            "Prepared Vulkan RGBA scratch surface: {}x{}, VkImage={}",
            width,
            height,
            reinterpret_cast<void*>(
                state.rgbaImage
            )
        );

        REX::DEBUG(
            "Vulkan RGBA checkpoint active: produced output will be diagnostic magenta until YUV sampling is wired in."
        );

        return true;
    }

    bool ensureVulkanCommandResources(
        producedFrameVulkanState& state,
        AVVulkanDeviceContext& vkDeviceContext)
    {
        if (
            state.commandPool != VK_NULL_HANDLE &&
            state.commandBuffer != VK_NULL_HANDLE &&
            state.queue != VK_NULL_HANDLE)
        {
            return true;
        }

        if (!chooseVulkanProducerQueueFamily(
                vkDeviceContext,
                state.queueFamilyIndex))
        {
            REX::ERROR(
                "No Vulkan compute/transfer queue family is available for frame production."
            );

            return false;
        }

        const auto getDeviceQueue =
            loadVulkanDeviceProc<PFN_vkGetDeviceQueue>(
                vkDeviceContext,
                "vkGetDeviceQueue"
            );

        const auto createCommandPool =
            loadVulkanDeviceProc<PFN_vkCreateCommandPool>(
                vkDeviceContext,
                "vkCreateCommandPool"
            );

        const auto allocateCommandBuffers =
            loadVulkanDeviceProc<PFN_vkAllocateCommandBuffers>(
                vkDeviceContext,
                "vkAllocateCommandBuffers"
            );

        if (
            getDeviceQueue == nullptr ||
            createCommandPool == nullptr ||
            allocateCommandBuffers == nullptr)
        {
            REX::ERROR(
                "Failed to load Vulkan command-buffer functions."
            );

            return false;
        }

        getDeviceQueue(
            state.device,
            state.queueFamilyIndex,
            0,
            &state.queue
        );

        if (state.queue == VK_NULL_HANDLE)
        {
            REX::ERROR(
                "Failed to retrieve Vulkan producer queue."
            );

            return false;
        }

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType =
            VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags =
            VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex =
            state.queueFamilyIndex;

        const VkResult poolResult =
            createCommandPool(
                state.device,
                &poolInfo,
                state.allocator,
                &state.commandPool
            );

        if (poolResult != VK_SUCCESS)
        {
            REX::ERROR(
                "Failed to create Vulkan producer command pool: {}",
                static_cast<int>(poolResult)
            );

            return false;
        }

        VkCommandBufferAllocateInfo commandInfo{};
        commandInfo.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        commandInfo.commandPool =
            state.commandPool;
        commandInfo.level =
            VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;

        const VkResult commandResult =
            allocateCommandBuffers(
                state.device,
                &commandInfo,
                &state.commandBuffer
            );

        if (commandResult != VK_SUCCESS)
        {
            REX::ERROR(
                "Failed to allocate Vulkan producer command buffer: {}",
                static_cast<int>(commandResult)
            );

            return false;
        }

        REX::TRACE(
            "Prepared Vulkan producer queue family {}.",
            state.queueFamilyIndex
        );

        return true;
    }

    bool submitVulkanFrame(
        AVVkFrame& vkFrame,
        AVHWFramesContext& framesContext,
        AVVulkanFramesContext& vkFramesContext,
        AVVulkanDeviceContext& vkDeviceContext,
        producedFrameVulkanState& state)
    {
        (void)vkFrame;
        (void)vkFramesContext;

        if (
            state.image == VK_NULL_HANDLE ||
            state.memory == VK_NULL_HANDLE ||
            state.rgbaImage == VK_NULL_HANDLE ||
            state.rgbaMemory == VK_NULL_HANDLE ||
            state.rgbaWidth == 0 ||
            state.rgbaHeight == 0)
        {
            return false;
        }

        if (!ensureVulkanCommandResources(
                state,
                vkDeviceContext))
        {
            return false;
        }

        const auto resetCommandPool =
            loadVulkanDeviceProc<PFN_vkResetCommandPool>(
                vkDeviceContext,
                "vkResetCommandPool"
            );

        const auto beginCommandBuffer =
            loadVulkanDeviceProc<PFN_vkBeginCommandBuffer>(
                vkDeviceContext,
                "vkBeginCommandBuffer"
            );

        const auto cmdPipelineBarrier =
            loadVulkanDeviceProc<PFN_vkCmdPipelineBarrier>(
                vkDeviceContext,
                "vkCmdPipelineBarrier"
            );

        const auto cmdClearColorImage =
            loadVulkanDeviceProc<PFN_vkCmdClearColorImage>(
                vkDeviceContext,
                "vkCmdClearColorImage"
            );

        const auto cmdCopyImage =
            loadVulkanDeviceProc<PFN_vkCmdCopyImage>(
                vkDeviceContext,
                "vkCmdCopyImage"
            );

        const auto endCommandBuffer =
            loadVulkanDeviceProc<PFN_vkEndCommandBuffer>(
                vkDeviceContext,
                "vkEndCommandBuffer"
            );

        const auto queueSubmit =
            loadVulkanDeviceProc<PFN_vkQueueSubmit>(
                vkDeviceContext,
                "vkQueueSubmit"
            );

        const auto queueWaitIdle =
            loadVulkanDeviceProc<PFN_vkQueueWaitIdle>(
                vkDeviceContext,
                "vkQueueWaitIdle"
            );

        if (
            resetCommandPool == nullptr ||
            beginCommandBuffer == nullptr ||
            cmdPipelineBarrier == nullptr ||
            cmdClearColorImage == nullptr ||
            cmdCopyImage == nullptr ||
            endCommandBuffer == nullptr ||
            queueSubmit == nullptr ||
            queueWaitIdle == nullptr)
        {
            REX::ERROR(
                "Failed to load Vulkan submission functions."
            );

            return false;
        }

        const VkResult resetResult =
            resetCommandPool(
                state.device,
                state.commandPool,
                0
            );

        if (resetResult != VK_SUCCESS)
        {
            REX::ERROR(
                "Failed to reset Vulkan producer command pool: {}",
                static_cast<int>(resetResult)
            );

            return false;
        }

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags =
            VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        const VkResult beginResult =
            beginCommandBuffer(
                state.commandBuffer,
                &beginInfo
            );

        if (beginResult != VK_SUCCESS)
        {
            REX::ERROR(
                "Failed to begin Vulkan producer command buffer: {}",
                static_cast<int>(beginResult)
            );

            return false;
        }

        VkImageSubresourceRange colorRange{};
        colorRange.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        colorRange.baseMipLevel = 0;
        colorRange.levelCount = 1;
        colorRange.baseArrayLayer = 0;
        colorRange.layerCount = 1;

        VkImageMemoryBarrier rgbaToDestination{};
        rgbaToDestination.sType =
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        rgbaToDestination.srcAccessMask = 0;
        rgbaToDestination.dstAccessMask =
            VK_ACCESS_TRANSFER_WRITE_BIT;
        rgbaToDestination.oldLayout =
            VK_IMAGE_LAYOUT_UNDEFINED;
        rgbaToDestination.newLayout =
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        rgbaToDestination.srcQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        rgbaToDestination.dstQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        rgbaToDestination.image =
            state.rgbaImage;
        rgbaToDestination.subresourceRange =
            colorRange;

        cmdPipelineBarrier(
            state.commandBuffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &rgbaToDestination
        );

        // Diagnostic checkpoint: until the YUV sampling shader is wired in,
        // fill the canonical RGBA scratch surface with magenta. If the produced
        // TGA is magenta, Vulkan scratch -> imported D3D11 texture is proven.
        VkClearColorValue checkpointColor{};
        checkpointColor.float32[0] = 1.0f;
        checkpointColor.float32[1] = 0.0f;
        checkpointColor.float32[2] = 1.0f;
        checkpointColor.float32[3] = 1.0f;

        cmdClearColorImage(
            state.commandBuffer,
            state.rgbaImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &checkpointColor,
            1,
            &colorRange
        );

        VkImageMemoryBarrier rgbaToSource{};
        rgbaToSource.sType =
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        rgbaToSource.srcAccessMask =
            VK_ACCESS_TRANSFER_WRITE_BIT;
        rgbaToSource.dstAccessMask =
            VK_ACCESS_TRANSFER_READ_BIT;
        rgbaToSource.oldLayout =
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        rgbaToSource.newLayout =
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        rgbaToSource.srcQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        rgbaToSource.dstQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        rgbaToSource.image =
            state.rgbaImage;
        rgbaToSource.subresourceRange =
            colorRange;

        VkImageMemoryBarrier outputAcquire{};
        outputAcquire.sType =
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        outputAcquire.srcAccessMask = 0;
        outputAcquire.dstAccessMask =
            VK_ACCESS_TRANSFER_WRITE_BIT;
        outputAcquire.oldLayout =
            VK_IMAGE_LAYOUT_GENERAL;
        outputAcquire.newLayout =
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        outputAcquire.srcQueueFamilyIndex =
            VK_QUEUE_FAMILY_EXTERNAL;
        outputAcquire.dstQueueFamilyIndex =
            state.queueFamilyIndex;
        outputAcquire.image =
            state.image;
        outputAcquire.subresourceRange =
            colorRange;

        VkImageMemoryBarrier preCopyBarriers[2]{
            rgbaToSource,
            outputAcquire
        };

        cmdPipelineBarrier(
            state.commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            2,
            preCopyBarriers
        );

        VkImageCopy copyRegion{};
        copyRegion.srcSubresource.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.srcSubresource.mipLevel = 0;
        copyRegion.srcSubresource.baseArrayLayer = 0;
        copyRegion.srcSubresource.layerCount = 1;
        copyRegion.dstSubresource =
            copyRegion.srcSubresource;
        copyRegion.extent = {
            state.rgbaWidth,
            state.rgbaHeight,
            1
        };

        cmdCopyImage(
            state.commandBuffer,
            state.rgbaImage,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            state.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &copyRegion
        );

        VkImageMemoryBarrier outputRelease{};
        outputRelease.sType =
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        outputRelease.srcAccessMask =
            VK_ACCESS_TRANSFER_WRITE_BIT;
        outputRelease.dstAccessMask = 0;
        outputRelease.oldLayout =
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        outputRelease.newLayout =
            VK_IMAGE_LAYOUT_GENERAL;
        outputRelease.srcQueueFamilyIndex =
            state.queueFamilyIndex;
        outputRelease.dstQueueFamilyIndex =
            VK_QUEUE_FAMILY_EXTERNAL;
        outputRelease.image =
            state.image;
        outputRelease.subresourceRange =
            colorRange;

        cmdPipelineBarrier(
            state.commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &outputRelease
        );

        const VkResult endResult =
            endCommandBuffer(
                state.commandBuffer
            );

        if (endResult != VK_SUCCESS)
        {
            REX::ERROR(
                "Failed to end Vulkan producer command buffer: {}",
                static_cast<int>(endResult)
            );

            return false;
        }

        VkSubmitInfo submitInfo{};
        submitInfo.sType =
            VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers =
            &state.commandBuffer;

        if (vkDeviceContext.lock_queue != nullptr)
        {
            vkDeviceContext.lock_queue(
                framesContext.device_ctx,
                state.queueFamilyIndex,
                0
            );
        }

        const VkResult submitResult =
            queueSubmit(
                state.queue,
                1,
                &submitInfo,
                VK_NULL_HANDLE
            );

        VkResult waitResult =
            VK_SUCCESS;

        if (submitResult == VK_SUCCESS)
        {
            // Checkpoint only. This deliberately blocks so the following D3D11
            // dump/read cannot race the Vulkan copy. Replace with keyed-mutex /
            // semaphore handoff once the complete YUV path is validated.
            waitResult =
                queueWaitIdle(
                    state.queue
                );
        }

        if (vkDeviceContext.unlock_queue != nullptr)
        {
            vkDeviceContext.unlock_queue(
                framesContext.device_ctx,
                state.queueFamilyIndex,
                0
            );
        }

        if (submitResult != VK_SUCCESS)
        {
            REX::ERROR(
                "Vulkan producer queue submission failed: {}",
                static_cast<int>(submitResult)
            );

            return false;
        }

        if (waitResult != VK_SUCCESS)
        {
            REX::ERROR(
                "Vulkan producer queue wait failed: {}",
                static_cast<int>(waitResult)
            );

            return false;
        }

        return true;
    }
}


std::string makeFrameDumpPath(
    const std::string& basePath,
    const char* suffix)
{
    const std::filesystem::path path{
        basePath
    };

    return (
        path.parent_path() /
        (
            path.stem().string() +
            "_" +
            suffix +
            path.extension().string()
        )
    ).string();
}

namespace {

    bool writeTga(
        const char* path,
        const std::uint8_t* data,
        int width,
        int height,
        int stride
    );

bool dumpProducedFrame(
    const producedFrame& frame,
    const char* outputPath)
{
    if (
        frame.texture == nullptr ||
        outputPath == nullptr ||
        frame.width == 0 ||
        frame.height == 0)
    {
        return false;
    }

    auto* device =
        getD3D11Device();

    auto* context =
        getD3D11DeviceContext();

    if (
        device == nullptr ||
        context == nullptr)
    {
        return false;
    }

    REX::INFO(
        "Writing produced frame dump to {}",
        outputPath
    );

    // The produced texture is GPU-only, so make a CPU-readable staging texture.

    REX::W32::D3D11_TEXTURE2D_DESC desc{};
    frame.texture->GetDesc(
        &desc
    );

    REX::W32::D3D11_TEXTURE2D_DESC stagingDesc =
        desc;

    stagingDesc.usage =
        REX::W32::D3D11_USAGE_STAGING;

    stagingDesc.bindFlags = 0;

    stagingDesc.cpuAccessFlags =
        REX::W32::D3D11_CPU_ACCESS_READ;

    // Staging resources cannot retain the shared/keyed-mutex flags.
    stagingDesc.miscFlags = 0;

    REX::W32::ID3D11Texture2D*
        stagingTexture = nullptr;

    const auto createResult =
        device->CreateTexture2D(
            &stagingDesc,
            nullptr,
            &stagingTexture
        );

    if (
        createResult < 0 ||
        stagingTexture == nullptr)
    {
        REX::WARN(
            "Failed to create produced-frame staging texture: 0x{:08X}",
            static_cast<std::uint32_t>(
                createResult
            )
        );

        return false;
    }

    // GPU texture -> CPU-readable staging texture.

    context->CopyResource(
        stagingTexture,
        frame.texture
    );

    REX::W32::D3D11_MAPPED_SUBRESOURCE
        mapped{};

    const auto mapResult =
        context->Map(
            stagingTexture,
            0,
            REX::W32::D3D11_MAP_READ,
            0,
            &mapped
        );

    if (mapResult < 0)
    {
        REX::WARN(
            "Failed to map produced-frame staging texture: 0x{:08X}",
            static_cast<std::uint32_t>(
                mapResult
            )
        );

        stagingTexture->Release();

        return false;
    }

    // Our canonical texture is RGBA8. TGA expects B,G,R,A byte ordering.

    const int stride =
        static_cast<int>(
            frame.width * 4
        );

    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(stride) *
        static_cast<std::size_t>(
            frame.height
        )
    );

    for (
        std::uint32_t y = 0;
        y < frame.height;
        ++y)
    {
        const auto* source =
            static_cast<const std::uint8_t*>(
                mapped.data
            ) +
            static_cast<std::size_t>(y) *
                mapped.rowPitch;

        auto* destination =
            pixels.data() +
            static_cast<std::size_t>(y) *
                stride;

        for (
            std::uint32_t x = 0;
            x < frame.width;
            ++x)
        {
            destination[0] =
                source[2]; // B

            destination[1] =
                source[1]; // G

            destination[2] =
                source[0]; // R

            destination[3] =
                source[3]; // A

            source += 4;
            destination += 4;
        }
    }

    context->Unmap(
        stagingTexture,
        0
    );

    stagingTexture->Release();

    return writeTga(
        outputPath,
        pixels.data(),
        static_cast<int>(
            frame.width
        ),
        static_cast<int>(
            frame.height
        ),
        stride
    );
}

bool dumpDecodedFrame(
        const AVFrame& frame,
        const char* outputPath)
    {
        if (
            outputPath == nullptr ||
            frame.width <= 0 ||
            frame.height <= 0)
        {
            return false;
        }

        const AVFrame* source = &frame;

        AVFrame* transferred = nullptr;

        const auto frameFormat =
            static_cast<AVPixelFormat>(
                frame.format
            );

        const auto* descriptor =
            av_pix_fmt_desc_get(
                frameFormat
            );

        const bool hardwareFrame =
            descriptor != nullptr &&
            (
                descriptor->flags &
                AV_PIX_FMT_FLAG_HWACCEL
            );

        if (hardwareFrame)
        {
            transferred =
                av_frame_alloc();

            if (transferred == nullptr)
            {
                return false;
            }

            const int transferResult =
                av_hwframe_transfer_data(
                    transferred,
                    &frame,
                    0
                );

            if (transferResult < 0)
            {
                REX::ERROR(
                    "Failed to transfer hardware frame for dump: {}.",
                    transferResult
                );

                av_frame_free(
                    &transferred
                );

                return false;
            }

            source = transferred;
        }

        const int width =
            source->width;

        const int height =
            source->height;

        const auto sourceFormat =
            static_cast<AVPixelFormat>(
                source->format
            );

        SwsContext* sws =
            sws_getContext(
                width,
                height,
                sourceFormat,
                width,
                height,
                AV_PIX_FMT_BGRA,
                SWS_BILINEAR,
                nullptr,
                nullptr,
                nullptr
            );

        if (sws == nullptr)
        {
            av_frame_free(
                &transferred
            );

            REX::ERROR(
                "Failed to create swscale context for decoded frame dump."
            );

            return false;
        }

        const int stride =
            width * 4;

        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(stride) *
            static_cast<std::size_t>(height)
        );

        std::uint8_t* destinationData[4]{
            pixels.data(),
            nullptr,
            nullptr,
            nullptr
        };

        int destinationStride[4]{
            stride,
            0,
            0,
            0
        };

        const int convertedHeight =
            sws_scale(
                sws,
                source->data,
                source->linesize,
                0,
                height,
                destinationData,
                destinationStride
            );

        sws_freeContext(
            sws
        );

        av_frame_free(
            &transferred
        );

        if (convertedHeight != height)
        {
            REX::ERROR(
                "Decoded frame conversion failed: {}/{} rows converted.",
                convertedHeight,
                height
            );

            return false;
        }

        REX::INFO(
            "Writing decoded frame dump to {}",
            outputPath
        );

        return writeTga(
            outputPath,
            pixels.data(),
            width,
            height,
            stride
        );
    }

    AVPixelFormat getHardwareFormat(
        AVCodecContext* codecContext,
        const AVPixelFormat* formats)
    {
        if (
            codecContext == nullptr ||
            codecContext->opaque == nullptr ||
            formats == nullptr)
        {
            REX::ERROR(
                "FFmpeg supplied invalid hardware format context."
            );

            return AV_PIX_FMT_NONE;
        }

        const auto* hardwarePixelFormat =
            static_cast<const AVPixelFormat*>(
                codecContext->opaque
            );

        REX::INFO(
            "FFmpeg requested hardware pixel format selection."
        );

        for (
            const AVPixelFormat* format = formats;
            *format != AV_PIX_FMT_NONE;
            ++format)
        {
            const char* formatName =
                av_get_pix_fmt_name(*format);

            const AVPixFmtDescriptor* descriptor =
                av_pix_fmt_desc_get(*format);

            const bool hardwareFormat =
                descriptor != nullptr &&
                (
                    descriptor->flags &
                    AV_PIX_FMT_FLAG_HWACCEL
                );

            REX::INFO(
                "Offered pixel format: {} ({}) hardware={}",
                formatName
                    ? formatName
                    : "unknown",
                static_cast<int>(*format),
                hardwareFormat
            );

            if (*format == *hardwarePixelFormat)
            {
                REX::INFO(
                    "Selecting hardware pixel format: {}",
                    formatName
                        ? formatName
                        : "unknown"
                );

                return *format;
            }
        }

        REX::ERROR(
            "FFmpeg did not offer the selected hardware pixel format."
        );

        return AV_PIX_FMT_NONE;
    }

    const AVCodecHWConfig* findHardwareConfig(
        const AVCodec* codec,
        AVHWDeviceType deviceType)
    {
        if (codec == nullptr)
        {
            return nullptr;
        }

        for (int index = 0;; ++index)
        {
            const AVCodecHWConfig* config =
                avcodec_get_hw_config(
                    codec,
                    index
                );

            if (config == nullptr)
            {
                return nullptr;
            }

            if (
                config->device_type == deviceType &&
                (
                    config->methods &
                    AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX
                ))
            {
                return config;
            }
        }
    }

    bool writeTga( //TGA is simplest, most direct-to-write image format, find a conversion tool, or a tool that opens it if you don't like that.
        const char* path,
        const std::uint8_t* data,
        int width,
        int height,
        int stride)
    {
        if (
            path == nullptr ||
            data == nullptr ||
            width <= 0 ||
            height <= 0 ||
            stride < width * 4)
        {
            return false;
        }

        std::ofstream file(
            path,
            std::ios::binary
        );

        if (!file)
        {
            return false;
        }

        std::uint8_t header[18]{};

        // Uncompressed true-color image.
        header[2] = 2;

        // Width.
        header[12] = static_cast<std::uint8_t>(width & 0xFF);
        header[13] = static_cast<std::uint8_t>((width >> 8) & 0xFF);

        // Height.
        header[14] = static_cast<std::uint8_t>(height & 0xFF);
        header[15] = static_cast<std::uint8_t>((height >> 8) & 0xFF);

        // 32 bits per pixel.
        header[16] = 32;

        // 8 alpha bits + top-left origin.
        header[17] = 0x28;

        file.write(
            reinterpret_cast<const char*>(header),
            sizeof(header)
        );

        for (int y = 0; y < height; ++y)
        {
            file.write(
                reinterpret_cast<const char*>(
                    data + static_cast<std::size_t>(y) * stride
                ),
                static_cast<std::streamsize>(width * 4)
            );
        }

        return file.good();
    }
}

double decoder::getDuration() const
{
    if (
        formatContext == nullptr ||
        videoStreamIndex < 0 ||
        videoStreamIndex >=
            static_cast<int>(
                formatContext->nb_streams
            ))
    {
        return -1.0;
    }

    const AVStream* stream =
        formatContext
            ->streams[videoStreamIndex];

    if (
        stream->duration !=
            AV_NOPTS_VALUE &&
        stream->duration > 0)
    {
        return
            static_cast<double>(
                stream->duration
            ) *
            av_q2d(
                stream->time_base
            );
    }

    if (
        formatContext->duration !=
            AV_NOPTS_VALUE &&
        formatContext->duration > 0)
    {
        return
            static_cast<double>(
                formatContext->duration
            ) /
            AV_TIME_BASE;
    }

    return -1.0;
}

bool decoder::seek(
    double timestamp)
{
    if (
        formatContext == nullptr ||
        codecContext == nullptr ||
        videoStreamIndex < 0 ||
        !std::isfinite(timestamp))
    {
        return false;
    }
    REX::TRACE("Decoder is seeking");

    AVStream* stream =
        formatContext
            ->streams[videoStreamIndex];

    const auto timestampUs =
        static_cast<std::int64_t>(
            std::llround(
                timestamp *
                AV_TIME_BASE
            )
        );

    const auto targetTimestamp =
        av_rescale_q(
            timestampUs,
            AV_TIME_BASE_Q,
            stream->time_base
        );

    if (packet != nullptr)
    {
        av_packet_unref(packet);
    }

    const int result =
        av_seek_frame(
            formatContext,
            videoStreamIndex,
            targetTimestamp,
            AVSEEK_FLAG_BACKWARD
        );

    if (result < 0)
    {
        REX::TRACE(
            "FFmpeg seek to {:.3f}s failed: {}",
            timestamp,
            result
        );

        return false;
    }

    avcodec_flush_buffers(
        codecContext
    );

    decoderDraining = false;
    decoderEOF = false;
    currentTimestamp = -1.0;

    REX::TRACE(
        "Decoder seeked toward {:.3f}s.",
        timestamp
    );

    return true;
}

bool decoder::initializeHardwareDevice(
    AVHWDeviceType deviceType)
{
    av_buffer_unref(
        &hardwareDeviceContext
    );

    const char* deviceName =
        av_hwdevice_get_type_name(
            deviceType
        );

    REX::DEBUG(
        "Trying FFmpeg hardware device: {}",
        deviceName
            ? deviceName
            : "unknown"
    );

    AVDictionary* options = nullptr;

    if (deviceType == AV_HWDEVICE_TYPE_VULKAN)
    {
        av_dict_set(
            &options,
            "device_extensions",
            "VK_KHR_win32_keyed_mutex",
            0
        );
    }

    const int result =
        av_hwdevice_ctx_create(
            &hardwareDeviceContext,
            deviceType,
            nullptr,
            options,
            0
        );

    av_dict_free(
        &options
    );

    if (result < 0)
    {
        hardwareDeviceContext = nullptr;

        REX::ERROR(
            "Failed to create FFmpeg hardware device {}: {}",
            deviceName
                ? deviceName
                : "unknown",
            result
        );

        return false;
    }

    REX::INFO(
        "FFmpeg hardware device initialized: {}",
        deviceName
            ? deviceName
            : "unknown"
    );

    return true;
}

std::shared_ptr<producedFrame>
decoder::acquireProducedFrame(
    int width,
    int height)
{
    std::scoped_lock lock(producedFramesMutex);

    for (auto& output : producedFrames)
    {
        if (
            output.use_count() == 1 &&
            output->width ==
                static_cast<std::uint32_t>(width) &&
            output->height ==
                static_cast<std::uint32_t>(height))
        {
            return output;
        }
    }

    auto output =
        createProducedFrame(width, height);

    if (!output)
        return nullptr;

    producedFrames.emplace_back(output);

    REX::TRACE(
        "Produced frame pool grew to {} surfaces.",
        producedFrames.size()
    );

    return output;
}


namespace
{
    std::atomic<std::uint64_t>
        frameDumpGeneration{0};

    std::mutex frameDumpPathMutex;

    std::string frameDumpPath;
}

void decoder::clearProducedFrames()
{

    {
        std::scoped_lock lock(
            producedFramesMutex
        );

        producedFrames.clear();
    }
}

std::shared_ptr<producedFrame>
decoder::frameProduce(
    const AVFrame* frame)
{
    if (frame == nullptr)
        return nullptr;

    auto output =
        acquireProducedFrame(
            frame->width,
            frame->height
        );

    if (!output)
        return nullptr;

    const bool vulkan =
        frame->format ==
            AV_PIX_FMT_VULKAN;

    const bool d3d11 =
        frame->format ==
            AV_PIX_FMT_D3D11;

    bool produced = false;

    if (vulkan)
    {
        produced =
            frameProduceVulkan(
                frame,
                *output
            );
    }
    else if (d3d11)
    {
        produced =
            frameProduceD3D11(
                frame,
                *output
            );
    }

    const auto dumpGeneration =
        frameDumpGeneration.load(
            std::memory_order_acquire
        );

    if (
        dumpGeneration !=
            handledProducedFrameDumpGeneration)
    {
        handledProducedFrameDumpGeneration =
            dumpGeneration;

        std::string basePath;

        {
            std::scoped_lock lock(
                frameDumpPathMutex
            );

            basePath =
                frameDumpPath;
        }

        if (produced)
        {
            const auto outputPath =
                makeFrameDumpPath(
                    basePath,
                    vulkan
                        ? "vulkan"
                        : "d3d11"
                );

            if (!dumpProducedFrame(
                    *output,
                    outputPath.c_str()))
            {
                REX::WARN(
                    "Produced frame asset "
                    "was not dumped."
                );
            }
        }
        else
        {
            REX::DEBUG(
                "Produced frame dump skipped: "
                "no produced frame asset exists."
            );
        }
    }

    if (!produced)
        return nullptr;

    return output;
}

producedFrame::~producedFrame()
{
    vulkanState.reset();

    if (texture != nullptr)
    {
        texture->Release();
        texture = nullptr;
    }
}

std::shared_ptr<producedFrame>
decoder::createProducedFrame(
    int width,
    int height)
{
    if (width <= 0 || height <= 0)
    {
        return nullptr;
    }

    auto* device =
        getD3D11Device();

    if (device == nullptr)
    {
        return nullptr;
    }

    REX::W32::D3D11_TEXTURE2D_DESC textureDesc{};

    textureDesc.width =
        static_cast<std::uint32_t>(width);

    textureDesc.height =
        static_cast<std::uint32_t>(height);

    textureDesc.mipLevels = 1;
    textureDesc.arraySize = 1;

    textureDesc.format =
        REX::W32::DXGI_FORMAT_R8G8B8A8_UNORM;

    textureDesc.sampleDesc.count = 1;
    textureDesc.sampleDesc.quality = 0;

    textureDesc.usage =
        REX::W32::D3D11_USAGE_DEFAULT;

    textureDesc.bindFlags =
        REX::W32::D3D11_BIND_SHADER_RESOURCE |
        REX::W32::D3D11_BIND_RENDER_TARGET;

    textureDesc.cpuAccessFlags = 0;

    textureDesc.miscFlags =
        REX::W32::D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
        REX::W32::D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    auto output =
        std::make_shared<producedFrame>();

    REX::TRACE(
        "Creating produced D3D11 texture: {}x{}",
        width,
        height
    );

    const auto result =
        device->CreateTexture2D(
            &textureDesc,
            nullptr,
            &output->texture
        );

    REX::TRACE(
        "CreateTexture2D returned 0x{:08X}, texture={}",
        static_cast<std::uint32_t>(result),
        static_cast<void*>(output->texture)
    );

    if (result < 0)
    {
        return nullptr;
    }

    output->width =
        static_cast<std::uint32_t>(width);

    output->height =
        static_cast<std::uint32_t>(height);

    return output;
}

bool decoder::frameProduceVulkan(
    const AVFrame* frame,
    producedFrame& output)
{
    if (
        frame == nullptr ||
        output.texture == nullptr ||
        frame->format != AV_PIX_FMT_VULKAN ||
        frame->data[0] == nullptr ||
        frame->hw_frames_ctx == nullptr)
    {
        return false;
    }

    auto* vkFrame =
        reinterpret_cast<AVVkFrame*>(
            frame->data[0]
        );

    auto* framesContext =
        reinterpret_cast<AVHWFramesContext*>(
            frame->hw_frames_ctx->data
        );

    if (
        framesContext == nullptr ||
        framesContext->device_ctx == nullptr ||
        framesContext->hwctx == nullptr)
    {
        return false;
    }

    auto* vkFramesContext =
        static_cast<AVVulkanFramesContext*>(
            framesContext->hwctx
        );

    auto* vkDeviceContext =
        static_cast<AVVulkanDeviceContext*>(
            framesContext->device_ctx->hwctx
        );

    if (
        vkFrame == nullptr ||
        vkFramesContext == nullptr ||
        vkDeviceContext == nullptr)
    {
        return false;
    }

    if (!output.vulkanState)
    {
        if (!ensureProducedFrameVulkanState(
                output,
                *framesContext,
                *vkDeviceContext))
        {
            return false;
        }
    }

    auto& outputState =
        *output.vulkanState;

    REX::TRACE(
        "Producing Vulkan frame: {}x{}, VkImage={}",
        frame->width,
        frame->height,
        reinterpret_cast<void*>(
            vkFrame->img[0]
        )
    ); // Checkpoint, we've by this point proven a Vulkan decoded frame and it's state. From here on we are preparing to send it ownward to Fallout's D3D11 device.

    if (!ensureVulkanRgbaSurface(
            outputState,
            *framesContext,
            *vkFramesContext,
            *vkDeviceContext,
            frame->width,
            frame->height))
    {
        return false;
    }

    if (!submitVulkanFrame(
            *vkFrame,
            *framesContext,
            *vkFramesContext,
            *vkDeviceContext,
            outputState))
    {
        return false;
    }

    REX::TRACE(
        "Submitting Vulkan RGBA surface: {}x{}, VkImage={}",
        frame->width,
        frame->height,
        reinterpret_cast<void*>(
            outputState.rgbaImage
        )
    ); // Final checkpoint where we can now confirm if we've properly converted.

    return true;
}

bool decoder::frameProduceD3D11(
    const AVFrame* frame,
    producedFrame& output)
{
    if (
        frame == nullptr ||
        output.texture == nullptr ||
        frame->format != AV_PIX_FMT_D3D11)
    {
        return false;
    }

    // TODO:
    // Convert/copy FFmpeg D3D11 frame into
    // canonical output.texture.

    return false;
}

void frameDump(
    const char* outputPath)
{
    if (outputPath == nullptr)
        return;

    {
        std::scoped_lock lock(
            frameDumpPathMutex
        );

        frameDumpPath =
            outputPath;
    }

    frameDumpGeneration.fetch_add(
        1,
        std::memory_order_release
    );

    REX::DEBUG(
        "Decoded frame dump requested."
    );
}

double decoder::getFrameTimestamp(
    const AVFrame* frame) const
{
    if (
        frame == nullptr ||
        formatContext == nullptr ||
        videoStreamIndex < 0 ||
        frame->best_effort_timestamp == AV_NOPTS_VALUE)
    {
        return -1.0;
    }

    const AVStream* stream =
        formatContext->streams[videoStreamIndex];

    return
        frame->best_effort_timestamp *
        av_q2d(stream->time_base);
}


double decoder::getCurrentTimestamp() const
{
    return currentTimestamp;
}

decodeResult decoder::decodeNextFrame(
    AVFrame* outputFrame)
{
    if (
        formatContext == nullptr ||
        codecContext == nullptr ||
        outputFrame == nullptr)
    {
        return {
            decodeStatus::ffmpegError,
            AVERROR(EINVAL)
        };
    }

    if (decoderEOF)
    {
        return {
            decodeStatus::endOfFile,
            0
        };
    }

    av_frame_unref(outputFrame);

    while (true)
    {
        // First ask FFmpeg whether it already has a decoded frame ready.
        REX::TRACE("Calling to receive a frame...");
        const int receiveResult =
            avcodec_receive_frame(
                codecContext,
                outputFrame
            );

            REX::TRACE("We've received receiveResult: {}", receiveResult);

        if (receiveResult >= 0)
        {
            currentTimestamp =
                getFrameTimestamp(outputFrame); //Get timestamp of output frame rather than decoder frame.

        const auto dumpGeneration =
            frameDumpGeneration.load(
                std::memory_order_acquire
            );

        if (
            dumpGeneration !=
                handledDecodedFrameDumpGeneration)
        {
            handledDecodedFrameDumpGeneration =
                dumpGeneration;

            std::string basePath;

            {
                std::scoped_lock lock(
                    frameDumpPathMutex
                );

                basePath =
                    frameDumpPath;
            }

            const auto outputPath =
                makeFrameDumpPath(
                    basePath,
                    "decoded"
                );

            if (!dumpDecodedFrame(
                    *outputFrame,
                    outputPath.c_str()))
            {
                REX::WARN(
                    "Decoded frame asset was not dumped."
                );
            }
        }

            return {
                decodeStatus::frameReady,
                0
            };
        }

        if (receiveResult == AVERROR_EOF)
        {
            decoderEOF = true;

            return {
                decodeStatus::endOfFile,
                0
            };
        }

        if (receiveResult != AVERROR(EAGAIN))
        {
            return {
                decodeStatus::ffmpegError,
                receiveResult
            };
        }

        // EAGAIN means FFmpeg needs more compressed data.
        if (decoderDraining)
        {

            decoderEOF = true;

            return{
             decodeStatus::endOfFile,
             0
            };
        }

        bool packetSent = false;

        while (!packetSent)
        {
            REX::TRACE("Calling to read frame...");
            const int readResult =
                av_read_frame(
                    formatContext,
                    packet
                );
                REX::TRACE("We've received readResult: {}", readResult);

            if (readResult < 0)
            {
                // Demuxer reached the end. Flush the decoder.
                const int flushResult =
                    avcodec_send_packet(
                        codecContext,
                        nullptr
                    );

                decoderDraining = true;

                if (
                    flushResult < 0 &&
                    flushResult != AVERROR_EOF)
                {
                    return {
                        decodeStatus::ffmpegError,
                        flushResult
                    };
                }

                break;
            }

            if (packet->stream_index != videoStreamIndex)
            {
                av_packet_unref(packet);
                continue;
            }


                REX::TRACE("Decode worker is now running.");

            const int sendResult =
                avcodec_send_packet(
                    codecContext,
                    packet
                );

                REX::TRACE("We've received sendResult: {}", sendResult);

            av_packet_unref(packet);

            if (sendResult == AVERROR(EAGAIN))
            {
                // Decoder wants us to receive something first.
                break;
            }

            if (sendResult < 0)
            {
                return {
                    decodeStatus::ffmpegError,
                    sendResult
                };
            }

            packetSent = true;
        }
    }
}


bool decoder::initializeVideoDecoder()
{
    if (formatContext == nullptr)
    {
        return false;
    }

    if (codecContext != nullptr)
    {
        avcodec_free_context(&codecContext);
        av_buffer_unref(&hardwareDeviceContext);
    }

    av_packet_free(&packet);

    videoStreamIndex = av_find_best_stream(
        formatContext,
        AVMEDIA_TYPE_VIDEO,
        -1,
        -1,
        &videoCodec,
        0
    );

    if (videoStreamIndex < 0)
    {
        return false;
    }

    codecContext = avcodec_alloc_context3(videoCodec);

    if (codecContext == nullptr)
    {
        return false;
    }

    AVStream* videoStream =
        formatContext->streams[videoStreamIndex];

    if (avcodec_parameters_to_context(
            codecContext,
            videoStream->codecpar) < 0)
    {
        avcodec_free_context(
            &codecContext
        );

        return false;
    }

    const AVCodecHWConfig* hardwareConfig =
        findHardwareConfig(
            videoCodec,
            AV_HWDEVICE_TYPE_VULKAN
        );

    if (hardwareConfig != nullptr)
    {
        hardwareDeviceType =
            AV_HWDEVICE_TYPE_VULKAN;

        hardwarePixelFormat =
            hardwareConfig->pix_fmt;

        REX::TRACE(
            "Codec {} supports Vulkan hardware decoding.",
            videoCodec->name
        );
    }
    else
    {
        hardwareConfig =
            findHardwareConfig(
                videoCodec,
                AV_HWDEVICE_TYPE_D3D11VA
            );

        if (hardwareConfig == nullptr)
        {
            REX::ERROR(
                "Codec {} supports neither Vulkan nor D3D11VA hardware decoding.",
                videoCodec->name
            );

            avcodec_free_context(
                &codecContext
            );

            return false;
        }

        hardwareDeviceType =
            AV_HWDEVICE_TYPE_D3D11VA;

        hardwarePixelFormat =
            hardwareConfig->pix_fmt;

        REX::DEBUG(
            "Codec {} supports D3D11VA hardware decoding.",
            videoCodec->name
        );
    }


    if (!initializeHardwareDevice(
            hardwareDeviceType))
    {
        if (
            hardwareDeviceType !=
            AV_HWDEVICE_TYPE_VULKAN)
        {
            avcodec_free_context(
                &codecContext
            );

            return false;
        }

        REX::ERROR(
            "Vulkan device unavailable; trying D3D11VA."
        );

        hardwareConfig =
            findHardwareConfig(
                videoCodec,
                AV_HWDEVICE_TYPE_D3D11VA
            );

        if (hardwareConfig == nullptr)
        {
            avcodec_free_context(
                &codecContext
            );

            return false;
        }

        hardwareDeviceType =
            AV_HWDEVICE_TYPE_D3D11VA;

        hardwarePixelFormat =
            hardwareConfig->pix_fmt;

        if (!initializeHardwareDevice(
                hardwareDeviceType))
        {
            avcodec_free_context(
                &codecContext
            );

            return false;
        }

    }


    codecContext->opaque =
        &hardwarePixelFormat;

    codecContext->get_format =
        getHardwareFormat;


    codecContext->hw_device_ctx =
        av_buffer_ref(
            hardwareDeviceContext
        );

    if (codecContext->hw_device_ctx == nullptr)
    {
        avcodec_free_context(
            &codecContext
        );

        av_buffer_unref(
            &hardwareDeviceContext
        );

        return false;
    }
            const int openResult =
                avcodec_open2(
                    codecContext,
                    videoCodec,
                    nullptr
                );

            if (openResult < 0)
            {
                REX::ERROR(
                    "Failed to open hardware video decoder: {}",
                    openResult
                );

                avcodec_free_context(
                    &codecContext
                );

                return false;
            }

            packet =
                av_packet_alloc();

            if (packet == nullptr)
            {
                avcodec_free_context(
                    &codecContext
                );

                return false;
            }

            decoderDraining = false;
            decoderEOF = false;

            return true;
        }

        decoder::~decoder()
        {
            close();
        }
        bool decoder::open(const char* path)
    {
        if (formatContext != nullptr)
        {
            close();
        } //If there is a formatContext we need to close it first.


        if (avformat_open_input(
                &formatContext,
                path,
                nullptr,
                nullptr) < 0)
        {
            formatContext = nullptr; //Release formatContext, we failed to open. Maybe have a failure flag?

            return false;
        }

        if (avformat_find_stream_info(
                formatContext,
                nullptr) < 0)
        {
            close();

            return false;
        }

        const auto dumpGeneration =
            frameDumpGeneration.load(
                std::memory_order_acquire
            );

        handledDecodedFrameDumpGeneration =
            dumpGeneration;

        handledProducedFrameDumpGeneration =
            dumpGeneration;

        return true;
    }

    void decoder::close()
    {
        av_packet_free(&packet);

        if (codecContext != nullptr)
        {
            avcodec_free_context(&codecContext);
        }


        av_buffer_unref(&hardwareDeviceContext);

        if (formatContext != nullptr)
        {
            avformat_close_input(&formatContext);
        }

        videoCodec = nullptr;
        videoStreamIndex = -1;

        decoderDraining = false;
        decoderEOF = false;
        clearProducedFrames();
    }
            //Get ffmpeg logs.
    namespace
    {
        void ffmpegLogCallback(
            void*,
            int level,
            const char* format,
            va_list args)
        {
            if (level > AV_LOG_VERBOSE)
            {
                return;
            }

            char buffer[1024]{};

            vsnprintf(
                buffer,
                sizeof(buffer),
                format,
                args
            );

            REX::TRACE(
                "[FFmpeg] {}",
                buffer
            );
        }
    }


    void initializeFFmpegLogging()
    {
        av_log_set_level(
            AV_LOG_VERBOSE
        );

        av_log_set_callback(
            ffmpegLogCallback
        );
    }
}
