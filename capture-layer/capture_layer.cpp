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
#include "client.hpp"
#include <mutex>
#include <memory>

#ifndef _WIN32
#include <unistd.h>
#endif

extern "C"
{
VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
VK_LAYER_PYROFLING_CAPTURE_vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
VK_LAYER_PYROFLING_CAPTURE_vkGetDeviceProcAddr(VkDevice device, const char *pName);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
VK_LAYER_PYROFLING_CAPTURE_vkGetInstanceProcAddr(VkInstance instance, const char *pName);
}

struct ExportableImage
{
	VkImage image;
	VkDeviceMemory memory;

	VkSemaphore acquireSemaphore;
	VkSemaphore releaseSemaphore;
	VkCommandPool cmdPool;
	VkCommandBuffer cmdBuffer;
	VkFence fence;
	uint32_t currentQueueFamily;

	bool liveAcquirePayload;
	bool acquired;
	bool ready;
	bool fencePending;
};

struct Instance;
struct Device;

// One surface can be associated with one swapchain at a time.
struct SurfaceState
{
	explicit SurfaceState(Instance *instance);
	~SurfaceState();
	VkResult processPresent(VkQueue queue, uint32_t index);
	void setActiveDeviceAndSwapchain(Device *device, const VkSwapchainCreateInfoKHR *pCreateInfo, VkSwapchainKHR chain);
	void freeImage(ExportableImage &img);
	bool initImageGroup(uint32_t count);
	bool sendImageGroup();

	std::unique_ptr<PyroFling::Client> client;
	std::vector<ExportableImage> image;
	PyroFling::ImageGroupMessage::WireFormat imageGroupWire = {};

	// One surface can be active on a surface at any one time.
	// We don't actively validate that, but we need to make sure
	// that we're copying from a swapchain that matches what we expect.
	// When activeSwapchain or device changes in vkCreateSwapchainKHR, we might have to reinit the image group.
	Device *device = nullptr;
	Instance *instance = nullptr;
	VkSwapchainKHR activeSwapchain = VK_NULL_HANDLE;
	std::vector<VkImage> swapImages;
	VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
	VkPhysicalDevice activePhysicalDevice = VK_NULL_HANDLE;

	uint32_t width = 0;
	uint32_t height = 0;
	VkSurfaceFormatKHR format = {};
	uint64_t image_group_serial = 0;
	uint64_t present_id = 2;
	uint64_t complete_present_id = 0;
	unsigned retry_counter = 0;

	void initClient(VkPhysicalDevice gpu);
	bool handleEvent(PyroFling::Message &msg);
	bool pollConnection();
	bool waitConnection();
	bool acquire(uint32_t &index);
};

enum class SyncMode
{
	Default,
	Server,
	Client
};

struct Instance
{
	void init(VkInstance instance_, const VkApplicationInfo *pApplicationInfo, PFN_vkGetInstanceProcAddr gpa_);

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

	SyncMode getSyncMode() const
	{
		return syncMode;
	}

	unsigned forcesNumImages() const
	{
		return forceImages;
	}

	void unregisterSurface(VkSurfaceKHR surface);
	SurfaceState *registerSurface(VkSurfaceKHR surface);
	void unregisterDevice(Device *device);
	void unregisterSwapchain(Device *device, VkSwapchainKHR swapchain);
	SurfaceState *findActiveSurfaceLocked(Device *device, VkSwapchainKHR swapchain);

	VkInstance instance = VK_NULL_HANDLE;
	VkLayerInstanceDispatchTable table = {};
	PFN_vkGetInstanceProcAddr gpa = nullptr;
	std::string applicationName;
	std::string engineName;
	SyncMode syncMode = SyncMode::Default;
	unsigned forceImages = 0;

	std::mutex surfaceLock;
	std::unordered_map<VkSurfaceKHR, std::unique_ptr<SurfaceState>> surfaces;
};

void Instance::unregisterSurface(VkSurfaceKHR surface)
{
	std::lock_guard<std::mutex> holder{surfaceLock};
	auto itr = surfaces.find(surface);
	if (itr != surfaces.end())
		surfaces.erase(itr);
}

SurfaceState *Instance::registerSurface(VkSurfaceKHR surface)
{
	std::lock_guard<std::mutex> holder{surfaceLock};
	auto &surf = surfaces[surface];
	surf.reset(new SurfaceState{this});
	return surf.get();
}

void Instance::unregisterDevice(Device *device)
{
	std::lock_guard<std::mutex> holder{surfaceLock};
	for (auto &surf : surfaces)
		if (surf.second->device == device)
			surf.second->setActiveDeviceAndSwapchain(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE);
}

void Instance::unregisterSwapchain(Device *device, VkSwapchainKHR swapchain)
{
	// Keep the device reference around so we can
	// reuse resources in case the swapchain is just being resized or similar.
	std::lock_guard<std::mutex> holder{surfaceLock};
	for (auto &surf : surfaces)
		if (surf.second->activeSwapchain == swapchain && surf.second->device == device)
			surf.second->setActiveDeviceAndSwapchain(device, VK_NULL_HANDLE, VK_NULL_HANDLE);
}

SurfaceState *Instance::findActiveSurfaceLocked(Device *device, VkSwapchainKHR swapchain)
{
	for (auto &surf : surfaces)
		if (surf.second->activeSwapchain == swapchain && surf.second->device == device)
			return surf.second.get();
	return nullptr;
}

void Instance::init(VkInstance instance_, const VkApplicationInfo *pApplicationInfo, PFN_vkGetInstanceProcAddr gpa_)
{
	if (pApplicationInfo)
	{
		if (pApplicationInfo->pApplicationName)
			applicationName = pApplicationInfo->pApplicationName;
		if (pApplicationInfo->pEngineName)
			engineName = pApplicationInfo->pEngineName;
	}

	instance = instance_;
	gpa = gpa_;
	layerInitInstanceDispatchTable(instance, &table, gpa);

	const char *env = getenv("PYROFLING_SYNC");
	if (env)
	{
		if (strcmp(env, "server") == 0)
			syncMode = SyncMode::Server;
		else if (strcmp(env, "client") == 0)
			syncMode = SyncMode::Client;
	}

	env = getenv("PYROFLING_IMAGES");
	if (env)
		forceImages = strtoul(env, nullptr, 0);
}

struct Device
{
	void init(VkPhysicalDevice gpu_, VkDevice device_, Instance *instance_, PFN_vkGetDeviceProcAddr gpa,
			  PFN_vkSetDeviceLoaderData setDeviceLoaderData_,
	          const VkDeviceCreateInfo *pCreateInfo);

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

	bool presentRequiresWrap(VkQueue queue, const VkPresentInfoKHR *pPresentInfo);
	VkResult present(VkQueue queue, VkSwapchainKHR swapchain, uint32_t index);
};

#include "dispatch_wrapper.hpp"

SurfaceState::SurfaceState(Instance *instance_)
	: instance(instance_)
{
	initClient(VK_NULL_HANDLE);
}

void SurfaceState::freeImage(ExportableImage &img)
{
	auto &table = *device->getTable();
	// These should already be signalled, otherwise we wouldn't be able to destroy swapchains safely.
	if (img.fence && img.fencePending)
		table.WaitForFences(device->getDevice(), 1, &img.fence, VK_TRUE, UINT64_MAX);
	table.DestroyFence(device->getDevice(), img.fence, nullptr);
	table.DestroySemaphore(device->getDevice(), img.acquireSemaphore, nullptr);
	table.DestroySemaphore(device->getDevice(), img.releaseSemaphore, nullptr);
	table.DestroyImage(device->getDevice(), img.image, nullptr);
	table.FreeMemory(device->getDevice(), img.memory, nullptr);
	table.DestroyCommandPool(device->getDevice(), img.cmdPool, nullptr);
	img.cmdBuffer = VK_NULL_HANDLE;
	img = {};
}

bool SurfaceState::handleEvent(PyroFling::Message &msg)
{
	if (auto *acq = PyroFling::maybe_get<PyroFling::AcquireImageMessage>(msg))
	{
		if (acq->wire.image_group_serial != image_group_serial)
			return true;
		if (acq->wire.index >= image.size())
			return false;

		auto &img = image[acq->wire.index];

		if (img.acquired)
			return false;

		img.acquired = true;

		// Need to verify the acquire semaphore has been waited on before we import a new payload.
		if (img.fencePending)
		{
			if (device->getTable()->WaitForFences(device->getDevice(), 1, &img.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
				return false;
			if (device->getTable()->ResetFences(device->getDevice(), 1, &img.fence) != VK_SUCCESS)
				return false;
			img.fencePending = false;
		}

		if (acq->wire.vk_external_semaphore_type)
		{
			VkImportSemaphoreFdInfoKHR sem_info = { VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR };
			sem_info.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
			sem_info.handleType = static_cast<VkExternalSemaphoreHandleTypeFlagBits>(acq->wire.vk_external_semaphore_type);
			sem_info.semaphore = img.acquireSemaphore;
			sem_info.fd = acq->fd.get_native_handle();
			if (device->getTable()->ImportSemaphoreFdKHR(device->getDevice(), &sem_info) == VK_SUCCESS)
			{
				img.liveAcquirePayload = true;
				acq->fd.release();
			}
			else
				return false;
		}
		else
		{
			if (acq->fd)
			{
				// Blocking acquire.
				uint64_t count;
				if (::read(acq->fd.get_native_handle(), &count, sizeof(count)) != sizeof(count))
					return false;
			}
			img.liveAcquirePayload = false;
		}
	}
	else if (auto *retire = PyroFling::maybe_get<PyroFling::RetireImageMessage>(msg))
	{
		if (retire->wire.image_group_serial != image_group_serial)
			return true;

		if (retire->wire.index >= image.size())
			return false;

		auto &img = image[retire->wire.index];
		if (img.ready)
			return false;
		img.ready = true;
	}
	else if (auto *complete = PyroFling::maybe_get<PyroFling::FrameCompleteMessage>(msg))
	{
		if (complete->wire.image_group_serial != image_group_serial)
			return true;
		complete_present_id = complete->wire.presented_id;
	}
	else
		return false;

	return true;
}

void SurfaceState::initClient(VkPhysicalDevice gpu)
{
	if (activePhysicalDevice && gpu != activePhysicalDevice)
	{
		client.reset();
		activePhysicalDevice = VK_NULL_HANDLE;
	}

	if (!client)
	{
		try
		{
			activePhysicalDevice = VK_NULL_HANDLE;
			const char *env = getenv("PYROFLING_SERVER");
			client = std::make_unique<PyroFling::Client>(env ? env : "/tmp/pyrofling-socket");

			PyroFling::ClientHelloMessage::WireFormat hello = {};
			hello.intent = PyroFling::ClientIntent::VulkanExternalStream;
			snprintf(hello.name, sizeof(hello.name), "%s - %s",
			         instance->applicationName.empty() ? "default" : instance->applicationName.c_str(),
			         instance->engineName.empty() ? "default" : instance->engineName.c_str());

			uint64_t serial = client->send_wire_message(hello);
			if (serial)
			{
				client->set_serial_handler(serial, [](const PyroFling::Message &msg) {
					return msg.get_type() == PyroFling::MessageType::ServerHello;
				});
			}
			else
				client.reset();

			client->set_default_serial_handler([](PyroFling::Message &msg) {
				return msg.get_type() == PyroFling::MessageType::OK;
			});

			client->set_event_handler([this](PyroFling::Message &msg) {
				return handleEvent(msg);
			});
		}
		catch (const std::exception &)
		{
		}
	}

	if (client && !activePhysicalDevice && gpu)
	{
		PyroFling::DeviceMessage::WireFormat wire = {};

		VkPhysicalDeviceProperties2 props2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
		VkPhysicalDeviceIDProperties idProps = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
		props2.pNext = &idProps;
		instance->getTable()->GetPhysicalDeviceProperties2KHR(gpu, &props2);

		wire.luid_valid = idProps.deviceLUIDValid;
		memcpy(wire.luid, idProps.deviceLUID, VK_LUID_SIZE);
		memcpy(wire.device_uuid, idProps.deviceUUID, VK_UUID_SIZE);
		memcpy(wire.driver_uuid, idProps.driverUUID, VK_UUID_SIZE);
		if (!client->send_wire_message(wire))
			client.reset();

		if (!image.empty() && !sendImageGroup())
			client.reset();
	}

	activePhysicalDevice = gpu;
}

bool SurfaceState::pollConnection()
{
	int ret;
	while ((ret = client->wait_reply(0)) > 0)
	{
	}
	return ret >= 0;
}

bool SurfaceState::waitConnection()
{
	return client->wait_reply() > 0;
}

bool SurfaceState::acquire(uint32_t &index)
{
	index = UINT32_MAX;

	do
	{
		for (uint32_t i = 0, n = image.size(); i < n; i++)
		{
			auto &img = image[i];
			if (img.ready && img.acquired)
			{
				index = i;
				break;
			}
		}

		if (index == UINT32_MAX && !waitConnection())
			break;
	} while (index == UINT32_MAX);
	return index != UINT32_MAX;
}

VkResult SurfaceState::processPresent(VkQueue queue, uint32_t index)
{
	VkResult res;
	auto &table = *device->getTable();

	if (!client)
	{
		if (++retry_counter >= 30)
		{
			initClient(activePhysicalDevice);
			retry_counter = 0;
		}
	}

	if (!client)
		return VK_SUCCESS;

	if (!pollConnection())
	{
		client.reset();
		return VK_SUCCESS;
	}

	// Blocking in present isn't great.
	// If we implement WSI ourselves, we would deal with it more properly where acquire ties to client acquire.
	uint32_t clientIndex;
	if (!acquire(clientIndex))
	{
		client.reset();
		return VK_SUCCESS;
	}

	auto &img = image[clientIndex];

	if (img.liveAcquirePayload)
	{
		const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submit.pWaitDstStageMask = &wait_stage;
		submit.pWaitSemaphores = &img.acquireSemaphore;
		submit.waitSemaphoreCount = 1;
		if ((res = table.QueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)) != VK_SUCCESS)
			return res;
		img.liveAcquirePayload = false;
	}

	if (!img.cmdPool || (img.cmdPool && img.currentQueueFamily != device->queueToFamilyIndex(queue)))
	{
		img.currentQueueFamily = device->queueToFamilyIndex(queue);
		table.DestroyCommandPool(device->getDevice(), img.cmdPool, nullptr);
		VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		poolInfo.queueFamilyIndex = img.currentQueueFamily;
		poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
		if ((res = table.CreateCommandPool(device->getDevice(), &poolInfo, nullptr, &img.cmdPool)) != VK_SUCCESS)
			return res;

		VkCommandBufferAllocateInfo cmdAllocateInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		cmdAllocateInfo.commandBufferCount = 1;
		cmdAllocateInfo.commandPool = img.cmdPool;
		cmdAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		if ((res = table.AllocateCommandBuffers(device->getDevice(), &cmdAllocateInfo, &img.cmdBuffer)) != VK_SUCCESS)
			return res;

		// Have to initialize the loader dispatch since we're calling it inline.
		device->setDeviceLoaderData(device->getDevice(), img.cmdBuffer);
	}

	table.ResetCommandPool(device->getDevice(), img.cmdPool, 0);
	VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	table.BeginCommandBuffer(img.cmdBuffer, &beginInfo);

	VkImageMemoryBarrier imgBarriers[2] = {};

	imgBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	imgBarriers[0].image = img.image;
	imgBarriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imgBarriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	imgBarriers[0].srcAccessMask = 0;
	imgBarriers[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	imgBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	imgBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	imgBarriers[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

	imgBarriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	imgBarriers[1].image = swapImages[index];
	imgBarriers[1].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	imgBarriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	imgBarriers[1].srcAccessMask = 0;
	imgBarriers[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	imgBarriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	imgBarriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	imgBarriers[1].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

	table.CmdPipelineBarrier(img.cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                         0, 0, nullptr, 0, nullptr, 2, imgBarriers);

	VkImageCopy region = {};
	region.extent.width = width;
	region.extent.height = height;
	region.extent.depth = 1;
	region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	table.CmdCopyImage(img.cmdBuffer, swapImages[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					   img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	imgBarriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	imgBarriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	imgBarriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	imgBarriers[0].dstAccessMask = 0;
	imgBarriers[0].srcQueueFamilyIndex = img.currentQueueFamily;
	imgBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
	imgBarriers[1].srcAccessMask = 0;
	imgBarriers[1].dstAccessMask = 0;
	imgBarriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	imgBarriers[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	table.CmdPipelineBarrier(img.cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
	                         0, 0, nullptr, 0, nullptr, 2, imgBarriers);

	if ((res = table.EndCommandBuffer(img.cmdBuffer)) != VK_SUCCESS)
		return res;

	VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submit.pSignalSemaphores = &img.releaseSemaphore;
	submit.signalSemaphoreCount = 1;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &img.cmdBuffer;
	if ((res = table.QueueSubmit(queue, 1, &submit, img.fence)) != VK_SUCCESS)
		return res;

	VkSemaphoreGetFdInfoKHR semGetFdInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR };
	semGetFdInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
	semGetFdInfo.semaphore = img.releaseSemaphore;
	int fd;
	if ((res = table.GetSemaphoreFdKHR(device->getDevice(), &semGetFdInfo, &fd)) != VK_SUCCESS)
		return res;
	PyroFling::FileHandle release_fd{fd};

	PyroFling::PresentImageMessage::WireFormat wire = {};
	wire.image_group_serial = image_group_serial;
	wire.index = clientIndex;
	wire.vk_external_semaphore_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;

	if (instance->getSyncMode() == SyncMode::Server)
		wire.period = 1;
	else if (instance->getSyncMode() == SyncMode::Client)
		wire.period = 0;
	else
		wire.period = presentMode == VK_PRESENT_MODE_FIFO_KHR || presentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR ? 1 : 0;

	wire.vk_old_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	wire.vk_new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	wire.id = ++present_id;
	if (!client->send_wire_message(wire, &release_fd, 1))
	{
		client.reset();
		return VK_SUCCESS;
	}

	img.acquired = false;
	img.ready = false;
	img.fencePending = true;

	if (wire.period > 0)
	{
		// Ensure proper pacing.
		// Acquire/Retire events may arrive in un-paced order, but completion events are well-paced.
		// In 2 image mode, we basically need to block until next heartbeat completes.

		while (complete_present_id + (image.size() - 2) < present_id)
		{
			if (client->wait_reply() < 0)
			{
				client.reset();
				return VK_SUCCESS;
			}
		}
	}

	return VK_SUCCESS;
}

bool SurfaceState::sendImageGroup()
{
	std::vector<PyroFling::FileHandle> fds(image.size());

	for (uint32_t i = 0; i < image.size(); i++)
	{
		VkMemoryGetFdInfoKHR memoryGetInfo = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR };
		memoryGetInfo.memory = image[i].memory;
		memoryGetInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
		int fd;
		if (device->getTable()->GetMemoryFdKHR(device->getDevice(), &memoryGetInfo, &fd) == VK_SUCCESS)
			fds[i] = PyroFling::FileHandle{fd};
		else
			return false;
	}

	if (!(image_group_serial = client->send_wire_message(imageGroupWire, fds.data(), fds.size())))
		return false;

	present_id = 0;
	complete_present_id = 0;

	for (auto &img : image)
	{
		img.ready = true;
		img.acquired = true;

		if (img.fencePending)
		{
			if (device->getTable()->WaitForFences(device->getDevice(), 1, &img.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
				return false;
			if (device->getTable()->ResetFences(device->getDevice(), 1, &img.fence) != VK_SUCCESS)
				return false;
			img.fencePending = false;
		}
	}

	return true;
}

bool SurfaceState::initImageGroup(uint32_t count)
{
	auto &table = *device->getTable();

	VkImageFormatListCreateInfoKHR formatList = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO };
	VkFormat mutableFormats[2];
	formatList.pViewFormats = mutableFormats;

	VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	info.extent.width = width;
	info.extent.height = height;
	info.extent.depth = 1;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = format.format;
	info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	             VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	             VK_IMAGE_USAGE_SAMPLED_BIT;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	info.arrayLayers = 1;
	info.mipLevels = 1;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;

	if (info.format == VK_FORMAT_R8G8B8A8_SRGB || info.format == VK_FORMAT_R8G8B8A8_UNORM)
	{
		mutableFormats[0] = VK_FORMAT_R8G8B8A8_UNORM;
		mutableFormats[1] = VK_FORMAT_R8G8B8A8_SRGB;
		info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
		formatList.viewFormatCount = 2;
		info.pNext = &formatList;
	}
	else if (info.format == VK_FORMAT_B8G8R8A8_SRGB || info.format == VK_FORMAT_B8G8R8A8_UNORM)
	{
		mutableFormats[0] = VK_FORMAT_B8G8R8A8_UNORM;
		mutableFormats[1] = VK_FORMAT_B8G8R8A8_SRGB;
		info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
		formatList.viewFormatCount = 2;
		info.pNext = &formatList;
	}
	else if (info.format == VK_FORMAT_A8B8G8R8_SRGB_PACK32 || info.format == VK_FORMAT_A8B8G8R8_UNORM_PACK32)
	{
		mutableFormats[0] = VK_FORMAT_A8B8G8R8_UNORM_PACK32;
		mutableFormats[1] = VK_FORMAT_A8B8G8R8_SRGB_PACK32;
		info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
		formatList.viewFormatCount = 2;
		info.pNext = &formatList;
	}

	VkExternalMemoryImageCreateInfo externalInfo = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
	externalInfo.pNext = info.pNext;
	info.pNext = &externalInfo;
#ifndef _WIN32
	externalInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif

	for (uint32_t i = 0; i < count; i++)
	{
		image.push_back({});
		ExportableImage &exp = image.back();
		exp.acquired = true;
		exp.ready = true;

		if (table.CreateImage(device->getDevice(), &info, nullptr, &exp.image) != VK_SUCCESS)
			return false;

		VkMemoryRequirements reqs;
		table.GetImageMemoryRequirements(device->getDevice(), exp.image, &reqs);

		VkPhysicalDeviceMemoryProperties memProps;
		device->getInstance()->getTable()->GetPhysicalDeviceMemoryProperties(device->getPhysicalDevice(), &memProps);
		uint32_t index = UINT32_MAX;
		for (uint32_t type_index = 0; type_index < memProps.memoryTypeCount; type_index++)
		{
			if ((memProps.memoryTypes[type_index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
			    (reqs.memoryTypeBits & (1u << type_index)))
			{
				index = type_index;
				break;
			}
		}

		if (index == UINT32_MAX)
			return false;

		VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		VkMemoryDedicatedAllocateInfo dedicatedInfo = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
		VkExportMemoryAllocateInfo exportInfo = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
		allocInfo.allocationSize = reqs.size;
		allocInfo.memoryTypeIndex = index;
		allocInfo.pNext = &dedicatedInfo;
		dedicatedInfo.image = exp.image;
		dedicatedInfo.pNext = &exportInfo;
		exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

		if (table.AllocateMemory(device->getDevice(), &allocInfo, nullptr, &exp.memory) != VK_SUCCESS)
			return false;
		if (table.BindImageMemory(device->getDevice(), exp.image, exp.memory, 0) != VK_SUCCESS)
			return false;

		VkSemaphoreCreateInfo semCreateInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		VkExportSemaphoreCreateInfo semExportInfo = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO };
		if (table.CreateSemaphore(device->getDevice(), &semCreateInfo, nullptr, &exp.acquireSemaphore) != VK_SUCCESS)
			return false;
		semCreateInfo.pNext = &semExportInfo;
		semExportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
		if (table.CreateSemaphore(device->getDevice(), &semCreateInfo, nullptr, &exp.releaseSemaphore) != VK_SUCCESS)
			return false;

		VkFenceCreateInfo fence = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		if (device->getTable()->CreateFence(device->getDevice(), &fence, nullptr, &exp.fence) != VK_SUCCESS)
			return false;
	}

	imageGroupWire.width = info.extent.width;
	imageGroupWire.height = info.extent.height;
	imageGroupWire.vk_format = info.format;
	imageGroupWire.vk_color_space = format.colorSpace;
	imageGroupWire.vk_num_view_formats = formatList.viewFormatCount;
	memcpy(imageGroupWire.vk_view_formats, formatList.pViewFormats, formatList.viewFormatCount * sizeof(VkFormat));
	imageGroupWire.vk_external_memory_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	imageGroupWire.num_images = count;
	imageGroupWire.vk_image_flags = info.flags;
	imageGroupWire.vk_image_usage = info.usage;

	return true;
}

void SurfaceState::setActiveDeviceAndSwapchain(Device *device_, const VkSwapchainCreateInfoKHR *pCreateInfo,
                                               VkSwapchainKHR chain)
{
	if (device != device_)
	{
		for (auto &img : image)
			freeImage(img);
		image.clear();
		activeSwapchain = VK_NULL_HANDLE;
		device = device_;
	}

	if (device)
		initClient(device->getPhysicalDevice());

	if (activeSwapchain == chain || chain == VK_NULL_HANDLE)
	{
		activeSwapchain = chain;
		return;
	}

	auto &info = *pCreateInfo;

	presentMode = info.presentMode;
	activeSwapchain = chain;
	if (instance->getSyncMode() == SyncMode::Server)
		presentMode = VK_PRESENT_MODE_MAILBOX_KHR;

	uint32_t count;
	device->getTable()->GetSwapchainImagesKHR(device->getDevice(), chain, &count, nullptr);
	swapImages.resize(count);
	device->getTable()->GetSwapchainImagesKHR(device->getDevice(), chain, &count, swapImages.data());

	// If nothing meaningfully changed, just go ahead and update the input images.
	if (info.imageExtent.width == width && info.imageExtent.height == height &&
	    info.imageFormat == format.format && info.imageColorSpace == format.colorSpace)
	{
		return;
	}

	width = info.imageExtent.width;
	height = info.imageExtent.height;
	format.format = info.imageFormat;
	format.colorSpace = info.imageColorSpace;

	for (auto &img : image)
		freeImage(img);
	image.clear();

	unsigned forced = instance->forcesNumImages();
	if (forced < 2)
		forced = 3;

	if (!initImageGroup(forced))
		client.reset();

	if (client && !sendImageGroup())
		client.reset();
}

SurfaceState::~SurfaceState()
{
	if (device)
		for (auto &img : image)
			freeImage(img);
}

uint32_t Device::queueToFamilyIndex(VkQueue queue) const
{
	for (auto &info : queueToFamily)
		if (info.queue == queue)
			return info.familyIndex;
	return VK_QUEUE_FAMILY_IGNORED;
}

bool Device::presentRequiresWrap(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
	uint32_t family = queueToFamilyIndex(queue);

	// Shouldn't happen.
	if (family == VK_QUEUE_FAMILY_IGNORED)
		return false;

	// TODO: Also verify that the queue is capable of copies.
	// Present only queues do not exist in the real world as far as I'm aware though ...

	auto *inst = getInstance();
	std::lock_guard<std::mutex> holder{inst->surfaceLock};
	for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++)
		if (inst->findActiveSurfaceLocked(this, pPresentInfo->pSwapchains[i]))
			return true;

	return false;
}

VkResult Device::present(VkQueue queue, VkSwapchainKHR swapchain, uint32_t index)
{
	auto *inst = getInstance();
	SurfaceState *surface;
	{
		std::lock_guard<std::mutex> holder{inst->surfaceLock};
		surface = inst->findActiveSurfaceLocked(this, swapchain);
	}

	if (surface)
		return surface->processPresent(queue, index);
	else
		return VK_SUCCESS;
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
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                                                     const VkAllocationCallbacks *pAllocator, VkInstance *pInstance)
{
	auto *chainInfo = getChainInfo(pCreateInfo, VK_LAYER_LINK_INFO);

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
	addUniqueExtension(enabledExtensions, VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);
	addUniqueExtension(enabledExtensions, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
	tmpCreateInfo.enabledExtensionCount = enabledExtensions.size();
	tmpCreateInfo.ppEnabledExtensionNames = enabledExtensions.data();

	chainInfo->u.pLayerInfo = chainInfo->u.pLayerInfo->pNext;
	auto res = fpCreateInstance(&tmpCreateInfo, pAllocator, pInstance);
	if (res != VK_SUCCESS)
		return res;

	{
		std::lock_guard<std::mutex> holder{globalLock};
		auto *layer = createLayerData(getDispatchKey(*pInstance), instanceData);
		layer->init(*pInstance, pCreateInfo->pApplicationInfo, fpGetInstanceProcAddr);
	}

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

static VKAPI_ATTR void VKAPI_CALL DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *pAllocator)
{
	auto *layer = getInstanceLayer(instance);
	layer->unregisterSurface(surface);
	layer->getTable()->DestroySurfaceKHR(instance, surface, pAllocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(
		VkPhysicalDevice                            physicalDevice,
		const char*                                 pLayerName,
		uint32_t*                                   pPropertyCount,
		VkExtensionProperties*                      pProperties)
{
	if (pLayerName && strcmp(pLayerName, "VK_LAYER_pyrofling_capture") == 0)
	{
		*pPropertyCount = 0;
		return VK_SUCCESS;
	}

	auto *layer = getInstanceLayer(physicalDevice);

	if (layer->getSyncMode() != SyncMode::Server)
	{
		return layer->getTable()->EnumerateDeviceExtensionProperties(
				physicalDevice, pLayerName, pPropertyCount, pProperties);
	}

	// The surface and display queries are all instance extensions,
	// and thus the loader is responsible for dealing with it.
	uint32_t count = 0;
	layer->getTable()->EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, &count, nullptr);
	std::vector<VkExtensionProperties> props(count);
	layer->getTable()->EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, &count, props.data());

	// When we force MAILBOX mode, present wait/id is not meaningful and will cause very jank frame pacing.
	// Disable any dependent extension as well that is built on top of present ID/wait.
	auto itr = std::remove_if(props.begin(), props.end(), [](const VkExtensionProperties &prop) -> bool {
		return strcmp(prop.extensionName, VK_KHR_PRESENT_ID_EXTENSION_NAME) == 0 ||
		       strcmp(prop.extensionName, VK_KHR_PRESENT_WAIT_EXTENSION_NAME) == 0 ||
		       strcmp(prop.extensionName, VK_NV_LOW_LATENCY_2_EXTENSION_NAME) == 0;
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
	auto fpGetDeviceProcAddr = chainInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
	auto fpCreateDevice = layer->getTable()->CreateDevice;
	auto fpEnumerateDeviceExtensionProperties = layer->getTable()->EnumerateDeviceExtensionProperties;

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

	// If these are not supported for whatever reason,
	// we will just not wrap entry points and pass through all device functions.
	auto tmpCreateInfo = *pCreateInfo;
	addUniqueExtension(enabledExtensions, supportedExts, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME);
	addUniqueExtension(enabledExtensions, supportedExts, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
	addUniqueExtension(enabledExtensions, supportedExts, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
	addUniqueExtension(enabledExtensions, supportedExts, VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME);
	addUniqueExtension(enabledExtensions, supportedExts, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
#ifndef _WIN32
	addUniqueExtension(enabledExtensions, supportedExts, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
	addUniqueExtension(enabledExtensions, supportedExts, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
#endif
	tmpCreateInfo.enabledExtensionCount = enabledExtensions.size();
	tmpCreateInfo.ppEnabledExtensionNames = enabledExtensions.data();

	// Advance the link info for the next element on the chain
	chainInfo->u.pLayerInfo = chainInfo->u.pLayerInfo->pNext;

	auto res = fpCreateDevice(gpu, &tmpCreateInfo, pAllocator, pDevice);
	if (res != VK_SUCCESS)
		return res;

	{
		std::lock_guard<std::mutex> holder{globalLock};
		auto *device = createLayerData(getDispatchKey(*pDevice), deviceData);
		device->init(gpu, *pDevice, layer, fpGetDeviceProcAddr, fpSetDeviceLoaderData, &tmpCreateInfo);
	}

	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator)
{
	void *key = getDispatchKey(device);
	auto *layer = getLayerData(key, deviceData);
	layer->getInstance()->unregisterDevice(layer);
	layer->getTable()->DestroyDevice(device, pAllocator);

	std::lock_guard<std::mutex> holder{ globalLock };
	destroyLayerData(key, deviceData);
}

static VKAPI_ATTR VkResult VKAPI_CALL
CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator,
                   VkSwapchainKHR *pSwachain)
{
	auto *layer = getDeviceLayer(device);

	// Probably need to query support for this, but really ...
	auto info = *pCreateInfo;
	info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

	auto result = layer->getTable()->CreateSwapchainKHR(device, &info, pAllocator, pSwachain);
	if (result != VK_SUCCESS)
		return result;

	auto *instance = layer->getInstance();
	auto *surface = instance->registerSurface(pCreateInfo->surface);
	surface->setActiveDeviceAndSwapchain(layer, pCreateInfo, *pSwachain);
	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *pAllocator)
{
	auto *layer = getDeviceLayer(device);
	layer->getTable()->DestroySwapchainKHR(device, swapchain, pAllocator);
	layer->getInstance()->unregisterSwapchain(layer, swapchain);
}

static VKAPI_ATTR VkResult VKAPI_CALL
QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
	auto *layer = getDeviceLayer(queue);

	// If we have no connections associated with this present, just pass it through.
	if (!layer->presentRequiresWrap(queue, pPresentInfo))
		return layer->getTable()->QueuePresentKHR(queue, pPresentInfo);

	VkResult result;

	// Wait semaphore count is generally just 1, so don't bother allocating wait dst stage arrays.
	for (uint32_t i = 0; i < pPresentInfo->waitSemaphoreCount; i++)
	{
		VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		submit.waitSemaphoreCount = 1;
		submit.pWaitDstStageMask = &waitStage;
		submit.pWaitSemaphores = &pPresentInfo->pWaitSemaphores[i];
		if ((result = layer->getTable()->QueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)) != VK_SUCCESS)
			return result;
	}

	for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++)
	{
		VkSwapchainKHR swap = pPresentInfo->pSwapchains[i];
		uint32_t index = pPresentInfo->pImageIndices[i];

		// We're just concerned with fatal errors here like DEVICE_LOST etc.
		if ((result = layer->present(queue, swap, index)) != VK_SUCCESS)
			return result;
	}

	// Resignal the semaphores when we're done blitting so that the normal WSI request goes through.
	if (pPresentInfo->waitSemaphoreCount)
	{
		VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submit.signalSemaphoreCount = pPresentInfo->waitSemaphoreCount;
		submit.pSignalSemaphores = pPresentInfo->pWaitSemaphores;
		if ((result = layer->getTable()->QueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)) != VK_SUCCESS)
			return result;
	}

	result = layer->getTable()->QueuePresentKHR(queue, pPresentInfo);
	return result;
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
		{ "vkGetInstanceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(VK_LAYER_PYROFLING_CAPTURE_vkGetInstanceProcAddr) },
		{ "vkEnumerateDeviceExtensionProperties", reinterpret_cast<PFN_vkVoidFunction>(EnumerateDeviceExtensionProperties) },
		{ "vkCreateDevice", reinterpret_cast<PFN_vkVoidFunction>(CreateDevice) },
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
		{ "vkDestroySurfaceKHR", reinterpret_cast<PFN_vkVoidFunction>(DestroySurfaceKHR) },
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
		{ "vkGetDeviceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(VK_LAYER_PYROFLING_CAPTURE_vkGetDeviceProcAddr) },
		{ "vkQueuePresentKHR", reinterpret_cast<PFN_vkVoidFunction>(QueuePresentKHR) },
		{ "vkCreateSwapchainKHR", reinterpret_cast<PFN_vkVoidFunction>(CreateSwapchainKHR) },
		{ "vkDestroySwapchainKHR", reinterpret_cast<PFN_vkVoidFunction>(DestroySwapchainKHR) },
		{ "vkDestroyDevice", reinterpret_cast<PFN_vkVoidFunction>(DestroyDevice) },
	};

	for (auto &cmd : coreDeviceCommands)
		if (strcmp(cmd.name, pName) == 0)
			return cmd.proc;

	return nullptr;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
VK_LAYER_PYROFLING_CAPTURE_vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
	Device *layer;
	{
		std::lock_guard<std::mutex> holder{globalLock};
		layer = getLayerData(getDispatchKey(device), deviceData);
	}

	auto proc = layer->getTable()->GetDeviceProcAddr(device, pName);

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
VK_LAYER_PYROFLING_CAPTURE_vkGetInstanceProcAddr(VkInstance instance, const char *pName)
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
VK_LAYER_PYROFLING_CAPTURE_vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct)
{
	if (pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT || pVersionStruct->loaderLayerInterfaceVersion < 2)
		return VK_ERROR_INITIALIZATION_FAILED;

	if (pVersionStruct->loaderLayerInterfaceVersion > CURRENT_LOADER_LAYER_INTERFACE_VERSION)
		pVersionStruct->loaderLayerInterfaceVersion = CURRENT_LOADER_LAYER_INTERFACE_VERSION;

	if (pVersionStruct->loaderLayerInterfaceVersion >= 2)
	{
		pVersionStruct->pfnGetInstanceProcAddr = VK_LAYER_PYROFLING_CAPTURE_vkGetInstanceProcAddr;
		pVersionStruct->pfnGetDeviceProcAddr = VK_LAYER_PYROFLING_CAPTURE_vkGetDeviceProcAddr;
		pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
	}

	return VK_SUCCESS;
}
