/* Copyright (c) 2025 Hans-Kristian Arntzen for Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#define RAPIDJSON_HAS_STDSTRING 1
#define RAPIDJSON_PARSE_DEFAULT_FLAGS kParseIterativeFlag
#define SPV_ENABLE_UTILITY_CODE
#include "rapidjson/document.h"

#include "dispatch_helper.hpp"
#include "path_utils.hpp"
#include "fossilize_hasher.hpp"
#include "compiler.hpp"
#include "spirv_glsl.hpp"
#include <string>
#include <algorithm>
#include <vector>
#include <mutex>
#include <unordered_set>
#include <stdlib.h>

extern "C"
{
VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
VK_LAYER_PYROVEIL_vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
VK_LAYER_PYROVEIL_vkGetDeviceProcAddr(VkDevice device, const char *pName);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
VK_LAYER_PYROVEIL_vkGetInstanceProcAddr(VkInstance instance, const char *pName);
}

struct ScratchAllocator
{
	void *copyRaw(const void *data, size_t size);
	void *allocBytes(size_t size);

	template <typename T>
	T *copy(const T *v, size_t count)
	{
		return static_cast<T *>(copyRaw(static_cast<const void *>(v), count * sizeof(*v)));
	}

	struct Block
	{
		uint8_t *data;
		size_t offset;
		size_t size;
	};

	std::aligned_storage<1024, alignof(std::max_align_t)> baseBlock;
	Block block = { reinterpret_cast<uint8_t *>(&baseBlock), 0, sizeof(baseBlock) };

	struct MallocDeleter { void operator()(void *ptr) { ::free(ptr); } };
	std::vector<std::unique_ptr<void, MallocDeleter>> blocks;
};

void *ScratchAllocator::allocBytes(size_t size)
{
	size_t offset = (block.offset + alignof(std::max_align_t) - 1) & ~(alignof(std::max_align_t) - 1);

	if (offset + size < block.size)
	{
		void *copyData = block.data + offset;
		block.offset = offset + size;
		return copyData;
	}
	else
	{
		auto allocSize = std::max<size_t>(4096, size);
		blocks.emplace_back(::malloc(allocSize));
		block = { static_cast<uint8_t *>(blocks.back().get()), 0, allocSize };
		block.offset = size;
		return block.data;
	}
}

void *ScratchAllocator::copyRaw(const void *data, size_t size)
{
	void *ptr = allocBytes(size);
	if (ptr)
		memcpy(ptr, data, size);
	return ptr;
}

struct Action
{
	Hash hash = 0;
	bool glslRoundtrip = false;
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

	VkInstance instance = VK_NULL_HANDLE;
	VkLayerInstanceDispatchTable table = {};
	PFN_vkGetInstanceProcAddr gpa = nullptr;
	std::string applicationName;
	std::string engineName;
	bool active = false;

	void parseConfig(const rapidjson::Document &doc);

	struct Match
	{
		Hash fossilizeModuleHash = 0;
		Hash fossilizeModuleHashLo = 0;
		Hash fossilizeModuleHashHi = 0;
		std::string opStringSearch;
		spv::ExecutionModel spirvExecutionModel = spv::ExecutionModelMax;
		Action action;
	};
	std::vector<Match> globalMatches;
	std::vector<std::string> disabledExtensions;
	std::string roundtripCachePath;
	std::string configPath;
};

void Instance::parseConfig(const rapidjson::Document &doc)
{
	if (!doc.HasMember("version") || !doc["version"].IsInt() || doc["version"].GetUint() != 2)
	{
		fprintf(stderr, "pyroveil: Unexpected version.\n");
		return;
	}

	if (!doc.HasMember("type") || !doc["type"].IsString() || std::string(doc["type"].GetString()) != "pyroveil")
	{
		fprintf(stderr, "pyroveil: Unexpected type field.\n");
		return;
	}

	if (!doc.HasMember("matches"))
		return;

	if (doc.HasMember("disabledExtensions"))
	{
		auto &exts = doc["disabledExtensions"];
		for (auto itr = exts.Begin(); itr != exts.End(); ++itr)
			disabledExtensions.emplace_back(itr->GetString());
	}

	active = true;

	auto &matches = doc["matches"];

	for (auto itr = matches.Begin(); itr != matches.End(); ++itr)
	{
		auto &match = *itr;

		Match m;

		if (match.HasMember("fossilizeModuleHash"))
		{
			auto &v = match["fossilizeModuleHash"];
			if (v.IsString())
			{
				m.fossilizeModuleHash = strtoull(v.GetString(), nullptr, 16);
				fprintf(stderr, "pyroveil: Adding match for fossilizeModuleHash: %016llx.\n", static_cast<unsigned long long>(m.fossilizeModuleHash));
			}
		}

		if (match.HasMember("fossilizeModuleHashRange"))
		{
			auto &v = match["fossilizeModuleHashRange"];
			if (v.IsArray() && v.Size() == 2 && v[0].IsString() && v[1].IsString())
			{
				m.fossilizeModuleHashLo = strtoull(v[0].GetString(), nullptr, 16);
				m.fossilizeModuleHashHi = strtoull(v[1].GetString(), nullptr, 16);
				fprintf(stderr, "pyroveil: Adding match for fossilizeModuleHash range [%016llx, %016llx].\n",
				        static_cast<unsigned long long>(m.fossilizeModuleHashLo),
				        static_cast<unsigned long long>(m.fossilizeModuleHashHi));
			}
		}

		if (match.HasMember("opStringSearch"))
		{
			auto &v = match["opStringSearch"];
			if (v.IsString())
			{
				m.opStringSearch = v.GetString();
				fprintf(stderr, "pyroveil: Adding match for OpString: %s.\n", m.opStringSearch.c_str());
			}
		}

		if (match.HasMember("spirvExecutionModel"))
		{
			auto &v = match["spirvExecutionModel"];
			if (v.IsUint())
			{
				m.spirvExecutionModel = spv::ExecutionModel(v.GetUint());
				fprintf(stderr, "pyroveil: Adding match for spirvExecutionModel: %u (%s).\n",
				        m.spirvExecutionModel, spv::ExecutionModelToString(m.spirvExecutionModel));
			}
		}

		if (match.HasMember("action"))
		{
			auto &v = match["action"];
			if (v.HasMember("glsl-roundtrip"))
				m.action.glslRoundtrip = v["glsl-roundtrip"].GetBool();
			if (m.action.glslRoundtrip)
				fprintf(stderr, "pyroveil: Adding GLSL roundtrip via SPIRV-Cross for match.\n");
		}

		globalMatches.push_back(std::move(m));
	}

	if (doc.HasMember("roundtripCache"))
	{
		auto &v = doc["roundtripCache"];
		if (v.IsString())
		{
			roundtripCachePath = Path::relpath(configPath, v.GetString());
			fprintf(stderr, "pyroveil: Configured roundtripCachePath to \"%s\".\n", roundtripCachePath.c_str());
		}
	}
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

	const char *env = getenv("PYROVEIL_CONFIG");
	if (!env)
		env = "pyroveil.json";

	struct FileDeleter { void operator()(FILE *file) { if (file) fclose(file); } };
	std::unique_ptr<FILE, FileDeleter> file(fopen(env, "rb"));

	if (file)
	{
		configPath = env;

		fprintf(stderr, "pyroveil: Found config in %s!\n", env);
		fseek(file.get(), 0, SEEK_END);
		size_t len = ftell(file.get());
		rewind(file.get());
		std::string buf;
		buf.resize(len);
		if (fread(&buf[0], 1, len, file.get()) != len)
		{
			fprintf(stderr, "pyroveil: Failed to read config.\n");
			return;
		}

		rapidjson::Document doc;
		rapidjson::ParseResult res = doc.Parse(buf);

		if (!res)
		{
			fprintf(stderr, "pyroveil: JSON parse failed: %d\n", res.Code());
			return;
		}

		parseConfig(doc);
	}
	else
	{
		fprintf(stderr, "pyroveil: Could not find config in %s. Disabling hooking.\n", env);
	}
}

static const struct
{
	VkStructureType sType;
	size_t size;
} structSizes[] = {
	{ VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, sizeof(VkDebugUtilsObjectNameInfoEXT) },
	{ VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO, sizeof(VkPipelineRobustnessCreateInfo) },
	{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT, sizeof(VkPipelineShaderStageModuleIdentifierCreateInfoEXT) },
	{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, sizeof(VkPipelineShaderStageRequiredSubgroupSizeCreateInfo) },
	{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, sizeof(VkShaderModuleCreateInfo) },
	{ VK_STRUCTURE_TYPE_SHADER_MODULE_VALIDATION_CACHE_CREATE_INFO_EXT, sizeof(VkShaderModuleValidationCacheCreateInfoEXT) },
};

static const void *copyPnextChain(const void *pnext, ScratchAllocator &alloc)
{
	const VkBaseInStructure *newPnext = nullptr;

	while (pnext)
	{
		auto &sin = *static_cast<const VkBaseInStructure *>(pnext);
		VkBaseInStructure *node = nullptr;
		for (auto &s : structSizes)
		{
			if (s.sType == sin.sType)
			{
				node = static_cast<VkBaseInStructure *>(alloc.copyRaw(pnext, s.size));
				break;
			}
		}

		if (node)
		{
			node->pNext = newPnext;
			newPnext = node;
			pnext = sin.pNext;
		}
		else
			return nullptr;
	}

	return newPnext;
}

static std::string extractString(const uint32_t *pCode, uint32_t count)
{
	std::string ret;
	for (uint32_t i = 0; i < count; i++)
	{
		uint32_t w = pCode[i];

		for (uint32_t j = 0; j < 4; j++, w >>= 8)
		{
			char c = char(w & 0xff);
			if (c == '\0')
				return ret;
			ret += c;
		}
	}

	return ret;
}

static Hash computeHashShaderModule(const VkShaderModuleCreateInfo &createInfo)
{
	// Match Fossilize hash to make it easier to mess around.
	Hasher h;
	h.data(createInfo.pCode, createInfo.codeSize);
	h.u32(createInfo.flags);
	return h.get();
}

struct Device
{
	void init(VkPhysicalDevice gpu_, VkDevice device_, Instance *instance_, PFN_vkGetDeviceProcAddr gpa);

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

	Action checkOverrideShader(const VkShaderModuleCreateInfo &createInfo, bool knowsEntryPoint,
	                           spv::ExecutionModel *model, uint32_t *spirvVersion) const;
	bool overrideShader(VkShaderModuleCreateInfo &createInfo,
	                    const char *pName, VkShaderStageFlagBits stage,
	                    ScratchAllocator &alloc) const;
	void overrideStage(VkPipelineShaderStageCreateInfo *stageInfo, ScratchAllocator &alloc) const;
	bool overrideShaderFromCache(Hash hash, VkShaderModuleCreateInfo &createInfo,
	                             const char *pName, VkShaderStageFlagBits stage,
	                             ScratchAllocator &alloc) const;
	void placeOverrideShaderInCache(Hash hash, const VkShaderModuleCreateInfo &createInfo,
	                                const char *pName, VkShaderStageFlagBits stage) const;

	VkPhysicalDevice gpu = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	Instance *instance = nullptr;
	VkLayerDispatchTable table = {};

	mutable std::mutex lock;
	std::unordered_set<VkShaderModule> overriddenModules;
};

Action Device::checkOverrideShader(const VkShaderModuleCreateInfo &createInfo, bool knowsEntryPoint,
                                   spv::ExecutionModel *model, uint32_t *spirvVersion) const
{
	uint32_t codeSize = createInfo.codeSize / sizeof(uint32_t);
	const auto *data = createInfo.pCode;
	uint32_t numEntryPoints = 0;
	uint32_t offset = 5;

	Action action;
	action.hash = computeHashShaderModule(createInfo);

	for (auto &match : instance->globalMatches)
	{
		if (match.fossilizeModuleHash && match.fossilizeModuleHash == action.hash)
		{
			action.glslRoundtrip = action.glslRoundtrip || match.action.glslRoundtrip;
			fprintf(stderr, "pyroveil: Found match for fossilizeModuleHash: %016llx.\n",
			        static_cast<unsigned long long>(action.hash));
		}

		if ((match.fossilizeModuleHashLo || match.fossilizeModuleHashHi) &&
		    action.hash >= match.fossilizeModuleHashLo &&
		    action.hash <= match.fossilizeModuleHashHi)
		{
			action.glslRoundtrip = action.glslRoundtrip || match.action.glslRoundtrip;
			fprintf(stderr, "pyroveil: Found ranged match for fossilizeModuleHash: %016llx.\n",
			        static_cast<unsigned long long>(action.hash));
		}
	}

	if (codeSize >= 2)
		*spirvVersion = data[1];

	while (offset < codeSize)
	{
		auto op = static_cast<spv::Op>(data[offset] & 0xffff);
		uint32_t count = (data[offset] >> 16) & 0xffff;

		if (offset + count > codeSize)
			return {};

		if (op == spv::OpFunction)
		{
			// We're now declaring code, so just stop parsing, there cannot be any capability ops after this.
			break;
		}
		else if (op == spv::OpString && count > 2)
		{
			auto str = extractString(data + offset + 2, count - 2);
			for (auto &match : instance->globalMatches)
			{
				if (!match.opStringSearch.empty() && str.find(match.opStringSearch) != std::string::npos)
				{
					action.glslRoundtrip = action.glslRoundtrip || match.action.glslRoundtrip;
					fprintf(stderr, "pyroveil: Found match for opStringSearch: \"%s\" in %016llx.\n",
					        str.c_str(), static_cast<unsigned long long>(action.hash));
				}
			}
		}
		else if (op == spv::OpEntryPoint)
		{
			numEntryPoints++;
			*model = static_cast<spv::ExecutionModel>(data[offset + 1]);
			for (auto &match : instance->globalMatches)
			{
				if (*model == match.spirvExecutionModel)
				{
					action.glslRoundtrip = action.glslRoundtrip || match.action.glslRoundtrip;
					fprintf(stderr, "pyroveil: Found match for execution model in %016llx.\n", static_cast<unsigned long long>(action.hash));
				}
			}
		}

		offset += count;
	}

	// We cannot deal with multiple entry points for plain shader module creation when we don't know the stage.
	// It's implementable, but we don't care.
	if (numEntryPoints > 1 && !knowsEntryPoint)
		action.glslRoundtrip = false;

	return action;
}

static std::string generateCachePath(const std::string &cachePath, Hash hash, const char *pName, VkShaderStageFlagBits stage)
{
	char hashStr[17];
	sprintf(hashStr, "%016llx", static_cast<unsigned long long>(hash));
	std::string path = cachePath + "/" + hashStr;

	if (pName)
	{
		path += ".";
		path += pName;
		if (stage != 0)
		{
			path += ".";
			path += std::to_string(stage);
		}
	}

	path += ".spv";

	return path;
}

bool Device::overrideShaderFromCache(Hash hash, VkShaderModuleCreateInfo &createInfo, const char *pName,
                                     VkShaderStageFlagBits stage, ScratchAllocator &alloc) const
{
	auto path = generateCachePath(instance->roundtripCachePath, hash, pName, stage);

	FILE *file = fopen(path.c_str(), "rb");
	if (!file)
		return false;

	fseek(file, 0, SEEK_END);
	size_t len = ftell(file);
	rewind(file);

	auto *pCode = static_cast<uint32_t *>(alloc.allocBytes(len));
	bool ret = false;

	if (fread(pCode, 1, len, file) == len)
	{
		ret = true;
		createInfo.pCode = pCode;
		createInfo.codeSize = len;
		fprintf(stderr, "pyroveil: Pulled %s from roundtrip cache.\n", path.c_str());
	}

	fclose(file);
	return ret;
}

void Device::placeOverrideShaderInCache(Hash hash, const VkShaderModuleCreateInfo &createInfo, const char *pName,
                                        VkShaderStageFlagBits stage) const
{
	auto path = generateCachePath(instance->roundtripCachePath, hash, pName, stage);
	auto pathTmp = path + ".tmp";

	// This can happen concurrently, so need atomic rename technique.
	FILE *file = fopen(pathTmp.c_str(), "wbx");
	if (!file)
		return;

	bool success = fwrite(createInfo.pCode, 1, createInfo.codeSize, file) == createInfo.codeSize;
	fclose(file);

	if (!success)
	{
		fprintf(stderr, "pyroveil: Failed to write complete file to %s.\n", pathTmp.c_str());
		remove(pathTmp.c_str());
		return;
	}

	if (rename(pathTmp.c_str(), path.c_str()) != 0)
		fprintf(stderr, "pyroveil: Failed to rename file from %s to %s.\n", pathTmp.c_str(), path.c_str());

	fprintf(stderr, "pyroveil: Successfully placed %s in cache.\n", path.c_str());
}

bool Device::overrideShader(VkShaderModuleCreateInfo &createInfo,
                            const char *entry, VkShaderStageFlagBits stage,
                            ScratchAllocator &alloc) const
{
	auto model = spv::ExecutionModelMax;
	uint32_t spirvVersion = 0;

	auto action = checkOverrideShader(createInfo, !entry, &model, &spirvVersion);
	if (!action.glslRoundtrip)
		return false;

	if (!instance->roundtripCachePath.empty())
		if (overrideShaderFromCache(action.hash, createInfo, entry, stage, alloc))
			return true;

	std::string glsl;

	if (!instance->roundtripCachePath.empty())
		placeOverrideShaderInCache(action.hash, createInfo, "orig", VkShaderStageFlagBits(0));

	try
	{
		spirv_cross::CompilerGLSL compiler(createInfo.pCode, createInfo.codeSize / sizeof(uint32_t));
		spirv_cross::CompilerGLSL::Options opts;
		opts.version = 460;
		opts.es = false;
		opts.vulkan_semantics = true;

		const auto stageToModel = [](VkShaderStageFlagBits moduleStage) {
			switch (moduleStage)
			{
			case VK_SHADER_STAGE_VERTEX_BIT: return spv::ExecutionModelVertex;
			case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: return spv::ExecutionModelTessellationControl;
			case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return spv::ExecutionModelTessellationEvaluation;
			case VK_SHADER_STAGE_GEOMETRY_BIT: return spv::ExecutionModelGeometry;
			case VK_SHADER_STAGE_FRAGMENT_BIT: return spv::ExecutionModelFragment;
			case VK_SHADER_STAGE_COMPUTE_BIT: return spv::ExecutionModelGLCompute;
			case VK_SHADER_STAGE_MESH_BIT_EXT: return spv::ExecutionModelMeshEXT;
			case VK_SHADER_STAGE_TASK_BIT_EXT: return spv::ExecutionModelTaskEXT;
			case VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR: return spv::ExecutionModelClosestHitKHR;
			case VK_SHADER_STAGE_ANY_HIT_BIT_KHR: return spv::ExecutionModelAnyHitKHR;
			case VK_SHADER_STAGE_MISS_BIT_KHR: return spv::ExecutionModelMissKHR;
			case VK_SHADER_STAGE_INTERSECTION_BIT_KHR: return spv::ExecutionModelIntersectionKHR;
			case VK_SHADER_STAGE_RAYGEN_BIT_KHR: return spv::ExecutionModelRayGenerationKHR;
			case VK_SHADER_STAGE_CALLABLE_BIT_KHR: return spv::ExecutionModelCallableKHR;
			default: return spv::ExecutionModelMax;
			}
		};

		if (entry)
		{
			model = stageToModel(stage);
			compiler.set_entry_point(entry, model);
		}

		compiler.set_common_options(opts);
		glsl = compiler.compile();
	}
	catch (const std::exception &e)
	{
		fprintf(stderr, "pyroveil: SPIRV-Cross threw error: %s.\n", e.what());
		return false;
	}

	auto spirv = compileToSpirv(generateCachePath(instance->roundtripCachePath, action.hash, entry, stage), glsl, model, spirvVersion);
	if (!spirv.empty())
	{
		createInfo.pCode = alloc.copy(spirv.data(), spirv.size());
		createInfo.codeSize = spirv.size() * sizeof(uint32_t);
		if (!instance->roundtripCachePath.empty())
			placeOverrideShaderInCache(action.hash, createInfo, entry, stage);
		return true;
	}
	else
	{
		fprintf(stderr, "pyroveil: Failed to roundtrip shader.\n");
		return false;
	}
}

void Device::overrideStage(VkPipelineShaderStageCreateInfo *stageInfo, ScratchAllocator &alloc) const
{
	auto *moduleCreateInfo = findChain<VkShaderModuleCreateInfo>(stageInfo->pNext, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
	if (moduleCreateInfo && moduleCreateInfo->codeSize)
	{
		auto replaced = *moduleCreateInfo;
		if (overrideShader(replaced, stageInfo->pName, stageInfo->stage, alloc))
		{
			const void *pnext = copyPnextChain(stageInfo->pNext, alloc);
			if (!pnext)
			{
				fprintf(stderr, "pyroveil: Failed to copy pNext chain. Cannot override.\n");
				return;
			}

			stageInfo->pNext = pnext;
			stageInfo->pName = "main";
			moduleCreateInfo = findChain<VkShaderModuleCreateInfo>(pnext, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
			auto &mut = const_cast<VkShaderModuleCreateInfo &>(*moduleCreateInfo);
			mut.pCode = replaced.pCode;
			mut.codeSize = replaced.codeSize;
		}
	}
	else if (stageInfo->module)
	{
		std::lock_guard<std::mutex> holder{lock};
		if (overriddenModules.count(stageInfo->module))
			stageInfo->pName = "main";
	}
}

#include "dispatch_wrapper.hpp"

void Device::init(VkPhysicalDevice gpu_, VkDevice device_, Instance *instance_, PFN_vkGetDeviceProcAddr gpa)
{
	gpu = gpu_;
	device = device_;
	instance = instance_;
	layerInitDeviceDispatchTable(device, &table, gpa);
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                                                     const VkAllocationCallbacks *pAllocator, VkInstance *pInstance)
{
	auto *chainInfo = getChainInfo(pCreateInfo, VK_LAYER_LINK_INFO);

	auto fpGetInstanceProcAddr = chainInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	auto fpCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(fpGetInstanceProcAddr(nullptr, "vkCreateInstance"));
	if (!fpCreateInstance)
		return VK_ERROR_INITIALIZATION_FAILED;

	chainInfo->u.pLayerInfo = chainInfo->u.pLayerInfo->pNext;
	auto res = fpCreateInstance(pCreateInfo, pAllocator, pInstance);
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

static VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice gpu, const VkDeviceCreateInfo *pCreateInfo,
                                                   const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
	auto *layer = getInstanceLayer(gpu);
	auto *chainInfo = getChainInfo(pCreateInfo, VK_LAYER_LINK_INFO);

	auto fpGetDeviceProcAddr = chainInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
	auto fpCreateDevice = layer->getTable()->CreateDevice;

	// Advance the link info for the next element on the chain
	chainInfo->u.pLayerInfo = chainInfo->u.pLayerInfo->pNext;

	auto res = fpCreateDevice(gpu, pCreateInfo, pAllocator, pDevice);
	if (res != VK_SUCCESS)
		return res;

	{
		std::lock_guard<std::mutex> holder{globalLock};
		auto *device = createLayerData(getDispatchKey(*pDevice), deviceData);
		device->init(gpu, *pDevice, layer, fpGetDeviceProcAddr);
	}

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

static VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(
	VkDevice device,
	VkPipelineCache pipelineCache,
	uint32_t createInfoCount,
	const VkGraphicsPipelineCreateInfo *pCreateInfos,
	const VkAllocationCallbacks *pAllocator,
	VkPipeline *pPipelines)
{
	auto *layer = getDeviceLayer(device);

	ScratchAllocator scratch;
	auto *createInfos = scratch.copy(pCreateInfos, createInfoCount);

	for (uint32_t i = 0; i < createInfoCount; i++)
	{
		createInfos[i].pStages = scratch.copy(createInfos[i].pStages, createInfos[i].stageCount);
		for (uint32_t j = 0; j < createInfos[i].stageCount; j++)
			layer->overrideStage(const_cast<VkPipelineShaderStageCreateInfo *>(&createInfos[i].pStages[j]), scratch);
	}

	return layer->getTable()->CreateGraphicsPipelines(device, pipelineCache,
	                                                  createInfoCount, createInfos, pAllocator,
	                                                  pPipelines);
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateComputePipelines(
	VkDevice device,
	VkPipelineCache pipelineCache,
	uint32_t createInfoCount,
	const VkComputePipelineCreateInfo *pCreateInfos,
	const VkAllocationCallbacks *pAllocator,
	VkPipeline *pPipelines)
{
	auto *layer = getDeviceLayer(device);

	ScratchAllocator scratch;
	auto *createInfos = scratch.copy(pCreateInfos, createInfoCount);

	for (uint32_t i = 0; i < createInfoCount; i++)
		layer->overrideStage(&createInfos[i].stage, scratch);

	return layer->getTable()->CreateComputePipelines(device, pipelineCache,
	                                                 createInfoCount, createInfos, pAllocator,
	                                                 pPipelines);
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateRayTracingPipelinesKHR(
	VkDevice device,
	VkDeferredOperationKHR deferredOperation,
	VkPipelineCache pipelineCache,
	uint32_t createInfoCount,
	const VkRayTracingPipelineCreateInfoKHR *pCreateInfos,
	const VkAllocationCallbacks *pAllocator,
	VkPipeline *pPipelines)
{
	auto *layer = getDeviceLayer(device);

	ScratchAllocator scratch;
	auto *createInfos = scratch.copy(pCreateInfos, createInfoCount);

	for (uint32_t i = 0; i < createInfoCount; i++)
	{
		createInfos[i].pStages = scratch.copy(createInfos[i].pStages, createInfos[i].stageCount);
		for (uint32_t j = 0; j < createInfos[i].stageCount; j++)
			layer->overrideStage(const_cast<VkPipelineShaderStageCreateInfo *>(&createInfos[i].pStages[j]), scratch);
	}

	return layer->getTable()->CreateRayTracingPipelinesKHR(device, deferredOperation, pipelineCache,
	                                                       createInfoCount, pCreateInfos, pAllocator,
	                                                       pPipelines);
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateShaderModule(
	VkDevice device,
	const VkShaderModuleCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkShaderModule *pShaderModule)
{
	auto *layer = getDeviceLayer(device);

	ScratchAllocator scratch;
	bool overrides = false;
	auto tmpCreateInfo = *pCreateInfo;
	if (layer->overrideShader(tmpCreateInfo, nullptr, VK_SHADER_STAGE_ALL, scratch))
		overrides = true;

	VkResult vr = layer->getTable()->CreateShaderModule(device, &tmpCreateInfo, pAllocator, pShaderModule);
	if (vr != VK_SUCCESS)
		return vr;

	if (overrides)
	{
		std::lock_guard<std::mutex> holder{layer->lock};
		layer->overriddenModules.insert(*pShaderModule);
	}

	return vr;
}

static VKAPI_ATTR void VKAPI_CALL DestroyShaderModule(
		VkDevice device,
		VkShaderModule shaderModule,
		const VkAllocationCallbacks *pAllocator)
{
	auto *layer = getDeviceLayer(device);

	{
		std::lock_guard<std::mutex> holder{layer->lock};
		layer->overriddenModules.erase(shaderModule);
	}

	layer->getTable()->DestroyShaderModule(device, shaderModule, pAllocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateShadersEXT(
	VkDevice device,
	uint32_t createInfoCount,
	const VkShaderCreateInfoEXT *pCreateInfos,
	const VkAllocationCallbacks *pAllocator,
	VkShaderEXT *pShaders)
{
	auto *layer = getDeviceLayer(device);

	ScratchAllocator scratch;
	auto *shaders = scratch.copy(pCreateInfos, createInfoCount);
	for (uint32_t i = 0; i < createInfoCount; i++)
	{
		VkShaderModuleCreateInfo info = {
			VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
			shaders[i].codeSize, static_cast<const uint32_t *>(shaders[i].pCode),
		};

		if (layer->overrideShader(info, shaders[i].pName, shaders[i].stage, scratch))
		{
			shaders[i].pCode = info.pCode;
			shaders[i].codeSize = info.codeSize;
			shaders[i].pName = "main";
		}
	}

	return layer->getTable()->CreateShadersEXT(device, createInfoCount, shaders, pAllocator, pShaders);
}

static VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(
	VkPhysicalDevice physicalDevice, const char *pLayerName, uint32_t *pPropertyCount, VkExtensionProperties *pProperties)
{
	auto *layer = getInstanceLayer(physicalDevice);
	uint32_t count = 0;
	VkResult vr;

	vr = layer->getTable()->EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, &count, nullptr);
	if (vr)
		return vr;
	std::vector<VkExtensionProperties> props(count);
	vr = layer->getTable()->EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, &count, props.data());
	if (vr)
		return vr;

	// Filter out extensions we cannot deal with yet in SPIRV-Cross.
	auto itr = std::remove_if(props.begin(), props.end(), [layer, pProperties](const VkExtensionProperties &ext) {
		for (auto &disabledExt : layer->disabledExtensions)
		{
			if (disabledExt == ext.extensionName)
			{
				// Only log once.
				if (pProperties)
					fprintf(stderr, "pyroveil: Disabling extension %s.\n", ext.extensionName);
				return true;
			}
		}
		return false;
	});
	props.erase(itr, props.end());

	if (!pProperties)
	{
		*pPropertyCount = uint32_t(props.size());
		vr = VK_SUCCESS;
	}
	else
	{
		vr = *pPropertyCount < props.size() ? VK_INCOMPLETE : VK_SUCCESS;
		auto to_copy = std::min<uint32_t>(*pPropertyCount, props.size());
		std::copy(props.begin(), props.begin() + to_copy, pProperties);
	}

	return vr;
}

static void adjustProps2(VkPhysicalDeviceProperties2 *pProperties)
{
	// Override the module identifier algorithm to make sure we get the chance to replace shaders.
	auto *ident = findChainMutable<VkPhysicalDeviceShaderModuleIdentifierPropertiesEXT>(
			pProperties->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_PROPERTIES_EXT);

	if (ident)
	{
		for (auto &v : ident->shaderModuleIdentifierAlgorithmUUID)
			v ^= 0xaa;
	}
}

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties2(
		VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2 *pProperties)
{
	auto *layer = getInstanceLayer(physicalDevice);
	layer->getTable()->GetPhysicalDeviceProperties2(physicalDevice, pProperties);
	adjustProps2(pProperties);
}

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties2KHR(
		VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2 *pProperties)
{
	auto *layer = getInstanceLayer(physicalDevice);
	layer->getTable()->GetPhysicalDeviceProperties2KHR(physicalDevice, pProperties);
	adjustProps2(pProperties);
}

static PFN_vkVoidFunction interceptCoreInstanceCommand(const char *pName)
{
	static const struct
	{
		const char *name;
		PFN_vkVoidFunction proc;
		bool forceActive;
	} coreInstanceCommands[] = {
		{ "vkCreateInstance", reinterpret_cast<PFN_vkVoidFunction>(CreateInstance) },
		{ "vkDestroyInstance", reinterpret_cast<PFN_vkVoidFunction>(DestroyInstance) },
		{ "vkGetInstanceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(VK_LAYER_PYROVEIL_vkGetInstanceProcAddr) },
		{ "vkCreateDevice", reinterpret_cast<PFN_vkVoidFunction>(CreateDevice) },
		{ "vkEnumerateDeviceExtensionProperties", reinterpret_cast<PFN_vkVoidFunction>(EnumerateDeviceExtensionProperties) },
		{ "vkGetPhysicalDeviceProperties2", reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceProperties2) },
		{ "vkGetPhysicalDeviceProperties2KHR", reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceProperties2KHR) },
	};

	for (auto &cmd : coreInstanceCommands)
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
		{ "vkGetDeviceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(VK_LAYER_PYROVEIL_vkGetDeviceProcAddr) },
		{ "vkDestroyDevice", reinterpret_cast<PFN_vkVoidFunction>(DestroyDevice) },
		{ "vkCreateShaderModule", reinterpret_cast<PFN_vkVoidFunction>(CreateShaderModule) },
		{ "vkDestroyShaderModule", reinterpret_cast<PFN_vkVoidFunction>(DestroyShaderModule) },
		{ "vkCreateShadersEXT", reinterpret_cast<PFN_vkVoidFunction>(CreateShadersEXT) },
		{ "vkCreateGraphicsPipelines", reinterpret_cast<PFN_vkVoidFunction>(CreateGraphicsPipelines) },
		{ "vkCreateComputePipelines", reinterpret_cast<PFN_vkVoidFunction>(CreateComputePipelines) },
		{ "vkCreateRayTracingPipelinesKHR", reinterpret_cast<PFN_vkVoidFunction>(CreateRayTracingPipelinesKHR) },
	};

	for (auto &cmd : coreDeviceCommands)
		if (strcmp(cmd.name, pName) == 0)
			return cmd.proc;

	return nullptr;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
VK_LAYER_PYROVEIL_vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
	Device *layer;
	{
		std::lock_guard<std::mutex> holder{globalLock};
		layer = getLayerData(getDispatchKey(device), deviceData);
	}

	auto proc = layer->getTable()->GetDeviceProcAddr(device, pName);

	// If we're not wrapping we need to ensure the device is destroyed as expected, but otherwise, nothing else.
	if (!layer->getInstance()->active)
	{
		if (strcmp(pName, "vkDestroyDevice") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(DestroyDevice);
		else
			return proc;
	}

	// If the underlying implementation returns nullptr, we also need to return nullptr.
	// This means we never expose wrappers which will end up dispatching into nullptr.
	// If we're bypassing ourselves, just return the layered proc addr as-is.
	if (proc)
	{
		auto wrapped_proc = interceptDeviceCommand(pName);
		if (wrapped_proc)
			proc = wrapped_proc;
	}

	return proc;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
VK_LAYER_PYROVEIL_vkGetInstanceProcAddr(VkInstance instance, const char *pName)
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
	if (proc && (layer->active || strcmp(pName, "vkDestroyDevice") == 0))
	{
		auto wrapped_proc = interceptDeviceCommand(pName);
		if (wrapped_proc)
			proc = wrapped_proc;
	}

	return proc;
}

VKAPI_ATTR VkResult VKAPI_CALL
VK_LAYER_PYROVEIL_vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct)
{
	if (pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT || pVersionStruct->loaderLayerInterfaceVersion < 2)
		return VK_ERROR_INITIALIZATION_FAILED;

	if (pVersionStruct->loaderLayerInterfaceVersion > CURRENT_LOADER_LAYER_INTERFACE_VERSION)
		pVersionStruct->loaderLayerInterfaceVersion = CURRENT_LOADER_LAYER_INTERFACE_VERSION;

	if (pVersionStruct->loaderLayerInterfaceVersion >= 2)
	{
		pVersionStruct->pfnGetInstanceProcAddr = VK_LAYER_PYROVEIL_vkGetInstanceProcAddr;
		pVersionStruct->pfnGetDeviceProcAddr = VK_LAYER_PYROVEIL_vkGetDeviceProcAddr;
		pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
	}

	return VK_SUCCESS;
}
