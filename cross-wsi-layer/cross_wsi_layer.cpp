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
#include <mutex>
#include <algorithm>
#include <thread>
#include <condition_variable>
#include <queue>
#include <chrono>

extern "C"
{
VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
VK_LAYER_PYROFLING_CROSS_WSI_vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
VK_LAYER_PYROFLING_CROSS_WSI_vkGetDeviceProcAddr(VkDevice device, const char *pName);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
VK_LAYER_PYROFLING_CROSS_WSI_vkGetInstanceProcAddr(VkInstance instance, const char *pName);
}

// These have to be supported by sink GPU rather than source GPU.
static const char *redirectedExtensions[] = {
	VK_KHR_PRESENT_ID_EXTENSION_NAME,
	VK_KHR_PRESENT_WAIT_EXTENSION_NAME,
	VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
};

// Block any extension that we don't explicitly wrap yet.
// For sinkGpu situation we invent dummy handles for VkSwapchainKHR.
static const char *blockedExtensions[] = {
	VK_KHR_DISPLAY_SWAPCHAIN_EXTENSION_NAME,
	VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME,
	VK_KHR_SHARED_PRESENTABLE_IMAGE_EXTENSION_NAME,
	VK_AMD_DISPLAY_NATIVE_HDR_EXTENSION_NAME,
	VK_EXT_DISPLAY_CONTROL_EXTENSION_NAME,
	VK_EXT_HDR_METADATA_EXTENSION_NAME,
	VK_KHR_INCREMENTAL_PRESENT_EXTENSION_NAME,
	"VK_EXT_full_screen_exclusive",
};

struct Instance;
struct Device;

struct Instance
{
	void init(VkInstance instance_, PFN_vkGetInstanceProcAddr gpa_, PFN_vkSetInstanceLoaderData setInstanceLoaderData_,
	          PFN_vkLayerCreateDevice layerCreateDevice_, PFN_vkLayerDestroyDevice layerDestroyDevice_);

	const VkLayerInstanceDispatchTable *getTable() const
	{
		return &table;
	}

	VkInstance getInstance() const
	{
		return instance;
	}

	PFN_vkVoidFunction getProcAddr(const char *pName) const
	{
		return gpa(instance, pName);
	}

	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice sinkGpu = VK_NULL_HANDLE;
	VkPhysicalDevice sourceGpu = VK_NULL_HANDLE;
	VkLayerInstanceDispatchTable table = {};
	PFN_vkGetInstanceProcAddr gpa = nullptr;
	PFN_vkSetInstanceLoaderData setInstanceLoaderData = nullptr;
	PFN_vkLayerCreateDevice layerCreateDevice = nullptr;
	PFN_vkLayerDestroyDevice layerDestroyDevice = nullptr;
	uint32_t sinkGpuQueueFamily = VK_QUEUE_FAMILY_IGNORED;

	VkPhysicalDevice findPhysicalDevice(const char *tag) const;
};

VkPhysicalDevice Instance::findPhysicalDevice(const char *tag) const
{
	uint32_t count = 0;
	table.EnumeratePhysicalDevices(instance, &count, nullptr);
	std::vector<VkPhysicalDevice> gpus(count);
	table.EnumeratePhysicalDevices(instance, &count, gpus.data());

	for (uint32_t i = 0; i < count; i++)
	{
		VkPhysicalDeviceProperties2 props2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
		table.GetPhysicalDeviceProperties2KHR(gpus[i], &props2);

		if (strstr(props2.properties.deviceName, tag) != nullptr)
			return gpus[i];
	}

	return VK_NULL_HANDLE;
}

void Instance::init(VkInstance instance_, PFN_vkGetInstanceProcAddr gpa_, PFN_vkSetInstanceLoaderData setInstanceLoaderData_,
                    PFN_vkLayerCreateDevice layerCreateDevice_, PFN_vkLayerDestroyDevice layerDestroyDevice_)
{
	instance = instance_;
	gpa = gpa_;
	setInstanceLoaderData = setInstanceLoaderData_;
	layerCreateDevice = layerCreateDevice_;
	layerDestroyDevice = layerDestroyDevice_;
	layerInitInstanceDispatchTable(instance, &table, gpa);

	if (const char *env = getenv("CROSS_WSI_SINK"))
		sinkGpu = findPhysicalDevice(env);
	if (const char *env = getenv("CROSS_WSI_SOURCE"))
		sourceGpu = findPhysicalDevice(env);

	if (sinkGpu)
	{
		uint32_t count = 0;
		table.GetPhysicalDeviceQueueFamilyProperties(sinkGpu, &count, nullptr);
		std::vector<VkQueueFamilyProperties> props(count);
		table.GetPhysicalDeviceQueueFamilyProperties(sinkGpu, &count, props.data());

		// Assume we can present with this queue. Somewhat sloppy, but whatever.
		for (uint32_t i = 0; i < count; i++)
		{
			if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
			{
				sinkGpuQueueFamily = i;
				break;
			}
		}

		if (sinkGpuQueueFamily == VK_QUEUE_FAMILY_IGNORED)
			sinkGpu = VK_NULL_HANDLE;
	}
}

struct Swapchain
{
	explicit Swapchain(Device *device);
	~Swapchain();

	VkResult init(const VkSwapchainCreateInfoKHR *pCreateInfo);

	VkResult releaseSwapchainImages(const VkReleaseSwapchainImagesInfoEXT *pReleaseInfo);

	void setPresentId(uint64_t id);
	void setPresentMode(VkPresentModeKHR mode);

	VkResult queuePresent(VkQueue queue, uint32_t index, VkFence fence);
	VkResult acquire(uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex);
	VkResult getSwapchainImages(uint32_t *pSwapchainImageCount, VkImage *pSwapchainImages);
	void retire();

	struct Buffer
	{
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
	};

	struct Image
	{
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
	};

	struct SwapchainImage
	{
		void *externalHostMemory = nullptr;
		Buffer sinkBuffer, sourceBuffer;
		Image sinkImage, sourceImage;
		VkFence sourceFence = VK_NULL_HANDLE;
		VkSemaphore sinkSemaphore = VK_NULL_HANDLE;
		VkCommandBuffer sourceCmd = VK_NULL_HANDLE;
		VkCommandBuffer sinkCmd = VK_NULL_HANDLE;
	};

	struct
	{
		VkCommandPool pool = VK_NULL_HANDLE;
		uint32_t family = VK_QUEUE_FAMILY_IGNORED;
	} sourceCmdPool, sinkCmdPool;

	Device *device;
	std::vector<SwapchainImage> images;
	std::queue<uint32_t> acquireQueue;
	VkResult swapchainStatus = VK_SUCCESS;
	VkSwapchainKHR sinkSwapchain = VK_NULL_HANDLE;

	std::mutex lock;
	std::condition_variable cond;
	std::thread worker;
	uint64_t submitCount = 0;
	uint64_t processedSourceCount = 0;

	uint32_t width = 0;
	uint32_t height = 0;

	struct Work
	{
		uint64_t presentId;
		uint32_t index;
		VkPresentModeKHR mode;
		bool setsMode;
	};
	Work nextWork = {};
	std::queue<Work> workQueue;

	static VkCommandPool createCommandPool(VkDevice device, const VkLayerDispatchTable &table, uint32_t family);
	VkResult initSourceCommands(uint32_t familyIndex);
	VkResult submitSourceWork(VkQueue queue, uint32_t index, VkFence fence);
	VkResult markResult(VkResult err);
};

struct Device
{
	void init(VkPhysicalDevice gpu_, VkDevice device_, Instance *instance_, PFN_vkGetDeviceProcAddr gpa,
			  PFN_vkSetDeviceLoaderData setDeviceLoaderData_,
	          const VkDeviceCreateInfo *pCreateInfo);

	~Device();

	const VkLayerDispatchTable *getTable() const
	{
		return &table;
	}

	VkDevice getDevice() const
	{
		return device;
	}

	VkPhysicalDevice getPhysicalDevice() const
	{
		return gpu;
	}

	Instance *getInstance() const
	{
		return instance;
	}

	PFN_vkSetDeviceLoaderData setDeviceLoaderData = nullptr;
	VkPhysicalDevice gpu = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	Instance *instance = nullptr;
	VkLayerDispatchTable table = {};

	struct QueueInfo
	{
		VkQueue queue;
		uint32_t familyIndex;
	};
	std::vector<QueueInfo> queueToFamily;
	uint32_t queueToFamilyIndex(VkQueue queue) const;

	VkDevice sinkDevice = VK_NULL_HANDLE;
	VkLayerDispatchTable sinkTable = {};
	VkQueue sinkQueue = VK_NULL_HANDLE;
	std::mutex sinkQueueLock;

	VkPhysicalDevicePresentWaitFeaturesKHR waitFeatures;
	VkPhysicalDevicePresentIdFeaturesKHR idFeatures;
	VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maint1Features;
};

#include "dispatch_wrapper.hpp"

Device::~Device()
{
	if (sinkDevice)
		instance->layerDestroyDevice(sinkDevice, nullptr, sinkTable.DestroyDevice);
}

uint32_t Device::queueToFamilyIndex(VkQueue queue) const
{
	for (auto &q : queueToFamily)
		if (q.queue == queue)
			return q.familyIndex;

	return VK_QUEUE_FAMILY_IGNORED;
}

void Device::init(VkPhysicalDevice gpu_, VkDevice device_, Instance *instance_, PFN_vkGetDeviceProcAddr gpa,
				  PFN_vkSetDeviceLoaderData setDeviceLoaderData_,
                  const VkDeviceCreateInfo *pCreateInfo)
{
	gpu = gpu_;
	device = device_;
	instance = instance_;
	setDeviceLoaderData = setDeviceLoaderData_;
	layerInitDeviceDispatchTable(device, &table, gpa);

	for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++)
	{
		auto &info = pCreateInfo->pQueueCreateInfos[i];
		if (info.flags != 0)
			continue;

		uint32_t family = info.queueFamilyIndex;
		for (uint32_t j = 0; j < info.queueCount; j++)
		{
			VkQueue queue;
			table.GetDeviceQueue(device, family, j, &queue);
			queueToFamily.push_back({ queue, family });
		}
	}

	bool usesSwapchain = findExtension(pCreateInfo->ppEnabledExtensionNames,
	                                   pCreateInfo->enabledExtensionCount,
	                                   VK_KHR_SWAPCHAIN_EXTENSION_NAME);

	if (usesSwapchain && instance->sinkGpu && gpu != instance->sinkGpu)
	{
		VkDeviceCreateInfo createInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
		VkDeviceQueueCreateInfo queueInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
		const float prio = 0.5f;
		createInfo.queueCreateInfoCount = 1;
		createInfo.pQueueCreateInfos = &queueInfo;
		queueInfo.queueCount = 1;
		queueInfo.queueFamilyIndex = instance->sinkGpuQueueFamily;
		queueInfo.pQueuePriorities = &prio;

		VkPhysicalDeviceFeatures2 features2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };

		std::vector<const char *> enabledExtensions = {
			VK_KHR_SWAPCHAIN_EXTENSION_NAME,
			VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
		};

		for (uint32_t i = 0; i < pCreateInfo->enabledLayerCount; i++)
		{
			const char *ext = pCreateInfo->ppEnabledExtensionNames[i];
			if (findExtension(redirectedExtensions, ext))
			{
				enabledExtensions.push_back(ext);

				if (strcmp(ext, VK_KHR_PRESENT_WAIT_EXTENSION_NAME) == 0)
				{
					waitFeatures = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
					                 nullptr, VK_TRUE };
					waitFeatures.pNext = features2.pNext;
					features2.pNext = &waitFeatures;
				}
				else if (strcmp(ext, VK_KHR_PRESENT_ID_EXTENSION_NAME) == 0)
				{
					idFeatures = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
					               nullptr, VK_TRUE };
					idFeatures.pNext = features2.pNext;
					features2.pNext = &idFeatures;
				}
				else if (strcmp(ext, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME) == 0)
				{
					maint1Features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT,
					                   nullptr, VK_TRUE };
					maint1Features.pNext = features2.pNext;
					features2.pNext = &maint1Features;
				}
			}
		}

		createInfo.ppEnabledExtensionNames = enabledExtensions.data();
		createInfo.enabledExtensionCount = uint32_t(enabledExtensions.size());
		createInfo.pNext = &features2;

		PFN_vkGetDeviceProcAddr gdpa = nullptr;
		if (instance->layerCreateDevice(instance->instance, instance->sinkGpu, &createInfo, nullptr, &sinkDevice,
		                                VK_LAYER_PYROFLING_CROSS_WSI_vkGetInstanceProcAddr, &gdpa) != VK_SUCCESS)
			return;

		layerInitDeviceDispatchTable(sinkDevice, &sinkTable, gdpa);
		sinkTable.GetDeviceQueue(sinkDevice, instance->sinkGpuQueueFamily, 0, &sinkQueue);
	}
}

Swapchain::Swapchain(Device *device_)
	: device(device_)
{
}

VkCommandPool Swapchain::createCommandPool(VkDevice device, const VkLayerDispatchTable &table, uint32_t family)
{
	VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	VkCommandPool pool = VK_NULL_HANDLE;

	poolInfo.queueFamilyIndex = family;
	table.CreateCommandPool(device, &poolInfo, nullptr, &pool);
	return pool;
}

VkResult Swapchain::initSourceCommands(uint32_t familyIndex)
{
	if (familyIndex != sourceCmdPool.family)
	{
		// Wait until all source commands are done processing.
		std::unique_lock<std::mutex> holder{lock};
		cond.wait(holder, [this]() {
			return submitCount == processedSourceCount;
		});

		device->table.DestroyCommandPool(device->device, sourceCmdPool.pool, nullptr);
		sourceCmdPool.pool = VK_NULL_HANDLE;
	}

	if (sourceCmdPool.pool == VK_NULL_HANDLE)
	{
		sourceCmdPool.pool = createCommandPool(device->device, device->table, familyIndex);
		sourceCmdPool.family = familyIndex;
	}

	// We have messed with sync objects at this point, so we must return DEVICE_LOST on failure here.
	// Should never happen though ...

	// Just record the commands up front,
	// they are immutable for a given swapchain anyway.
	if (sourceCmdPool.pool != VK_NULL_HANDLE)
	{
		for (auto &image : images)
		{
			VkCommandBufferAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
			allocInfo.commandBufferCount = 1;
			allocInfo.commandPool = sourceCmdPool.pool;
			allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			if (device->table.AllocateCommandBuffers(device->device, &allocInfo, &image.sourceCmd) != VK_SUCCESS)
				return VK_ERROR_DEVICE_LOST;

			// Dispatchable object.
			device->setDeviceLoaderData(device->device, image.sourceCmd);

			VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
			device->table.BeginCommandBuffer(image.sourceCmd, &beginInfo);
			{
				VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
				barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
				barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT,
				                             0, VK_REMAINING_MIP_LEVELS,
				                             0, VK_REMAINING_ARRAY_LAYERS };
				barrier.image = image.sourceImage.image;
				barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

				device->table.CmdPipelineBarrier(image.sourceCmd,
												 VK_PIPELINE_STAGE_TRANSFER_BIT,
												 VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
												 0, nullptr,
												 0, nullptr,
												 1, &barrier);

				VkBufferImageCopy copy = {};
				copy.imageExtent = { width, height, 1 };
				copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
				device->table.CmdCopyImageToBuffer(image.sourceCmd,
				                                   image.sourceImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				                                   image.sourceBuffer.buffer,
				                                   1, &copy);

				barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
				barrier.dstAccessMask = 0;

				device->table.CmdPipelineBarrier(image.sourceCmd,
				                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
				                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
				                                 0, nullptr,
				                                 0, nullptr,
				                                 1, &barrier);
			}

			if (device->table.EndCommandBuffer(image.sourceCmd) != VK_SUCCESS)
				return VK_ERROR_DEVICE_LOST;
		}

		return VK_SUCCESS;
	}
	else
	{
		return VK_ERROR_DEVICE_LOST;
	}
}

VkResult Swapchain::submitSourceWork(VkQueue queue, uint32_t index, VkFence fence)
{
	VkResult result = initSourceCommands(device->queueToFamilyIndex(queue));
	if (result != VK_SUCCESS)
		return result;

	VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &images[index].sourceCmd;
	result = device->table.QueueSubmit(queue, 1, &submit, images[index].sourceFence);
	if (result != VK_SUCCESS)
		return result;

	// EXT_swapchain_maintenance1 fence.
	if (fence != VK_NULL_HANDLE)
		return device->table.QueueSubmit(queue, 0, nullptr, fence);
	else
		return VK_SUCCESS;
}

VkResult Swapchain::init(const VkSwapchainCreateInfoKHR *pCreateInfo)
{
	auto tmpCreateInfo = *pCreateInfo;
	auto *oldSwap = reinterpret_cast<Swapchain *>(pCreateInfo->oldSwapchain);
	if (oldSwap)
	{
		oldSwap->retire();
		tmpCreateInfo.oldSwapchain = oldSwap->sinkSwapchain;
	}

	tmpCreateInfo.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	tmpCreateInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	tmpCreateInfo.pQueueFamilyIndices = nullptr;
	tmpCreateInfo.queueFamilyIndexCount = 0;
	VkResult result = device->sinkTable.CreateSwapchainKHR(
			device->sinkDevice, &tmpCreateInfo, nullptr, &sinkSwapchain);

	if (result != VK_SUCCESS)
		return result;

	uint32_t count;
	device->sinkTable.GetSwapchainImagesKHR(device->sinkDevice, sinkSwapchain, &count, nullptr);
	images.resize(count);
	std::vector<VkImage> vkImages(count);
	device->sinkTable.GetSwapchainImagesKHR(device->sinkDevice, sinkSwapchain, &count, vkImages.data());

	sinkCmdPool.pool = createCommandPool(device->sinkDevice, device->sinkTable, device->getInstance()->sinkGpuQueueFamily);

	return VK_SUCCESS;
}

void Swapchain::retire()
{
	{
		std::lock_guard<std::mutex> holder{lock};
		swapchainStatus = VK_ERROR_OUT_OF_DATE_KHR;
		cond.notify_one();
	}

	if (worker.joinable())
		worker.join();

	// Release swapchain images so that oldSwapchain has a chance to work better.
	while (!acquireQueue.empty() && device->maint1Features.swapchainMaintenance1)
	{
		VkReleaseSwapchainImagesInfoEXT releaseInfo = { VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_EXT };
		releaseInfo.pImageIndices = &acquireQueue.front();
		releaseInfo.imageIndexCount = 1;
		releaseInfo.swapchain = sinkSwapchain;
		device->sinkTable.ReleaseSwapchainImagesEXT(device->sinkDevice, &releaseInfo);
		acquireQueue.pop();
	}
}

VkResult Swapchain::queuePresent(VkQueue queue, uint32_t index, VkFence fence)
{
	return VK_ERROR_SURFACE_LOST_KHR;
}

VkResult Swapchain::getSwapchainImages(uint32_t *pSwapchainImageCount, VkImage *pSwapchainImages)
{
	if (pSwapchainImages)
	{
		VkResult res = images.size() <= *pSwapchainImageCount ? VK_SUCCESS : VK_INCOMPLETE;
		if (images.size() < *pSwapchainImageCount)
			*pSwapchainImageCount = uint32_t(images.size());

		uint32_t outCount = *pSwapchainImageCount;
		for (uint32_t i = 0; i < outCount; i++)
			pSwapchainImages[i] = images[i].sourceImage.image;

		return res;
	}
	else
	{
		*pSwapchainImageCount = uint32_t(images.size());
		return VK_SUCCESS;
	}
}

VkResult Swapchain::markResult(VkResult err)
{
	std::lock_guard<std::mutex> holder{lock};

	if (err == VK_SUCCESS)
		return swapchainStatus;

	if (err < 0 || swapchainStatus == VK_SUCCESS)
		swapchainStatus = err;

	return swapchainStatus;
}

VkResult Swapchain::acquire(uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex)
{
	{
		std::unique_lock<std::mutex> holder{lock};
		if (timeout != UINT64_MAX)
		{
			auto t = std::chrono::steady_clock::now();
			t += std::chrono::nanoseconds(timeout);

			bool done = cond.wait_until(holder, t, [this]() {
				return acquireQueue.empty() || swapchainStatus != VK_SUCCESS;
			});

			if (!done)
				return timeout ? VK_TIMEOUT : VK_NOT_READY;
		}
		else
		{
			cond.wait(holder, [this]() {
				return acquireQueue.empty() || swapchainStatus != VK_SUCCESS;
			});
		}

		if (swapchainStatus != VK_SUCCESS)
			return swapchainStatus;

		*pImageIndex = acquireQueue.front();
		acquireQueue.pop();
	}

	// Need to synthesize a signal operation.

	if (semaphore != VK_NULL_HANDLE)
	{
		VkPhysicalDeviceExternalSemaphoreInfo semInfo = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO };
		VkExternalSemaphoreProperties props = { VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES };

		semInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
		device->instance->getTable()->GetPhysicalDeviceExternalSemaphorePropertiesKHR(
				device->gpu, &semInfo, &props);

		if (props.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT)
		{
			VkImportSemaphoreFdInfoKHR importInfo = { VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR };
			importInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
			importInfo.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
			importInfo.semaphore = semaphore;
			// FD -1 is treated as an already signalled payload, neat!
			importInfo.fd = -1;
			VkResult res = device->table.ImportSemaphoreFdKHR(device->device, &importInfo);
			if (res != VK_SUCCESS)
			{
				acquireQueue.push(*pImageIndex);
				return res;
			}
		}
		else
		{
			acquireQueue.push(*pImageIndex);
			return markResult(VK_ERROR_SURFACE_LOST_KHR);
		}
	}

	if (fence != VK_NULL_HANDLE)
	{
		VkPhysicalDeviceExternalFenceInfo fenceInfo = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO };
		VkExternalFenceProperties props = { VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES };

		fenceInfo.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
		device->instance->getTable()->GetPhysicalDeviceExternalFencePropertiesKHR(
				device->gpu, &fenceInfo, &props);

		if (props.externalFenceFeatures & VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT)
		{
			VkImportFenceFdInfoKHR importInfo = { VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR };
			importInfo.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
			importInfo.flags = VK_FENCE_IMPORT_TEMPORARY_BIT;
			importInfo.fence = fence;
			// FD -1 is treated as an already signalled payload, neat!
			importInfo.fd = -1;
			VkResult res = device->table.ImportFenceFdKHR(device->device, &importInfo);
			if (res != VK_SUCCESS)
			{
				acquireQueue.push(*pImageIndex);
				return res;
			}
		}
		else
		{
			acquireQueue.push(*pImageIndex);
			return markResult(VK_ERROR_SURFACE_LOST_KHR);
		}
	}

	return VK_SUCCESS;
}

VkResult Swapchain::releaseSwapchainImages(const VkReleaseSwapchainImagesInfoEXT *pReleaseInfo)
{
	for (uint32_t i = 0; i < pReleaseInfo->imageIndexCount; i++)
		acquireQueue.push(pReleaseInfo->pImageIndices[i]);

	return VK_SUCCESS;
}

Swapchain::~Swapchain()
{
	{
		std::lock_guard<std::mutex> holder{lock};
		swapchainStatus = VK_ERROR_SURFACE_LOST_KHR;
		cond.notify_one();
	}

	if (worker.joinable())
		worker.join();

	{
		std::lock_guard<std::mutex> holder{device->sinkQueueLock};
		device->sinkTable.QueueWaitIdle(device->sinkQueue);
	}
	device->sinkTable.DestroySwapchainKHR(device->sinkDevice, sinkSwapchain, nullptr);

	for (auto &image : images)
	{
		device->table.DestroyBuffer(device->device, image.sourceBuffer.buffer, nullptr);
		device->table.FreeMemory(device->device, image.sourceBuffer.memory, nullptr);
		device->table.DestroyImage(device->device, image.sourceImage.image, nullptr);
		device->table.FreeMemory(device->device, image.sourceImage.memory, nullptr);
		device->table.DestroyFence(device->device, image.sourceFence, nullptr);

		device->sinkTable.DestroyBuffer(device->sinkDevice, image.sinkBuffer.buffer, nullptr);
		device->sinkTable.FreeMemory(device->sinkDevice, image.sinkBuffer.memory, nullptr);
		device->sinkTable.DestroyImage(device->sinkDevice, image.sinkImage.image, nullptr);
		device->sinkTable.FreeMemory(device->sinkDevice, image.sinkImage.memory, nullptr);
		device->sinkTable.DestroySemaphore(device->sinkDevice, image.sinkSemaphore, nullptr);

		// Free this last. This is important to avoid spurious device lost
		// when submitting something with live VkDeviceMemory that references freed host memory.
		free(image.externalHostMemory);
	}

	device->table.DestroyCommandPool(device->device, sourceCmdPool.pool, nullptr);
	device->sinkTable.DestroyCommandPool(device->device, sinkCmdPool.pool, nullptr);
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                                                     const VkAllocationCallbacks *pAllocator, VkInstance *pInstance)
{
	auto *chainInfo = getChainInfo(pCreateInfo, VK_LAYER_LINK_INFO);
	auto *callbackInfo = getChainInfo(pCreateInfo, VK_LOADER_DATA_CALLBACK);
	auto *createDeviceCallback = getChainInfo(pCreateInfo, VK_LOADER_LAYER_CREATE_DEVICE_CALLBACK);
	auto fpSetInstanceLoaderData = callbackInfo->u.pfnSetInstanceLoaderData;
	auto fpCreateDevice = createDeviceCallback->u.layerDevice.pfnLayerCreateDevice;
	auto fpDestroyDevice = createDeviceCallback->u.layerDevice.pfnLayerDestroyDevice;

	auto fpGetInstanceProcAddr = chainInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	auto fpCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(fpGetInstanceProcAddr(nullptr, "vkCreateInstance"));
	if (!fpCreateInstance)
		return VK_ERROR_INITIALIZATION_FAILED;

	std::vector<const char *> enabledExtensions;

	// There seems to be no way to query which instance extensions are available here, so just yolo it.
	// The Mesa WSI layer seems to do just this.
	// Apparently the loader is responsible for filtering out anything that is unsupported.

	if (pCreateInfo->enabledExtensionCount)
	{
		enabledExtensions = {pCreateInfo->ppEnabledExtensionNames,
		                     pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount};
	}

	auto tmpCreateInfo = *pCreateInfo;
	addUniqueExtension(enabledExtensions, VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME);
	addUniqueExtension(enabledExtensions, VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME);
	addUniqueExtension(enabledExtensions, VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);
	addUniqueExtension(enabledExtensions, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
	tmpCreateInfo.enabledExtensionCount = enabledExtensions.size();
	tmpCreateInfo.ppEnabledExtensionNames = enabledExtensions.data();

	chainInfo->u.pLayerInfo = chainInfo->u.pLayerInfo->pNext;
	auto res = fpCreateInstance(&tmpCreateInfo, pAllocator, pInstance);
	if (res != VK_SUCCESS)
		return res;

	Instance *layer;
	{
		std::lock_guard<std::mutex> holder{globalLock};
		layer = createLayerData(getDispatchKey(*pInstance), instanceData);
	}
	layer->init(*pInstance, fpGetInstanceProcAddr, fpSetInstanceLoaderData, fpCreateDevice, fpDestroyDevice);

	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator)
{
	void *key = getDispatchKey(instance);
	auto *layer = getLayerData(key, instanceData);
	layer->getTable()->DestroyInstance(instance, pAllocator);

	std::lock_guard<std::mutex> holder{ globalLock };
	destroyLayerData(key, instanceData);
}

static VKAPI_ATTR VkResult VKAPI_CALL EnumeratePhysicalDevices(
		VkInstance instance, uint32_t *pPhysicalDeviceCount, VkPhysicalDevice *pPhysicalDevices)
{
	auto *layer = getInstanceLayer(instance);
	if (layer->sourceGpu)
	{
		if (pPhysicalDevices)
		{
			VkResult res = *pPhysicalDeviceCount != 0 ? VK_SUCCESS : VK_INCOMPLETE;
			if (*pPhysicalDeviceCount != 0)
				pPhysicalDevices[0] = layer->sourceGpu;
			return res;
		}
		else
		{
			*pPhysicalDeviceCount = 1;
			return VK_SUCCESS;
		}
	}
	else
	{
		return layer->getTable()->EnumeratePhysicalDevices(instance, pPhysicalDeviceCount, pPhysicalDevices);
	}
}

static VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(
		VkPhysicalDevice                            physicalDevice,
		const char*                                 pLayerName,
		uint32_t*                                   pPropertyCount,
		VkExtensionProperties*                      pProperties)
{
	if (pLayerName && strcmp(pLayerName, "VK_LAYER_pyrofling_cross_wsi") == 0)
	{
		*pPropertyCount = 0;
		return VK_SUCCESS;
	}

	auto *layer = getInstanceLayer(physicalDevice);

	// On the primary GPU, we just punch through anyway.
	if (!layer->sinkGpu || physicalDevice == layer->sinkGpu)
		return layer->getTable()->EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);

	// The surface and display queries are all instance extensions,
	// and thus the loader is responsible for dealing with it.
	uint32_t count = 0;
	layer->getTable()->EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, &count, nullptr);
	std::vector<VkExtensionProperties> props(count);
	layer->getTable()->EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, &count, props.data());

	layer->getTable()->EnumerateDeviceExtensionProperties(layer->sinkGpu, pLayerName, &count, nullptr);
	std::vector<VkExtensionProperties> redirectedProps(count);
	layer->getTable()->EnumerateDeviceExtensionProperties(layer->sinkGpu, pLayerName, &count, redirectedProps.data());

	// For redirected extensions, both source and sink must support it.
	// Rewriting PDF2 chains is generally quite problematic.
	auto itr = std::remove_if(props.begin(), props.end(), [&redirectedProps](const VkExtensionProperties &prop) -> bool {
		if (findExtension(redirectedExtensions, prop.extensionName))
			return !findExtension(redirectedProps, prop.extensionName);
		else if (findExtension(blockedExtensions, prop.extensionName))
			return true;
		return false;
	});
	props.erase(itr, props.end());

	if (pProperties)
	{
		VkResult res = *pPropertyCount >= props.size() ? VK_SUCCESS : VK_INCOMPLETE;
		*pPropertyCount = std::min<uint32_t>(*pPropertyCount, props.size());
		memcpy(pProperties, props.data(), *pPropertyCount * sizeof(*pProperties));
		return res;
	}
	else
	{
		*pPropertyCount = uint32_t(props.size());
		return VK_SUCCESS;
	}
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice gpu, const VkDeviceCreateInfo *pCreateInfo,
                                                   const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
	auto *layer = getInstanceLayer(gpu);
	auto *chainInfo = getChainInfo(pCreateInfo, VK_LAYER_LINK_INFO);
	auto *callbackInfo = getChainInfo(pCreateInfo, VK_LOADER_DATA_CALLBACK);

	auto fpSetDeviceLoaderData = callbackInfo->u.pfnSetDeviceLoaderData;
	auto fpGetInstanceProcAddr = chainInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	auto fpGetDeviceProcAddr = chainInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
	auto fpCreateDevice =
			reinterpret_cast<PFN_vkCreateDevice>(fpGetInstanceProcAddr(layer->getInstance(), "vkCreateDevice"));
	if (!fpCreateDevice)
		return VK_ERROR_INITIALIZATION_FAILED;

	auto fpEnumerateDeviceExtensionProperties =
			reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
					fpGetInstanceProcAddr(layer->getInstance(), "vkEnumerateDeviceExtensionProperties"));

	if (!fpEnumerateDeviceExtensionProperties)
		return VK_ERROR_INITIALIZATION_FAILED;

	// Querying supported device extensions works unlike in CreateInstance since we have a layer chain set up.
	uint32_t supportedCount;
	fpEnumerateDeviceExtensionProperties(gpu, nullptr, &supportedCount, nullptr);
	std::vector<VkExtensionProperties> supportedExts(supportedCount);
	fpEnumerateDeviceExtensionProperties(gpu, nullptr, &supportedCount, supportedExts.data());

	std::vector<const char *> enabledExtensions;
	if (pCreateInfo->enabledExtensionCount)
	{
		enabledExtensions = {pCreateInfo->ppEnabledExtensionNames,
		                     pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount};
	}

	auto tmpCreateInfo = *pCreateInfo;

	bool usesSwapchain = findExtension(pCreateInfo->ppEnabledExtensionNames,
	                                   pCreateInfo->enabledExtensionCount,
	                                   VK_KHR_SWAPCHAIN_EXTENSION_NAME);

	if (usesSwapchain && gpu != layer->sinkGpu && layer->sinkGpu)
	{
		// If these are not supported for whatever reason,
		// we will just not wrap entry points and pass through all device functions.
		addUniqueExtension(enabledExtensions, supportedExts, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME);
		addUniqueExtension(enabledExtensions, supportedExts, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
		addUniqueExtension(enabledExtensions, supportedExts, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
		addUniqueExtension(enabledExtensions, supportedExts, VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME);
		addUniqueExtension(enabledExtensions, supportedExts, VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME);
		addUniqueExtension(enabledExtensions, supportedExts, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
		addUniqueExtension(enabledExtensions, supportedExts, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
#ifndef _WIN32
		addUniqueExtension(enabledExtensions, supportedExts, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
		addUniqueExtension(enabledExtensions, supportedExts, VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME);
		addUniqueExtension(enabledExtensions, supportedExts, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
#endif
		tmpCreateInfo.enabledExtensionCount = enabledExtensions.size();
		tmpCreateInfo.ppEnabledExtensionNames = enabledExtensions.data();
	}

	// Advance the link info for the next element on the chain
	chainInfo->u.pLayerInfo = chainInfo->u.pLayerInfo->pNext;

	auto res = fpCreateDevice(gpu, &tmpCreateInfo, pAllocator, pDevice);
	if (res != VK_SUCCESS)
		return res;

	Device *device;
	{
		std::lock_guard<std::mutex> holder{globalLock};
		device = createLayerData(getDispatchKey(*pDevice), deviceData);
	}
	device->init(gpu, *pDevice, layer, fpGetDeviceProcAddr, fpSetDeviceLoaderData, &tmpCreateInfo);
	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator)
{
	void *key = getDispatchKey(device);
	auto *layer = getLayerData(key, deviceData);
	layer->getTable()->DestroyDevice(device, pAllocator);

	std::lock_guard<std::mutex> holder{ globalLock };
	destroyLayerData(key, deviceData);
}

static VKAPI_ATTR VkResult VKAPI_CALL
CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator,
                   VkSwapchainKHR *pSwapchain)
{
	auto *layer = getDeviceLayer(device);
	if (!layer->sinkDevice)
		return layer->getTable()->CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);

	auto result = layer->getTable()->CreateSwapchainKHR(layer->sinkDevice, pCreateInfo, pAllocator, pSwapchain);
	if (result != VK_SUCCESS)
		return result;

	auto *swap = new Swapchain(layer);

	auto res = swap->init(pCreateInfo);
	if (res != VK_SUCCESS)
		delete swap;
	else
		*pSwapchain = reinterpret_cast<VkSwapchainKHR>(swap);

	return res;
}

static VKAPI_ATTR void VKAPI_CALL
DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *pAllocator)
{
	auto *layer = getDeviceLayer(device);
	if (!layer->sinkDevice)
		return layer->getTable()->DestroySwapchainKHR(device, swapchain, pAllocator);

	auto *swap = reinterpret_cast<Swapchain *>(swapchain);
	delete swap;
}

static VKAPI_ATTR VkResult VKAPI_CALL
GetSwapchainImagesKHR(
		VkDevice                                    device,
		VkSwapchainKHR                              swapchain,
		uint32_t*                                   pSwapchainImageCount,
		VkImage*                                    pSwapchainImages)
{
	auto *layer = getDeviceLayer(device);
	if (!layer->sinkDevice)
		return layer->getTable()->GetSwapchainImagesKHR(device, swapchain, pSwapchainImageCount, pSwapchainImages);

	auto *swap = reinterpret_cast<Swapchain *>(swapchain);
	return swap->getSwapchainImages(pSwapchainImageCount, pSwapchainImages);
}

static VKAPI_ATTR VkResult VKAPI_CALL
AcquireNextImageKHR(
		VkDevice                                    device,
		VkSwapchainKHR                              swapchain,
		uint64_t                                    timeout,
		VkSemaphore                                 semaphore,
		VkFence                                     fence,
		uint32_t*                                   pImageIndex)
{
	auto *layer = getDeviceLayer(device);
	if (!layer->sinkDevice)
		return layer->getTable()->AcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);

	auto *swap = reinterpret_cast<Swapchain *>(swapchain);
	return swap->acquire(timeout, semaphore, fence, pImageIndex);
}

static VKAPI_ATTR VkResult VKAPI_CALL
AcquireNextImage2KHR(
		VkDevice                                    device,
		const VkAcquireNextImageInfoKHR*            pAcquireInfo,
		uint32_t*                                   pImageIndex)
{
	auto *layer = getDeviceLayer(device);
	if (!layer->sinkDevice)
		return layer->getTable()->AcquireNextImage2KHR(device, pAcquireInfo, pImageIndex);

	auto *swap = reinterpret_cast<Swapchain *>(pAcquireInfo->semaphore);
	return swap->acquire(pAcquireInfo->timeout, pAcquireInfo->semaphore, pAcquireInfo->fence, pImageIndex);
}

static VKAPI_ATTR VkResult VKAPI_CALL
ReleaseSwapchainImagesEXT(
		VkDevice                                    device,
		const VkReleaseSwapchainImagesInfoEXT*      pReleaseInfo)
{
	auto *layer = getDeviceLayer(device);
	if (!layer->sinkDevice)
		return layer->getTable()->ReleaseSwapchainImagesEXT(device, pReleaseInfo);

	auto *swap = reinterpret_cast<Swapchain *>(pReleaseInfo->swapchain);
	return swap->releaseSwapchainImages(pReleaseInfo);
}

static VKAPI_ATTR VkResult VKAPI_CALL
QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
	auto *layer = getDeviceLayer(queue);
	if (!layer->sinkDevice)
		return layer->getTable()->QueuePresentKHR(queue, pPresentInfo);

	VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	submitInfo.pWaitDstStageMask = &waitStage;
	submitInfo.waitSemaphoreCount = 1;

	for (uint32_t i = 0; i < pPresentInfo->waitSemaphoreCount; i++)
	{
		submitInfo.pWaitSemaphores = &pPresentInfo->pWaitSemaphores[i];
		VkResult res = layer->getTable()->QueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
		if (res != VK_SUCCESS)
			return res;
	}

	VkResult res = VK_SUCCESS;
	bool isSuboptimal = false;
	bool isSurfaceLost = false;
	bool isDeviceLost = false;

	auto *fence = findChain<VkSwapchainPresentFenceInfoEXT>(
			pPresentInfo->pNext, VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT);

	for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++)
	{
		auto *swap = reinterpret_cast<Swapchain *>(pPresentInfo->pSwapchains[i]);
		VkResult result = swap->queuePresent(queue, pPresentInfo->pImageIndices[i],
											 fence ? fence->pFences[i] : VK_NULL_HANDLE);

		if (result == VK_SUBOPTIMAL_KHR)
			isSuboptimal = true;
		else if (result == VK_ERROR_SURFACE_LOST_KHR)
			isSurfaceLost = true;
		else if (result == VK_ERROR_DEVICE_LOST)
			isDeviceLost = true;

		if (pPresentInfo->pResults)
			pPresentInfo->pResults[i] = result;

		// What exactly are we supposed to return here?
		if (result < 0)
			res = result;
	}

	if (isDeviceLost)
		res = VK_ERROR_DEVICE_LOST;
	else if (isSurfaceLost)
		res = VK_ERROR_SURFACE_LOST_KHR;
	else if (res == VK_SUCCESS && isSuboptimal)
		res = VK_SUBOPTIMAL_KHR;

	return res;
}

// Always redirect any physical device surface query.
#define WRAPPED_SURFACE_TRIVIAL(sym, ...) \
	auto *layer = getInstanceLayer(physicalDevice); \
	if (layer->sinkGpu) \
		physicalDevice = layer->sinkGpu; \
	return layer->getTable()->sym(__VA_ARGS__)

static VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceSupportKHR(
		VkPhysicalDevice                            physicalDevice,
		uint32_t                                    queueFamilyIndex,
		VkSurfaceKHR                                surface,
		VkBool32*                                   pSupported)
{
	auto *layer = getInstanceLayer(physicalDevice);
	if (layer->sinkGpu)
	{
		if (physicalDevice != layer->sinkGpu)
		{
			// Need to make sure we can copy the swapchain image at least.
			// I.e. no pure sparse queue or something silly like that.
			uint32_t count = 0;
			layer->getTable()->GetPhysicalDeviceQueueFamilyProperties(physicalDevice,
																	  &count,
																	  nullptr);
			std::vector<VkQueueFamilyProperties> props(count);
			layer->getTable()->GetPhysicalDeviceQueueFamilyProperties(physicalDevice,
			                                                          &count,
			                                                          props.data());

			constexpr VkQueueFlags flags =
					VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
			if (queueFamilyIndex >= count || (props[queueFamilyIndex].queueFlags & flags) == 0)
			{
				*pSupported = VK_FALSE;
				return VK_SUCCESS;
			}
		}

		// We only intend to present on this specific queue on sink device.
		physicalDevice = layer->sinkGpu;
		queueFamilyIndex = layer->sinkGpuQueueFamily;
	}

	return layer->getTable()->GetPhysicalDeviceSurfaceSupportKHR(physicalDevice, queueFamilyIndex, surface, pSupported);
}

static VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceCapabilitiesKHR(
		VkPhysicalDevice                            physicalDevice,
		VkSurfaceKHR                                surface,
		VkSurfaceCapabilitiesKHR*                   pSurfaceCapabilities)
{
	WRAPPED_SURFACE_TRIVIAL(GetPhysicalDeviceSurfaceCapabilitiesKHR, physicalDevice, surface, pSurfaceCapabilities);
}

static VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceFormatsKHR(
		VkPhysicalDevice                            physicalDevice,
		VkSurfaceKHR                                surface,
		uint32_t*                                   pSurfaceFormatCount,
		VkSurfaceFormatKHR*                         pSurfaceFormats)
{
	// Technically, we might have to filter this against supported formats on source GPU to determine renderable formats, etc, but w/e.
	WRAPPED_SURFACE_TRIVIAL(GetPhysicalDeviceSurfaceFormatsKHR, physicalDevice, surface, pSurfaceFormatCount, pSurfaceFormats);
}

static VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfacePresentModesKHR(
		VkPhysicalDevice                            physicalDevice,
		VkSurfaceKHR                                surface,
		uint32_t*                                   pPresentModeCount,
		VkPresentModeKHR*                           pPresentModes)
{
	WRAPPED_SURFACE_TRIVIAL(GetPhysicalDeviceSurfacePresentModesKHR, physicalDevice, surface, pPresentModeCount, pPresentModes);
}

static VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceDisplayPropertiesKHR(
		VkPhysicalDevice                            physicalDevice,
		uint32_t*                                   pPropertyCount,
		VkDisplayPropertiesKHR*                     pProperties)
{
	WRAPPED_SURFACE_TRIVIAL(GetPhysicalDeviceDisplayPropertiesKHR, physicalDevice, pPropertyCount, pProperties);
}

static VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceDisplayPlanePropertiesKHR(
		VkPhysicalDevice                            physicalDevice,
		uint32_t*                                   pPropertyCount,
		VkDisplayPlanePropertiesKHR*                pProperties)
{
	WRAPPED_SURFACE_TRIVIAL(GetPhysicalDeviceDisplayPlanePropertiesKHR, physicalDevice, pPropertyCount, pProperties);
}

static VKAPI_ATTR VkResult VKAPI_CALL GetDisplayPlaneSupportedDisplaysKHR(
		VkPhysicalDevice                            physicalDevice,
		uint32_t                                    planeIndex,
		uint32_t*                                   pDisplayCount,
		VkDisplayKHR*                               pDisplays)
{
	WRAPPED_SURFACE_TRIVIAL(GetDisplayPlaneSupportedDisplaysKHR, physicalDevice, planeIndex, pDisplayCount, pDisplays);
}

static VKAPI_ATTR VkResult VKAPI_CALL GetDisplayModePropertiesKHR(
		VkPhysicalDevice                            physicalDevice,
		VkDisplayKHR                                display,
		uint32_t*                                   pPropertyCount,
		VkDisplayModePropertiesKHR*                 pProperties)
{
	WRAPPED_SURFACE_TRIVIAL(GetDisplayModePropertiesKHR, physicalDevice, display, pPropertyCount, pProperties);
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateDisplayModeKHR(
		VkPhysicalDevice                            physicalDevice,
		VkDisplayKHR                                display,
		const VkDisplayModeCreateInfoKHR*           pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkDisplayModeKHR*                           pMode)
{
	WRAPPED_SURFACE_TRIVIAL(CreateDisplayModeKHR, physicalDevice, display, pCreateInfo, pAllocator, pMode);
}

static VKAPI_ATTR VkResult VKAPI_CALL GetDisplayPlaneCapabilitiesKHR(
		VkPhysicalDevice                            physicalDevice,
		VkDisplayModeKHR                            mode,
		uint32_t                                    planeIndex,
		VkDisplayPlaneCapabilitiesKHR*              pCapabilities)
{
	WRAPPED_SURFACE_TRIVIAL(GetDisplayPlaneCapabilitiesKHR, physicalDevice, mode, planeIndex, pCapabilities);
}

static VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceCapabilities2KHR(
		VkPhysicalDevice                            physicalDevice,
		const VkPhysicalDeviceSurfaceInfo2KHR*      pSurfaceInfo,
		VkSurfaceCapabilities2KHR*                  pSurfaceCapabilities)
{
	WRAPPED_SURFACE_TRIVIAL(GetPhysicalDeviceSurfaceCapabilities2KHR, physicalDevice, pSurfaceInfo, pSurfaceCapabilities);
}

static VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceFormats2KHR(
		VkPhysicalDevice                            physicalDevice,
		const VkPhysicalDeviceSurfaceInfo2KHR*      pSurfaceInfo,
		uint32_t*                                   pSurfaceFormatCount,
		VkSurfaceFormat2KHR*                        pSurfaceFormats)
{
	// Technically, we might have to filter this against supported formats on source GPU to determine renderable formats, etc, but w/e.
	WRAPPED_SURFACE_TRIVIAL(GetPhysicalDeviceSurfaceFormats2KHR, physicalDevice, pSurfaceInfo, pSurfaceFormatCount, pSurfaceFormats);
}

static VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceDisplayProperties2KHR(
		VkPhysicalDevice                            physicalDevice,
		uint32_t*                                   pPropertyCount,
		VkDisplayProperties2KHR*                    pProperties)
{
	WRAPPED_SURFACE_TRIVIAL(GetPhysicalDeviceDisplayProperties2KHR, physicalDevice, pPropertyCount, pProperties);
}

static VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceDisplayPlaneProperties2KHR(
		VkPhysicalDevice                            physicalDevice,
		uint32_t*                                   pPropertyCount,
		VkDisplayPlaneProperties2KHR*               pProperties)
{
	WRAPPED_SURFACE_TRIVIAL(GetPhysicalDeviceDisplayPlaneProperties2KHR, physicalDevice, pPropertyCount, pProperties);
}

static VKAPI_ATTR VkResult VKAPI_CALL GetDisplayModeProperties2KHR(
		VkPhysicalDevice                            physicalDevice,
		VkDisplayKHR                                display,
		uint32_t*                                   pPropertyCount,
		VkDisplayModeProperties2KHR*                pProperties)
{
	WRAPPED_SURFACE_TRIVIAL(GetDisplayModeProperties2KHR, physicalDevice, display, pPropertyCount, pProperties);
}

static VKAPI_ATTR VkResult VKAPI_CALL GetDisplayPlaneCapabilities2KHR(
		VkPhysicalDevice                            physicalDevice,
		const VkDisplayPlaneInfo2KHR*               pDisplayPlaneInfo,
		VkDisplayPlaneCapabilities2KHR*             pCapabilities)
{
	WRAPPED_SURFACE_TRIVIAL(GetDisplayPlaneCapabilities2KHR, physicalDevice, pDisplayPlaneInfo, pCapabilities);
}

static VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceCapabilities2EXT(
		VkPhysicalDevice                            physicalDevice,
		VkSurfaceKHR                                surface,
		VkSurfaceCapabilities2EXT*                  pSurfaceCapabilities)
{
	WRAPPED_SURFACE_TRIVIAL(GetPhysicalDeviceSurfaceCapabilities2EXT, physicalDevice, surface, pSurfaceCapabilities);
}

static VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDevicePresentRectanglesKHR(
		VkPhysicalDevice                            physicalDevice,
		VkSurfaceKHR                                surface,
		uint32_t*                                   pRectCount,
		VkRect2D*                                   pRects)
{
	WRAPPED_SURFACE_TRIVIAL(GetPhysicalDevicePresentRectanglesKHR, physicalDevice, surface, pRectCount, pRects);
}

static VKAPI_ATTR VkResult VKAPI_CALL ReleaseDisplayEXT(
		VkPhysicalDevice                            physicalDevice,
		VkDisplayKHR                                display)
{
	WRAPPED_SURFACE_TRIVIAL(ReleaseDisplayEXT, physicalDevice, display);
}

static VKAPI_ATTR VkResult VKAPI_CALL AcquireDrmDisplayEXT(
		VkPhysicalDevice                            physicalDevice,
		int32_t                                     drmFd,
		VkDisplayKHR                                display)
{
	WRAPPED_SURFACE_TRIVIAL(AcquireDrmDisplayEXT, physicalDevice, drmFd, display);
}

static VKAPI_ATTR VkResult VKAPI_CALL GetDrmDisplayEXT(
		VkPhysicalDevice                            physicalDevice,
		int32_t                                     drmFd,
		uint32_t                                    connectorId,
		VkDisplayKHR*                               display)
{
	WRAPPED_SURFACE_TRIVIAL(GetDrmDisplayEXT, physicalDevice, drmFd, connectorId, display);
}

static PFN_vkVoidFunction interceptCoreInstanceCommand(const char *pName)
{
	static const struct
	{
		const char *name;
		PFN_vkVoidFunction proc;
	} coreInstanceCommands[] = {
		{ "vkCreateInstance", reinterpret_cast<PFN_vkVoidFunction>(CreateInstance) },
		{ "vkDestroyInstance", reinterpret_cast<PFN_vkVoidFunction>(DestroyInstance) },
		{ "vkGetInstanceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(VK_LAYER_PYROFLING_CROSS_WSI_vkGetInstanceProcAddr) },
		{ "vkCreateDevice", reinterpret_cast<PFN_vkVoidFunction>(CreateDevice) },
		{ "vkEnumerateDeviceExtensionProperties", reinterpret_cast<PFN_vkVoidFunction>(EnumerateDeviceExtensionProperties) },
		{ "vkEnumeratePhysicalDevices", reinterpret_cast<PFN_vkVoidFunction>(EnumeratePhysicalDevices) },
	};

	for (auto &cmd : coreInstanceCommands)
		if (strcmp(cmd.name, pName) == 0)
			return cmd.proc;

	return nullptr;
}

static PFN_vkVoidFunction interceptExtensionInstanceCommand(const char *pName)
{
	static const struct
	{
		const char *name;
		PFN_vkVoidFunction proc;
	} extInstanceCommands[] = {
#define F(sym) { "vk" #sym, reinterpret_cast<PFN_vkVoidFunction>(sym) }
		F(GetPhysicalDeviceSurfaceFormatsKHR),
		F(GetPhysicalDeviceSurfaceSupportKHR),
		F(GetPhysicalDeviceSurfaceCapabilitiesKHR),
		F(GetPhysicalDeviceSurfacePresentModesKHR),
		F(CreateDisplayModeKHR),
		F(GetDisplayModePropertiesKHR),
		F(GetDisplayPlaneSupportedDisplaysKHR),
		F(GetDisplayPlaneCapabilitiesKHR),
		F(GetPhysicalDeviceDisplayPlanePropertiesKHR),
		F(GetPhysicalDeviceDisplayPropertiesKHR),
		F(GetPhysicalDeviceSurfaceFormats2KHR),
		F(GetPhysicalDeviceSurfaceCapabilities2KHR),
		F(GetPhysicalDeviceDisplayProperties2KHR),
		F(GetPhysicalDeviceDisplayPlaneProperties2KHR),
		F(GetDisplayModeProperties2KHR),
		F(GetDisplayPlaneCapabilities2KHR),
		F(GetPhysicalDeviceSurfaceCapabilities2EXT),
		F(GetPhysicalDevicePresentRectanglesKHR),
		F(ReleaseDisplayEXT),
		F(AcquireDrmDisplayEXT),
		F(GetDrmDisplayEXT),
#undef F
	};

	for (auto &cmd : extInstanceCommands)
		if (strcmp(cmd.name, pName) == 0)
			return cmd.proc;

	return nullptr;

}

static PFN_vkVoidFunction interceptDeviceCommand(const char *pName)
{
	static const struct
	{
		const char *name;
		PFN_vkVoidFunction proc;
	} coreDeviceCommands[] = {
		{ "vkGetDeviceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(VK_LAYER_PYROFLING_CROSS_WSI_vkGetDeviceProcAddr) },
		{ "vkQueuePresentKHR", reinterpret_cast<PFN_vkVoidFunction>(QueuePresentKHR) },
		{ "vkCreateSwapchainKHR", reinterpret_cast<PFN_vkVoidFunction>(CreateSwapchainKHR) },
		{ "vkDestroySwapchainKHR", reinterpret_cast<PFN_vkVoidFunction>(DestroySwapchainKHR) },
		{ "vkGetSwapchainImagesKHR", reinterpret_cast<PFN_vkVoidFunction>(GetSwapchainImagesKHR) },
		{ "vkAcquireNextImageKHR", reinterpret_cast<PFN_vkVoidFunction>(AcquireNextImageKHR) },
		{ "vkAcquireNextImage2KHR", reinterpret_cast<PFN_vkVoidFunction>(AcquireNextImage2KHR) },
		{ "vkReleaseSwapchainImagesEXT", reinterpret_cast<PFN_vkVoidFunction>(ReleaseSwapchainImagesEXT) },
		{ "vkDestroyDevice", reinterpret_cast<PFN_vkVoidFunction>(DestroyDevice) },
	};

	for (auto &cmd : coreDeviceCommands)
		if (strcmp(cmd.name, pName) == 0)
			return cmd.proc;

	return nullptr;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
VK_LAYER_PYROFLING_CROSS_WSI_vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
	Device *layer;
	{
		std::lock_guard<std::mutex> holder{globalLock};
		layer = getLayerData(getDispatchKey(device), deviceData);
	}

	auto proc = layer->getTable()->GetDeviceProcAddr(device, pName);

	// Dummy layer, just punch through the device proc addr.
	// Only need to make sure we handle vkDestroyDevice properly.
	if (!layer->sinkDevice)
	{
		if (strcmp(pName, "vkDestroyDevice") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(DestroyDevice);
		else
			return proc;
	}

	// If the underlying implementation returns nullptr, we also need to return nullptr.
	// This means we never expose wrappers which will end up dispatching into nullptr.
	if (proc)
	{
		auto wrapped_proc = interceptDeviceCommand(pName);
		if (wrapped_proc)
			proc = wrapped_proc;
	}

	return proc;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
VK_LAYER_PYROFLING_CROSS_WSI_vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
	auto proc = interceptCoreInstanceCommand(pName);
	if (proc)
		return proc;

	Instance *layer;
	{
		std::lock_guard<std::mutex> holder{globalLock};
		layer = getLayerData(getDispatchKey(instance), instanceData);
	}

	proc = layer->getProcAddr(pName);

	// If the underlying implementation returns nullptr, we also need to return nullptr.
	// This means we never expose wrappers which will end up dispatching into nullptr.
	if (proc)
	{
		auto wrapped_proc = interceptExtensionInstanceCommand(pName);

		if (wrapped_proc)
		{
			proc = wrapped_proc;
		}
		else
		{
			wrapped_proc = interceptDeviceCommand(pName);
			if (wrapped_proc)
				proc = wrapped_proc;
		}
	}

	return proc;
}

VKAPI_ATTR VkResult VKAPI_CALL
VK_LAYER_PYROFLING_CROSS_WSI_vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct)
{
	if (pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT || pVersionStruct->loaderLayerInterfaceVersion < 2)
		return VK_ERROR_INITIALIZATION_FAILED;

	if (pVersionStruct->loaderLayerInterfaceVersion > CURRENT_LOADER_LAYER_INTERFACE_VERSION)
		pVersionStruct->loaderLayerInterfaceVersion = CURRENT_LOADER_LAYER_INTERFACE_VERSION;

	if (pVersionStruct->loaderLayerInterfaceVersion >= 2)
	{
		pVersionStruct->pfnGetInstanceProcAddr = VK_LAYER_PYROFLING_CROSS_WSI_vkGetInstanceProcAddr;
		pVersionStruct->pfnGetDeviceProcAddr = VK_LAYER_PYROFLING_CROSS_WSI_vkGetDeviceProcAddr;
		pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
	}

	return VK_SUCCESS;
}
