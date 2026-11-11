#pragma once

#include <filesystem>
#include <stdexcept>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/platform.h>
#include <rex/rex_app.h>

#include "bo2zm_autoexec.h"
#include "bo2zm_controllers.h"
#include "bo2zm_debug_options.h"
#include "bo2zm_gsc_mods.h"
#include "bo2zm_lifecycle.h"
#include "bo2zm_script_manager.h"
#include "bo2zm_name.h"
#include "bo2zm_pc_profiler.h"
#include "bo2zm_relaunch.h"
#include "bo2zm_switch.h"
#include "bo2zm_test_pad.h"
#include "bo2zm_online.h"
#include "bo2zm_updates.h"
#include "bo2zm_test_waits.h"
#include "native/bo2zm_native_system.h"
#include "native/render/bo2zm_graphics_settings.h"
#include "native/render/bo2zm_native_capture.h"
#include "native/render/bo2zm_native_precompile.h"

class Bo2zmApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Bo2zmApp>(new Bo2zmApp(ctx, "bo2zm",
        PPCImageConfig));
  }


  void OnLoadXexImage(std::string& xex_image) override {
    xex_image = "game:\\default_mp.xex";
  }


  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (!bo2zm::lifecycle::Prepare()) {
      throw std::runtime_error("Could not disable automated diagnostics after a fatal restart");
    }

    if (rex::cvar::GetFlagSource("offline_user_count") == rex::cvar::Source::kDefault) {
      rex::cvar::SetFlagByName("offline_user_count", "4");
    }
    bo2zm::InitSwitchServices();
    bo2zm::lifecycle::OnBoot();
    bo2zm::autoexec::ApplyVolume();
    bo2zm::controllers::WrapInput(config);
    bo2zm::test_pad::WrapInput(config);
    if (bo2zm::native::Active()) {
      config.graphics = bo2zm::native::CreateGraphicsSystem();
    }
  }


  void OnPostSetup() override {
    bo2zm::name::ApplySaved();
    bo2zm::pc_profiler::Start();
    bo2zm::test_waits::RunIfRequested();
    bo2zm::ApplyPendingRelaunch(user_data_root());
    bo2zm::scripts::Boot();
    bo2zm::gsc::Configure();


    bo2zm::capture::Boot([this]() -> rex::ui::Presenter* {
      const auto* rt = runtime();
      const auto* graphics = rt ? rt->graphics_system() : nullptr;
      return graphics ? graphics->presenter() : nullptr;
    });
  }


  bool OnHoldModuleLaunch(std::function<void()> start) override {
    return bo2zm::native::precompile::Hold(std::move(start), imgui_drawer());
  }

  void OnShutdown() override {
    bo2zm::capture::Stop();
    bo2zm::lifecycle::Stop();
    bo2zm::updates::Stop();
    bo2zm::online::Stop();
  }


  void OnConfigurePaths(rex::PathConfig& paths) override {
    std::error_code ec;
    auto dir = rex::filesystem::GetExecutableFolder();
#if REX_PLATFORM_SWITCH


    if (!std::filesystem::is_directory(dir / "game", ec) &&
        std::filesystem::is_directory("sdmc:/switch/bo2zm/game", ec)) {
      dir = "sdmc:/switch/bo2zm";
      paths.config_path = dir / "bo2zm.toml";
    }
#endif
    if (dir.empty()) {
      return;
    }
    if (paths.game_data_root.empty() && std::filesystem::is_directory(dir / "game", ec)) {
      paths.game_data_root = dir / "game";
    }
    if (paths.update_data_root.empty() && std::filesystem::is_directory(dir / "update", ec)) {
      paths.update_data_root = dir / "update";
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


  void OnConfigLoaded() override { bo2zm::debug::ConfigureStartup(user_data_root()); }


  void OnPostInitLogging() override {
    bo2zm::settings::RequestBootClocks();
    bo2zm::debug::AfterLoggingStarted();
  }


};
