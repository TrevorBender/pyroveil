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
	PFN_vkCreateDevice CreateDevice;
	PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
	PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2;
	PFN_vkGetPhysicalDeviceProperties2KHR GetPhysicalDeviceProperties2KHR;
};

// Device function pointer dispatch table
struct VkLayerDispatchTable
{
	PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
	PFN_vkDestroyDevice DestroyDevice;
	PFN_vkCreateShaderModule CreateShaderModule;
	PFN_vkDestroyShaderModule DestroyShaderModule;
	PFN_vkCreateShadersEXT CreateShadersEXT;
	PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines;
	PFN_vkCreateComputePipelines CreateComputePipelines;
	PFN_vkCreateRayTracingPipelinesKHR CreateRayTracingPipelinesKHR;
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

template <typename T>
static inline T *findChainMutable(void *pNext, VkStructureType sType)
{
	while (pNext)
	{
		auto *s = static_cast<const VkBaseOutStructure *>(pNext);
		if (s->sType == sType)
			return static_cast<T *>(pNext);

		pNext = s->pNext;
	}

	return nullptr;
}