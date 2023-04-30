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
VK_LAYER_PYROFLING_CROSS_WSI_vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
VK_LAYER_PYROFLING_CROSS_WSI_vkGetDeviceProcAddr(VkDevice device, const char *pName);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
VK_LAYER_PYROFLING_CROSS_WSI_vkGetInstanceProcAddr(VkInstance instance, const char *pName);
}

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
	VkLayerInstanceDispatchTable table = {};
	PFN_vkGetInstanceProcAddr gpa = nullptr;
	PFN_vkSetInstanceLoaderData setInstanceLoaderData = nullptr;
	PFN_vkLayerCreateDevice layerCreateDevice = nullptr;
	PFN_vkLayerDestroyDevice layerDestroyDevice = nullptr;
	uint32_t sinkGpuQueueFamily = VK_QUEUE_FAMILY_IGNORED;
};

void Instance::init(VkInstance instance_, PFN_vkGetInstanceProcAddr gpa_, PFN_vkSetInstanceLoaderData setInstanceLoaderData_,
                    PFN_vkLayerCreateDevice layerCreateDevice_, PFN_vkLayerDestroyDevice layerDestroyDevice_)
{
	instance = instance_;
	gpa = gpa_;
	setInstanceLoaderData = setInstanceLoaderData_;
	layerCreateDevice = layerCreateDevice_;
	layerDestroyDevice = layerDestroyDevice_;
	layerInitInstanceDispatchTable(instance, &table, gpa);

	const char *env = getenv("CROSS_WSI_SINK");
	if (env)
	{
		uint32_t count = 0;
		table.EnumeratePhysicalDevices(instance, &count, nullptr);
		std::vector<VkPhysicalDevice> gpus(count);
		table.EnumeratePhysicalDevices(instance, &count, gpus.data());

		for (uint32_t i = 0; i < count; i++)
		{
			VkPhysicalDeviceProperties2 props2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
			table.GetPhysicalDeviceProperties2KHR(gpus[i], &props2);

			if (strstr(props2.properties.deviceName, env) != nullptr)
			{
				sinkGpu = gpus[i];
				break;
			}
		}
	}

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
};

#include "dispatch_wrapper.hpp"

Device::~Device()
{
	if (sinkDevice)
		instance->layerDestroyDevice(sinkDevice, nullptr, sinkTable.DestroyDevice);
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

	bool usesSwapchain = false;
	for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++)
	{
		if (strcmp(pCreateInfo->ppEnabledExtensionNames[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0)
		{
			usesSwapchain = true;
			break;
		}
	}

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

		static const char *extensions[] = {
			VK_KHR_SWAPCHAIN_EXTENSION_NAME,
			VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
		};

		createInfo.ppEnabledExtensionNames = extensions;
		createInfo.enabledExtensionCount = sizeof(extensions) / sizeof(extensions[0]);
		PFN_vkGetDeviceProcAddr gdpa = nullptr;
		if (instance->layerCreateDevice(instance->instance, instance->sinkGpu, &createInfo, nullptr, &sinkDevice,
		                                VK_LAYER_PYROFLING_CROSS_WSI_vkGetInstanceProcAddr, &gdpa) != VK_SUCCESS)
			return;

		layerInitDeviceDispatchTable(sinkDevice, &sinkTable, gdpa);
		sinkTable.GetDeviceQueue(sinkDevice, instance->sinkGpuQueueFamily, 0, &sinkQueue);
	}
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

	// If these are not supported for whatever reason,
	// we will just not wrap entry points and pass through all device functions.
	auto tmpCreateInfo = *pCreateInfo;
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

static VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator)
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
                   VkSwapchainKHR *pSwachain)
{
	auto *layer = getDeviceLayer(device);

	// Probably need to query support for this, but really ...
	auto info = *pCreateInfo;
	info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

	auto result = layer->getTable()->CreateSwapchainKHR(device, &info, pAllocator, pSwachain);
	if (result != VK_SUCCESS)
		return result;

	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *pAllocator)
{
	auto *layer = getDeviceLayer(device);
	layer->getTable()->DestroySwapchainKHR(device, swapchain, pAllocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL
QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
	auto *layer = getDeviceLayer(queue);
	auto result = layer->getTable()->QueuePresentKHR(queue, pPresentInfo);
	return result;
}

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
