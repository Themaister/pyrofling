/* Copyright (c) 2023 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <memory>
#include <string.h>
#include <unordered_map>
#include <algorithm>
#include <vector>

// Isolate to just what we need. Should improve layer init time.

// Instance function pointer dispatch table
struct VkLayerInstanceDispatchTable
{
	PFN_vkDestroyInstance DestroyInstance;
	PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
	PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
	PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
	PFN_vkGetPhysicalDeviceProperties2KHR GetPhysicalDeviceProperties2KHR;
	PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
	PFN_vkDestroySurfaceKHR DestroySurfaceKHR;

	// Queries. We have to wrap these and forward to either sink device or passthrough.
	// VK_KHR_surface
	PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormatsKHR;
	PFN_vkGetPhysicalDeviceSurfaceSupportKHR GetPhysicalDeviceSurfaceSupportKHR;
	PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR;
	PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR;

	// VK_KHR_get_surface_capabilities2
	PFN_vkGetPhysicalDeviceSurfaceFormats2KHR GetPhysicalDeviceSurfaceFormats2KHR;
	PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR GetPhysicalDeviceSurfaceCapabilities2KHR;

	// VK_KHR_display
	PFN_vkCreateDisplayModeKHR CreateDisplayModeKHR;
	PFN_vkGetDisplayModePropertiesKHR GetDisplayModePropertiesKHR;
	PFN_vkGetDisplayPlaneCapabilitiesKHR GetDisplayPlaneCapabilitiesKHR;
	PFN_vkGetDisplayPlaneSupportedDisplaysKHR GetDisplayPlaneSupportedDisplaysKHR;
	PFN_vkGetPhysicalDeviceDisplayPlanePropertiesKHR GetPhysicalDeviceDisplayPlanePropertiesKHR;
	PFN_vkGetPhysicalDeviceDisplayPropertiesKHR GetPhysicalDeviceDisplayPropertiesKHR;

	// VK_KHR_get_display_properties2
	PFN_vkGetDisplayModeProperties2KHR GetDisplayModeProperties2KHR;
	PFN_vkGetDisplayPlaneCapabilities2KHR GetDisplayPlaneCapabilities2KHR;
	PFN_vkGetPhysicalDeviceDisplayPlaneProperties2KHR GetPhysicalDeviceDisplayPlaneProperties2KHR;
	PFN_vkGetPhysicalDeviceDisplayProperties2KHR GetPhysicalDeviceDisplayProperties2KHR;

	// External memory
	PFN_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR GetPhysicalDeviceExternalSemaphorePropertiesKHR;
	PFN_vkGetPhysicalDeviceExternalFencePropertiesKHR GetPhysicalDeviceExternalFencePropertiesKHR;
	PFN_vkGetPhysicalDeviceExternalBufferPropertiesKHR GetPhysicalDeviceExternalBufferPropertiesKHR;

	PFN_vkGetPhysicalDeviceSurfaceCapabilities2EXT GetPhysicalDeviceSurfaceCapabilities2EXT;
	PFN_vkGetPhysicalDevicePresentRectanglesKHR GetPhysicalDevicePresentRectanglesKHR;
	PFN_vkReleaseDisplayEXT ReleaseDisplayEXT;
	PFN_vkAcquireDrmDisplayEXT AcquireDrmDisplayEXT;
	PFN_vkGetDrmDisplayEXT GetDrmDisplayEXT;
};

// Device function pointer dispatch table
struct VkLayerDispatchTable
{
	PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
	PFN_vkDestroyDevice DestroyDevice;
	PFN_vkGetDeviceQueue GetDeviceQueue;

	PFN_vkCreateSwapchainKHR CreateSwapchainKHR;
	PFN_vkDestroySwapchainKHR DestroySwapchainKHR;
	PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
	PFN_vkAcquireNextImageKHR AcquireNextImageKHR;
	PFN_vkAcquireNextImage2KHR AcquireNextImage2KHR;
	PFN_vkReleaseSwapchainImagesEXT ReleaseSwapchainImagesEXT;

	PFN_vkQueueSubmit QueueSubmit;
	PFN_vkQueueWaitIdle QueueWaitIdle;
	PFN_vkQueuePresentKHR QueuePresentKHR;

	PFN_vkCreateCommandPool CreateCommandPool;
	PFN_vkDestroyCommandPool DestroyCommandPool;
	PFN_vkResetCommandPool ResetCommandPool;
	PFN_vkBeginCommandBuffer BeginCommandBuffer;
	PFN_vkEndCommandBuffer EndCommandBuffer;
	PFN_vkAllocateCommandBuffers AllocateCommandBuffers;

	PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
	PFN_vkCmdCopyImage CmdCopyImage;
	PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer;
	PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage;

	PFN_vkCreateFence CreateFence;
	PFN_vkWaitForFences WaitForFences;
	PFN_vkResetFences ResetFences;
	PFN_vkDestroyFence DestroyFence;

	PFN_vkCreateImage CreateImage;
	PFN_vkCreateBuffer CreateBuffer;
	PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
	PFN_vkAllocateMemory AllocateMemory;
	PFN_vkBindImageMemory BindImageMemory;
	PFN_vkDestroyImage DestroyImage;
	PFN_vkDestroyBuffer DestroyBuffer;
	PFN_vkFreeMemory FreeMemory;

	PFN_vkCreateSemaphore CreateSemaphore;
	PFN_vkDestroySemaphore DestroySemaphore;

#ifndef _WIN32
	PFN_vkGetSemaphoreFdKHR GetSemaphoreFdKHR;
	PFN_vkImportSemaphoreFdKHR ImportSemaphoreFdKHR;
	PFN_vkImportFenceFdKHR ImportFenceFdKHR;
	PFN_vkGetMemoryFdKHR GetMemoryFdKHR;
#endif
};

static inline VkLayerInstanceCreateInfo *getChainInfo(const VkInstanceCreateInfo *pCreateInfo, VkLayerFunction func)
{
	auto *chain_info = static_cast<const VkLayerInstanceCreateInfo *>(pCreateInfo->pNext);
	while (chain_info &&
	       !(chain_info->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && chain_info->function == func))
		chain_info = static_cast<const VkLayerInstanceCreateInfo *>(chain_info->pNext);
	return const_cast<VkLayerInstanceCreateInfo *>(chain_info);
}

static inline VkLayerDeviceCreateInfo *getChainInfo(const VkDeviceCreateInfo *pCreateInfo, VkLayerFunction func)
{
	auto *chain_info = static_cast<const VkLayerDeviceCreateInfo *>(pCreateInfo->pNext);
	while (chain_info &&
	       !(chain_info->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && chain_info->function == func))
		chain_info = static_cast<const VkLayerDeviceCreateInfo *>(chain_info->pNext);
	return const_cast<VkLayerDeviceCreateInfo *>(chain_info);
}

void layerInitDeviceDispatchTable(VkDevice device, VkLayerDispatchTable *table, PFN_vkGetDeviceProcAddr gpa);

void layerInitInstanceDispatchTable(VkInstance instance, VkLayerInstanceDispatchTable *table,
                                    PFN_vkGetInstanceProcAddr gpa);

#if CURRENT_LOADER_LAYER_INTERFACE_VERSION != 2
#error "Unexpected loader layer interface version."
#endif

#undef VK_LAYER_EXPORT
#ifdef _WIN32
#define VK_LAYER_EXPORT extern "C" __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
#define VK_LAYER_EXPORT extern "C" __attribute__((visibility("default")))
#else
#define VK_LAYER_EXPORT
#endif

void addUniqueExtension(std::vector<const char *> &extensions, const char *name);
void addUniqueExtension(std::vector<const char *> &extensions,
                        const std::vector<VkExtensionProperties> &allowed,
                        const char *name);

bool findExtension(const std::vector<VkExtensionProperties> &props, const char *ext);
bool findExtension(const char * const *ppExtensions, uint32_t count, const char *ext);

template <size_t N>
static inline bool findExtension(const char * (&exts)[N], const char *ext)
{
	return findExtension(exts, N, ext);
}

template <typename T>
static inline const T *findChain(const void *pNext, VkStructureType sType)
{
	while (pNext)
	{
		auto *s = static_cast<const VkBaseInStructure *>(pNext);
		if (s->sType == sType)
			return static_cast<const T *>(pNext);

		pNext = s->pNext;
	}

	return nullptr;
}