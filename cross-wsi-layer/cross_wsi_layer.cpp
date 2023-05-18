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
#include <unistd.h>

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
	VK_KHR_SWAPCHAIN_EXTENSION_NAME,
	VK_KHR_PRESENT_ID_EXTENSION_NAME,
	VK_KHR_PRESENT_WAIT_EXTENSION_NAME,
	VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
};

// Block any extension that we don't explicitly wrap or understand yet.
// For sinkGpu situation we invent dummy handles for VkSwapchainKHR.
static const char *blockedExtensions[] = {
	VK_KHR_DISPLAY_SWAPCHAIN_EXTENSION_NAME,
	VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME,
	VK_KHR_SHARED_PRESENTABLE_IMAGE_EXTENSION_NAME,
	VK_AMD_DISPLAY_NATIVE_HDR_EXTENSION_NAME,
	VK_EXT_DISPLAY_CONTROL_EXTENSION_NAME,
	VK_EXT_HDR_METADATA_EXTENSION_NAME,
	VK_KHR_INCREMENTAL_PRESENT_EXTENSION_NAME,
	VK_NV_PRESENT_BARRIER_EXTENSION_NAME,
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
	VkResult waitForPresent(uint64_t presentId, uint64_t timeout);
	void retire();

	struct Buffer
	{
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkDeviceSize size = 0;
	};

	struct Image
	{
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkDeviceSize size = 0;
	};

	struct SwapchainImage
	{
		void *externalHostMemory = nullptr;
		Buffer sinkBuffer, sourceBuffer;
		Image sinkImage, sourceImage;
		VkFence sourceFence = VK_NULL_HANDLE;
		VkSemaphore sourceAcquireSemaphore = VK_NULL_HANDLE;
		VkSemaphore sinkReleaseSemaphore = VK_NULL_HANDLE;
		VkFence sinkAcquireFence = VK_NULL_HANDLE;
		VkCommandBuffer sourceCmd = VK_NULL_HANDLE;
		VkCommandBuffer sinkCmd = VK_NULL_HANDLE;
	};

	struct
	{
		VkCommandPool pool = VK_NULL_HANDLE;
		uint32_t family = VK_QUEUE_FAMILY_IGNORED;
	} sourceCmdPool, sinkCmdPool;

	std::vector<VkFence> sinkFencePool;

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
	VkResult initSinkCommands();
	VkResult submitSourceWork(VkQueue queue, uint32_t index, VkFence fence);
	VkResult markResult(VkResult err);

	uint32_t getNumForwardProgressImages(const VkSwapchainCreateInfoKHR *pCreateInfo) const;
	VkResult pumpAcquireSinkImage();
	VkResult setupSwapchainImage(const VkSwapchainCreateInfoKHR *pCreateInfo, VkImage image, uint32_t index);

	VkResult allocateImageMemory(Image &img) const;
	VkResult importHostBuffer(VkDevice device, const VkLayerDispatchTable &table, Buffer &buf,
	                          const VkPhysicalDeviceMemoryProperties &memProps, void *hostPointer);

	void runWorker();
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
	std::mutex queueLock;

	VkPhysicalDevicePresentWaitFeaturesKHR waitFeatures;
	VkPhysicalDevicePresentIdFeaturesKHR idFeatures;
	VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maint1Features;
	VkPhysicalDeviceMemoryProperties sourceMemoryProps;
	VkPhysicalDeviceMemoryProperties sinkMemoryProps;

	VkResult forceSignalSemaphore(VkSemaphore semaphore);
	VkResult createExportableSignalledSemaphore(VkSemaphore *pSemaphore);
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

VkResult Device::forceSignalSemaphore(VkSemaphore semaphore)
{
	if (!queueToFamily.empty())
	{
		std::lock_guard<std::mutex> holder{queueLock};
		VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submit.signalSemaphoreCount = 1;
		submit.pSignalSemaphores = &semaphore;

		auto res = table.QueueSubmit(queueToFamily.front().queue, 1, &submit, VK_NULL_HANDLE);
		if (res != VK_SUCCESS)
			return res;
	}

	return VK_SUCCESS;
}

VkResult Device::createExportableSignalledSemaphore(VkSemaphore *pSemaphore)
{
	VkSemaphoreCreateInfo semInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	VkExportSemaphoreCreateInfo exportInfo = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO };
	exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
	semInfo.pNext = &exportInfo;

	auto res = table.CreateSemaphore(device, &semInfo, nullptr, pSemaphore);
	if (res != VK_SUCCESS)
	{
		*pSemaphore = VK_NULL_HANDLE;
		return res;
	}

	// It is unfortunate that there is no VK_SEMAPHORE_CREATE_SIGNALLED_BIT :(
	// If there is no queue, there is also no queue that can wait on an unsignalled semaphore :3
	res = forceSignalSemaphore(*pSemaphore);
	if (res != VK_SUCCESS)
	{
		*pSemaphore = VK_NULL_HANDLE;
		table.DestroySemaphore(device, *pSemaphore, nullptr);
	}
	return res;
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

	instance->getTable()->GetPhysicalDeviceMemoryProperties(gpu, &sourceMemoryProps);

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
		instance->getTable()->GetPhysicalDeviceMemoryProperties(instance->sinkGpu, &sinkMemoryProps);

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
			VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
		};

		for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++)
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

VkResult Swapchain::initSinkCommands()
{
	auto &table = device->sinkTable;
	auto vkDevice = device->sinkDevice;

	sinkCmdPool.pool = createCommandPool(vkDevice, table, device->getInstance()->sinkGpuQueueFamily);
	sinkCmdPool.family = device->getInstance()->sinkGpuQueueFamily;

	if (!sinkCmdPool.pool)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	for (auto &image : images)
	{
		VkCommandBufferAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		allocInfo.commandBufferCount = 1;
		allocInfo.commandPool = sinkCmdPool.pool;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		if (table.AllocateCommandBuffers(vkDevice, &allocInfo, &image.sinkCmd) != VK_SUCCESS)
			return VK_ERROR_DEVICE_LOST;

		// Dispatchable object.
		auto cmd = image.sinkCmd;
		device->setDeviceLoaderData(vkDevice, image.sinkCmd);

		VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		table.BeginCommandBuffer(cmd, &beginInfo);
		{
			VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
			barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT,
			                             0, VK_REMAINING_MIP_LEVELS,
			                             0, VK_REMAINING_ARRAY_LAYERS };
			barrier.image = image.sinkImage.image;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

			table.CmdPipelineBarrier(cmd,
			                         VK_PIPELINE_STAGE_TRANSFER_BIT,
			                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
			                         0, nullptr,
			                         0, nullptr,
			                         1, &barrier);

			VkBufferImageCopy copy = {};
			copy.imageExtent = { width, height, 1 };
			copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			table.CmdCopyBufferToImage(cmd,
			                           image.sinkBuffer.buffer,
			                           image.sinkImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			                           1, &copy);

			barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			barrier.dstAccessMask = 0;

			table.CmdPipelineBarrier(cmd,
			                         VK_PIPELINE_STAGE_TRANSFER_BIT,
			                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
			                         0, nullptr,
			                         0, nullptr,
			                         1, &barrier);
		}

		if (table.EndCommandBuffer(cmd) != VK_SUCCESS)
			return VK_ERROR_DEVICE_LOST;
	}

	return VK_SUCCESS;
}

VkResult Swapchain::initSourceCommands(uint32_t familyIndex)
{
	auto &table = device->table;
	auto vkDevice = device->device;

	if (familyIndex != sourceCmdPool.family)
	{
		// Wait until all source commands are done processing.
		std::unique_lock<std::mutex> holder{lock};
		cond.wait(holder, [this]() {
			return submitCount == processedSourceCount;
		});

		table.DestroyCommandPool(vkDevice, sourceCmdPool.pool, nullptr);
		sourceCmdPool.pool = VK_NULL_HANDLE;
	}

	if (sourceCmdPool.pool == VK_NULL_HANDLE)
	{
		sourceCmdPool.pool = createCommandPool(vkDevice, table, familyIndex);
		sourceCmdPool.family = familyIndex;
	}

	if (sourceCmdPool.pool == VK_NULL_HANDLE)
	{
		// We have messed with sync objects at this point, so we must return DEVICE_LOST on failure here.
		// Should never happen though ...
		return VK_ERROR_DEVICE_LOST;
	}

	// Just record the commands up front,
	// they are immutable for a given swapchain anyway.
	for (auto &image : images)
	{
		VkCommandBufferAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		allocInfo.commandBufferCount = 1;
		allocInfo.commandPool = sourceCmdPool.pool;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		if (table.AllocateCommandBuffers(vkDevice, &allocInfo, &image.sourceCmd) != VK_SUCCESS)
			return VK_ERROR_DEVICE_LOST;

		auto cmd = image.sourceCmd;

		// Dispatchable object.
		device->setDeviceLoaderData(vkDevice, cmd);

		VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		table.BeginCommandBuffer(cmd, &beginInfo);
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

			table.CmdPipelineBarrier(cmd,
			                         VK_PIPELINE_STAGE_TRANSFER_BIT,
			                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
			                         0, nullptr,
			                         0, nullptr,
			                         1, &barrier);

			VkBufferImageCopy copy = {};
			copy.imageExtent = { width, height, 1 };
			copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			table.CmdCopyImageToBuffer(cmd,
			                           image.sourceImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			                           image.sourceBuffer.buffer,
			                           1, &copy);

			barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			barrier.dstAccessMask = 0;

			VkBufferMemoryBarrier bufBarrier = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
			bufBarrier.buffer = image.sourceBuffer.buffer;
			bufBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			bufBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			bufBarrier.size = VK_WHOLE_SIZE;
			bufBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;

			table.CmdPipelineBarrier(cmd,
			                         VK_PIPELINE_STAGE_TRANSFER_BIT,
			                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0,
			                         0, nullptr,
			                         1, &bufBarrier,
			                         1, &barrier);
		}

		if (table.EndCommandBuffer(cmd) != VK_SUCCESS)
			return VK_ERROR_DEVICE_LOST;
	}

	return VK_SUCCESS;
}

VkResult Swapchain::importHostBuffer(VkDevice vkDevice, const VkLayerDispatchTable &table, Swapchain::Buffer &buf,
                                     const VkPhysicalDeviceMemoryProperties &memProps,
                                     void *hostPointer)
{
	VkMemoryHostPointerPropertiesEXT hostProps = { VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
	VkMemoryRequirements reqs;
	VkResult res;

	table.GetBufferMemoryRequirements(vkDevice, buf.buffer, &reqs);

	res = table.GetMemoryHostPointerPropertiesEXT(
			vkDevice, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
			buf.buffer, &hostProps);

	if (res != VK_SUCCESS)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	reqs.memoryTypeBits &= hostProps.memoryTypeBits;

	uint32_t memoryTypeIndex = UINT32_MAX;
	for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
	{
		if ((memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 &&
		    ((1u << i) & reqs.memoryTypeBits) != 0)
		{
			memoryTypeIndex = i;
			break;
		}
	}

	if (memoryTypeIndex == UINT32_MAX)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	VkMemoryDedicatedAllocateInfo dedicatedInfo = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
	VkImportMemoryHostPointerInfoEXT pointerInfo = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT };
	allocInfo.pNext = &dedicatedInfo;
	allocInfo.allocationSize = reqs.size;
	allocInfo.memoryTypeIndex = memoryTypeIndex;
	dedicatedInfo.pNext = &pointerInfo;

	pointerInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
	pointerInfo.pHostPointer = hostPointer;
	dedicatedInfo.buffer = buf.buffer;

	res = table.AllocateMemory(vkDevice, &allocInfo, nullptr, &buf.memory);
	if (res != VK_SUCCESS)
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;

	res = table.BindBufferMemory(vkDevice, buf.buffer, buf.memory, 0);
	if (res != VK_SUCCESS)
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;

	return VK_SUCCESS;
}

VkResult Swapchain::allocateImageMemory(Image &img) const
{
	VkMemoryRequirements reqs;
	device->table.GetImageMemoryRequirements(device->device, img.image, &reqs);

	uint32_t memoryTypeIndex = UINT32_MAX;
	for (uint32_t i = 0; i < device->sourceMemoryProps.memoryTypeCount; i++)
	{
		if ((device->sourceMemoryProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 &&
		    (reqs.memoryTypeBits & (1u << i)) != 0)
		{
			memoryTypeIndex = i;
			break;
		}
	}

	if (memoryTypeIndex == UINT32_MAX)
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;

	VkMemoryDedicatedAllocateInfo dedicatedInfo = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
	VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	dedicatedInfo.image = img.image;
	allocInfo.allocationSize = reqs.size;
	allocInfo.memoryTypeIndex = memoryTypeIndex;
	allocInfo.pNext = &dedicatedInfo;

	VkResult res = device->table.AllocateMemory(device->device, &allocInfo, nullptr, &img.memory);
	if (res != VK_SUCCESS)
		return res;

	res = device->table.BindImageMemory(device->device, img.image, img.memory, 0);
	if (res != VK_SUCCESS)
		return res;

	img.size = reqs.size;
	return VK_SUCCESS;
}

VkResult Swapchain::setupSwapchainImage(const VkSwapchainCreateInfoKHR *pCreateInfo, VkImage image, uint32_t index)
{
	auto &img = images[index];
	VkImageFormatListCreateInfo formatList;
	VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	img.sinkImage.image = image;

	if (const auto *fmt = findChain<VkImageFormatListCreateInfo>(
			pCreateInfo->pNext, VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO))
	{
		formatList = *fmt;
		formatList.pNext = imageInfo.pNext;
		imageInfo.pNext = &formatList;
	}

	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.extent = { pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height, 1 };
	imageInfo.format = pCreateInfo->imageFormat;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = pCreateInfo->imageArrayLayers;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.sharingMode = pCreateInfo->imageSharingMode;
	imageInfo.pQueueFamilyIndices = pCreateInfo->pQueueFamilyIndices;
	imageInfo.queueFamilyIndexCount = pCreateInfo->queueFamilyIndexCount;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.usage = pCreateInfo->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	if (pCreateInfo->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR)
		imageInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;

	VkResult res = device->table.CreateImage(device->device, &imageInfo, nullptr, &img.sourceImage.image);
	if (res != VK_SUCCESS)
		return res;

	res = allocateImageMemory(img.sourceImage);
	if (res != VK_SUCCESS)
		return res;

	VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	VkExternalMemoryBufferCreateInfo externalInfo = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
	bufferInfo.size = img.sourceImage.size;
	bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bufferInfo.pNext = &externalInfo;
	externalInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;

	device->table.CreateBuffer(device->device, &bufferInfo, nullptr, &img.sourceBuffer.buffer);
	device->sinkTable.CreateBuffer(device->sinkDevice, &bufferInfo, nullptr, &img.sinkBuffer.buffer);

	VkMemoryRequirements sourceReqs, sinkReqs;
	device->table.GetBufferMemoryRequirements(device->device, img.sourceBuffer.buffer, &sourceReqs);
	device->sinkTable.GetBufferMemoryRequirements(device->sinkDevice, img.sinkBuffer.buffer, &sinkReqs);

	size_t bufferSize = std::max<size_t>(sourceReqs.size, sinkReqs.size);
	bufferSize = (bufferSize + 64 * 1024 - 1) & ~size_t(64 * 1024 - 1);

	// Somewhat crude to use image size here, but it's physically impossible for it
	// to be smaller than the linear, unpadded buffer size.
	// The more sensible approach is to hook GetSurfaceFormatsKHR instead.
	img.externalHostMemory = aligned_alloc(64 * 1024, bufferSize);
	if (!img.externalHostMemory)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	res = importHostBuffer(device->device, device->table, img.sourceBuffer,
	                       device->sourceMemoryProps, img.externalHostMemory);
	if (res != VK_SUCCESS)
		return res;

	res = importHostBuffer(device->sinkDevice, device->sinkTable, img.sinkBuffer,
	                       device->sinkMemoryProps, img.externalHostMemory);
	if (res != VK_SUCCESS)
		return res;

	VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	res = device->table.CreateFence(device->device, &fenceInfo, nullptr, &img.sourceFence);
	if (res != VK_SUCCESS)
		return res;

	VkSemaphoreCreateInfo semInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	res = device->sinkTable.CreateSemaphore(device->sinkDevice, &semInfo, nullptr, &img.sinkReleaseSemaphore);
	if (res != VK_SUCCESS)
		return res;

	res = device->createExportableSignalledSemaphore(&img.sourceAcquireSemaphore);

	return VK_SUCCESS;
}

VkResult Swapchain::pumpAcquireSinkImage()
{
	if (sinkFencePool.empty())
	{
		VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		VkFence fence;
		auto res = device->sinkTable.CreateFence(device->sinkDevice, &fenceInfo, nullptr, &fence);
		if (res != VK_SUCCESS)
			return markResult(res);

		sinkFencePool.push_back(fence);
	}

	VkFence fence = sinkFencePool.back();
	sinkFencePool.pop_back();

	uint32_t index;
	VkResult res = device->sinkTable.AcquireNextImageKHR(device->sinkDevice,
														 sinkSwapchain,
														 UINT64_MAX, VK_NULL_HANDLE, fence, &index);

	if (res >= 0)
	{
		if (images[index].sinkAcquireFence)
		{
			if (device->sinkTable.WaitForFences(
					device->sinkDevice, 1, &images[index].sinkAcquireFence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
				return markResult(VK_ERROR_DEVICE_LOST);

			if (device->sinkTable.ResetFences(device->sinkDevice, 1, &images[index].sinkAcquireFence) != VK_SUCCESS)
				return markResult(VK_ERROR_DEVICE_LOST);

			sinkFencePool.push_back(images[index].sinkAcquireFence);
		}

		images[index].sinkAcquireFence = fence;

		std::lock_guard<std::mutex> holder{lock};
		acquireQueue.push(index);
	}

	return markResult(res);
}

void Swapchain::runWorker()
{
	Work work = {};

	for (;;)
	{
		{
			std::unique_lock<std::mutex> holder{lock};
			cond.wait(holder, [this]() {
				return !workQueue.empty() || swapchainStatus < 0;
			});

			if (swapchainStatus < 0)
				break;

			work = workQueue.front();
			workQueue.pop();
		}

		VkPresentIdKHR presentId = { VK_STRUCTURE_TYPE_PRESENT_ID_KHR };
		VkSwapchainPresentModeInfoEXT modeInfo = { VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT };
		VkPresentInfoKHR presentInfo = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };

		presentInfo.pSwapchains = &sinkSwapchain;
		presentInfo.swapchainCount = 1;
		presentInfo.pWaitSemaphores = &images[work.index].sinkReleaseSemaphore;
		presentInfo.waitSemaphoreCount = 1;
		presentInfo.pImageIndices = &work.index;

		if (work.presentId)
		{
			presentId.swapchainCount = 1;
			presentId.pPresentIds = &work.presentId;
			presentId.pNext = presentInfo.pNext;
			presentInfo.pNext = &presentId;
		}

		if (work.setsMode)
		{
			modeInfo.swapchainCount = 1;
			modeInfo.pPresentModes = &work.mode;
			modeInfo.pNext = presentInfo.pNext;
			presentInfo.pNext = &modeInfo;
		}

		if (device->table.WaitForFences(device->device, 1, &images[work.index].sourceFence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
		{
			markResult(VK_ERROR_DEVICE_LOST);
			break;
		}

		if (device->table.ResetFences(device->device, 1, &images[work.index].sourceFence) != VK_SUCCESS)
		{
			markResult(VK_ERROR_DEVICE_LOST);
			break;
		}

		{
			std::lock_guard<std::mutex> holder{lock};
			processedSourceCount++;
			cond.notify_one();
		}

		if (device->sinkTable.WaitForFences(device->sinkDevice, 1, &images[work.index].sinkAcquireFence,
		                                    VK_TRUE, UINT64_MAX) != VK_SUCCESS)
		{
			markResult(VK_ERROR_DEVICE_LOST);
			break;
		}

		if (device->sinkTable.ResetFences(device->sinkDevice, 1, &images[work.index].sinkAcquireFence) != VK_SUCCESS)
		{
			markResult(VK_ERROR_DEVICE_LOST);
			break;
		}

		VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &images[work.index].sinkCmd;
		submit.signalSemaphoreCount = 1;
		submit.pSignalSemaphores = &images[work.index].sinkReleaseSemaphore;

		VkResult result;
		{
			std::lock_guard<std::mutex> holder{device->sinkQueueLock};
			result = device->sinkTable.QueueSubmit(device->sinkQueue, 1, &submit, images[work.index].sinkAcquireFence);
		}

		if (markResult(result))
			break;

		result = device->sinkTable.QueuePresentKHR(device->sinkQueue, &presentInfo);
		if (markResult(result) < 0)
			break;

		if (pumpAcquireSinkImage() < 0)
			break;
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

	// Re-trigger the external semaphore, so we can re-export a fresh payload.
	submit.pSignalSemaphores = &images[index].sourceAcquireSemaphore;
	submit.signalSemaphoreCount = 1;

	{
		std::lock_guard<std::mutex> holder{device->queueLock};
		result = device->table.QueueSubmit(queue, 1, &submit, images[index].sourceFence);
	}

	if (result != VK_SUCCESS)
		return markResult(result);

	// EXT_swapchain_maintenance1 fence.
	if (fence != VK_NULL_HANDLE)
	{
		std::lock_guard<std::mutex> holder{device->queueLock};
		return markResult(device->table.QueueSubmit(queue, 0, nullptr, fence));
	}
	else
		return markResult(VK_SUCCESS);
}

uint32_t Swapchain::getNumForwardProgressImages(const VkSwapchainCreateInfoKHR *pCreateInfo) const
{
	// Determine how many images we have to acquire to keep forward progress going.
	// For every present request, the worker thread will acquire another image.

	auto *instanceTable = device->getInstance()->getTable();
	auto *modes = findChain<VkSwapchainPresentModesCreateInfoEXT>(
			pCreateInfo->pNext, VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT);
	auto count = uint32_t(images.size());
	uint32_t minImageCount = 0;

	if (modes)
	{
		VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR };
		VkSurfaceCapabilities2KHR caps = { VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR };
		VkSurfacePresentModeEXT mode = { VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_EXT };

		// Need to consider minImageCount for each present mode individually.
		surfaceInfo.surface = pCreateInfo->surface;
		surfaceInfo.pNext = &mode;

		for (uint32_t i = 0; i < modes->presentModeCount; i++)
		{
			mode.presentMode = modes->pPresentModes[i];
			instanceTable->GetPhysicalDeviceSurfaceCapabilities2KHR(device->getInstance()->sinkGpu, &surfaceInfo, &caps);
			minImageCount = std::max<uint32_t>(minImageCount, caps.surfaceCapabilities.minImageCount);
		}
	}
	else
	{
		VkSurfaceCapabilitiesKHR caps;
		instanceTable->GetPhysicalDeviceSurfaceCapabilitiesKHR(device->getInstance()->sinkGpu, pCreateInfo->surface, &caps);
		minImageCount = caps.minImageCount;
	}

	// Also considers broken applications that request less than minImageCount.
	if (count < minImageCount)
		return 1;
	else
		return count - minImageCount + 1;
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

	tmpCreateInfo.pNext = nullptr;
	tmpCreateInfo.flags = 0;
	tmpCreateInfo.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	tmpCreateInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	tmpCreateInfo.pQueueFamilyIndices = nullptr;
	tmpCreateInfo.queueFamilyIndexCount = 0;

	// Only consider pNext structs we care about.
	VkSwapchainPresentScalingCreateInfoEXT scalingInfo;
	VkSwapchainPresentModesCreateInfoEXT modesInfo;

	if (const auto *modes = findChain<VkSwapchainPresentModesCreateInfoEXT>(
			pCreateInfo->pNext, VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT))
	{
		modesInfo = *modes;
		modesInfo.pNext = tmpCreateInfo.pNext;
		tmpCreateInfo.pNext = &modesInfo;
	}

	if (const auto *scaling = findChain<VkSwapchainPresentScalingCreateInfoEXT>(
			pCreateInfo->pNext, VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_SCALING_CREATE_INFO_EXT))
	{
		scalingInfo = *scaling;
		scalingInfo.pNext = tmpCreateInfo.pNext;
		tmpCreateInfo.pNext = &scalingInfo;
	}

	VkResult result = device->sinkTable.CreateSwapchainKHR(
			device->sinkDevice, &tmpCreateInfo, nullptr, &sinkSwapchain);

	width = pCreateInfo->imageExtent.width;
	height = pCreateInfo->imageExtent.height;

	if (result != VK_SUCCESS)
		return result;

	uint32_t count;
	device->sinkTable.GetSwapchainImagesKHR(device->sinkDevice, sinkSwapchain, &count, nullptr);
	std::vector<VkImage> vkImages(count);
	device->sinkTable.GetSwapchainImagesKHR(device->sinkDevice, sinkSwapchain, &count, vkImages.data());

	images.resize(count);
	for (uint32_t i = 0; i < count; i++)
	{
		VkResult res = setupSwapchainImage(pCreateInfo, vkImages[i], i);
		if (res != VK_SUCCESS)
			return res;
	}

	result = initSinkCommands();
	if (result < 0)
		return result;

	uint32_t numForwardProgressImages = getNumForwardProgressImages(pCreateInfo);
	for (uint32_t i = 0; i < numForwardProgressImages; i++)
		markResult(pumpAcquireSinkImage());

	worker = std::thread(&Swapchain::runWorker, this);
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

void Swapchain::setPresentId(uint64_t id)
{
	nextWork.presentId = id;
}

void Swapchain::setPresentMode(VkPresentModeKHR mode)
{
	nextWork.setsMode = true;
	nextWork.mode = mode;
}

VkResult Swapchain::queuePresent(VkQueue queue, uint32_t index, VkFence fence)
{
	VkResult result = submitSourceWork(queue, index, fence);
	if (result < 0)
		return markResult(result);

	nextWork.index = index;

	{
		std::lock_guard<std::mutex> holder{lock};
		workQueue.push(nextWork);
		submitCount++;
		cond.notify_one();
	}

	nextWork.presentId = 0;
	nextWork.setsMode = false;
	return markResult(VK_SUCCESS);
}

VkResult Swapchain::waitForPresent(uint64_t presentId, uint64_t timeout)
{
	return device->sinkTable.WaitForPresentKHR(device->sinkDevice, sinkSwapchain,
	                                           presentId, timeout);
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

	// Wake up any sleepers on unusual results.
	cond.notify_one();
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
				return !acquireQueue.empty() || swapchainStatus < 0;
			});

			if (!done)
				return timeout ? VK_TIMEOUT : VK_NOT_READY;
		}
		else
		{
			cond.wait(holder, [this]() {
				return !acquireQueue.empty() || swapchainStatus < 0;
			});
		}

		if (swapchainStatus < 0)
			return swapchainStatus;

		*pImageIndex = acquireQueue.front();
		acquireQueue.pop();
	}

	// Need to synthesize a signal operation.

	if (semaphore != VK_NULL_HANDLE)
	{
		VkSemaphoreGetFdInfoKHR getInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR };
		getInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
		getInfo.semaphore = images[*pImageIndex].sourceAcquireSemaphore;
		int fd = -1;

		auto res = device->table.GetSemaphoreFdKHR(device->device, &getInfo, &fd);
		if (res != VK_SUCCESS)
			return res;

		VkImportSemaphoreFdInfoKHR importInfo = { VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR };
		importInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
		importInfo.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
		importInfo.semaphore = semaphore;
		importInfo.fd = fd;
		res = device->table.ImportSemaphoreFdKHR(device->device, &importInfo);
		if (res != VK_SUCCESS)
		{
			::close(fd);
			acquireQueue.push(*pImageIndex);
			return markResult(res);
		}
	}

	if (fence != VK_NULL_HANDLE)
	{
		// Work around lack of support for SYNC_FD. Export an already signalled fence and import it.
		// WSI has temporary import semantics, so it's not spec compliant to use vkQueueSubmit().
		VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		VkExportFenceCreateInfo exportInfo = { VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO };
		VkFence dummyFence = VK_NULL_HANDLE;

		exportInfo.handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT;
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		fenceInfo.pNext = &exportInfo;
		auto res = device->table.CreateFence(device->device, &fenceInfo, nullptr, &dummyFence);
		if (res != VK_SUCCESS)
			return res;

		VkFenceGetFdInfoKHR getInfo = { VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR };
		int fd = -1;

		getInfo.fence = dummyFence;
		getInfo.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT;
		if (res != device->table.GetFenceFdKHR(device->device, &getInfo, &fd))
			return res;

		VkImportFenceFdInfoKHR importInfo = { VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR };
		importInfo.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT;
		importInfo.flags = VK_FENCE_IMPORT_TEMPORARY_BIT;
		importInfo.fence = fence;
		importInfo.fd = fd;
		res = device->table.ImportFenceFdKHR(device->device, &importInfo);
		device->table.DestroyFence(device->device, dummyFence, nullptr);

		if (res != VK_SUCCESS)
		{
			::close(fd);
			acquireQueue.push(*pImageIndex);
			return markResult(res);
		}
	}

	return markResult(VK_SUCCESS);
}

VkResult Swapchain::releaseSwapchainImages(const VkReleaseSwapchainImagesInfoEXT *pReleaseInfo)
{
	for (uint32_t i = 0; i < pReleaseInfo->imageIndexCount; i++)
	{
		uint32_t index = pReleaseInfo->pImageIndices[i];

		// Need to be able to signal again, so create another dummy semaphore.
		device->table.DestroySemaphore(device->device, images[index].sourceAcquireSemaphore, nullptr);
		auto res = device->createExportableSignalledSemaphore(&images[index].sourceAcquireSemaphore);
		if (res != VK_SUCCESS)
			return res;

		acquireQueue.push(index);
	}

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
		device->table.DestroySemaphore(device->device, image.sourceAcquireSemaphore, nullptr);

		device->sinkTable.DestroyBuffer(device->sinkDevice, image.sinkBuffer.buffer, nullptr);
		device->sinkTable.FreeMemory(device->sinkDevice, image.sinkBuffer.memory, nullptr);
		// sinkImage is owned by swapchain.
		device->sinkTable.DestroySemaphore(device->sinkDevice, image.sinkReleaseSemaphore, nullptr);
		device->sinkTable.DestroyFence(device->sinkDevice, image.sinkAcquireFence, nullptr);

		// Free this last. This is important to avoid spurious device lost
		// when submitting something with live VkDeviceMemory that references freed host memory.
		free(image.externalHostMemory);
	}

	for (auto &fence : sinkFencePool)
		device->sinkTable.DestroyFence(device->sinkDevice, fence, nullptr);

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
WaitForPresentKHR(
		VkDevice                                    device,
		VkSwapchainKHR                              swapchain,
		uint64_t                                    presentId,
		uint64_t                                    timeout)
{
	auto *layer = getDeviceLayer(device);
	if (!layer->sinkDevice)
		return layer->getTable()->WaitForPresentKHR(device, swapchain, presentId, timeout);

	auto *swap = reinterpret_cast<Swapchain *>(swapchain);
	return swap->waitForPresent(presentId, timeout);
}

static VKAPI_ATTR VkResult VKAPI_CALL
QueueSubmit(
		VkQueue                                     queue,
		uint32_t                                    submitCount,
		const VkSubmitInfo*                         pSubmits,
		VkFence                                     fence)
{
	auto *layer = getDeviceLayer(queue);
	if (!layer->sinkDevice)
		return layer->getTable()->QueueSubmit(queue, submitCount, pSubmits, fence);

	std::lock_guard<std::mutex> holder{layer->queueLock};
	return layer->getTable()->QueueSubmit(queue, submitCount, pSubmits, fence);
}

static VKAPI_ATTR VkResult VKAPI_CALL
QueueSubmit2KHR(
		VkQueue                                     queue,
		uint32_t                                    submitCount,
		const VkSubmitInfo2*                        pSubmits,
		VkFence                                     fence)
{
	auto *layer = getDeviceLayer(queue);
	if (!layer->sinkDevice)
		return layer->getTable()->QueueSubmit2KHR(queue, submitCount, pSubmits, fence);

	std::lock_guard<std::mutex> holder{layer->queueLock};
	return layer->getTable()->QueueSubmit2KHR(queue, submitCount, pSubmits, fence);
}

static VKAPI_ATTR VkResult VKAPI_CALL
QueueSubmit2(
		VkQueue                                     queue,
		uint32_t                                    submitCount,
		const VkSubmitInfo2*                        pSubmits,
		VkFence                                     fence)
{
	auto *layer = getDeviceLayer(queue);
	if (!layer->sinkDevice)
		return layer->getTable()->QueueSubmit2(queue, submitCount, pSubmits, fence);

	std::lock_guard<std::mutex> holder{layer->queueLock};
	return layer->getTable()->QueueSubmit2(queue, submitCount, pSubmits, fence);
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
		std::lock_guard<std::mutex> holder{layer->queueLock};
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
	auto *ids = findChain<VkPresentIdKHR>(pPresentInfo->pNext, VK_STRUCTURE_TYPE_PRESENT_ID_KHR);
	auto *mode = findChain<VkSwapchainPresentModeInfoEXT>(
			pPresentInfo->pNext, VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT);

	for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++)
	{
		auto *swap = reinterpret_cast<Swapchain *>(pPresentInfo->pSwapchains[i]);

		if (ids)
			swap->setPresentId(ids->pPresentIds[i]);
		if (mode)
			swap->setPresentMode(mode->pPresentModes[i]);

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
		{ "vkWaitForPresentKHR", reinterpret_cast<PFN_vkVoidFunction>(WaitForPresentKHR) },
		{ "vkQueueSubmit", reinterpret_cast<PFN_vkVoidFunction>(QueueSubmit) },
		{ "vkQueueSubmit2", reinterpret_cast<PFN_vkVoidFunction>(QueueSubmit2) },
		{ "vkQueueSubmit2KHR", reinterpret_cast<PFN_vkVoidFunction>(QueueSubmit2KHR) },
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
