#pragma once

#include "../../app/src/bo2zm_output_volume.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <system_error>

#include <rex/audio/downmix.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/rex_app.h>

#if REX_PLATFORM_SWITCH
#include <switch.h>
#endif

#if BO2CP_NATIVE
#include "native/bo2zm_native_system.h"
#include "native/render/bo2zm_native_capture.h"
#include "native/render/bo2zm_native_precompile.h"
#include "bo2zm_debug_options.h"
#include "bo2zm_name.h"
#include "bo2zm_script_manager.h"
#include "bo2zm_switch.h"
#include "bo2zm_test_pad.h"
#endif

REXCVAR_DECLARE(int32_t, bo2cp_volume);

namespace bo2cp {
void ApplyPendingRelaunch(const std::filesystem::path& user_dir);
void StartWatchdog(const std::filesystem::path& logs_dir);
}

class Bo2cpApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Bo2cpApp>(new Bo2cpApp(ctx, "bo2cp",
        PPCImageConfig));
  }


#if BO2CP_NATIVE


  void OnConfigLoaded() override { bo2zm::debug::ConfigureStartup(user_data_root()); }
  void OnPostInitLogging() override { bo2zm::debug::AfterLoggingStarted(); }
#endif
  void OnPreSetup(rex::RuntimeConfig& config) override {
#if !REX_PLATFORM_SWITCH

    if (const char* volume = std::getenv("BO2_PC_TEST_VOLUME");
        volume && rex::cvar::GetFlagSource("bo2cp_volume") == rex::cvar::Source::kDefault) {
      rex::cvar::SetFlagByName("bo2cp_volume", volume);
    }
#endif


    if (rex::cvar::GetFlagByName("gpu_plugin").empty()) {
      rex::cvar::SetFlagByName("gpu_plugin", "xenos");
    }
#if BO2CP_NATIVE
    if (bo2zm::native::Active()) {


      rex::cvar::SetFlagByName("bo2zm_native_volume_by_writes", "false");
      config.graphics = bo2zm::native::CreateGraphicsSystem();
    }
    bo2zm::test_pad::WrapInput(config);
#endif
#if BO2CP_NATIVE

    bo2zm::InitSwitchServices();
    bo2zm::name::ApplySaved();
#elif REX_PLATFORM_SWITCH

    if (R_FAILED(socketInitializeDefault())) {
      REXLOG_ERROR("socketInitializeDefault failed");
    }
#endif
    const int volume = bo2zm::audio::OutputVolumePercent(REXCVAR_GET(bo2cp_volume));
    rex::audio::SetOutputGain(float(volume) / 100.0f);
    REXLOG_INFO("sound output volume {} %", volume);
  }


  void OnPostSetup() override {
    bo2cp::ApplyPendingRelaunch(user_data_root());
#if BO2CP_NATIVE
    bo2zm::scripts::Boot();
#endif

    bo2cp::StartWatchdog(rex::filesystem::GetExecutableFolder() / "logs");
#if BO2CP_NATIVE

    bo2zm::capture::Boot([this]() -> rex::ui::Presenter* {
      const auto* rt = runtime();
      const auto* graphics = rt ? rt->graphics_system() : nullptr;
      return graphics ? graphics->presenter() : nullptr;
    });
#endif
  }

#if BO2CP_NATIVE
  void OnShutdown() override { bo2zm::capture::Stop(); }
#endif
#if BO2CP_NATIVE

  bool OnHoldModuleLaunch(std::function<void()> start) override {
    return bo2zm::native::precompile::Hold(std::move(start), imgui_drawer());
  }
#endif


  void OnConfigurePaths(rex::PathConfig& paths) override {
    std::error_code ec;
    const auto dir = rex::filesystem::GetExecutableFolder();
    if (dir.empty()) {
      return;
    }
    auto data = dir;
    if (!std::filesystem::is_directory(data / "game", ec)) {
      data = dir.parent_path() / "bo2zm";
    }
    if (paths.game_data_root.empty() && std::filesystem::is_directory(data / "game", ec)) {
      paths.game_data_root = data / "game";
    }
    if (paths.update_data_root.empty() && std::filesystem::is_directory(data / "update", ec)) {
      paths.update_data_root = data / "update";
    }
#if REX_PLATFORM_SWITCH
    paths.user_data_root = dir / "user";
    paths.cache_root = dir / "user" / "cache";
#else


    if (std::filesystem::is_directory(dir / "user", ec) &&
        paths.user_data_root == rex::filesystem::GetUserFolder() / GetName()) {
      if (paths.cache_root == paths.user_data_root / "cache") {
        paths.cache_root = dir / "user" / "cache";
      }
      paths.user_data_root = dir / "user";
    }
#endif
  }
};
