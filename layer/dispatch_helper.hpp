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

// Isolate to just what we need. Should improve layer init time.

// Instance function pointer dispatch table
struct VkLayerInstanceDispatchTable
{
	PFN_vkDestroyInstance DestroyInstance;
	PFN_vkGetPhysicalDeviceProperties2KHR GetPhysicalDeviceProperties2KHR;
	PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;

	// Queries
	PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormatsKHR;
	PFN_vkGetPhysicalDeviceSurfaceFormats2KHR GetPhysicalDeviceSurfaceFormats2KHR;

	// External memory
	PFN_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR GetPhysicalDeviceExternalSemaphorePropertiesKHR;
	PFN_vkGetPhysicalDeviceExternalBufferPropertiesKHR GetPhysicalDeviceExternalBufferPropertiesKHR;

	PFN_vkDestroySurfaceKHR DestroySurfaceKHR;
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

	PFN_vkQueueSubmit QueueSubmit;
	PFN_vkQueuePresentKHR QueuePresentKHR;

	PFN_vkCreateCommandPool CreateCommandPool;
	PFN_vkDestroyCommandPool DestroyCommandPool;
	PFN_vkResetCommandPool ResetCommandPool;
	PFN_vkBeginCommandBuffer BeginCommandBuffer;
	PFN_vkEndCommandBuffer EndCommandBuffer;
	PFN_vkAllocateCommandBuffers AllocateCommandBuffers;

	PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
	PFN_vkCmdCopyImage CmdCopyImage;

	PFN_vkCreateFence CreateFence;
	PFN_vkWaitForFences WaitForFences;
	PFN_vkResetFences ResetFences;
	PFN_vkDestroyFence DestroyFence;

	PFN_vkCreateImage CreateImage;
	PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
	PFN_vkAllocateMemory AllocateMemory;
	PFN_vkBindImageMemory BindImageMemory;
	PFN_vkDestroyImage DestroyImage;
	PFN_vkFreeMemory FreeMemory;

	PFN_vkCreateSemaphore CreateSemaphore;
	PFN_vkDestroySemaphore DestroySemaphore;

#ifndef _WIN32
	PFN_vkGetSemaphoreFdKHR GetSemaphoreFdKHR;
	PFN_vkImportSemaphoreFdKHR ImportSemaphoreFdKHR;
	PFN_vkGetMemoryFdKHR GetMemoryFdKHR;
#endif
};

static inline VkLayerDeviceCreateInfo *getChainInfo(const VkInstanceCreateInfo *pCreateInfo, VkLayerFunction func)
{
	auto *chain_info = static_cast<const VkLayerDeviceCreateInfo *>(pCreateInfo->pNext);
	while (chain_info &&
	       !(chain_info->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && chain_info->function == func))
		chain_info = static_cast<const VkLayerDeviceCreateInfo *>(chain_info->pNext);
	return const_cast<VkLayerDeviceCreateInfo *>(chain_info);
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

static inline void *getDispatchKey(void *ptr)
{
	return *static_cast<void **>(ptr);
}

template <typename T>
static inline T *getLayerData(void *key, const std::unordered_map<void *, std::unique_ptr<T>> &m)
{
	auto itr = m.find(key);
	if (itr != end(m))
		return itr->second.get();
	else
		return nullptr;
}

template <typename T, typename... TArgs>
static inline T *createLayerData(void *key, std::unordered_map<void *, std::unique_ptr<T>> &m, TArgs &&... args)
{
	auto *ptr = new T(std::forward<TArgs>(args)...);
	m[key] = std::unique_ptr<T>(ptr);
	return ptr;
}

template <typename T>
static inline void destroyLayerData(void *key, std::unordered_map<void *, std::unique_ptr<T>> &m)
{
	auto itr = m.find(key);
	m.erase(itr);
}
