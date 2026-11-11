// SPDX-License-Identifier: GPL-3.0-only


#include "native/bo2zm_native_vulkan_profile.h"
#include "sdk_provider_probe.h"
#include <rex/logging.h>
#include <rex/ui/presenter.h>
#include <stdexcept>
#include <spdlog/sinks/ringbuffer_sink.h>

SdkProviderProbeResult ProbeSdkProvider(SdkProbeProgress progress, void* context) {
  SdkProviderProbeResult result;
  auto capture = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(64);
  capture->set_level(spdlog::level::warn);
  try {
    rex::InitLoggingEarly();
    rex::AddSink(capture);
    progress(context, "sdk_vulkan_provider_create");
    auto provider = bo2zm::native::CreateNativeVulkanProvider(true);
    if (!provider) throw std::runtime_error("SDK native Vulkan provider creation failed");
    result.provider_created = true;
    auto* device = provider->vulkan_device();
    const auto& p = device->properties();
    result.gpu = p.deviceName;
    result.ubo_only = device->native_ubo_only();
    result.shader_int64_enabled = p.shaderInt64;
    result.vertex_stores_enabled = p.vertexPipelineStoresAndAtomics;
    result.descriptor_features_enabled = p.bufferDeviceAddress && p.runtimeDescriptorArray &&
        p.shaderSampledImageArrayDynamicIndexing && p.descriptorBindingPartiallyBound &&
        p.descriptorBindingSampledImageUpdateAfterBind && p.descriptorBindingUpdateUnusedWhilePending;
    if (!result.ubo_only || result.shader_int64_enabled || result.vertex_stores_enabled ||
        !result.descriptor_features_enabled) throw std::runtime_error("SDK enabled an incompatible native feature profile");
    progress(context, "sdk_presenter_create");
    auto presenter = provider->CreatePresenter();
    if (!presenter) throw std::runtime_error("SDK Vulkan presenter creation failed");
    result.presenter_created = true;
    bool passed = true;
    {
      const auto family = device->queue_family_graphics_compute();
      auto acquisition = device->AcquireQueue(family, 0);
      progress(context, "sdk_ubo_draw");
      result.ubo = ProbePortUboGraphics(device->physical_device(), device->device(), acquisition.queue(), family);
      passed = result.ubo.pixels_match && result.ubo.error.empty();
      result.pixels_checked = result.ubo.pixels_checked;
      using bo2zm::texture::BcFormat;
      const BcFormat formats[] = {BcFormat::BC1,BcFormat::BC2,BcFormat::BC3,BcFormat::BC4,BcFormat::BC5};
      const char* stages[] = {"sdk_bc1_mips", "sdk_bc2_mips", "sdk_bc3_mips", "sdk_bc4_mips", "sdk_bc5_mips"};
      for (unsigned i = 0; i < 5; ++i) {
        progress(context, stages[i]);
        auto& r = result.textures[i];
        r = ProbePortBcMipGraphics(device->physical_device(), device->device(), acquisition.queue(), family, formats[i]);
        passed = passed && r.cpu_decode_match && r.texture_uploaded && r.pixels_match && r.error.empty() && r.subresources_checked == 30;
        result.pixels_checked += r.pixels_checked;
        result.subresources_checked += r.subresources_checked;
        if (!r.error.empty()) throw std::runtime_error(std::string(stages[i]) + ": " + r.error);
      }
    }
    progress(context, "sdk_provider_shutdown");
    presenter.reset(); provider.reset(); result.destroyed = true;
    result.passed = passed;
  } catch (const std::exception& error) { result.error = error.what(); }
  rex::FlushLogging();
  rex::RemoveSink(capture);
  for (const auto& message : capture->last_formatted()) {
    if (result.sdk_messages.size() + message.size() > 12000) break;
    result.sdk_messages += message;
  }
  return result;
}
