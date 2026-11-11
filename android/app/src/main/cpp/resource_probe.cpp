// SPDX-License-Identifier: GPL-3.0-only

#include <jni.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <spdlog/sinks/ringbuffer_sink.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using rex::X_STATUS;
using namespace rex::filesystem;
std::string quoted(const std::string& value) {
  std::string out="\"";
  for(unsigned char c:value) {
    if(c=='"'||c=='\\') {out+='\\';out+=char(c);}
    else if(c<32) {constexpr char h[]="0123456789abcdef";out+="\\u00";out+=h[c>>4];out+=h[c&15];}
    else out+=char(c);
  }
  return out+'"';
}
struct Json {
  std::map<std::string,std::string> fields;
  void text(const std::string& key,const std::string& value) {fields[key]=quoted(value);}
  void number(const std::string& key,size_t value) {fields[key]=std::to_string(value);}
  void flag(const std::string& key,bool value) {fields[key]=value?"true":"false";}
  std::string str() const {std::string out="{";for(auto&[k,v]:fields) {if(out.size()>1)out+=',';out+=quoted(k)+':'+v;}return out+'}';}
};
std::string array(const std::vector<std::string>& items) {std::string out="[";for(auto&item:items) {if(out.size()>1)out+=',';out+=item;}return out+']';}
void require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
struct FileCloser {void operator()(File* file) const {if(file)file->Destroy();}};
using OwnedFile=std::unique_ptr<File,FileCloser>;
OwnedFile open(Entry* entry) {
  File* file=nullptr;require(entry && !(entry->attributes()&kFileAttributeDirectory),"Required file not found in SDK VFS");
  require(entry->Open(FileAccess::kFileReadData,&file)==0 && file,"Cannot open SDK file for reading");return OwnedFile(file);
}
void exact(File& file,std::span<uint8_t> bytes,size_t offset) {
  size_t count=0;require(file.ReadSync(bytes,offset,&count)==0&&count==bytes.size(),"Short or failed read through SDK VFS");
}
void copy(Entry* entry,const std::filesystem::path& output,size_t maximum) {
  require(entry && entry->size()>0 && entry->size()<=maximum,"File exceeds private preparation budget");
  auto file=open(entry);std::ofstream out(output,std::ios::binary|std::ios::trunc);require(bool(out),"Cannot create private staged file");
  std::array<uint8_t,128*1024> data{};
  for(size_t at=0;at<entry->size();) {size_t count=std::min(data.size(),entry->size()-at);exact(*file,{data.data(),count},at);out.write(reinterpret_cast<char*>(data.data()),count);require(bool(out),"Private staged write failed");at+=count;}
  out.close();require(bool(out),"Private staged close failed");
}
struct Zone {Entry* entry;std::string origin,guest,name;};
std::string lower(std::string text) {for(char&c:text)if(c>='A'&&c<='Z')c=char(c-'A'+'a');return text;}
void zones(Entry* root,const std::string& origin,std::vector<Zone>& out,unsigned depth=0) {
  require(root && depth<=32,"Invalid or deeply nested game directory");
  for(auto& item:root->children()) {
    if(item->attributes()&kFileAttributeDirectory)zones(item.get(),origin,out,depth+1);
    else if(lower(item->name()).ends_with(".ff")) {
      require(out.size()<4096,"Too many fastfiles in selected packages");
      out.push_back({item.get(),origin,origin+":\\"+item->path(),lower(item->name())});
    }
  }
}
int priority(const Zone& zone) {
  constexpr const char* names[]={"code_post_gfx_zm.ff","patch_ui_zm.ff","patch_zm.ff","frontend_patch.ff","patch.ff","common_zm.ff","weapon_attach_shared.ff","code_pre_gfx_zm.ff"};
  for(unsigned i=0;i<std::size(names);++i)if(zone.name==names[i])return int(i)*2+(zone.origin=="update"?0:1);
  return 100;
}
struct Progress {
  JNIEnv* env;jobject activity;Json& report;
  void operator()(const char* phase) {
    report.text("phase",phase);report.text("status","working");
    auto cls=env->GetObjectClass(activity);auto method=env->GetMethodID(cls,"onResourceReport","(Ljava/lang/String;)V");
    auto message=env->NewStringUTF(report.str().c_str());if(method)env->CallVoidMethod(activity,method,message);
    env->DeleteLocalRef(message);env->DeleteLocalRef(cls);
    if(env->ExceptionCheck()){env->ExceptionClear();throw std::runtime_error("Resource progress callback failed");}
  }
};
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_bo2port_diagnostic_DiagnosticActivity_nativePrepareResources(JNIEnv* env,jobject activity,jint game_fd,jintArray data_fds,jint update_fd,jstring directory) {
  Json report;report.flag("game_entry_executed",false);report.flag("full_game_app_linked",false);
  report.flag("patched_image_loaded_this_run",false);report.flag("private_game_resources",true);
  report.text("scope","Real SDK VFS, read-only SAF descriptors, exact executable copies and bounded encrypted menu-zone export; no game launch");
  auto capture=std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(48);capture->set_level(spdlog::level::warn);
  rex::InitLoggingEarly();rex::AddSink(capture);
  try {
    Progress progress{env,activity,report};progress("resources_open_descriptors");
    const auto count=env->GetArrayLength(data_fds);require(count>1&&count<=128,"Unsupported fragment count");
    std::vector<jint> values(count);env->GetIntArrayRegion(data_fds,0,count,values.data());
    require(!env->ExceptionCheck(),"Cannot read resource descriptors");
    const char* text=env->GetStringUTFChars(directory,nullptr);require(text,"No private staging directory");
    const std::filesystem::path output(text);env->ReleaseStringUTFChars(directory,text);
    require(output.is_absolute()&&std::filesystem::is_directory(output),"Invalid private staging directory");
    std::filesystem::create_directories(output/"checks");std::filesystem::create_directories(output/"zones");
    auto base=std::make_unique<StfsContainerDevice>("\\Device\\BO2Base","[saf-base]");
    progress("resources_mount_svod");require(base->InitializeFromFileDescriptors(game_fd,values),"SDK cannot mount the SVOD descriptors");
    auto update=std::make_unique<StfsContainerDevice>("\\Device\\BO2Update","[saf-update]");
    progress("resources_mount_stfs");require(update->InitializeFromFileDescriptors(update_fd,{}),"SDK cannot mount the update descriptor");
    require(base->header().metadata.execution_info.title_id==0x415608C3 && update->header().metadata.execution_info.title_id==0x415608C3,"Wrong game title in native package headers");
    require(base->header().metadata.execution_info.media_id==update->header().metadata.execution_info.media_id,"Base/update media IDs differ");
    report.flag("base_mounted",true);report.flag("update_mounted",true);
    report.flag("devices_read_only",base->is_read_only()&&update->is_read_only());report.number("fragment_count",count);
    VirtualFileSystem vfs;require(vfs.RegisterDevice(std::move(base))&&vfs.RegisterDevice(std::move(update)),"Cannot register SDK devices");
    require(vfs.RegisterSymbolicLink("game:","\\Device\\BO2Base")&&vfs.RegisterSymbolicLink("update:","\\Device\\BO2Update"),"Cannot bind game/update VFS links");
    progress("resources_verify_input_reads");
    copy(vfs.ResolvePath("game:\\default_mp.xex"),output/"checks/default_mp.xex",32*1024*1024);
    copy(vfs.ResolvePath("update:\\default_mp.xexp"),output/"checks/default_mp.xexp",32*1024*1024);
    report.flag("executable_copies_complete",true);
    std::vector<Zone> found;zones(vfs.ResolvePath("game:"),"game",found);zones(vfs.ResolvePath("update:"),"update",found);
    report.number("fastfiles_found",found.size());
    std::sort(found.begin(),found.end(),[](const auto&a,const auto&b){if(priority(a)!=priority(b))return priority(a)<priority(b);if(a.guest.size()!=b.guest.size())return a.guest.size()<b.guest.size();return a.guest<b.guest;});
    std::vector<std::string> catalogue,exports;size_t total=0;unsigned skipped=0;
    for(const auto&zone:found) {
      Json row;row.text("origin",zone.origin);row.text("guest_path",zone.guest);row.number("bytes",zone.entry->size());
      if(catalogue.size()<128)catalogue.push_back(row.str());
      if(priority(zone)==100)continue;
      if(zone.entry->size()<316||zone.entry->size()>28*1024*1024-total) {++skipped;continue;}
      report.text("current_file",zone.guest);progress("resources_inspect_boot_zone");
      auto input=open(zone.entry);std::array<uint8_t,312> head{};exact(*input,head,0);
      const std::string magic(reinterpret_cast<char*>(head.data()),8);
      const uint32_t version=(uint32_t(head[8])<<24)|(uint32_t(head[9])<<16)|(uint32_t(head[10])<<8)|head[11];
      require((magic=="TAffx100"||magic=="TAff0100")&&version==146,"Unsupported boot fastfile header");
      require(std::string(reinterpret_cast<char*>(head.data()+12),8)=="PHEEBs71","Boot fastfile auth header missing");
      const auto folder=output/"zones"/zone.origin;std::filesystem::create_directories(folder);

      const auto target=folder/zone.name;if(std::filesystem::exists(target))continue;
      report.text("current_file",zone.guest);progress("resources_export_boot_zone");
      copy(zone.entry,target,28*1024*1024-total);total+=zone.entry->size();
      row.text("file","zones/"+zone.origin+"/"+zone.name);row.text("magic",magic);row.number("version",version);exports.push_back(row.str());
    }
    report.fields["catalogue"]=array(catalogue);report.flag("catalogue_truncated",found.size()>catalogue.size());
    report.fields["exports"]=array(exports);report.number("exported_bytes",total);report.number("exported_files",exports.size());report.number("boot_zones_skipped_for_budget",skipped);
    report.flag("vfs_passed",true);require(!exports.empty(),"No boot fastfiles fit the 28 MiB private batch budget");
    report.flag("passed",true);report.text("phase","resources_native_complete");report.text("status","native_complete");
  } catch(const std::exception& error) {
    if(env->ExceptionCheck())env->ExceptionClear();report.flag("passed",false);report.text("phase","resources_error");report.text("status","error");report.text("error",error.what());
  }
  rex::FlushLogging();rex::RemoveSink(capture);
  std::string warnings;for(const auto&message:capture->last_formatted()){if(warnings.size()+message.size()>10000)break;warnings+=message;}
  if(!warnings.empty())report.text("sdk_warnings",warnings);
  return env->NewStringUTF(report.str().c_str());
}
