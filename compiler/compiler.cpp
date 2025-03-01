/* Copyright (c) 2025 Hans-Kristian Arntzen for Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include "compiler.hpp"
#include "shaderc/shaderc.hpp"

std::vector<uint32_t> compileToSpirv(const std::string &name, const std::string &glsl, spv::ExecutionModel stage, uint32_t spirvVersion)
{
	shaderc::Compiler compiler;
	shaderc::CompileOptions options;
	options.SetOptimizationLevel(shaderc_optimization_level_zero);
	options.SetGenerateDebugInfo();

	shaderc_shader_kind kind;
	switch (stage)
	{
	case spv::ExecutionModelVertex: kind = shaderc_glsl_vertex_shader; break;
	case spv::ExecutionModelTessellationControl: kind = shaderc_glsl_tess_control_shader; break;
	case spv::ExecutionModelTessellationEvaluation: kind = shaderc_glsl_tess_evaluation_shader; break;
	case spv::ExecutionModelGeometry: kind = shaderc_glsl_geometry_shader; break;
	case spv::ExecutionModelFragment: kind = shaderc_glsl_fragment_shader; break;
	case spv::ExecutionModelGLCompute: kind = shaderc_glsl_compute_shader; break;
	case spv::ExecutionModelMeshEXT: kind = shaderc_glsl_mesh_shader; break;
	case spv::ExecutionModelTaskEXT: kind = shaderc_glsl_task_shader; break;
	case spv::ExecutionModelClosestHitKHR: kind = shaderc_glsl_closesthit_shader; break;
	case spv::ExecutionModelAnyHitKHR: kind = shaderc_glsl_anyhit_shader; break;
	case spv::ExecutionModelMissKHR: kind = shaderc_glsl_miss_shader; break;
	case spv::ExecutionModelIntersectionKHR: kind = shaderc_glsl_intersection_shader; break;
	case spv::ExecutionModelRayGenerationKHR: kind = shaderc_glsl_raygen_shader; break;
	case spv::ExecutionModelCallableKHR: kind = shaderc_glsl_callable_shader; break;
	default: return {};
	}

	if (spirvVersion >= 0x10600)
		options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
	else if (spirvVersion >= 0x10500)
		options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
	else if (spirvVersion >= 0x10300)
		options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
	else
		options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_0);

	options.SetTargetSpirv(shaderc_spirv_version(spirvVersion));
	options.SetSourceLanguage(shaderc_source_language_glsl);

	shaderc::SpvCompilationResult result;

	result = compiler.CompileGlslToSpv(glsl, kind, name.c_str(), options);
	if (result.GetCompilationStatus() != shaderc_compilation_status_success)
	{
		fprintf(stderr, "pyroveil: Failed to convert GLSL: %s\n", result.GetErrorMessage().c_str());
		return {};
	}

	return { result.cbegin(), result.cend() };
}
