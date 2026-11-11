// SPDX-License-Identifier: GPL-3.0-only

#include <jni.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <rex/android.h>
#include <rex/atomic_ref.h>
#include <rex/image_info.h>
#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xex_module.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>
#include <rex/thread/fiber.h>
#include <rex/thread/timer_queue.h>

extern const rex::PPCImageInfo PPCImageConfig;
namespace rex::memory { void AndroidInitialize(); }
namespace rex::thread { void AndroidInitialize(); }

namespace {
using namespace std::chrono_literals;
using rex::X_STATUS;
std::mutex probe_mutex;
std::once_flag sdk_initialized;
std::string quote(const std::string& value) {
  std::string out = "\"";
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if (c < 32) { const char* h="0123456789abcdef"; out += "\\u00"; out+=h[c>>4];out+=h[c&15]; }
    else out += static_cast<char>(c);
  }
  return out + '"';
}
struct Report {
  JNIEnv* env; jobject activity; std::string checkpoint;
  std::map<std::string,std::string> values;
  void text(const std::string& key,const std::string& value) { values[key]=quote(value); }
  void flag(const std::string& key,bool value) { values[key]=value?"true":"false"; }
  void number(const std::string& key,uint64_t value) { values[key]=std::to_string(value); }
  std::string json() const {
    std::string out="{";bool first=true;
    for(const auto& [k,v]:values) { if(!first)out+=',';first=false;out+=quote(k)+':'+v; }
    return out+'}';
  }
  void phase(const std::string& name) {
    text("phase",name);const auto data=json();
    { std::ofstream f(checkpoint);f<<data;f.flush(); }
    jclass cls=env->GetObjectClass(activity);
    auto method=env->GetMethodID(cls,"onRuntimeReport","(Ljava/lang/String;)V");
    jstring message=env->NewStringUTF(data.c_str());
    if(method)env->CallVoidMethod(activity,method,message);
    env->DeleteLocalRef(message);env->DeleteLocalRef(cls);
    if(env->ExceptionCheck()) {env->ExceptionClear();throw std::runtime_error("Runtime progress callback failed");}
  }
};
std::string string(JNIEnv* env,jstring value) {
  const char* text=env->GetStringUTFChars(value,nullptr);if(!text)throw std::runtime_error("JNI string unavailable");
  std::string copy=text;env->ReleaseStringUTFChars(value,text);return copy;
}
void require(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
struct FiberCheck { rex::thread::Fiber* root; int step=0;bool done=false,valid=true; };
void fiber_entry(void* argument) {
  auto& state=*static_cast<FiberCheck*>(argument);
  for(int i=1;i<=64;++i) {
    volatile uint64_t retained=0x1234567800000000ull+static_cast<uint64_t>(i);
    state.step=i;rex::thread::Fiber::SwitchTo(state.root);
    state.valid &= retained==0x1234567800000000ull+static_cast<uint64_t>(i);
  }
  state.done=true;rex::thread::Fiber::SwitchTo(state.root);
  std::abort();
}
void test_fibers(Report& r) {
  auto* root=rex::thread::Fiber::ConvertCurrentThread();require(root,"Cannot convert thread to fiber");
  FiberCheck state{root};auto* child=rex::thread::Fiber::Create(256*1024,fiber_entry,&state);
  if(!child) { root->Destroy();throw std::runtime_error("Cannot create fiber"); }
  for(int i=1;i<=64;++i) { rex::thread::Fiber::SwitchTo(child);state.valid &= state.step==i; }
  rex::thread::Fiber::SwitchTo(child);state.valid &= state.done;
  child->Destroy();root->Destroy();r.flag("fibers.passed",state.valid);r.number("fibers.round_trips",65);
  require(state.valid,"Fiber state was not preserved");
}
void test_waits(Report& r) {
  auto event=rex::thread::Event::CreateAutoResetEvent(false);require(bool(event),"Cannot create SDK event");
  alignas(4) uint32_t word=0;uint32_t payload=0;
  std::thread publisher([&] {payload=0x1234ABCD;rex::atomic_ref<uint32_t>(word).store(1,std::memory_order_release);event->Set();});
  const auto result=rex::thread::Wait(event.get(),false,3s);publisher.join();
  bool ok=result==rex::thread::WaitResult::kSuccess && rex::atomic_ref<uint32_t>(word).load(std::memory_order_acquire)==1 && payload==0x1234ABCD;
  r.flag("event_atomic.passed",ok);require(ok,"SDK event/acquire-release test failed");
  std::atomic<bool> fired{false};
  auto timer=rex::thread::QueueTimerOnce([&](void*) {fired.store(true,std::memory_order_release);event->Set();},nullptr,std::chrono::steady_clock::now()+10ms);
  const auto wait=rex::thread::Wait(event.get(),false,3s);
  if(auto item=timer.lock())item->Disarm();
  ok=wait==rex::thread::WaitResult::kSuccess && fired.load(std::memory_order_acquire);
  r.flag("timer.passed",ok);require(ok,"SDK timer test failed");
}
void test_aliases(rex::Runtime& runtime,Report& r) {
  auto* memory=runtime.memory();const uint32_t guest=memory->SystemHeapAlloc(4096,32,rex::memory::kSystemHeapPhysical);
  require(guest>=0xE0000000u,"No physical allocation for alias test");
  const uint32_t physical=(guest&0x1FFFFFFFu)+0x1000u;
  auto* a=memory->TranslateVirtual<volatile uint32_t*>(guest);
  auto* b=memory->TranslateVirtual<volatile uint32_t*>(0xA0000000u+physical);
  *a=0x13572468;bool ok=*b==0x13572468;*b=0xFEDCBA98;ok &= *a==0xFEDCBA98;
  memory->SystemHeapFree(guest);r.flag("runtime_memory.aliases_passed",ok);require(ok,"Real SDK physical aliases differ");
}
std::string image_hash(JNIEnv* env,jobject activity,void* data,size_t bytes) {
  jclass cls=env->GetObjectClass(activity);
  auto method=env->GetStaticMethodID(cls,"digestMemory","(Ljava/nio/ByteBuffer;)Ljava/lang/String;");
  jobject buffer=env->NewDirectByteBuffer(data,static_cast<jlong>(bytes));
  auto digest=static_cast<jstring>(env->CallStaticObjectMethod(cls,method,buffer));
  if(env->ExceptionCheck() || !digest) {env->ExceptionClear();throw std::runtime_error("Cannot hash loaded image");}
  auto result=string(env,digest);env->DeleteLocalRef(digest);env->DeleteLocalRef(buffer);env->DeleteLocalRef(cls);return result;
}
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_bo2port_diagnostic_DiagnosticActivity_nativeProbeRuntime(JNIEnv* env,jobject activity,jstring game_dir,jstring files_dir,jstring library_dir) {
  std::lock_guard<std::mutex> guard(probe_mutex);
  const auto files=string(env,files_dir),game=string(env,game_dir),libraries=string(env,library_dir);
  Report report{env,activity,files+"/runtime-checkpoint.json",{}};
  report.flag("runtime_linked",true);report.flag("recompiled_game_linked",true);
  report.flag("game_entry_executed",false);report.flag("renderer_integrated",false);
  report.text("scope","Real SDK + complete generated module; tool-mode loading and isolated checks only");
  report.number("generated_translation_units",137);report.number("host_page_bytes",static_cast<uint64_t>(sysconf(_SC_PAGESIZE)));
  std::unique_ptr<rex::Runtime> runtime;
  const std::string log_path=files+"/runtime-sdk.log";
  try {
    report.phase("libraries_loaded");
    require(sysconf(_SC_PAGESIZE)==4096,"This runtime build currently requires 4-KB host pages");
    rex::ConfigureAndroidPaths(libraries,files);
    std::call_once(sdk_initialized,[] {rex::InitLoggingEarly();rex::cvar::ApplyEnvironment();rex::memory::AndroidInitialize();rex::thread::AndroidInitialize();});
    rex::LogConfig config;config.log_file=log_path.c_str();config.flush_level=spdlog::level::info;rex::InitLogging(config);
    test_fibers(report);test_waits(report);report.phase("platform_checks_passed");
    std::filesystem::create_directories(files+"/runtime-user");
    runtime=std::make_unique<rex::Runtime>(game,files+"/runtime-user",std::filesystem::path{},files+"/runtime-cache");
    report.phase("runtime_setup_started");
    auto status=runtime->Setup(PPCImageConfig,rex::RuntimeConfig{.kernel_init=rex::kernel::InitializeKernel,.tool_mode=true});
    report.number("setup_status",status);require(status==X_STATUS_SUCCESS,"Runtime::Setup failed");
    report.flag("runtime_setup_passed",true);test_aliases(*runtime,report);
    size_t count=0;for(auto* m=PPCImageConfig.func_mappings;m->guest;++m)++count;
    report.number("registered_function_mappings",count);report.phase("xex_loading_started");
    status=runtime->LoadXexImage("game:\\default_mp.xex");report.number("xex_load_status",status);
    require(status==X_STATUS_SUCCESS,"Runtime::LoadXexImage failed");
    auto module=runtime->kernel_state()->GetExecutableModule();require(bool(module),"Loaded executable module missing");
    const auto* xex=module->xex_module();const uint32_t bytes=xex->image_size();
    report.number("image_bytes",bytes);report.number("entry_point",xex->entry_point());
    auto hash=image_hash(env,activity,runtime->memory()->TranslateVirtual(xex->base_address()),bytes);
    report.text("image_sha256",hash);
    bool matches=bytes==47054848 && hash=="ecb2d7ce653be4c4fc8945e0083e222570266b5a770896819427be69d463f72e";
    report.flag("tu18_image_matches",matches);require(matches,"Loaded image differs from the verified TU18 reference");
    report.phase("tu18_loaded_verified");

    auto* function=runtime->function_dispatcher()->GetFunction(0x8277D4C8);
    require(function,"Isolated compiled function is not registered");
    PPCContext context{};context.r3.u64=0;function(context,runtime->virtual_membase());
    report.number("compiled_leaf.address",0x8277D4C8);report.number("compiled_leaf.r3",context.r3.u64);
    report.flag("compiled_leaf.passed",context.r3.u64==16);require(context.r3.u64==16,"Compiled leaf returned an unexpected value");
    module.reset();report.phase("runtime_shutdown_started");runtime.reset();report.flag("runtime_shutdown_passed",true);
    report.flag("passed",true);report.phase("runtime_probe_complete");
  } catch(const std::exception& error) {
    report.text("error",error.what());report.flag("passed",false);
    report.phase("runtime_probe_error");runtime.reset();
  }
  rex::FlushLogging();
  {std::ifstream file(log_path);std::ostringstream data;data<<file.rdbuf();auto log=data.str();if(log.size()>20000)log=log.substr(log.size()-20000);report.text("sdk_log_tail",log);}
  const auto result=report.json();{std::ofstream file(report.checkpoint);file<<result;}
  return env->NewStringUTF(result.c_str());
}
