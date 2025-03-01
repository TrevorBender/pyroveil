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
	F(CreateShaderModule);
	F(DestroyShaderModule);
	F(CreateShadersEXT);
	F(CreateGraphicsPipelines);
	F(CreateComputePipelines);
	F(CreateRayTracingPipelinesKHR);
#undef F
}

void layerInitInstanceDispatchTable(VkInstance instance, VkLayerInstanceDispatchTable *table,
                                    PFN_vkGetInstanceProcAddr gpa)
{
#define F(fun) table->fun = (PFN_vk##fun)gpa(instance, "vk"#fun)
	*table = {};
	F(DestroyInstance);
	F(EnumerateDeviceExtensionProperties);
	F(CreateDevice);
	F(GetPhysicalDeviceProperties2);
	F(GetPhysicalDeviceProperties2KHR);
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

bool findExtension(const std::vector<VkExtensionProperties> &props, const char *ext)
{
	for (auto &prop : props)
		if (strcmp(prop.extensionName, ext) == 0)
			return true;

	return false;
}

bool findExtension(const char * const *ppExtensions, uint32_t count, const char *ext)
{
	for (uint32_t i = 0; i < count; i++)
		if (strcmp(ppExtensions[i], ext) == 0)
			return true;

	return false;
}
