// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "graphics_probe.h"
#include <array>
struct SdkProviderProbeResult {
  bool passed = false, provider_created = false, presenter_created = false;
  bool ubo_only = false, shader_int64_enabled = false, vertex_stores_enabled = false;
  bool descriptor_features_enabled = false, destroyed = false;
  uint32_t pixels_checked = 0, subresources_checked = 0;
  std::string gpu, error, sdk_messages;
  GraphicsProbeResult ubo;
  std::array<GraphicsProbeResult, 5> textures;
};
using SdkProbeProgress = void (*)(void*, const char*);
SdkProviderProbeResult ProbeSdkProvider(SdkProbeProgress progress, void* context);
