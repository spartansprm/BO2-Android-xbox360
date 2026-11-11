// SPDX-License-Identifier: GPL-3.0-only


#ifndef BO2_ANDROID_APP_VERSION
#define BO2_ANDROID_APP_VERSION "0.55.0"
#endif


#if BO2_ANDROID_CAMPAIGN
#include "generated/default/bo2cp_init.h"
#include "bo2cp_app.h"
#else
#include "generated/default_mp/bo2zm_init.h"
#include "bo2zm_app.h"
#endif
#include "bo2zm_preset.h"
#include "native/render/bo2zm_native_cache_paths.h"
#include "bo2zm_android_perf_hint.h"
#include "native/render/bo2zm_native_performance.h"
#include "native/render/bo2zm_display_state.h"
#include "native/render/bo2zm_native_draw_batch.h"
#include "native/render/bo2zm_native_pass_profile.h"
#include "native/render/bo2zm_native_scene_sync.h"
#include "native/render/bo2zm_native_pixel_probe.h"
#include "native/render/bo2zm_native_shaders.h"
#include "native/render/bo2zm_native_render.h"
#include "native/render/bo2zm_hitch_waits.h"
#include "content_mount.h"
#include "dlc_package.h"
#include "fault_evidence.h"
#include <rex/filesystem/devices/stfs_container_file.h>
#include <rex/system/kernel_state.h>
#include "shader_capture.h"
#include "touch_state.h"
#include "touch_gamepad.h"
#include "touch_look.h"
#include <rex/android.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/system/user_module.h>
#include <rex/system/xex_module.h>
#include <rex/ui/windowed_app_context_sdl.h>
#include <SDL3/SDL.h>
#include <jni.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <sstream>
#include <locale>
#include <string>
#include <vector>

using rex::X_STATUS;
using rex::X_RESULT;
namespace rex::memory {void AndroidInitialize();}
namespace rex::thread {void AndroidInitialize();}
extern std::atomic<uint64_t> g_bo2zm_frames_game;
extern "C" REX_FUNC(__imp__xstart);


REXCVAR_DEFINE_BOOL(bo2zm_android_relaunch, false, "BO2",
                    "Android: this start is a mode change from the game's menu; follow its pending launch")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
#if BO2_ANDROID_CAMPAIGN

REXCVAR_DEFINE_INT32(bo2cp_volume, 100, "BO2", "Output volume, 0-100 percent").range(0, 100)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_STRING(bo2zm_test_mode, "", "BO2", "Android launcher: the mode it started (cp)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
#endif
namespace {
std::mutex state_mutex;
std::map<std::string,std::string> state;
std::filesystem::path files,bootstrap;
std::vector<int> fds;
std::vector<int> dlc_fds;
std::vector<std::string> dlc_names;
std::string run_identifier;
bool configured=false;
bool detailed_gpu_marks_requested=false;
bool draw_batch_requested=true;
bool gpu_pass_profile_requested=true;
bool transfer_dst_trial_requested=false;
bool disable_scene_pass_dependencies_requested=false;
bool disable_scene_transfer_barriers_requested=false;
bool disable_scene_output_read_barrier_requested=false;
int scene_pass_sync_mode_requested=0;
bool disable_async_pipelines_requested=false;
std::atomic<uint64_t> pixel_probe_saved_trial{0};
std::atomic<bool> stop_monitor{false};
std::thread monitor;
bo2zm::android::TouchState touch;
std::atomic<uint64_t> touch_polls{0},touch_active_polls{0};
JavaVM* vm=nullptr;


jclass game_activity_class=nullptr;
jmethodID request_relaunch_method=nullptr;
std::string json_quote(const std::string& value) {
  std::string s="\"";for(unsigned char c:value) {
    if(c=='"'||c=='\\'){s+='\\';s+=char(c);}
    else if(c<32){constexpr char h[]="0123456789abcdef";s+="\\u00";s+=h[c>>4];s+=h[c&15];}
    else s+=char(c);
  }return s+'"';
}
void write_locked() {
  if(files.empty())return;
  const auto pipeline_file=bo2zm::native::cache::PipelineFile();
  std::error_code pipeline_error;
  const auto pipeline_bytes=std::filesystem::file_size(pipeline_file,pipeline_error);
  state["pipeline_cache_path"]=json_quote(pipeline_file.string());
  state["pipeline_cache_file_present"]=pipeline_error?"false":"true";
  state["pipeline_cache_file_bytes"]=std::to_string(pipeline_error?0:pipeline_bytes);
  state["loaded_map"]=json_quote(bo2zm::preset::LoadedMap());
  state["loaded_location"]=json_quote(bo2zm::preset::LoadedLocation());
  state["graphics_preset_label"]=json_quote(bo2zm::preset::PresetLabel());
  state["graphics_options"]=json_quote(bo2zm::preset::GraphicsOptionsDiagnostic());
  state["texture_mips_minus"]=std::to_string(bo2zm::preset::MipsMinus());
  const auto pixel_probe_snapshot=bo2zm::native::pixel_probe::Read();
  state["pixel_probe"]=bo2zm::native::pixel_probe::Json(pixel_probe_snapshot);
  const auto t=touch.Get();
  state["touch_controls"]="{\"guest_user\":0,\"synthetic\":true,\"packet\":"+std::to_string(t.packet)+
    ",\"polls\":"+std::to_string(touch_polls.load())+",\"active_polls\":"+std::to_string(touch_active_polls.load())+
    ",\"buttons\":"+std::to_string(t.buttons)+",\"lt\":"+std::to_string(t.lt)+",\"rt\":"+std::to_string(t.rt)+
    ",\"lx\":"+std::to_string(t.lx)+",\"ly\":"+std::to_string(t.ly)+",\"rx\":"+std::to_string(t.rx)+",\"ry\":"+std::to_string(t.ry)+"}";
  const auto c=bo2zm::android::capture::Snapshot();
  state["shader_capture"]="{\"calls\":"+std::to_string(c.calls)+",\"known\":"+std::to_string(c.known)+
    ",\"unique\":"+std::to_string(c.unique)+",\"duplicates\":"+std::to_string(c.duplicates)+
    ",\"rejected\":"+std::to_string(c.rejected)+",\"limited\":"+std::to_string(c.limited)+
    ",\"errors\":"+std::to_string(c.errors)+",\"bytes\":"+std::to_string(c.bytes)+
    ",\"last_error\":"+json_quote(c.last_error)+"}";
  std::string data="{";for(auto&[k,v]:state){if(data.size()>1)data+=',';data+=json_quote(k)+':'+v;}data+='}';
  const auto path=files/"game-start-checkpoint.json",temp=files/"game-start-checkpoint.json.tmp";
  int fd=::open(temp.c_str(),O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC,0600);
  if(fd<0)throw std::runtime_error("Cannot create game checkpoint");
  size_t at=0;while(at<data.size()){ssize_t n=::write(fd,data.data()+at,data.size()-at);if(n<0&&errno==EINTR)continue;if(n<=0){::close(fd);throw std::runtime_error("Game checkpoint write failed");}at+=size_t(n);}
  if(fsync(fd)){::close(fd);throw std::runtime_error("Game checkpoint fsync failed");}::close(fd);
  if(::rename(temp.c_str(),path.c_str()))throw std::runtime_error("Game checkpoint replace failed");


  if(pixel_probe_snapshot.state==bo2zm::native::pixel_probe::State::kComplete)
    pixel_probe_saved_trial.store(pixel_probe_snapshot.trial,std::memory_order_release);
}
void phase(const char* name) {
  std::lock_guard guard(state_mutex);state["phase"]=json_quote(name);
  state["guest_swap_calls"]=std::to_string(g_bo2zm_frames_game.load(std::memory_order_relaxed));write_locked();
}
void text(const char* k,const std::string& v){std::lock_guard guard(state_mutex);state[k]=json_quote(v);}
void flag(const char* k,bool v){std::lock_guard guard(state_mutex);state[k]=v?"true":"false";}
void number(const char* k,int v){std::lock_guard guard(state_mutex);state[k]=std::to_string(v);}
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::string utf(JNIEnv* env,jstring value) {
  require(value,"Missing Android argument");const char* c=env->GetStringUTFChars(value,nullptr);require(c,"Android argument decode failed");
  std::string result(c);env->ReleaseStringUTFChars(value,c);return result;
}
void stop() {stop_monitor=true;if(monitor.joinable())monitor.join();}

class TouchPad final:public rex::input::InputDriver {
  static constexpr auto id=static_cast<rex::input::DeviceId>(0x414E44524F4944ull);
 public:
  TouchPad():InputDriver(nullptr,0){}
  X_STATUS Setup()override{return X_STATUS_SUCCESS;}
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out)override{out.push_back({id,0,"Android touch controls","bo2-android-touch",true});}
  X_RESULT GetDeviceState(rex::input::DeviceId device,rex::input::X_INPUT_STATE* s)override {
    if(device!=id)return X_ERROR_DEVICE_NOT_CONNECTED;
    auto t=touch.Get();const bool active=is_active();++touch_polls;if(active&&!t.Neutral())++touch_active_polls;
    if(s)bo2zm::android::FillTouchGamepad(t,*s,active);return X_ERROR_SUCCESS;
  }
  X_RESULT GetDeviceCapabilities(rex::input::DeviceId device,uint32_t,rex::input::X_INPUT_CAPABILITIES* c)override {
    if(device!=id)return X_ERROR_DEVICE_NOT_CONNECTED;
    if(c){std::memset(c,0,sizeof(*c));c->type=1;c->sub_type=1;c->gamepad.buttons=0xF3FF;c->gamepad.left_trigger=c->gamepad.right_trigger=255;c->gamepad.thumb_lx=c->gamepad.thumb_ly=c->gamepad.thumb_rx=c->gamepad.thumb_ry=32767;}return X_ERROR_SUCCESS;
  }
  X_RESULT SetDeviceVibration(rex::input::DeviceId device,rex::input::X_INPUT_VIBRATION*)override{return device==id?X_ERROR_SUCCESS:X_ERROR_DEVICE_NOT_CONNECTED;}
  X_RESULT GetDeviceKeystroke(rex::input::DeviceId device,uint32_t,rex::input::X_INPUT_KEYSTROKE*)override{return device==id?X_ERROR_EMPTY:X_ERROR_DEVICE_NOT_CONNECTED;}
};


REXCVAR_DEFINE_BOOL(bo2zm_android_adpf, true, "BO2",
                    "Android: tell the system which threads set the frame time (ADPF PerformanceHint)");
REXCVAR_DEFINE_INT32(bo2zm_android_adpf_target_ms, 22, "BO2",
                     "Android ADPF: target frame work duration in ms. Keep it below the real frame time (~28 ms) "
                     "so the system asks for more CPU capacity instead of settling at the lowest clock")
    .range(5, 100);


REXCVAR_DEFINE_BOOL(bo2zm_android_front_copy, true, "BO2",
                    "Android: force the front buffer copy before the Swap (bo2zm_native_lazy_front=false). false = no copy");

std::string ContainerReadCacheJson() {
  const auto c=rex::filesystem::StfsReadCacheStats();
  std::ostringstream out;out.imbue(std::locale::classic());
  out<<"{\"hits\":"<<c.hits<<",\"misses\":"<<c.misses<<",\"bytes_from_ram\":"<<c.bytes_ram
     <<",\"bytes_from_storage\":"<<c.bytes_disk<<",\"entries\":"<<c.entries<<",\"live_bytes\":"<<c.live_bytes
     <<",\"evictions\":"<<c.evictions<<",\"cap_mb\":"<<c.cap_mb<<",\"below_floor\":"<<c.low_floor
     <<",\"above_ceiling\":"<<c.over_ceiling<<",\"sweep_reads\":"<<c.sequential
     <<",\"out_of_memory\":"<<(c.no_memory?"true":"false")<<'}';
  return out.str();
}
#if BO2_ANDROID_CAMPAIGN


using GameApp=Bo2cpApp;
constexpr const char* kProgramName="bo2cp";
constexpr uint32_t kImageBytes=38731776;
constexpr uint32_t kEntryPoint=0x825C69B8;
constexpr const char* kImageSha256=BO2CP_ANDROID_IMAGE_SHA256;
constexpr const char* kBootZones[]={"game:\\code_post_gfx.ff","game:\\common.ff"};
constexpr const char* kBootZonesMissing="Campaign boot fastfiles missing from VFS";
constexpr const char* kConfigFile="game-android-cp.toml";
#else
using GameApp=Bo2zmApp;
constexpr const char* kProgramName="bo2zm";
constexpr uint32_t kImageBytes=47054848;
constexpr uint32_t kEntryPoint=0x827DB460;
constexpr const char* kImageSha256="ecb2d7ce653be4c4fc8945e0083e222570266b5a770896819427be69d463f72e";
constexpr const char* kBootZones[]={"game:\\code_post_gfx_zm.ff","game:\\common_zm.ff"};
constexpr const char* kBootZonesMissing="Zombies boot fastfiles missing from VFS";
constexpr const char* kConfigFile="game-android.toml";
#endif
class AndroidBo2zmApp final:public GameApp {
 public:
  explicit AndroidBo2zmApp(rex::ui::WindowedAppContext& ctx):GameApp(ctx,kProgramName,PPCImageConfig){}
  void OnConfigurePaths(rex::PathConfig& p)override {


    p.game_data_root=bootstrap;p.user_data_root=files/"game-user";p.update_data_root.clear();
    p.cache_root=p.user_data_root/BO2ZM_NATIVE_CACHE_FOLDER;p.config_path=files/kConfigFile;
    std::filesystem::create_directories(p.user_data_root);std::filesystem::create_directories(p.cache_root);
  }
  void OnConfigLoaded()override {
    GameApp::OnConfigLoaded();rex::cvar::SetFlagByName("bo2zm_renderer","native");


    if(rex::cvar::GetFlagByName("bo2zm_perf_v046")=="true"){
      rex::cvar::SetFlagByName("present_dedicated_queue","true");
    }
    bo2zm::android_perf::SetEnabled(REXCVAR_GET(bo2zm_android_adpf));
    bo2zm::android_perf::SetTargetMs(double(REXCVAR_GET(bo2zm_android_adpf_target_ms)));


    require(rex::cvar::SetFlagByName("bo2zm_native_precompile","true"),
            "Cannot enable initial pipeline preparation");
    require(rex::cvar::GetFlagByName("bo2zm_native_precompile") == "true",
            "Initial pipeline preparation did not apply");
    flag("pipeline_precompile_before_game",true);


    const bool allow_async_pipelines = !disable_async_pipelines_requested;
    require(rex::cvar::SetFlagByName("bo2zm_native_async_pipelines",
                                     allow_async_pipelines ? "true" : "false"),
            "Cannot configure pipeline compilation policy");
    require(rex::cvar::GetFlagByName("bo2zm_native_async_pipelines") ==
                (allow_async_pipelines ? "true" : "false"),
            "Pipeline compilation policy did not apply");
    flag("pipeline_async_enabled", allow_async_pipelines);
    flag("pipeline_sync_trial_requested", disable_async_pipelines_requested);
    text("pipeline_compile_strategy", allow_async_pipelines ?
         "asynchronous-skip-until-ready" : "synchronous-block-until-ready");


    if (REXCVAR_GET(bo2zm_android_front_copy)) {
      require(rex::cvar::SetFlagByName("bo2zm_native_lazy_front","false"),"Cannot select copied front buffer");
    }
    flag("frontbuffer_copy_enabled",rex::cvar::GetFlagByName("bo2zm_native_lazy_front")=="false");

    flag("deferred_clears_enabled",rex::cvar::GetFlagByName("bo2zm_native_deferred_clears")=="true");

    flag("guest_clears_in_pass_enabled",rex::cvar::GetFlagByName("bo2zm_native_guest_clears_in_pass")=="true");
    flag("volume_eager_enabled",rex::cvar::GetFlagByName("bo2zm_native_volume_eager")=="true");
    text("texture_mip_floor",rex::cvar::GetFlagByName("bo2zm_texture_mip_floor"));

    text("runtime_shader_translate",rex::cvar::GetFlagByName("bo2zm_native_shader_translate"));
#if BO2ZM_RUNTIME_SHADERS
    flag("runtime_shader_translator_linked",true);
#else
    flag("runtime_shader_translator_linked",false);
#endif


    text("present_own_thread",rex::cvar::GetFlagByName("present_own_thread"));
    text("present_paint_profile",rex::cvar::GetFlagByName("present_paint_profile"));
    text("android_thread_placement",rex::cvar::GetFlagByName("android_thread_placement"));

    for(const char* name:{"bo2zm_perf_v046","bo2zm_perf_prepass_fuse_rect_clears","bo2zm_perf_fit_area_to_clear","bo2zm_perf_edram_dead_stores","bo2zm_perf_drop_unread_fronts",
                          "bo2zm_perf_lazy_resolves","bo2zm_perf_resolve_alias","bo2zm_perf_recorder_kick",
                          "bo2zm_perf_recorder_wake_bytes","bo2zm_perf_recorder_spins",
                          "bo2zm_perf_pass_profile_serialize","bo2zm_perf_ab_s","present_wait_paint_unlocked","present_dedicated_queue",

                          "bo2zm_perf_v055","bo2zm_perf_readonly_depth"})
      text(name,rex::cvar::GetFlagByName(name));
    text("scene_scale_percent",rex::cvar::GetFlagByName("bo2zm_native_scene_scale"));

    text("scene_resolution",rex::cvar::GetFlagByName("bo2zm_native_scene_resolution"));

    text("output_resolution",rex::cvar::GetFlagByName("bo2zm_native_output_resolution"));
    text("screen_mode",rex::cvar::GetFlagByName("bo2zm_native_screen_mode"));
    text("display_size",rex::cvar::GetFlagByName("bo2zm_native_display_width")+"x"+
         rex::cvar::GetFlagByName("bo2zm_native_display_height"));
    flag("output_image_barriers_enabled",true);
    require(rex::cvar::SetFlagByName("bo2zm_native_hash_sampling","0"),"Cannot select full texture hash checks");
    flag("full_texture_hash_enabled",rex::cvar::GetFlagByName("bo2zm_native_hash_sampling")=="0");


    constexpr const char* unchecked_cache_flags[] = {
      "bo2zm_native_vertex_cache_by_writes", "bo2zm_native_vertex_cache_sum",
      "bo2zm_native_vertex_cache_sum_ab", "bo2zm_native_index_cache",
      "bo2zm_native_index_cache_ab", "bo2zm_native_texture_by_writes",
      "bo2zm_native_texture_by_writes_ab"
    };
    for (const char* name : unchecked_cache_flags) {
      require(rex::cvar::SetFlagByName(name,"false"),"Cannot select content-checked renderer caches");
      require(rex::cvar::GetFlagByName(name)=="false","Renderer cache configuration did not take effect");
    }
    flag("vertex_cache_by_writes_enabled",false);
    flag("vertex_cache_full_hash_selected",true);
    flag("index_cache_by_writes_enabled",false);
    flag("texture_cache_by_writes_enabled",false);


    require(rex::cvar::SetFlagByName("bo2zm_native_scene_sync","true"),"Cannot enable scene image dependencies");
    require(rex::cvar::GetFlagByName("bo2zm_native_scene_sync")=="true","Scene synchronization did not take effect");

    require(scene_pass_sync_mode_requested>=0 && scene_pass_sync_mode_requested<=10,
            "Invalid scene pass synchronization test mode");
    const bool precise_patch = rex::cvar::GetFlagByName("bo2zm_native_scene_sync_precise") == "true";
    const int precise_mask = std::stoi(rex::cvar::GetFlagByName("bo2zm_native_scene_sync_mask"));
    require(precise_mask>=0 && precise_mask<=255, "Invalid precise GPU hazard mask");


    const int scene_pass_mode = precise_patch ? 0 :
        (disable_scene_pass_dependencies_requested ? 6 : scene_pass_sync_mode_requested);
    require(rex::cvar::SetFlagByName("bo2zm_native_scene_pass_mode",
                                    std::to_string(scene_pass_mode)),
            "Cannot configure scene pass synchronization mode");
    require(rex::cvar::GetFlagByName("bo2zm_native_scene_pass_mode") ==
                std::to_string(scene_pass_mode),
            "Scene pass synchronization mode did not apply");
    flag("scene_dependencies_precise",precise_patch);
    number("scene_precise_mask",precise_mask);
    text("scene_pass_sync_strategy",
         precise_patch?"precise-hazard-patch":scene_pass_mode==0?"full-original":
         scene_pass_mode==1?"incoming-only":
         scene_pass_mode==2?"outgoing-only":
         scene_pass_mode==3?"scoped-subpass-access":
         scene_pass_mode==4?"main-scene-only":
         scene_pass_mode==5?"converted-copy-only":
         scene_pass_mode==6?"no-external-dependencies":
         scene_pass_mode==7?"convert-full-main-outgoing":
         scene_pass_mode==8?"convert-full-main-incoming":
         scene_pass_mode==9?"convert-full-main-depth-only":
                            "convert-full-main-color-only");
    number("scene_pass_sync_mode",scene_pass_mode);

    const std::pair<const char*,bool> scene_guards[] = {
      {"bo2zm_native_scene_pass_dependencies", precise_patch || scene_pass_mode!=6},
      {"bo2zm_native_scene_transfer_barriers", precise_patch || !disable_scene_transfer_barriers_requested},
      {"bo2zm_native_scene_output_read_barrier", precise_patch || !disable_scene_output_read_barrier_requested}
    };
    for(const auto& [name,enabled] : scene_guards) {
      require(rex::cvar::SetFlagByName(name,enabled ? "true" : "false"),
              "Cannot select scene synchronization protection");
      require(rex::cvar::GetFlagByName(name) == (enabled ? "true" : "false"),
              "scene synchronization protection not applied");
    }
    flag("scene_image_dependencies_enabled",precise_patch || scene_pass_mode!=6);
    flag("scene_transfer_barriers_enabled",precise_patch || !disable_scene_transfer_barriers_requested);
    flag("resolved_output_barrier_enabled",precise_patch || !disable_scene_output_read_barrier_requested);
    flag("scene_image_dependencies_disabled_requested",scene_pass_mode==6);
    flag("scene_transfer_barriers_disabled_requested",disable_scene_transfer_barriers_requested);
    flag("resolved_output_barrier_disabled_requested",disable_scene_output_read_barrier_requested);
    text("scene_output_barrier_note",disable_scene_output_read_barrier_requested
        ? "output read barrier OFF; since_target fallback remains"
        : "output read barrier ON");
    const bool transfer_dst_trial_effective =
        transfer_dst_trial_requested && !disable_scene_transfer_barriers_requested && !precise_patch;
    require(rex::cvar::SetFlagByName("bo2zm_native_transfer_dst_only",
                                    transfer_dst_trial_effective ? "true" : "false"),
            "Cannot select experimental transfer consumer scope");
    require(rex::cvar::GetFlagByName("bo2zm_native_transfer_dst_only") ==
                (transfer_dst_trial_effective ? "true" : "false"),
            "Transfer consumer scope did not take effect");
    flag("scene_transfer_dst_experimental",transfer_dst_trial_effective);
    flag("scene_transfer_dst_trial_requested",transfer_dst_trial_requested);
    text("scene_transfer_strategy",disable_scene_transfer_barriers_requested
        ? "disabled by the transfer dependency switch" : (transfer_dst_trial_effective
          ? "all producer accesses; TRANSFER-only consumer" : "original all commands"));


    require(rex::cvar::SetFlagByName("bo2zm_native_precise_marks","true"),"Cannot select GPU endpoint stages");
    require(rex::cvar::SetFlagByName("bo2zm_native_precise_marks_alternate_s","0"),"Cannot disable alternate GPU stages");
    require(rex::cvar::SetFlagByName("bo2zm_native_gpu_category_marks",detailed_gpu_marks_requested?"true":"false"),"Cannot select GPU timing mode");
    require(rex::cvar::GetFlagByName("bo2zm_native_gpu_category_marks")==
            (detailed_gpu_marks_requested?"true":"false"),"GPU timing mode did not take effect");
    require(rex::cvar::SetFlagByName("bo2zm_native_draw_batch",draw_batch_requested?"true":"false"),"Cannot select draw batching");
    require(rex::cvar::GetFlagByName("bo2zm_native_draw_batch")==
        (draw_batch_requested?"true":"false"),"Draw batching mode did not take effect");
    require(rex::cvar::SetFlagByName("bo2zm_native_gpu_pass_profile",gpu_pass_profile_requested?"true":"false"),"Cannot select sampled GPU pass timing");
    require(rex::cvar::GetFlagByName("bo2zm_native_gpu_pass_profile")==
        (gpu_pass_profile_requested?"true":"false"),"GPU pass timing mode did not take effect");
    flag("gpu_pass_profile_requested",gpu_pass_profile_requested);
    text("gpu_pass_profile_sampling","one of 64 eligible jobs after 10 seconds in foreground gameplay; no image change");
    flag("draw_batch_requested",draw_batch_requested);
    text("draw_batch_strategy","ordered contiguous pure triangle-list draws; identical state; no sorting");
    flag("gpu_timestamp_category_marks_enabled",detailed_gpu_marks_requested);
    text("gpu_timestamp_mode",detailed_gpu_marks_requested?"categories":"endpoints");
    text("gpu_timestamp_queries_reset_per_job",detailed_gpu_marks_requested?"128":"2");
    text("gpu_timestamp_endpoint_stages","BOTTOM_OF_PIPE");


    for (const char* name : {"bo2zm_native_test_scissor", "bo2zm_native_test_scissor_alternate_s"}) {
      require(rex::cvar::SetFlagByName(name,"0"),"Cannot reset legacy scissor diagnostic");
      require(rex::cvar::GetFlagByName(name)=="0","Legacy scissor diagnostic remains active");
    }
    flag("pixel_probe_manual_start_required",true);


    require(rex::cvar::SetFlagByName("bo2zm_native_pipelines_save_in_match","2"),"Cannot enable in-match pipeline saves");


    require(rex::cvar::SetFlagByName("bo2zm_native_dynamic_state","false"),"Cannot enable dynamic pipeline state");


    require(rex::cvar::SetFlagByName("bo2zm_native_compile_threads","3"),"Cannot set pipeline compile threads");
    require(rex::cvar::SetFlagByName("bo2zm_native_compile_helpers_in_match","true"),"Cannot enable in-match compile helpers");
    text("pipeline_compile_threads",rex::cvar::GetFlagByName("bo2zm_native_compile_threads"));
    flag("pipeline_compile_helpers_in_match",rex::cvar::GetFlagByName("bo2zm_native_compile_helpers_in_match")=="true");
    flag("pipeline_dynamic_state_requested",rex::cvar::GetFlagByName("bo2zm_native_dynamic_state")=="true");
    flag("pipeline_cache_private_dir_selected",true);
    text("pipeline_cache_save_in_match",rex::cvar::GetFlagByName("bo2zm_native_pipelines_save_in_match"));
    text("native_optimization","O2-no-fast-math");
    flag("compact_bc_requested",rex::cvar::GetFlagByName("bo2zm_native_compact_bc")=="true");
    text("texture_cache_limit_mib",rex::cvar::GetFlagByName("bo2zm_native_textures_mb_max"));
    std::error_code cache_error;
    const bool cache_present=std::filesystem::is_regular_file(bo2zm::native::cache::PipelineFile(),cache_error);
    flag("pipeline_cache_present_at_config_load",cache_present&&!cache_error);
    flag("luminance_uint16_statistics_enabled",true);
    flag("output_gamma_statistics_enabled",true);
    phase("game_configured_copied_frontbuffer");
  }
  void OnPreSetup(rex::RuntimeConfig& config)override {
    phase("game_presentation_setup");GameApp::OnPreSetup(config);
    const auto original=config.input_factory;
    config.input_factory=[original](bool tool) {
      auto input=original(tool);auto* system=dynamic_cast<rex::input::InputSystem*>(input.get());
      require(system,"Original SDL input system unavailable");system->AddDriver(std::make_unique<TouchPad>());return input;
    };
  }
  void OnPostLoadXexImage()override {
    phase("game_tu18_verify");
    auto module=runtime()->kernel_state()->GetExecutableModule();require(bool(module),"Loaded module missing");
    const auto* xex=module->xex_module();require(xex->image_size()==kImageBytes&&xex->entry_point()==kEntryPoint,"Unexpected loaded image size/entrypoint");
    JNIEnv* env=nullptr;require(vm&&vm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6)==JNI_OK,"Game JNI thread unavailable");
    jclass cls=env->FindClass("org/bo2port/diagnostic/ExecutableDigest");
    auto method=cls?env->GetStaticMethodID(cls,"digestMemory","(Ljava/nio/ByteBuffer;)Ljava/lang/String;"):nullptr;require(method,"Image hash method missing");
    jobject buffer=env->NewDirectByteBuffer(runtime()->memory()->TranslateVirtual(xex->base_address()),xex->image_size());
    auto value=static_cast<jstring>(env->CallStaticObjectMethod(cls,method,buffer));require(!env->ExceptionCheck()&&value,"Loaded image hash failed");const auto hash=utf(env,value);
    env->DeleteLocalRef(value);env->DeleteLocalRef(buffer);env->DeleteLocalRef(cls);
    text("loaded_image_sha256",hash);require(hash==kImageSha256,"Loaded image differs from verified TU18");
    flag("tu18_image_matches",true);phase("game_mount_resources");require(fds.size()>3,"Game descriptors not configured");
    bo2zm::android::MountContent(*runtime()->file_system(),fds.front(),{fds.data()+2,fds.size()-2},fds[1]);
    std::string dlc_report="[";uint32_t mounted=0;
    for(size_t i=0;i<dlc_fds.size();++i) {
      if(i)dlc_report+=',';
      dlc_report+="{\"package\":"+json_quote(dlc_names[i]);
      const std::string mount="\\Device\\AndroidBO2DLC\\"+std::to_string(i);
      try {
        auto package=bo2zm::android::OpenDlcPackage(dlc_fds[i],dlc_names[i],mount,0x415608C3);
        dlc_report+=",\"license_mask\":"+std::to_string(package.header_license);


        if(package.content.license!=package.header_license)dlc_report+=",\"license_mask_game\":"+std::to_string(package.content.license);


        std::string markers="[";for(const auto& marker:package.markers){if(markers.size()>1)markers+=',';markers+=json_quote(marker);}markers+=']';
        const bool map_package=!package.markers.empty();
        require(runtime()->file_system()->RegisterDevice(std::move(package.device)),"DLC VFS device registration failed");
        if(!runtime()->kernel_state()->content_manager()->RegisterReadOnlyContent(package.content.data,mount,package.content.license)) {
          runtime()->file_system()->UnregisterDevice(mount);throw std::runtime_error("DLC XAM registration failed");
        }
        ++mounted;dlc_report+=std::string(",\"status\":\"")+(map_package?"mounted_read_only":"mounted_read_only_no_map_markers")+
          "\",\"markers\":"+markers;
      }catch(const std::exception& error){dlc_report+=",\"status\":\"error\",\"error\":"+json_quote(error.what());}
      dlc_report+='}';
    }
    dlc_report+=']';
    {std::lock_guard guard(state_mutex);state["dlc_content"]=dlc_report;state["dlc_packages_mounted"]=std::to_string(mounted);}
    require(runtime()->file_system()->ResolvePath(kBootZones[0])&&runtime()->file_system()->ResolvePath(kBootZones[1]),kBootZonesMissing);
    flag("resources_mounted",true);phase("game_resources_ready");
  }
  void OnPostSetup()override {
    phase("game_original_post_setup");GameApp::OnPostSetup();
    bo2zm::android::fault::Start(files,run_identifier,runtime()->memory()->TranslateVirtual(0));
    flag("runtime_setup_complete",true);phase("game_waiting_for_guest_launch");
    stop_monitor=false;monitor=std::thread([] {
      while(!stop_monitor.load()){std::this_thread::sleep_for(std::chrono::seconds(1));if(stop_monitor)break;
        try{std::lock_guard guard(state_mutex);state["guest_swap_calls"]=std::to_string(g_bo2zm_frames_game.load());write_locked();}catch(...){} }
    });
  }
  void OnPreLaunchModule()override{flag("guest_launch_requested",true);phase("game_prepare_guest_thread");}
  void OnPostLaunchModule(rex::system::XThread*)override{flag("guest_thread_prepared",true);phase("game_guest_thread_prepared");}
  void OnShutdown()override{stop();phase("game_shutdown");GameApp::OnShutdown();bo2zm::android::fault::Stop();}
};
}


extern "C" __attribute__((visibility("default"))) bool bo2_android_request_relaunch(const char* program) {
  if(!vm||!game_activity_class||!request_relaunch_method||!program)return false;
  JNIEnv* env=nullptr;bool attached=false;
  if(vm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6)!=JNI_OK) {

    const auto attach=[](auto* machine,JNIEnv** out) {
      if constexpr(requires{machine->AttachCurrentThread(out,nullptr);})return machine->AttachCurrentThread(out,nullptr);
      else return machine->AttachCurrentThread(reinterpret_cast<void**>(out),nullptr);
    };
    if(attach(vm,&env)!=JNI_OK)return false;
    attached=true;
  }
  jstring name=env->NewStringUTF(program);
  jboolean accepted=name?env->CallStaticBooleanMethod(game_activity_class,request_relaunch_method,name):JNI_FALSE;
  if(env->ExceptionCheck()){env->ExceptionClear();accepted=JNI_FALSE;}
  if(name)env->DeleteLocalRef(name);
  if(attached)vm->DetachCurrentThread();
  try{text("relaunch_requested",program);flag("relaunch_accepted",accepted==JNI_TRUE);phase("game_relaunch_requested");}catch(...){}
  return accepted==JNI_TRUE;
}


REX_HOOK_RAW(xstart) {
  try{text("status","running");flag("game_entry_executed",true);phase("game_guest_entry_entered");}catch(...){}
  __imp__xstart(ctx,base);
  try{phase("game_guest_entry_returned");}catch(...){}
}

extern "C" JNIEXPORT void JNICALL
Java_org_bo2port_diagnostic_GameActivity_nativeTouchState(JNIEnv*,jobject,jint mask,jint lt,jint rt,jint lx,jint ly,jint rx,jint ry) {
  touch.Set(mask,lt,rt,lx,ly,rx,ry);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_bo2port_diagnostic_GameActivity_nativeTouchRelativeAvailable(JNIEnv*,jobject) { return JNI_TRUE; }

extern "C" JNIEXPORT void JNICALL
Java_org_bo2port_diagnostic_GameActivity_nativeTouchLook(JNIEnv*,jobject,jint action,jint pointer,jfloat x,jfloat y,jfloat width,jfloat height) {
  if (action == 3 || !bo2zm::preset::ClientInGame() || bo2zm::preset::FrontEnd()) {
    bo2zm::android::RelativeTouch().Cancel(); return;
  }
  bo2zm::android::RelativeTouch().Touch(action,pointer,x,y,width,height);
}

extern "C" JNIEXPORT void JNICALL
Java_org_bo2port_diagnostic_GameActivity_nativeGpuProfileForeground(JNIEnv*,jobject,jboolean on) {
  bo2zm::native::pass_profile::Foreground(on==JNI_TRUE);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_bo2port_diagnostic_GameActivity_nativePixelProbeCommand(JNIEnv*,jobject,jint command) {
  if (command == 1 || command == 3) {
    const bool client_in_game=bo2zm::preset::ClientInGame();
    const auto map=bo2zm::preset::LoadedMap();
    const auto kind=command==3?bo2zm::native::pixel_probe::Kind::kShader:bo2zm::native::pixel_probe::Kind::kScissor;
    if(command==3 && !bo2zm::native::pass_profile::supported.load(std::memory_order_relaxed))return JNI_FALSE;
    if(!bo2zm::native::pixel_probe::Start(client_in_game,!map.empty(),bo2zm::native::SwapsNative(),kind))return JNI_FALSE;
    text("pixel_probe_kind",bo2zm::native::pixel_probe::KindName(kind));
    flag("pixel_probe_start_client_in_game",client_in_game);
    flag("pixel_probe_start_map_notified",!map.empty());
    text("pixel_probe_start_map",map);
    text("pixel_probe_start_mode",rex::cvar::GetFlagByName("bo2zm_test_mode"));
    return JNI_TRUE;
  }
  bo2zm::native::pixel_probe::Cancel(command == 2 ? "background_or_exit" : "user");
  return JNI_TRUE;
}
extern "C" JNIEXPORT jstring JNICALL
Java_org_bo2port_diagnostic_GameActivity_nativePixelProbeSnapshot(JNIEnv* env,jobject) {
  const auto snapshot=bo2zm::native::pixel_probe::Read();
  auto value=bo2zm::native::pixel_probe::Json(snapshot);value.pop_back();
  value+=",\"saved\":"+std::string(snapshot.trial&&pixel_probe_saved_trial.load(std::memory_order_acquire)==snapshot.trial?"true":"false")+'}';
  return env->NewStringUTF(value.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_bo2port_diagnostic_GameActivity_nativePerformanceSnapshot(JNIEnv* env,jobject) {
  try {
  const auto gpu=bo2zm::native::performance::Read();
  std::ostringstream out;out.imbue(std::locale::classic());
  out << "{\"guest_swaps_total\":" << g_bo2zm_frames_game.load(std::memory_order_relaxed)
      << ",\"native_swaps_total\":" << bo2zm::native::SwapsNative()
      << ",\"client_in_game\":" << (bo2zm::preset::ClientInGame()?"true":"false")
      << ",\"gpu_window_available\":" << (gpu.sampled_ns?"true":"false")
      << ",\"gpu_window_id\":\"" << gpu.sampled_ns << '"'
      << ",\"gpu_report_thread_tid\":" << (gpu.report_thread_tid?std::to_string(gpu.report_thread_tid):"null")
      << ",\"gpu_window_age_ms\":" << (gpu.sampled_ns?std::to_string((bo2zm::native::performance::NowNs()-gpu.sampled_ns)/1000000):"null")
      << ",\"gpu_window_seconds\":" << gpu.interval_seconds
      << ",\"gpu_window_swaps\":" << gpu.swaps << ",\"gpu_jobs_measured\":" << gpu.jobs
      << ",\"gpu_jobs_with_category_marks\":" << gpu.category_jobs
      << ",\"gpu_timing_mode\":\"" << (!gpu.jobs?"unavailable":!gpu.category_jobs?"endpoints":gpu.category_jobs==gpu.jobs?"categories":"mixed") << '"'
      << ",\"gpu_ms_per_renderer_swap\":" << (gpu.swaps&&gpu.jobs?std::to_string(gpu.gpu_ms_per_swap):"null")
      << ",\"gpu_between_jobs_ms_per_renderer_swap\":" << (gpu.swaps&&gpu.jobs?std::to_string(gpu.gpu_gap_ms_per_swap):"null")
      << ",\"gpu_between_jobs_scope\":\"unmarked GPU-timeline interval; not a hardware idle percentage\""
      << ",\"ring_gpu_wait_ms_per_renderer_swap\":" << (gpu.swaps?std::to_string(gpu.gpu_wait_ms_per_swap):"null")
      << ",\"gpu_metric_scope\":\"existing Vulkan timed jobs; not a hardware utilization percentage\""
      << ",\"cumulative_wall_ns\":{";
  using namespace bo2zm::waits;
  const std::pair<const char*,uint64_t> metrics[]={
    {"ring_work",g_ns_ring_working.load()}, {"textures",g_ns_textures.load()},
    {"hash_raw",g_ns_hash_raw.load()}, {"hash_decoded",g_ns_hash_data.load()},
    {"wait_vertex_copies",g_ns_waiting_copies.load()}, {"create_textures",g_ns_create_textures.load()},
    {"wait_bindings",g_ns_waiting_bindings.load()}, {"wait_hashes",g_ns_waiting_hashes.load()}};
  bool comma=false;for(const auto&[name,value]:metrics){if(comma)out<<',';comma=true;out<<'"'<<name<<"\":"<<value;}
  const char* wait_names[]={"executor_handoff","preparer_handoff","ring_space","game_mid_call",
      "ring_no_job","ring_register_memory","executor_no_orders","preparer_list","preparer_out","preparer_scenery"};
  static_assert(sizeof(wait_names)/sizeof(*wait_names)==kNumTypes);
  out << "},\"game_waits_wall_ns\":{";
  for(uint32_t i=0;i<kNumTypes;++i){if(i)out<<',';out<<'"'<<wait_names[i]<<"\":"<<g_ns[i].load();}
  out << "},\"texture_uploads_total\":" << g_textures_uploads.load()
      << ",\"texture_upload_bytes_total\":" << g_bytes_uploaded.load()
      << ",\"texture_hash_bytes_total\":" << g_bytes_hash.load()
      << ",\"gpu_pass_profile\":" << bo2zm::native::pass_profile::Json()
      << ",\"draw_batch\":" << bo2zm::native::draw_batch::Json()
      << ",\"scene_transfer_barriers_original\":"
      << bo2zm::native::scene::transfer_barriers_original.load(std::memory_order_relaxed)
      << ",\"scene_transfer_barriers_experimental\":"
      << bo2zm::native::scene::transfer_barriers_experimental.load(std::memory_order_relaxed)
      << ",\"render_counters\":" << bo2zm::native::performance::CountersJson()


      << ",\"display\":" << bo2zm::display::Json()
      << ",\"adpf\":" << bo2zm::android_perf::Json()
      << ",\"container_read_cache\":" << ContainerReadCacheJson()


      << ",\"runtime_shaders\":" << bo2zm::native::RuntimeShadersJson()
      << ",\"pixel_probe\":" << bo2zm::native::pixel_probe::Json(bo2zm::native::pixel_probe::Read()) << '}';
  return env->NewStringUTF(out.str().c_str());
  }catch(...){return env->NewStringUTF("{\"unavailable\":true}");}
}
extern "C" JNIEXPORT jboolean JNICALL
Java_org_bo2port_diagnostic_GameActivity_nativeConfigureGame(JNIEnv* env,jobject,jint header,jintArray parts,jint update,jintArray dlc,jobjectArray names,jstring game,jstring private_files,jstring libraries,jstring run,jboolean detailed_marks,jboolean batch_draws,jboolean profile_passes,
                                                         jboolean transfer_dst_trial,jboolean disable_pass,
                                                         jboolean disable_transfers,jboolean disable_output_read,jint scene_pass_mode,
                                                         jboolean disable_async_pipelines) {
  try {
    require(!configured,"Game session already configured");env->GetJavaVM(&vm);files=utf(env,private_files);bootstrap=utf(env,game);
    if(jclass activity=env->FindClass("org/bo2port/diagnostic/GameActivity")) {
      game_activity_class=static_cast<jclass>(env->NewGlobalRef(activity));
      request_relaunch_method=env->GetStaticMethodID(activity,"requestRelaunch","(Ljava/lang/String;)Z");
      env->DeleteLocalRef(activity);
    }
    if(env->ExceptionCheck()){env->ExceptionClear();request_relaunch_method=nullptr;}
    require(files.is_absolute()&&bootstrap.is_absolute(),"Private game paths must be absolute");rex::ConfigureAndroidPaths(utf(env,libraries),files);
    const auto run_id=utf(env,run);
    require(run_id.size()==36&&run_id.find_first_not_of("0123456789abcdef-")==std::string::npos,"Invalid capture run ID");
    run_identifier=run_id;
    gpu_pass_profile_requested=profile_passes==JNI_TRUE;
    detailed_gpu_marks_requested=detailed_marks==JNI_TRUE && !gpu_pass_profile_requested;
    draw_batch_requested=batch_draws==JNI_TRUE;
    transfer_dst_trial_requested=transfer_dst_trial==JNI_TRUE;
    disable_scene_pass_dependencies_requested=disable_pass==JNI_TRUE;
    disable_scene_transfer_barriers_requested=disable_transfers==JNI_TRUE;
    disable_scene_output_read_barrier_requested=disable_output_read==JNI_TRUE;
    scene_pass_sync_mode_requested=scene_pass_mode;
    disable_async_pipelines_requested=disable_async_pipelines==JNI_TRUE;
    {std::lock_guard guard(state_mutex);state.clear();state["app_version"]=json_quote(BO2_ANDROID_APP_VERSION);state["run_id"]=json_quote(run_id);state["status"]=json_quote("starting");state["full_game_application_linked"]="true";state["native_renderer_linked"]="true";state["game_entry_executed"]="false";state["resources_read_only"]="true";}
    bo2zm::android::capture::Start(files/"shader-capture"/run_id);
    const auto count=env->GetArrayLength(parts);require(count>1&&count<=128,"Invalid fragment count");std::vector<jint> values(count);env->GetIntArrayRegion(parts,0,count,values.data());
    auto duplicate=[&](int fd){int own=fcntl(fd,F_DUPFD_CLOEXEC,0);require(own>=0,"Cannot retain game descriptor");fds.push_back(own);};
    duplicate(header);duplicate(update);for(int fd:values)duplicate(fd);
    require(dlc&&names,"Missing DLC descriptor arrays");const auto dlc_count=env->GetArrayLength(dlc);
    require(dlc_count<=32&&env->GetArrayLength(names)==dlc_count,"Invalid DLC descriptor count");
    std::vector<jint> dlc_values(dlc_count);if(dlc_count)env->GetIntArrayRegion(dlc,0,dlc_count,dlc_values.data());
    for(jsize i=0;i<dlc_count;++i) {
      auto name=static_cast<jstring>(env->GetObjectArrayElement(names,i));dlc_names.push_back(utf(env,name));env->DeleteLocalRef(name);
      int own=fcntl(dlc_values[i],F_DUPFD_CLOEXEC,0);require(own>=0,"Cannot retain DLC descriptor");dlc_fds.push_back(own);
    }
    {std::lock_guard guard(state_mutex);state["dlc_packages_found"]=std::to_string(dlc_count);}
    configured=true;phase("game_launch_configured");return JNI_TRUE;
  }catch(const std::exception& e){if(env->ExceptionCheck())env->ExceptionClear();text("error",e.what());try{phase("game_configuration_error");}catch(...){}for(int fd:fds)::close(fd);fds.clear();for(int fd:dlc_fds)::close(fd);dlc_fds.clear();dlc_names.clear();return JNI_FALSE;}
}


static void record_launch_arguments(int argc,char** argv){
  std::string joined,duplicates;std::vector<std::string> names;
  for(int i=1;i<argc;++i){
    const std::string argument=argv[i]?argv[i]:"";
    if(!joined.empty())joined+=' ';
    joined+=argument;
    if(argument.rfind("--",0)!=0)continue;
    const size_t eq=argument.find('=');
    const std::string name=argument.substr(2,eq==std::string::npos?std::string::npos:eq-2);
    bool seen=false;
    for(const std::string& known:names)if(known==name){seen=true;break;}
    if(!seen){names.push_back(name);continue;}
    if(duplicates.find(" "+name+" ")==std::string::npos&&duplicates.rfind(name+" ",0)!=0)duplicates+=name+' ';
  }
  text("launch_arguments",joined);
  text("launch_argument_duplicates",duplicates);
  const bool relaunch=rex::cvar::GetFlagByName("bo2zm_android_relaunch")=="true";
  flag("relaunch_start",relaunch);
  if((rex::cvar::GetFlagByName("bo2zm_test_mode").empty()&&!relaunch)||!duplicates.empty())
    text("launch_arguments_warning",duplicates.empty()
         ?"bo2zm_test_mode missing: the launcher's switches did not reach the game"
         :"repeated switches: the SDK's parser may have skipped launcher settings");
}

extern "C" __attribute__((visibility("default"))) int SDL_main(int argc,char**argv) {
  if(!configured)return EXIT_FAILURE;
  try {
    phase("game_platform_init");require(sysconf(_SC_PAGESIZE)==4096,"This game build requires 4-KiB host pages");
    rex::cvar::Init(argc,argv);rex::cvar::ApplyEnvironment();text("requested_mode",rex::cvar::GetFlagByName("bo2zm_test_mode"));
    record_launch_arguments(argc,argv);rex::InitLoggingEarly();rex::memory::AndroidInitialize();rex::thread::AndroidInitialize();
    rex::ui::SDLWindowedAppContext context;require(context.Initialize(),"SDL game context initialization failed");
    AndroidBo2zmApp app(context);phase("game_application_initialize");bool initialized=static_cast<rex::ui::WindowedApp&>(app).OnInitialize();flag("application_initialized",initialized);
    int result=initialized?context.RunMainMessageLoop():EXIT_FAILURE;app.InvokeOnDestroy();stop();text("status",result==EXIT_SUCCESS?"stopped":"error");phase("game_process_loop_finished");rex::FlushLogging();return result;
  }catch(const std::exception& e){if(vm){JNIEnv* env=nullptr;if(vm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6)==JNI_OK&&env->ExceptionCheck())env->ExceptionClear();}
    stop();text("status","error");text("error",e.what());try{phase("game_boot_error");}catch(...){}rex::FlushLogging();return EXIT_FAILURE;}
}
