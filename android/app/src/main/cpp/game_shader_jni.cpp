// SPDX-License-Identifier: GPL-3.0-only
#include "native/bo2zm_native_vulkan_profile.h"
#include "game_shader_probe.h"
#include <jni.h>
#include <rex/logging.h>
#include <spdlog/sinks/ringbuffer_sink.h>
#include <map>
#include <stdexcept>

namespace {
std::string quote(const std::string& value) {
  std::string out="\"";
  for(unsigned char c:value) {
    if(c=='"'||c=='\\'){out+='\\';out+=char(c);}
    else if(c<32){constexpr char h[]="0123456789abcdef";out+="\\u00";out+=h[c>>4];out+=h[c&15];}
    else out+=char(c);
  }
  return out+'"';
}
struct Report {
  std::map<std::string,std::string> values;
  void text(const char* k,const std::string& v){values[k]=quote(v);}
  void flag(const char* k,bool v){values[k]=v?"true":"false";}
  void number(const char* k,int64_t v){values[k]=std::to_string(v);}
  std::string serialize() const {std::string s="{";for(auto&[k,v]:values){if(s.size()>1)s+=',';s+=quote(k)+':'+v;}return s+'}';}
};
void update(Report& out,const GameShaderProbeResult& r) {
  out.flag("passed",r.passed);out.flag("index_loaded",r.index_loaded);out.flag("pack_verified",r.pack_verified);
  out.flag("objects_destroyed",r.objects_destroyed);out.number("expected_shaders",r.expected);
  out.number("modules_created",r.modules_created);out.number("pipelines_created",r.pipelines_created);
  out.number("vertex_pipelines",r.vertex);out.number("pixel_pipelines",r.pixel);
  out.number("current_shader_index",r.current);out.text("current_shader_stage",r.stage);
  out.text("phase",r.phase);out.number("vk_result",r.vk_result);out.number("compile_elapsed_ms",r.elapsed_ms);
  if(!r.error.empty())out.text("error",r.error);
}
void checkpoint(JNIEnv* env,jobject activity,Report& r) {
  auto cls=env->GetObjectClass(activity);auto method=env->GetMethodID(cls,"onGameShaderReport","(Ljava/lang/String;)V");
  auto text=env->NewStringUTF(r.serialize().c_str());if(method&&text)env->CallVoidMethod(activity,method,text);
  if(text)env->DeleteLocalRef(text);env->DeleteLocalRef(cls);
  if(env->ExceptionCheck()){env->ExceptionClear();throw std::runtime_error("Shader checkpoint callback failed");}
}
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_bo2port_diagnostic_DiagnosticActivity_nativeProbeGameShaders(JNIEnv* env,jobject activity,jstring directory,jint expected) {
  Report out;out.text("status","working");out.text("phase","game_shaders_create_provider");
  out.text("scope","Real pack/index readers and SDK Vulkan device; real game stages with authored complements; pipeline compilation only");
  out.flag("game_entry_executed",false);out.flag("draw_submitted",false);out.flag("full_game_renderer_linked",false);
  out.flag("provider_destroyed",false);out.flag("passed",false);
  auto sink=std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(32);sink->set_level(spdlog::level::warn);
  rex::InitLoggingEarly();rex::AddSink(sink);
  try {
    checkpoint(env,activity,out);
    const char* chars=env->GetStringUTFChars(directory,nullptr);if(!chars)throw std::runtime_error("No staged shader directory");
    std::filesystem::path path(chars);env->ReleaseStringUTFChars(directory,chars);
    if(!path.is_absolute())throw std::runtime_error("Shader staging path is not absolute");
    auto provider=bo2zm::native::CreateNativeVulkanProvider(false);
    if(!provider)throw std::runtime_error("SDK Vulkan provider creation failed");
    out.flag("provider_created",true);auto* device=provider->vulkan_device();const auto& p=device->properties();
    out.text("gpu",p.deviceName);out.flag("ubo_only",device->native_ubo_only());
    out.flag("shaderInt64_enabled",p.shaderInt64);out.flag("vertexPipelineStoresAndAtomics_enabled",p.vertexPipelineStoresAndAtomics);
    out.flag("scalarBlockLayout_enabled",p.scalarBlockLayout);
    if(!device->native_ubo_only()||p.shaderInt64||p.vertexPipelineStoresAndAtomics||!p.scalarBlockLayout)
      throw std::runtime_error("SDK enabled an incompatible shader feature profile");
    auto result=ProbeGameShaderPipelines(device->physical_device(),device->device(),path/"bo2zm_shaders.spvpack",
      [&](const auto& progress){update(out,progress);checkpoint(env,activity,out);});
    if(result.expected!=uint32_t(expected)){result.passed=false;result.error="Manifest and native shader counts differ";}
    update(out,result);out.flag("passed",false);out.text("phase","game_shaders_destroy_provider");
    checkpoint(env,activity,out);
    provider.reset();out.flag("provider_destroyed",true);
    update(out,result);out.text("status",result.passed?"passed":"error");
  }catch(const std::exception& error) {
    if(env->ExceptionCheck())env->ExceptionClear();out.flag("passed",false);out.text("status","error");out.text("error",error.what());
  }
  rex::FlushLogging();rex::RemoveSink(sink);std::string warnings;
  for(const auto& message:sink->last_formatted()){if(warnings.size()+message.size()>8000)break;warnings+=message;}
  if(!warnings.empty())out.text("sdk_warnings",warnings);
  return env->NewStringUTF(out.serialize().c_str());
}
