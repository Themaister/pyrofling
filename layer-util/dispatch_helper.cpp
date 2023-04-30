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

#include "dispatch_helper.hpp"

void layerInitDeviceDispatchTable(VkDevice device, VkLayerDispatchTable *table, PFN_vkGetDeviceProcAddr gpa)
{
	*table = {};
#define F(fun) table->fun = (PFN_vk##fun)gpa(device, "vk"#fun)
	F(GetDeviceProcAddr);
	F(DestroyDevice);
	F(GetDeviceQueue);
	F(CreateSwapchainKHR);
	F(DestroySwapchainKHR);
	F(GetSwapchainImagesKHR);
	F(QueueSubmit);
	F(QueuePresentKHR);
	F(CreateCommandPool);
	F(DestroyCommandPool);
	F(ResetCommandPool);
	F(BeginCommandBuffer);
	F(EndCommandBuffer);
	F(AllocateCommandBuffers);
	F(CmdPipelineBarrier);
	F(CmdCopyImage);
	F(CreateFence);
	F(WaitForFences);
	F(ResetFences);
	F(DestroyFence);
	F(CreateImage);
	F(GetImageMemoryRequirements);
	F(AllocateMemory);
	F(FreeMemory);
	F(BindImageMemory);
	F(DestroyImage);
	F(CreateSemaphore);
	F(DestroySemaphore);
#ifndef _WIN32
	F(GetSemaphoreFdKHR);
	F(ImportSemaphoreFdKHR);
	F(GetMemoryFdKHR);
#endif
#undef F
}

void layerInitInstanceDispatchTable(VkInstance instance, VkLayerInstanceDispatchTable *table,
                                    PFN_vkGetInstanceProcAddr gpa)
{
#define F(fun) table->fun = (PFN_vk##fun)gpa(instance, "vk"#fun)
	*table = {};
	F(DestroyInstance);
	F(EnumerateDeviceExtensionProperties);
	F(GetPhysicalDeviceQueueFamilyProperties);
	F(GetPhysicalDeviceMemoryProperties);
	F(GetPhysicalDeviceExternalSemaphorePropertiesKHR);
	F(GetPhysicalDeviceExternalFencePropertiesKHR);
	F(GetPhysicalDeviceExternalBufferPropertiesKHR);
	F(GetPhysicalDeviceProperties2KHR);
	F(EnumeratePhysicalDevices);

	F(GetPhysicalDeviceSurfaceFormatsKHR);
	F(GetPhysicalDeviceSurfaceSupportKHR);
	F(GetPhysicalDeviceSurfaceCapabilitiesKHR);
	F(GetPhysicalDeviceSurfacePresentModesKHR);
	F(GetPhysicalDeviceSurfaceFormats2KHR);
	F(GetPhysicalDeviceSurfaceCapabilities2KHR);
	F(GetPhysicalDeviceSurfaceCapabilities2EXT);
	F(CreateDisplayModeKHR);
	F(CreateDisplayPlaneSurfaceKHR);
	F(GetDisplayModePropertiesKHR);
	F(GetDisplayPlaneCapabilitiesKHR);
	F(GetDisplayPlaneSupportedDisplaysKHR);
	F(GetPhysicalDeviceDisplayPlanePropertiesKHR);
	F(GetPhysicalDeviceDisplayPropertiesKHR);
	F(GetDisplayModeProperties2KHR);
	F(GetDisplayPlaneCapabilities2KHR);
	F(GetPhysicalDeviceDisplayPlaneProperties2KHR);
	F(GetPhysicalDeviceDisplayProperties2KHR);
#undef F
}

void addUniqueExtension(std::vector<const char *> &extensions, const char *name)
{
	for (auto *ext : extensions)
		if (strcmp(ext, name) == 0)
			return;
	extensions.push_back(name);
}

void addUniqueExtension(std::vector<const char *> &extensions,
                        const std::vector<VkExtensionProperties> &allowed,
                        const char *name)
{
	for (auto *ext : extensions)
		if (strcmp(ext, name) == 0)
			return;

	for (auto &ext : allowed)
	{
		if (strcmp(ext.extensionName, name) == 0)
		{
			extensions.push_back(name);
			break;
		}
	}
}
