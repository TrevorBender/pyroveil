/* Copyright (c) 2025 Hans-Kristian Arntzen for Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <vector>
#include <string>
#include <stdint.h>
#include "spirv/unified1/spirv.hpp"

std::vector<uint32_t> compileToSpirv(const std::string &name, const std::string &glsl, spv::ExecutionModel model, uint32_t spirvVersion);