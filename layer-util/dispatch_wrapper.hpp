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

#include "dispatch_helper.hpp"
#include <mutex>
#include <vector>
#include <unordered_map>
#include <memory>

// Include after Instance and Device have been defined.

struct Instance;
struct Device;

// Global data structures to remap VkInstance and VkDevice to internal data structures.
static std::mutex globalLock;
static std::unordered_map<void *, std::unique_ptr<Instance>> instanceData;
static std::unordered_map<void *, std::unique_ptr<Device>> deviceData;

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

template <typename DeviceDispatchable>
static inline Device *getDeviceDispachableLayer(DeviceDispatchable d)
{
	// Need to hold a lock while querying the global hashmap, but not after it.
	void *key = getDispatchKey(d);
	std::lock_guard<std::mutex> holder{ globalLock };
	return getLayerData(key, deviceData);
}

static inline Device *getDeviceLayer(VkDevice device)
{
	return getDeviceDispachableLayer(device);
}

static inline Device *getDeviceLayer(VkQueue queue)
{
	return getDeviceDispachableLayer(queue);
}

template <typename InstanceDispatchable>
static inline Instance *getInstanceDispatchableLayer(InstanceDispatchable d)
{
	auto *key = getDispatchKey(d);
	std::lock_guard<std::mutex> holder{ globalLock };
	return getLayerData(key, instanceData);
}

static inline Instance *getInstanceLayer(VkInstance instance)
{
	return getInstanceDispatchableLayer(instance);
}

static inline Instance *getInstanceLayer(VkPhysicalDevice gpu)
{
	return getInstanceDispatchableLayer(gpu);
}