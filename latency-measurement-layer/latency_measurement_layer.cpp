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
#include <atomic>
#include <random>
#include <unistd.h>
#include <stdio.h>
#include "virtual_gamepad.hpp"
#include "pyro_client.hpp"
#include <sys/stat.h>

#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <fcntl.h>

extern "C"
{
VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
VK_LAYER_PYROFLING_LATENCY_MEASUREMENT_vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
VK_LAYER_PYROFLING_LATENCY_MEASUREMENT_vkGetDeviceProcAddr(VkDevice device, const char *pName);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
VK_LAYER_PYROFLING_LATENCY_MEASUREMENT_vkGetInstanceProcAddr(VkInstance instance, const char *pName);
}

static const char *blockedExtensions[] = {
	// We need to use presentID2 ourselves. Conflicts with GOOGLE display timing.
	VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME,
	// We need to use presentID2 ourselves. Conflicts with app using PresentID1.
	// We can cooperate with PresentID2 though.
	VK_KHR_PRESENT_ID_EXTENSION_NAME,
	VK_KHR_PRESENT_WAIT_EXTENSION_NAME,
	// We need to reserve present timing use to ourselves.
	VK_EXT_PRESENT_TIMING_EXTENSION_NAME,
};

struct Instance;
struct Device;

struct Instance
{
	void init(VkInstance instance_, PFN_vkGetInstanceProcAddr gpa_, PFN_vkSetInstanceLoaderData setInstanceLoaderData_);

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
	VkLayerInstanceDispatchTable table = {};
	PFN_vkGetInstanceProcAddr gpa = nullptr;
	PFN_vkSetInstanceLoaderData setInstanceLoaderData = nullptr;
};

void Instance::init(VkInstance instance_, PFN_vkGetInstanceProcAddr gpa_, PFN_vkSetInstanceLoaderData setInstanceLoaderData_)
{
	instance = instance_;
	gpa = gpa_;
	setInstanceLoaderData = setInstanceLoaderData_;
	layerInitInstanceDispatchTable(instance, &table, gpa);
}

struct Swapchain
{
	explicit Swapchain(Device *device);
	~Swapchain();

	VkResult init(const VkSwapchainCreateInfoKHR *pCreateInfo, VkSwapchainKHR swapchain);

	struct Buffer
	{
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkDeviceSize size = 0;
		void *mapped = nullptr;
	};

	struct SwapchainImage
	{
		Buffer buffer;
		VkImage image = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		bool busy = false;
	};

	struct
	{
		VkCommandPool pool = VK_NULL_HANDLE;
		uint32_t family = VK_QUEUE_FAMILY_IGNORED;
	} cmdPool;

	Device *device;
	std::vector<SwapchainImage> images;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;

	std::mutex lock;
	std::condition_variable cond;
	std::thread worker;

	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t representativeWidth = 0;
	uint32_t representativeHeight = 0;
	VkFormat format = {};
	double timeOffset = 0.0;

	struct FeedbackQueueEntry
	{
		uint64_t presentId;
		uint64_t submitted;
		uint64_t queueDone;
		uint64_t presentDone;
		VkPresentStageFlagsEXT presentStage;
	};

	enum class WorkType { WaitGPUWork, HandleFeedback };

	struct Work
	{
		WorkType type;
		union
		{
			struct
			{
				uint64_t presentId;
				uint32_t index;
			} waitWork;

			FeedbackQueueEntry feedbackEntry;
		} u;
	};

	std::queue<Work> workQueue;

	static VkCommandPool createCommandPool(VkDevice device, const VkLayerDispatchTable &table, uint32_t family);
	VkResult initSourceCommands(uint32_t familyIndex);
	VkResult submitSourceWork(VkQueue queue, uint32_t index, uint64_t presentId);
	VkResult setupSwapchainImage(const VkSwapchainCreateInfoKHR *pCreateInfo, VkImage image, uint32_t index);

	void runWorker();

	VkResult swapchainStatus = VK_SUCCESS;
	bool presentTiming = false;
	VkPresentStageFlagsEXT supportedStages = 0;
	uint64_t lastCandidateFrameId = 0;
	uint64_t lastStimulusTime = 0;

	uint64_t domainCounter = 0;
	uint64_t timeDomainId = 0;
	VkTimeDomainKHR timeDomain = {};

	enum { FeedbackQueueSize = 64 };

	FeedbackQueueEntry feedbackQueue[FeedbackQueueSize];
	uint32_t feedbackQueueSize = 0;
	uint64_t internalPresentId = 0;

	void pollFeedback();
	void updateFeedback(const VkPastPresentationTimingEXT &timing);
	void reportCompleteEventLocked(const FeedbackQueueEntry &entry);

	void runFakeInputStimulus();
	std::condition_variable fakeInputCond;
	std::thread fakeInputThread;
	std::mutex fakeInputMutex;
	bool fakeInputValid = false;

	struct FILEDeleter { void operator()(FILE *f) { if (f) fclose(f); } };
	std::unique_ptr<FILE, FILEDeleter> latencyReportFile;
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

	struct QueueInfo
	{
		VkQueue queue;
		uint32_t familyIndex;
	};
	std::vector<QueueInfo> queueToFamily;
	uint32_t queueToFamilyIndex(VkQueue queue) const;

	VkPhysicalDevicePresentId2FeaturesKHR id2Features;
	VkPhysicalDevicePresentTimingFeaturesEXT timingFeatures;

	VkLayerDispatchTable table = {};

	std::mutex swapchainLock;
	std::vector<std::pair<VkSwapchainKHR, std::unique_ptr<Swapchain>>> swapchains;
	void registerSwapchain(VkSwapchainKHR swapchain, std::unique_ptr<Swapchain> swap);
	void unregisterSwapchain(VkSwapchainKHR swapchain);
	Swapchain *getSwapchain(VkSwapchainKHR swapchain);

	VkPhysicalDeviceMemoryProperties memoryProps;
};

#include "dispatch_wrapper.hpp"

Device::~Device()
{
}

Swapchain *Device::getSwapchain(VkSwapchainKHR swapchain)
{
	std::lock_guard<std::mutex> holder{swapchainLock};
	for (auto &swap : swapchains)
		if (swap.first == swapchain)
			return swap.second.get();
	return nullptr;
}

void Device::registerSwapchain(VkSwapchainKHR swapchain, std::unique_ptr<Swapchain> swap)
{
	std::lock_guard<std::mutex> holder{swapchainLock};
	swapchains.emplace_back(swapchain, std::move(swap));
}

void Device::unregisterSwapchain(VkSwapchainKHR swapchain)
{
	std::lock_guard<std::mutex> holder{swapchainLock};

	auto itr = std::find_if(swapchains.begin(), swapchains.end(),
	                        [&](const std::pair<VkSwapchainKHR, std::unique_ptr<Swapchain>> &m)
	                        {
		                        return m.first == swapchain;
	                        });

	if (itr != swapchains.end())
		swapchains.erase(itr);
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

	instance->getTable()->GetPhysicalDeviceMemoryProperties(gpu, &memoryProps);

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
			setDeviceLoaderData(device, queue);
			queueToFamily.push_back({ queue, family });
		}
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
	auto &table = device->table;
	auto vkDevice = device->device;

	if (familyIndex != cmdPool.family)
	{
		// Wait until all source commands are done processing.
		std::unique_lock<std::mutex> holder{lock};
		cond.wait(holder, [this]() {
			for (auto &img : images)
				if (img.busy)
					return false;
			return true;
		});

		table.DestroyCommandPool(vkDevice, cmdPool.pool, nullptr);
		cmdPool.pool = VK_NULL_HANDLE;
	}
	else if (cmdPool.pool != VK_NULL_HANDLE)
	{
		// We're good.
		return VK_SUCCESS;
	}

	if (cmdPool.pool == VK_NULL_HANDLE)
	{
		cmdPool.pool = createCommandPool(vkDevice, table, familyIndex);
		cmdPool.family = familyIndex;
	}

	if (cmdPool.pool == VK_NULL_HANDLE)
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
		allocInfo.commandPool = cmdPool.pool;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		if (table.AllocateCommandBuffers(vkDevice, &allocInfo, &image.cmd) != VK_SUCCESS)
			return VK_ERROR_DEVICE_LOST;

		auto cmd = image.cmd;

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
			barrier.image = image.image;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

			table.CmdPipelineBarrier(cmd,
			                         VK_PIPELINE_STAGE_TRANSFER_BIT,
			                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
			                         0, nullptr,
			                         0, nullptr,
			                         1, &barrier);

			VkBufferImageCopy copy = {};
			copy.imageOffset = { int32_t(width - representativeWidth) / 2, int32_t(height - representativeHeight) / 2 };
			copy.imageExtent = { representativeWidth, representativeHeight, 1 };
			copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			copy.bufferRowLength = representativeWidth;
			copy.bufferImageHeight = representativeHeight;
			table.CmdCopyImageToBuffer(cmd,
			                           image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			                           image.buffer.buffer,
			                           1, &copy);

			// Show the capture area.
			barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.srcAccessMask = 0;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

			table.CmdPipelineBarrier(cmd,
				 VK_PIPELINE_STAGE_TRANSFER_BIT,
				 VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
				 0, nullptr,
				 0, nullptr,
				 1, &barrier);

			if (representativeWidth >= 16 && representativeHeight >= 16)
			{
				VkBufferImageCopy region = {};
				region.bufferOffset = representativeWidth * representativeHeight * sizeof(uint32_t);
				region.bufferRowLength = representativeWidth;
				region.bufferImageHeight = representativeHeight;
				region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
				region.imageExtent = { representativeWidth, representativeHeight, 1 };
				region.imageOffset = copy.imageOffset;

				table.CmdCopyBufferToImage(cmd, image.buffer.buffer, image.image,
				                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
			}

			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			barrier.dstAccessMask = 0;

			VkBufferMemoryBarrier bufBarrier = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
			bufBarrier.buffer = image.buffer.buffer;
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

VkResult Swapchain::setupSwapchainImage(const VkSwapchainCreateInfoKHR *, VkImage image, uint32_t index)
{
	auto &img = images[index];
	img.image = image;

	VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bufferInfo.size = representativeWidth * representativeHeight * sizeof(uint32_t) * 2;
	bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

	auto vr = device->table.CreateBuffer(device->device, &bufferInfo, nullptr, &img.buffer.buffer);
	if (vr != VK_SUCCESS)
		return vr;

	VkMemoryRequirements reqs;
	device->table.GetBufferMemoryRequirements(device->device, img.buffer.buffer, &reqs);

	uint32_t memoryTypeIndex = UINT32_MAX;
	for (uint32_t i = 0; i < device->memoryProps.memoryTypeCount; i++)
	{
		constexpr VkMemoryPropertyFlags required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                                           VK_MEMORY_PROPERTY_HOST_CACHED_BIT |
		                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		if ((device->memoryProps.memoryTypes[i].propertyFlags & required) == required &&
			((1u << i) & reqs.memoryTypeBits) != 0)
		{
			memoryTypeIndex = i;
			break;
		}
	}

	if (memoryTypeIndex == UINT32_MAX)
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;

	VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocInfo.allocationSize = reqs.size;
	allocInfo.memoryTypeIndex = memoryTypeIndex;

	auto res = device->table.AllocateMemory(device->device, &allocInfo, nullptr, &img.buffer.memory);
	if (res != VK_SUCCESS)
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;

	res = device->table.BindBufferMemory(device->device, img.buffer.buffer, img.buffer.memory, 0);
	if (res != VK_SUCCESS)
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;

	res = device->table.MapMemory(device->device, img.buffer.memory, 0, VK_WHOLE_SIZE, 0, &img.buffer.mapped);
	if (res != VK_SUCCESS)
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;

	VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	res = device->table.CreateFence(device->device, &fenceInfo, nullptr, &img.fence);
	if (res != VK_SUCCESS)
		return res;

	return VK_SUCCESS;
}

static double computeMSE8(const uint32_t *a, const uint32_t *b, size_t count)
{
	double mse = 0.0;

	for (size_t i = 0; i < count; i++)
	{
		auto pix_a = a[i];
		auto pix_b = b[i];

		int ra, ga, ba, rb, gb, bb;

		// RGBA vs BGRA flip is not important.
		ra = int((pix_a >> 0) & 0xff);
		ga = int((pix_a >> 8) & 0xff);
		ba = int((pix_a >> 16) & 0xff);

		rb = int((pix_b >> 0) & 0xff);
		gb = int((pix_b >> 8) & 0xff);
		bb = int((pix_b >> 16) & 0xff);

		int rdiff = ra - rb;
		int gdiff = ga - gb;
		int bdiff = ba - bb;

		mse += double(rdiff * rdiff + gdiff * gdiff + bdiff * bdiff);
	}

	return mse / double(3 * count);
}

static double computeMSE10(const uint32_t *a, const uint32_t *b, size_t count)
{
	double mse = 0.0;

	for (size_t i = 0; i < count; i++)
	{
		auto pix_a = a[i];
		auto pix_b = b[i];

		int ra, ga, ba, rb, gb, bb;

		// RGBA vs BGRA flip is not important.
		ra = int((pix_a >> 0) & 0x3ff);
		ga = int((pix_a >> 10) & 0x3ff);
		ba = int((pix_a >> 20) & 0x3ff);

		rb = int((pix_b >> 0) & 0x3ff);
		gb = int((pix_b >> 10) & 0x3ff);
		bb = int((pix_b >> 20) & 0x3ff);

		int rdiff = ra - rb;
		int gdiff = ga - gb;
		int bdiff = ba - bb;

		mse += double(rdiff * rdiff + gdiff * gdiff + bdiff * bdiff);
	}

	return mse / double(3 * count);
}

static uint64_t getTimeNS()
{
	timespec ts = {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

struct FakeMouse
{
	bool init();
	~FakeMouse();
	void sendRelativeImpulse(int x, int y);
	int fd = -1;
	void emit(int type, int code, int val);
};

void FakeMouse::emit(int type, int code, int val)
{
	input_event ie = {};
	ie.type = type;
	ie.code = code;
	ie.value = val;
	write(fd, &ie, sizeof(ie));
}

bool FakeMouse::init()
{
	uinput_setup usetup = {};

	fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0)
		return false;

	if (::ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) return false;
	if (::ioctl(fd, UI_SET_KEYBIT, BTN_LEFT) < 0) return false;
	if (::ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT) < 0) return false;

	if (::ioctl(fd, UI_SET_EVBIT, EV_REL) < 0) return false;
	if (::ioctl(fd, UI_SET_RELBIT, REL_X) < 0) return false;
	if (::ioctl(fd, UI_SET_RELBIT, REL_Y) < 0) return false;

	memset(&usetup, 0, sizeof(usetup));
	usetup.id.bustype = BUS_USB;
	usetup.id.vendor = PyroFling::VirtualGamepad::FAKE_VID;
	usetup.id.product = PyroFling::VirtualGamepad::FAKE_PID + 2;
	strcpy(usetup.name, "PyroFling Fake Mouse");

	if (::ioctl(fd, UI_DEV_SETUP, &usetup) < 0) return false;
	if (::ioctl(fd, UI_DEV_CREATE) < 0) return false;

	return true;
}

FakeMouse::~FakeMouse()
{
	if (fd >= 0)
	{
		ioctl(fd, UI_DEV_DESTROY);
		close(fd);
	}
}

void FakeMouse::sendRelativeImpulse(int x, int y)
{
	emit(EV_REL, REL_X, x);
	emit(EV_REL, REL_Y, y);
	emit(EV_SYN, SYN_REPORT, 0);
}

void Swapchain::runFakeInputStimulus()
{
	std::string fakeStimulusTriggerPath;
	if (const char *env = getenv("LATENCY_MEASUREMENT_TRIGGER"))
		fakeStimulusTriggerPath = env;
	else
		fakeStimulusTriggerPath = "/tmp/latency-measurement-trigger";

	FakeMouse fakeMouse;
	std::unique_ptr<PyroFling::VirtualGamepad> virtualGamepad;
	PyroFling::PyroStreamClient client;
	bool hasPyroClient = true;

	const char *pyroAddr = nullptr;
	const char *pyroPort = nullptr;

	if (const char *env = getenv("PYROFLING_IP"))
		pyroAddr = env;
	if (const char *env = getenv("PYROFLING_PORT"))
		pyroPort = env;

	bool hasFakeMouse = false;
	int mouseImpulseRange = 0;

	if (const char *env = getenv("LATENCY_MEASUREMENT_MOUSE"))
		mouseImpulseRange = atoi(env);

	if (mouseImpulseRange > 0)
		hasFakeMouse = fakeMouse.init();

	if (!hasFakeMouse)
	{
		// Many games don't like it if we connect a controller late like this.
		// If we can, piggyback off pyrofling server instead which can live "persistently".
		if (!pyroAddr || !pyroPort || !client.connect(pyroAddr, pyroPort) ||
			!client.handshake(PYRO_KICK_STATE_GAMEPAD_BIT))
		{
			hasPyroClient = false;
			virtualGamepad = std::make_unique<PyroFling::VirtualGamepad>(
				PyroFling::VirtualGamepad::FAKE_VID,
				PyroFling::VirtualGamepad::FAKE_PID + 1, "PyroFling Test Stimulus");
		}
	}

	// Report the neutral position.
	pyro_gamepad_state state = {};

	const auto sendGamepadState = [&]()
	{
		if (hasFakeMouse)
			return;

		state.buttons |= PYRO_PAD_MODE_BIT;

		if (hasPyroClient)
			client.send_gamepad_state(state);
		else
			virtualGamepad->report_state(state);
	};

	sendGamepadState();

	// Seed doesn't matter.
	std::mt19937 rng(0);

	bool lastInputWasActive = false;

	bool debugOutput = false;
	if (const char *env = getenv("LATENCY_MEASUREMENT_DEBUG"))
		debugOutput = strcmp(env, "1") == 0;

	for (;;)
	{
		if (hasPyroClient)
			client.flush_packet_queue();

		std::unique_lock<std::mutex> holder{fakeInputMutex};
		// An arbitrary heartbeat.
		fakeInputCond.wait_for(holder, std::chrono::milliseconds(23), [this]()
		{
			return swapchainStatus < 0;
		});

		if (swapchainStatus < 0)
			break;

		if (lastStimulusTime == 0)
			lastInputWasActive = false;

		// Keep the run going as long as this file exists.
		struct stat s = {};
		bool doInput = ::stat(fakeStimulusTriggerPath.c_str(), &s) == 0 && (s.st_mode & S_IFMT) == S_IFREG;

		if (!doInput)
		{
			lastCandidateFrameId = 0;
			lastStimulusTime = 0;
			latencyReportFile.reset();
			state = {};
			sendGamepadState();
			continue;
		}

		if (!latencyReportFile)
		{
			// Shut up stupid GCC warning.
			char self_exe[PATH_MAX - 256] = {};
			char path[PATH_MAX];

			if (readlink("/proc/self/exe", self_exe, sizeof(self_exe) - 1) < 0)
				strcpy(self_exe, "unknown");

			const char *base = strrchr(self_exe, '/');
			if (base)
				base++;

			if (!base || *base == '\0')
				base = "unknown";

			snprintf(path, sizeof(path), "/tmp/latency-measurement-%s-", base);

			auto t = std::time(nullptr);
			std::tm gmt;
			gmtime_r(&t, &gmt);

			strftime(path + strlen(path), sizeof(path) - strlen(path),
				"%Y-%m-%d-%H-%M-%S.csv", &gmt);

			latencyReportFile.reset(fopen(path, "w"));
			if (latencyReportFile)
			{
				const char *stage;
				if (supportedStages & VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT)
					stage = "FirstPixelVisible";
				else if (supportedStages & VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT)
					stage = "FirstPixelOut";
				else if (supportedStages & VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT)
					stage = "Dequeued";
				else
					stage = "???";

				fprintf(latencyReportFile.get(), "id,stimulus,queuepresent,queuedone,%s\n", stage);
			}
		}

		if (lastCandidateFrameId == 0)
		{
			auto delta = getTimeNS() - lastStimulusTime;
			int edge = int(delta / (200 * 1000 * 1000));

			// Positive edge. Lasts for 200ms, then idles for 1400ms.
			if (edge % 8 == 0 || lastStimulusTime == 0)
			{
				if (!lastInputWasActive)
				{
					// Trigger input stimulus. Grace period of 1 second. If we didn't get a clear
					// confirmation of feedback, retry.
					lastStimulusTime = getTimeNS();
					state = {};
					state.axis_rx = rng() & 1 ? 0x7fff : -0x7fff;
					state.axis_ry = rng() & 1 ? 0x7fff : -0x7fff;

					if (hasFakeMouse)
					{
						int mouseX = rng() & 1 ? mouseImpulseRange : -mouseImpulseRange;
						int mouseY = rng() & 1 ? mouseImpulseRange : -mouseImpulseRange;
						fakeMouse.sendRelativeImpulse(mouseX, mouseY);

						if (latencyReportFile && debugOutput)
							fprintf(latencyReportFile.get(), "DEBUG: Trigger fake mouse input (%d, %d)\n", mouseX, mouseY);
					}
					else if (debugOutput && latencyReportFile)
						fprintf(latencyReportFile.get(), "DEBUG: Trigger fake gamepad input.\n");
				}

				// Right analog stick usually controls the camera.
				// TODO: Add more configurability here, custom mouse input for FPS games, etc, etc.

				sendGamepadState();
				lastInputWasActive = true;
			}
			else
			{
				// Negative edge. We didn't register the input to cause any delta.
				state = {};
				sendGamepadState();
				lastInputWasActive = false;
			}
		}
		else
		{
			// We have a confirmed MSE delta that we're waiting for present feedback on.
			// Idle the input stimulus immediately.
			state = {};
			sendGamepadState();
			lastInputWasActive = false;
		}
	}

	state = {};
	sendGamepadState();
}

static void copyErrorSignalRGB10(uint32_t *dst, const uint32_t *as, const uint32_t *bs, size_t count)
{
	for (size_t i = 0; i < count; i++)
	{
		uint32_t a = as[i];
		uint32_t b = bs[i];

		int ra, ga, ba, rb, gb, bb;

		// RGBA vs BGRA flip is not important.
		// Any mismatch will cancel itself out.
		ra = int((a >> 0) & 0x3ff);
		ga = int((a >> 10) & 0x3ff);
		ba = int((a >> 20) & 0x3ff);

		rb = int((b >> 0) & 0x3ff);
		gb = int((b >> 10) & 0x3ff);
		bb = int((b >> 20) & 0x3ff);

		int rdiff = std::abs(ra - rb);
		int gdiff = std::abs(ga - gb);
		int bdiff = std::abs(ba - bb);

		// If HDR10, clamp to something that won't sear users eyes.
		rdiff = std::min<int>(rdiff * 10, 700);
		gdiff = std::min<int>(gdiff * 10, 700);
		bdiff = std::min<int>(bdiff * 10, 700);

		dst[i] = (rdiff << 0) | (gdiff << 10) | (bdiff << 20);
	}
}

static void copyErrorSignalRGBA8(uint32_t *dst, const uint32_t *as, const uint32_t *bs, size_t count)
{
	for (size_t i = 0; i < count; i++)
	{
		uint32_t a = as[i];
		uint32_t b = bs[i];

		int ra, ga, ba, rb, gb, bb;

		// RGBA vs BGRA flip is not important.
		// Any mismatch will cancel itself out.
		ra = int((a >> 0) & 0xff);
		ga = int((a >> 8) & 0xff);
		ba = int((a >> 16) & 0xff);

		rb = int((b >> 0) & 0xff);
		gb = int((b >> 8) & 0xff);
		bb = int((b >> 16) & 0xff);

		int rdiff = std::abs(ra - rb);
		int gdiff = std::abs(ga - gb);
		int bdiff = std::abs(ba - bb);

		rdiff = std::min<int>(rdiff * 10, 255);
		gdiff = std::min<int>(gdiff * 10, 255);
		bdiff = std::min<int>(bdiff * 10, 255);

		dst[i] = (rdiff << 0) | (gdiff << 8) | (bdiff << 16);
	}
}

void Swapchain::runWorker()
{
	Work work = {};

	constexpr int NumHistoryFrames = 16;
	auto prev = std::unique_ptr<uint32_t[]>(new uint32_t[representativeWidth * representativeHeight]);
	auto current = std::unique_ptr<uint32_t[]>(new uint32_t[representativeWidth * representativeHeight]);
	double mseHistory[NumHistoryFrames] = {};
	uint64_t numFeedbacksSinceDelta = 0;
	uint64_t stimulusTime = 0;
	uint64_t lastFeedbackId = 0;

	bool debugOutput = false;
	if (const char *env = getenv("LATENCY_MEASUREMENT_DEBUG"))
		debugOutput = strcmp(env, "1") == 0;

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

		if (work.type == WorkType::WaitGPUWork)
		{
			auto &waitWork = work.u.waitWork;
			if (device->table.WaitForFences(
					device->device, 1, &images[waitWork.index].fence,
					VK_TRUE, UINT64_MAX) != VK_SUCCESS ||
				device->table.ResetFences(device->device, 1, &images[waitWork.index].fence) != VK_SUCCESS)
			{
				break;
			}

			std::swap(prev, current);
			memmove(mseHistory, mseHistory + 1, sizeof(mseHistory) - sizeof(mseHistory[0]));
			memcpy(current.get(), images[waitWork.index].buffer.mapped, representativeWidth * representativeHeight * sizeof(uint32_t));

			// Show a negative of the history buffer to make it easier to tell where the damage rect is.
			if (format == VK_FORMAT_A2B10G10R10_UNORM_PACK32)
			{
				copyErrorSignalRGB10(
					static_cast<uint32_t *>(images[waitWork.index].buffer.mapped) +
					representativeWidth * representativeHeight,
					current.get(), prev.get(), representativeWidth * representativeHeight);
			}
			else
			{
				copyErrorSignalRGBA8(
					static_cast<uint32_t *>(images[waitWork.index].buffer.mapped) +
					representativeWidth * representativeHeight,
					current.get(), prev.get(), representativeWidth * representativeHeight);
			}

			// Unblock early.
			{
				std::lock_guard<std::mutex> holder{lock};
				images[waitWork.index].busy = false;
				cond.notify_one();
			}

			auto mseFunc = format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ? &computeMSE10 : &computeMSE8;
			mseHistory[NumHistoryFrames - 1] = mseFunc(current.get(), prev.get(), representativeWidth * representativeHeight);

			// Ensure the image was stable for the last N frames before we accept a delta.
			double maxHistoryMSE = 0.0;
			for (int i = 0; i < NumHistoryFrames - 1; i++)
				maxHistoryMSE = std::max<double>(maxHistoryMSE, mseHistory[i]);

			if (debugOutput)
			{
				std::lock_guard<std::mutex> holder{fakeInputMutex};
				if (latencyReportFile)
				{
					fprintf(latencyReportFile.get(),
							"DEBUG: PresentID: %llu, MSE: %.3f (current threshold: %.3f)\n",
							static_cast<unsigned long long>(waitWork.presentId),
							mseHistory[NumHistoryFrames - 1],
							maxHistoryMSE);
				}
			}

			// Measure a meaningful difference, which we presume was caused by (synthetic) player stimulus.
			constexpr double MinimumPixelDelta = 0.5;
			if (mseHistory[NumHistoryFrames - 1] > 16.0 * maxHistoryMSE &&
				mseHistory[NumHistoryFrames - 1] > MinimumPixelDelta * MinimumPixelDelta &&
				waitWork.presentId > NumHistoryFrames && !lastCandidateFrameId)
			{
				numFeedbacksSinceDelta = 0;
				std::lock_guard<std::mutex> holder{fakeInputMutex};
				// Once input thread sees that we have an MSE candidate, stop generating input.
				lastCandidateFrameId = waitWork.presentId;
				stimulusTime = lastStimulusTime;
				fakeInputCond.notify_one();

				if (latencyReportFile && debugOutput)
				{
					fprintf(latencyReportFile.get(), "DEBUG: PresentID: %llu committing to feedback\n",
							static_cast<unsigned long long>(waitWork.presentId));
				}
			}
		}
		else if (work.type == WorkType::HandleFeedback)
		{
			auto &entry = work.u.feedbackEntry;

			if (debugOutput)
			{
				std::lock_guard<std::mutex> holder{fakeInputMutex};
				if (latencyReportFile)
					fprintf(latencyReportFile.get(), "DEBUG: PresentID: feedback received\n");
			}

			// Deal with MAILBOX/IMMEDIATE which may discard frames in favor of replaced frames that actually hit screen.
			if (entry.presentId >= lastCandidateFrameId && lastFeedbackId < lastCandidateFrameId && stimulusTime)
			{
				std::lock_guard<std::mutex> holder{fakeInputMutex};
				if (latencyReportFile)
				{
					fprintf(latencyReportFile.get(),
					        "%llu,%.6f,%.6f,%.6f,%.6f\n",
					        static_cast<unsigned long long>(entry.presentId),
					        double(stimulusTime) * 1e-9 - timeOffset,
					        double(entry.submitted) * 1e-9 - timeOffset,
					        double(entry.queueDone) * 1e-9 - timeOffset,
					        double(entry.presentDone) * 1e-9 - timeOffset);
				}
			}

			// Ensure there is a grace period where we can capture "idle" frames.
			if (numFeedbacksSinceDelta++ == 2 * NumHistoryFrames)
			{
				// Restart generating input.
				std::lock_guard<std::mutex> holder{fakeInputMutex};
				lastCandidateFrameId = 0;
				lastStimulusTime = 0;
				fakeInputCond.notify_one();
			}

			lastFeedbackId = entry.presentId;
		}
	}

	std::lock_guard<std::mutex> holder{lock};
	swapchainStatus = VK_ERROR_DEVICE_LOST;
	cond.notify_all();
}

VkResult Swapchain::submitSourceWork(VkQueue queue, uint32_t index, uint64_t presentId)
{
	VkResult result = initSourceCommands(device->queueToFamilyIndex(queue));
	if (result != VK_SUCCESS)
		return result;

	VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &images[index].cmd;

	{
		std::unique_lock<std::mutex> holder{lock};
		cond.wait(holder, [&]()
		{
			return !images[index].busy || swapchainStatus < 0;
		});

		if (swapchainStatus < 0)
			return swapchainStatus;
	}

	result = device->table.QueueSubmit(queue, 1, &submit, images[index].fence);
	if (result != VK_SUCCESS)
		return result;

	std::lock_guard<std::mutex> holder{lock};

	images[index].busy = true;

	Work work = {};
	work.type = WorkType::WaitGPUWork;
	work.u.waitWork.presentId = presentId;
	work.u.waitWork.index = index;
	workQueue.push(work);
	cond.notify_one();

	return result;
}

VkResult Swapchain::init(const VkSwapchainCreateInfoKHR *pCreateInfo, VkSwapchainKHR swapchain_)
{
	width = pCreateInfo->imageExtent.width;
	height = pCreateInfo->imageExtent.height;
	format = pCreateInfo->imageFormat;
	swapchain = swapchain_;

	timeOffset = double(getTimeNS()) * 1e-9;

	representativeWidth = std::min<uint32_t>(width, 128);
	representativeHeight = std::min<uint32_t>(height, 128);

	uint32_t count;
	device->table.GetSwapchainImagesKHR(device->device, swapchain, &count, nullptr);
	std::vector<VkImage> vkImages(count);
	device->table.GetSwapchainImagesKHR(device->device, swapchain, &count, vkImages.data());

	images.resize(count);
	for (uint32_t i = 0; i < count; i++)
	{
		VkResult res = setupSwapchainImage(pCreateInfo, vkImages[i], i);
		if (res != VK_SUCCESS)
			return res;
	}

	if (presentTiming)
	{
		auto *table = device->getTable();

		VkSwapchainTimeDomainPropertiesEXT domainProperties =
				{ VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT };
		VkTimeDomainKHR domains[32];
		uint64_t timeDomainIds[32];
		domainProperties.pTimeDomains = domains;
		domainProperties.pTimeDomainIds = timeDomainIds;
		domainProperties.timeDomainCount = 32;
		if (table->GetSwapchainTimeDomainPropertiesEXT(
			device->device, swapchain, &domainProperties, &domainCounter) >= 0)
		{
			uint32_t i;
			for (i = 0; i < domainProperties.timeDomainCount; i++)
			{
				if (domains[i] == VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT)
				{
					timeDomainId = timeDomainIds[i];
					timeDomain = domains[i];
					break;
				}
			}

			// Should only happen if the implementation is non-compliant.
			if (i == domainProperties.timeDomainCount)
				presentTiming = false;
		}
		else
		{
			// Should only happen if the implement is non-compliant.
			presentTiming = false;
		}

		if (presentTiming && table->SetSwapchainPresentTimingQueueSizeEXT(
			device->device, swapchain, FeedbackQueueSize) != VK_SUCCESS)
		{
			presentTiming = false;
		}
	}

	worker = std::thread(&Swapchain::runWorker, this);
	fakeInputThread = std::thread(&Swapchain::runFakeInputStimulus, this);
	return VK_SUCCESS;
}

void Swapchain::reportCompleteEventLocked(const FeedbackQueueEntry &entry)
{
	Work analyzeWork = {};
	analyzeWork.type = WorkType::HandleFeedback;
	analyzeWork.u.feedbackEntry = entry;
	workQueue.push(analyzeWork);
	cond.notify_one();
}

void Swapchain::updateFeedback(const VkPastPresentationTimingEXT &timing)
{
	// Use this for subsequent presents. Shouldn't change.
	timeDomain = timing.timeDomain;
	timeDomainId = timing.timeDomainId;

	uint64_t queueDone = 0;
	uint64_t presentDone = 0;
	VkPresentStageFlagsEXT presentStage = 0;

	for (uint32_t stageIndex = 0; stageIndex < timing.presentStageCount; stageIndex++)
	{
		auto &stage = timing.pPresentStages[stageIndex];
		if (stage.stage == VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT)
			queueDone = stage.time;

		if ((stage.stage & (
			VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT |
			VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT |
			VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT)) != 0 && stage.stage > presentStage)
		{
			presentDone = stage.time;
			presentStage = stage.stage;
		}
	}

	// TODO: It's wasteful to calibrate *all* the time, but w/e.
	if (presentDone != 0 && queueDone != 0)
	{
		VkSwapchainCalibratedTimestampInfoEXT queueDoneStage = { VK_STRUCTURE_TYPE_SWAPCHAIN_CALIBRATED_TIMESTAMP_INFO_EXT };
		queueDoneStage.presentStage = VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT;
		queueDoneStage.timeDomainId = timeDomainId;
		queueDoneStage.swapchain = swapchain;

		VkSwapchainCalibratedTimestampInfoEXT presentDoneStage = { VK_STRUCTURE_TYPE_SWAPCHAIN_CALIBRATED_TIMESTAMP_INFO_EXT };
		presentDoneStage.presentStage = presentStage;
		presentDoneStage.timeDomainId = timeDomainId;
		presentDoneStage.swapchain = swapchain;

		VkCalibratedTimestampInfoKHR timestampInfo[3] =
		{
			{ VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, nullptr, VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR },
			{ VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, &queueDoneStage, VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT },
			{ VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, &presentDoneStage, VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT }
		};

		auto *table = device->getTable();
		uint64_t timestamps[3];
		uint64_t maxDeviation = 0;

		if (table->GetCalibratedTimestampsKHR(device->device, 3, timestampInfo,
				timestamps, &maxDeviation) == VK_SUCCESS)
		{
			uint64_t queueDelta = timestamps[1] - queueDone;
			queueDone = timestamps[0] - queueDelta;

			uint64_t presentDelta = timestamps[2] - presentDone;
			presentDone = timestamps[0] - presentDelta;
		}
		else
		{
			presentDone = 0;
			queueDone = 0;
		}
	}

	std::lock_guard<std::mutex> holder{lock};
	FeedbackQueueEntry completeEntry = {};

	for (uint32_t i = 0; i < feedbackQueueSize; i++)
	{
		if (feedbackQueue[i].presentId == timing.presentId)
		{
			completeEntry = feedbackQueue[i];
			completeEntry.queueDone = queueDone;
			completeEntry.presentDone = presentDone;
			completeEntry.presentStage = presentStage;

			feedbackQueue[i] = feedbackQueue[--feedbackQueueSize];
			break;
		}
	}

	if (completeEntry.presentDone && completeEntry.queueDone)
		reportCompleteEventLocked(completeEntry);
}

void Swapchain::pollFeedback()
{
	if (!presentTiming)
		return;

	auto *table = device->getTable();

	VkPastPresentationTimingInfoEXT pastInfo = { VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT };
	VkPastPresentationTimingPropertiesEXT pastTimingProperties =
		{ VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT };
	pastInfo.swapchain = swapchain;

	VkPastPresentationTimingEXT pastPresentationTiming[FeedbackQueueSize] = {};
	VkPresentStageTimeEXT pastTime[FeedbackQueueSize][4];
	for (uint32_t i = 0; i < FeedbackQueueSize; i++)
	{
		auto &prop = pastPresentationTiming[i];
		prop.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT;
		prop.pPresentStages = pastTime[i];
		prop.presentStageCount = 4;
	}

	pastTimingProperties.pPresentationTimings = pastPresentationTiming;
	pastTimingProperties.presentationTimingCount = FeedbackQueueSize;

	if (table->GetPastPresentationTimingEXT(device->device, &pastInfo, &pastTimingProperties) < 0)
		return;

	for (uint32_t i = 0; i < pastTimingProperties.presentationTimingCount; i++)
		updateFeedback(pastTimingProperties.pPresentationTimings[i]);
}

Swapchain::~Swapchain()
{
	{
		std::lock_guard<std::mutex> holder{lock};
		swapchainStatus = VK_ERROR_SURFACE_LOST_KHR;
		cond.notify_one();
		fakeInputCond.notify_one();
	}

	if (worker.joinable())
		worker.join();
	if (fakeInputThread.joinable())
		fakeInputThread.join();

	for (auto &image : images)
	{
		device->table.DestroyBuffer(device->device, image.buffer.buffer, nullptr);
		device->table.FreeMemory(device->device, image.buffer.memory, nullptr);
		device->table.DestroyFence(device->device, image.fence, nullptr);
	}

	device->table.DestroyCommandPool(device->device, cmdPool.pool, nullptr);
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                                                     const VkAllocationCallbacks *pAllocator, VkInstance *pInstance)
{
	auto *chainInfo = getChainInfo(pCreateInfo, VK_LAYER_LINK_INFO);
	auto *callbackInfo = getChainInfo(pCreateInfo, VK_LOADER_DATA_CALLBACK);
	auto fpSetInstanceLoaderData = callbackInfo->u.pfnSetInstanceLoaderData;

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
	addUniqueExtension(enabledExtensions, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
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
	layer->init(*pInstance, fpGetInstanceProcAddr, fpSetInstanceLoaderData);

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

static VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(
		VkPhysicalDevice                            physicalDevice,
		const char*                                 pLayerName,
		uint32_t*                                   pPropertyCount,
		VkExtensionProperties*                      pProperties)
{
	if (pLayerName && strstr(pLayerName, "VK_LAYER_pyrofling_latency_measurement"))
	{
		*pPropertyCount = 0;
		return VK_SUCCESS;
	}

	auto *layer = getInstanceLayer(physicalDevice);

	uint32_t count = 0;
	layer->getTable()->EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, &count, nullptr);
	std::vector<VkExtensionProperties> props(count);
	layer->getTable()->EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, &count, props.data());

	// Remove any problematic extensions.
	auto itr = std::remove_if(props.begin(), props.end(), [](const VkExtensionProperties &prop) -> bool {
		return findExtension(blockedExtensions, prop.extensionName);
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

	auto tmpCreateInfo = *pCreateInfo;

	bool usesSwapchain = findExtension(pCreateInfo->ppEnabledExtensionNames,
	                                   pCreateInfo->enabledExtensionCount,
	                                   VK_KHR_SWAPCHAIN_EXTENSION_NAME);

	VkPhysicalDevicePresentTimingFeaturesEXT presentTimingFeatures =
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT };
	VkPhysicalDevicePresentId2FeaturesKHR presentID2Features =
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR };

	// Don't bother handle every possible case. It's an opt-in layer.
	if (usesSwapchain)
	{
		addUniqueExtension(enabledExtensions, supportedExts, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);

		// If these are not supported for whatever reason,
		// we will just not wrap entry points and pass through all device functions.
		if (addUniqueExtension(enabledExtensions, supportedExts, VK_EXT_PRESENT_TIMING_EXTENSION_NAME))
		{
			if (const auto *timing = findChain<VkPhysicalDevicePresentTimingFeaturesEXT>(tmpCreateInfo.pNext,
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT))
			{
				presentTimingFeatures = *timing;
			}
			else
			{
				VkPhysicalDeviceFeatures2 features2 = {
					VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &presentTimingFeatures
				};
				layer->getTable()->GetPhysicalDeviceFeatures2KHR(gpu, &features2);
				presentTimingFeatures.pNext = const_cast<void *>(tmpCreateInfo.pNext);
				tmpCreateInfo.pNext = &presentTimingFeatures;
			}
		}

		if (addUniqueExtension(enabledExtensions, supportedExts, VK_KHR_PRESENT_ID_2_EXTENSION_NAME))
		{
			if (const auto *id2 = findChain<VkPhysicalDevicePresentId2FeaturesKHR>(tmpCreateInfo.pNext,
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR))
			{
				presentID2Features = *id2;
			}
			else
			{
				VkPhysicalDeviceFeatures2 features2 = {
					VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &presentID2Features
				};
				layer->getTable()->GetPhysicalDeviceFeatures2KHR(gpu, &features2);
				presentID2Features.pNext = const_cast<void *>(tmpCreateInfo.pNext);
				tmpCreateInfo.pNext = &presentID2Features;
			}
		}

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

	presentTimingFeatures.pNext = nullptr;
	presentID2Features.pNext = nullptr;
	device->id2Features = presentID2Features;
	device->timingFeatures = presentTimingFeatures;

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

	auto tmpInfo = *pCreateInfo;
	tmpInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

	VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR };
	surfaceInfo.surface = pCreateInfo->surface;

	VkSurfaceCapabilities2KHR surfaceCaps = { VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR };
	VkPresentTimingSurfaceCapabilitiesEXT timingCaps = { VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT };
	VkSurfaceCapabilitiesPresentId2KHR id2caps = { VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR };

	if (layer->id2Features.presentId2)
	{
		id2caps.pNext = surfaceCaps.pNext;
		surfaceCaps.pNext = &id2caps;
	}

	if (layer->timingFeatures.presentTiming)
	{
		timingCaps.pNext = surfaceCaps.pNext;
		surfaceCaps.pNext = &timingCaps;
	}

	layer->getInstance()->getTable()->GetPhysicalDeviceSurfaceCapabilities2KHR(layer->gpu,
			&surfaceInfo, &surfaceCaps);

	if (timingCaps.presentTimingSupported &&
		(timingCaps.presentStageQueries & VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT) &&
	    (timingCaps.presentStageQueries & (VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT |
	                                       VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT |
	                                       VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT)))
	{
		tmpInfo.flags |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT;
	}

	if (id2caps.presentId2Supported)
		tmpInfo.flags |= VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR;

	auto vr = layer->getTable()->CreateSwapchainKHR(device, &tmpInfo, pAllocator, pSwapchain);
	if (vr != VK_SUCCESS)
		return vr;

	auto swap = std::make_unique<Swapchain>(layer);

	swap->presentTiming = (tmpInfo.flags & VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT) != 0;
	swap->supportedStages = timingCaps.presentStageQueries;

	auto res = swap->init(pCreateInfo, *pSwapchain);
	if (res != VK_SUCCESS)
	{
		layer->getTable()->DestroySwapchainKHR(device, *pSwapchain, pAllocator);
		return res;
	}

	layer->registerSwapchain(*pSwapchain, std::move(swap));
	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *pAllocator)
{
	auto *layer = getDeviceLayer(device);
	layer->unregisterSwapchain(swapchain);
	return layer->getTable()->DestroySwapchainKHR(device, swapchain, pAllocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL
QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
	auto *layer = getDeviceLayer(queue);

	// Complicated case that "never" happens in the real world. Ignore it.
	if (pPresentInfo->swapchainCount != 1)
		return layer->getTable()->QueuePresentKHR(queue, pPresentInfo);

	// If we're not using present timing actively, just forward the call as-is.
	auto *swap = layer->getSwapchain(pPresentInfo->pSwapchains[0]);
	if (!swap->presentTiming)
		return layer->getTable()->QueuePresentKHR(queue, pPresentInfo);

	swap->pollFeedback();

	VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	submitInfo.pWaitDstStageMask = &waitStage;
	submitInfo.waitSemaphoreCount = 1;

	for (uint32_t i = 0; i < pPresentInfo->waitSemaphoreCount; i++)
	{
		submitInfo.pWaitSemaphores = &pPresentInfo->pWaitSemaphores[i];
		auto res = layer->getTable()->QueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
		if (res != VK_SUCCESS)
			return res;
	}

	auto *ids = findChain<VkPresentId2KHR>(pPresentInfo->pNext, VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR);

	VkPresentTimingsInfoEXT timingsInfo = { VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT };
	VkPresentTimingInfoEXT timingInfo = { VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT };
	VkPresentId2KHR fallbackId = { VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR };
	uint64_t presentId = ids && ids->swapchainCount ? ids->pPresentIds[0] : 0;
	auto tmpInfo = *pPresentInfo;

	for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++)
	{
		if (!swap->presentTiming)
			continue;

		// Hazard: If application flips between not using IDs and using IDs, we might end up with weirdness.
		// FIXME: Could add virtualization of IDs.
		if (!presentId)
		{
			presentId = ++swap->internalPresentId;

			fallbackId.pPresentIds = &presentId;
			fallbackId.swapchainCount = 1;

			fallbackId.pNext = tmpInfo.pNext;
			tmpInfo.pNext = &fallbackId;
		}
		else
		{
			swap->internalPresentId = std::max<uint64_t>(swap->internalPresentId, presentId);
		}

		auto res = swap->submitSourceWork(queue, pPresentInfo->pImageIndices[i], presentId);
		if (res != VK_SUCCESS)
			return res;

		// It's possible that app takes control of presentId2 but doesn't use it.
		// Shouldn't happen in real world scenarios.
		if (swap->feedbackQueueSize < Swapchain::FeedbackQueueSize && presentId)
		{
			timingsInfo.swapchainCount = 1;
			timingsInfo.pTimingInfos = &timingInfo;
			timingInfo.timeDomainId = swap->timeDomainId;
			timingInfo.presentStageQueries = swap->supportedStages;

			timingsInfo.pNext = tmpInfo.pNext;
			tmpInfo.pNext = &timingsInfo;

			uint64_t submittedTime = getTimeNS();
			swap->feedbackQueue[swap->feedbackQueueSize++] = { presentId, submittedTime };
		}
	}

	submitInfo.waitSemaphoreCount = 0;
	submitInfo.signalSemaphoreCount = 1;

	for (uint32_t i = 0; i < pPresentInfo->waitSemaphoreCount; i++)
	{
		submitInfo.pSignalSemaphores = &pPresentInfo->pWaitSemaphores[i];
		auto res = layer->getTable()->QueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
		if (res != VK_SUCCESS)
			return res;
	}

	// No sync objects to key off of, just fallback to a queue wait idle for now.
	if (pPresentInfo->waitSemaphoreCount == 0)
		layer->getTable()->QueueWaitIdle(queue);

	return layer->getTable()->QueuePresentKHR(queue, &tmpInfo);
}

static VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceSupportKHR(
		VkPhysicalDevice                            physicalDevice,
		uint32_t                                    queueFamilyIndex,
		VkSurfaceKHR                                surface,
		VkBool32*                                   pSupported)
{
	auto *layer = getInstanceLayer(physicalDevice);

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

	constexpr VkQueueFlags flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
	if (queueFamilyIndex >= count || (props[queueFamilyIndex].queueFlags & flags) == 0)
	{
		*pSupported = VK_FALSE;
		return VK_SUCCESS;
	}

	return layer->getTable()->GetPhysicalDeviceSurfaceSupportKHR(physicalDevice, queueFamilyIndex, surface, pSupported);
}

static bool isSupportedFormat(const VkSurfaceFormatKHR format)
{
	if (format.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR && format.colorSpace != VK_COLOR_SPACE_HDR10_ST2084_EXT)
		return false;

	switch (format.format)
	{
	case VK_FORMAT_B8G8R8A8_UNORM:
	case VK_FORMAT_B8G8R8A8_SRGB:
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_R8G8B8A8_SRGB:
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		return true;

	default:
		return false;
	}
}

static VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceFormatsKHR(
		VkPhysicalDevice                            physicalDevice,
		VkSurfaceKHR                                surface,
		uint32_t*                                   pSurfaceFormatCount,
		VkSurfaceFormatKHR*                         pSurfaceFormats)
{
	auto *layer = getInstanceLayer(physicalDevice);
	uint32_t count = 0;
	layer->getTable()->GetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &count, nullptr);
	std::vector<VkSurfaceFormatKHR> formats(count);
	layer->getTable()->GetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &count, formats.data());

	// We need to analyze the image on CPU. Only accept most basic format for now.
	auto itr = std::remove_if(formats.begin(), formats.end(), [](const VkSurfaceFormatKHR &fmt)
	{
		return !isSupportedFormat(fmt);
	});
	formats.erase(itr, formats.end());

	if (pSurfaceFormats)
	{
		VkResult res = *pSurfaceFormatCount >= formats.size() ? VK_SUCCESS : VK_INCOMPLETE;
		*pSurfaceFormatCount = std::min<uint32_t>(*pSurfaceFormatCount, formats.size());
		memcpy(pSurfaceFormats, formats.data(), *pSurfaceFormatCount * sizeof(*pSurfaceFormats));
		return res;
	}
	else
	{
		*pSurfaceFormatCount = uint32_t(formats.size());
		return VK_SUCCESS;
	}
}

static VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceFormats2KHR(
		VkPhysicalDevice                            physicalDevice,
		const VkPhysicalDeviceSurfaceInfo2KHR*      pSurfaceInfo,
		uint32_t*                                   pSurfaceFormatCount,
		VkSurfaceFormat2KHR*                        pSurfaceFormats)
{
	auto *layer = getInstanceLayer(physicalDevice);
	uint32_t count = 0;
	layer->getTable()->GetPhysicalDeviceSurfaceFormats2KHR(physicalDevice, pSurfaceInfo, &count, nullptr);
	std::vector<VkSurfaceFormat2KHR> formats(count);
	for (auto &fmt : formats)
		fmt.sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR;
	layer->getTable()->GetPhysicalDeviceSurfaceFormats2KHR(physicalDevice, pSurfaceInfo, &count, formats.data());

	// We need to analyze the image on CPU. Only accept most basic format for now.
	auto itr = std::remove_if(formats.begin(), formats.end(), [](const VkSurfaceFormat2KHR &fmt)
	{
		return !isSupportedFormat(fmt.surfaceFormat);
	});
	formats.erase(itr, formats.end());

	if (pSurfaceFormats)
	{
		VkResult res = *pSurfaceFormatCount >= formats.size() ? VK_SUCCESS : VK_INCOMPLETE;
		*pSurfaceFormatCount = std::min<uint32_t>(*pSurfaceFormatCount, formats.size());
		// Have to good way to support this right now if it's legal.
		for (uint32_t i = 0; i < *pSurfaceFormatCount; i++)
			if (pSurfaceFormats[i].pNext)
				return VK_ERROR_UNKNOWN;
		memcpy(pSurfaceFormats, formats.data(), *pSurfaceFormatCount * sizeof(*pSurfaceFormats));
		return res;
	}
	else
	{
		*pSurfaceFormatCount = uint32_t(formats.size());
		return VK_SUCCESS;
	}
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
		{ "vkGetInstanceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(VK_LAYER_PYROFLING_LATENCY_MEASUREMENT_vkGetInstanceProcAddr) },
		{ "vkCreateDevice", reinterpret_cast<PFN_vkVoidFunction>(CreateDevice) },
		{ "vkEnumerateDeviceExtensionProperties", reinterpret_cast<PFN_vkVoidFunction>(EnumerateDeviceExtensionProperties) },
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
		F(GetPhysicalDeviceSurfaceFormats2KHR),
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
		{ "vkGetDeviceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(VK_LAYER_PYROFLING_LATENCY_MEASUREMENT_vkGetDeviceProcAddr) },
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
VK_LAYER_PYROFLING_LATENCY_MEASUREMENT_vkGetDeviceProcAddr(VkDevice device, const char *pName)
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
VK_LAYER_PYROFLING_LATENCY_MEASUREMENT_vkGetInstanceProcAddr(VkInstance instance, const char *pName)
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
VK_LAYER_PYROFLING_LATENCY_MEASUREMENT_vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct)
{
	if (pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT || pVersionStruct->loaderLayerInterfaceVersion < 2)
		return VK_ERROR_INITIALIZATION_FAILED;

	if (pVersionStruct->loaderLayerInterfaceVersion > CURRENT_LOADER_LAYER_INTERFACE_VERSION)
		pVersionStruct->loaderLayerInterfaceVersion = CURRENT_LOADER_LAYER_INTERFACE_VERSION;

	if (pVersionStruct->loaderLayerInterfaceVersion >= 2)
	{
		pVersionStruct->pfnGetInstanceProcAddr = VK_LAYER_PYROFLING_LATENCY_MEASUREMENT_vkGetInstanceProcAddr;
		pVersionStruct->pfnGetDeviceProcAddr = VK_LAYER_PYROFLING_LATENCY_MEASUREMENT_vkGetDeviceProcAddr;
		pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
	}

	return VK_SUCCESS;
}
