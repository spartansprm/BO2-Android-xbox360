#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>

namespace rex::ui {
class ImGuiDrawer;
}
namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace bo2zm::native {

class NativeShaders;

namespace precompile {


bool Hold(std::function<void()> start, rex::ui::ImGuiDrawer* drawer);


bool Decide(const rex::ui::vulkan::VulkanDevice* device, const NativeShaders* library);

uint64_t Stamp();
bool KnownWarm();


void Begin(uint32_t total, uint32_t threads);
void Advance(bool compiled);
void End();


void Vouch(const std::filesystem::path& pipelines_file);
void Withdraw(const std::filesystem::path& pipelines_file);

}
}
