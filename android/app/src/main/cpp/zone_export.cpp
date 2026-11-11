// SPDX-License-Identifier: GPL-3.0-only

#include <jni.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/file.h>
#include "dlc_package.h"
#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace rex::filesystem;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
std::string utf(JNIEnv* env,jstring value){
  require(value,"Falta un nombre de paquete");const char* p=env->GetStringUTFChars(value,nullptr);
  require(p,"No se pudo leer un nombre de paquete");std::string out(p);env->ReleaseStringUTFChars(value,p);return out;
}
std::string quote(const std::string& s){
  std::string out="\"";for(unsigned char c:s){if(c=='"'||c=='\\'){out+='\\';out+=char(c);}
    else if(c<32){constexpr char h[]="0123456789abcdef";out+="\\u00";out+=h[c>>4];out+=h[c&15];}else out+=char(c);}
  return out+'"';
}
struct Zone{Entry* entry;std::string origin,path;};
void enumerate(Entry* root,const std::string& origin,std::vector<Zone>& zones,unsigned depth=0){
  require(root&&depth<=32,"Directorio de juego inválido o demasiado profundo");
  for(const auto& child:root->children()){
    if(child->attributes()&kFileAttributeDirectory){enumerate(child.get(),origin,zones,depth+1);continue;}
    auto n=child->name();for(char& c:n)if(c>='A'&&c<='Z')c=char(c-'A'+'a');
    if(!n.ends_with(".ff"))continue;
    require(zones.size()<8192&&child->path().size()<=1024,"Inventario de zonas fuera de límites");
    require(child->size()>0&&child->size()<=size_t(1024)*1024*1024,"Fastfile vacío o mayor de 1 GiB");
    zones.push_back({child.get(),origin,child->path()});
  }
}
struct Closer{void operator()(File* file)const{if(file)file->Destroy();}};
struct Sink {
  JNIEnv* env;jobject object;jclass cls;jbyteArray buffer;
  jmethodID check_start,check_end,data,inventory,zone_start,zone_end;
  Sink(JNIEnv* e,jobject o):env(e),object(o){
    require(object,"Falta el destino de exportación");cls=env->GetObjectClass(object);
    check_start=env->GetMethodID(cls,"onCheckStart","(Ljava/lang/String;J)V");
    check_end=env->GetMethodID(cls,"onCheckEnd","()V");
    data=env->GetMethodID(cls,"onData","([BI)V");
    inventory=env->GetMethodID(cls,"onInventory","(Ljava/lang/String;IJ)V");
    zone_start=env->GetMethodID(cls,"onZoneStart","(ILjava/lang/String;Ljava/lang/String;J)V");
    zone_end=env->GetMethodID(cls,"onZoneEnd","()V");
    require(!env->ExceptionCheck()&&check_start&&check_end&&data&&inventory&&zone_start&&zone_end,"Interfaz de exportación JNI incompleta");
    buffer=env->NewByteArray(128*1024);require(buffer,"No se pudo reservar el buffer de exportación");
  }
  ~Sink(){env->DeleteLocalRef(buffer);env->DeleteLocalRef(cls);}
  void checked(){require(!env->ExceptionCheck(),"La exportación se interrumpió en el destino");}
  void copy(Entry* entry){
    require(entry&&!(entry->attributes()&kFileAttributeDirectory),"Archivo de origen ausente");
    File* raw=nullptr;require(entry->Open(FileAccess::kFileReadData,&raw)==0&&raw,"No se pudo abrir un archivo del juego");
    std::unique_ptr<File,Closer> file(raw);std::array<uint8_t,128*1024> bytes{};
    for(size_t at=0;at<entry->size();){
      const size_t n=std::min(bytes.size(),entry->size()-at);size_t got=0;
      require(file->ReadSync({bytes.data(),n},at,&got)==0&&got==n,"Lectura incompleta de un archivo del juego");
      env->SetByteArrayRegion(buffer,0,jint(n),reinterpret_cast<const jbyte*>(bytes.data()));checked();
      env->CallVoidMethod(object,data,buffer,jint(n));checked();at+=n;
    }
  }
  void verify(Entry* entry,const char* kind){
    require(entry&&entry->size()>0&&entry->size()<=32*1024*1024,"Ejecutable/parche fuera de límites");
    auto text=env->NewStringUTF(kind);env->CallVoidMethod(object,check_start,text,jlong(entry->size()));env->DeleteLocalRef(text);checked();
    copy(entry);env->CallVoidMethod(object,check_end);checked();
  }
};
}

extern "C" JNIEXPORT void JNICALL
Java_org_bo2port_diagnostic_DiagnosticActivity_nativeExportZones(JNIEnv* env,jobject,jint header,jintArray fragments,
    jint patch,jintArray dlc,jobjectArray names,jobject output){
  try{
    Sink sink(env,output);
    require(fragments&&dlc&&names,"Descriptores incompletos");
    const auto n=env->GetArrayLength(fragments),nd=env->GetArrayLength(dlc);
    require(n>1&&n<=128&&nd<=32&&nd==env->GetArrayLength(names),"Cantidad de paquetes fuera de límites");
    std::vector<jint> parts(n),extras(nd);env->GetIntArrayRegion(fragments,0,n,parts.data());
    if(nd)env->GetIntArrayRegion(dlc,0,nd,extras.data());sink.checked();
    std::vector<std::unique_ptr<StfsContainerDevice>> devices;
    auto base=std::make_unique<StfsContainerDevice>("\\Device\\BO2ZoneBase","[saf-base]");
    auto update=std::make_unique<StfsContainerDevice>("\\Device\\BO2ZoneUpdate","[saf-update]");
    require(base->InitializeFromFileDescriptors(header,parts)&&update->InitializeFromFileDescriptors(patch,{}),"No se pudieron montar juego/TU18");
    using bo2zm::android::DlcHeaderValue;
    const auto& bh=base->header();const auto& uh=update->header();
    require(DlcHeaderValue(&bh.metadata.execution_info.title_id)==0x415608C3&&
        DlcHeaderValue(&uh.metadata.execution_info.title_id)==0x415608C3&&
        DlcHeaderValue(&bh.metadata.execution_info.media_id)==DlcHeaderValue(&uh.metadata.execution_info.media_id),"Juego y actualización no coinciden");
    require(base->is_read_only()&&update->is_read_only(),"Los orígenes deben ser de solo lectura");
    sink.verify(base->ResolvePath("default_mp.xex"),"base");sink.verify(update->ResolvePath("default_mp.xexp"),"patch");
    std::vector<Zone> zones;enumerate(base->ResolvePath(""),"game",zones);enumerate(update->ResolvePath(""),"update",zones);
    devices.push_back(std::move(base));devices.push_back(std::move(update));
    for(jint i=0;i<nd;++i){
      auto text=static_cast<jstring>(env->GetObjectArrayElement(names,i));const auto name=utf(env,text);env->DeleteLocalRef(text);
      auto package=bo2zm::android::OpenDlcPackage(extras[i],name,"\\Device\\BO2ZoneDLC"+std::to_string(i),0x415608C3);
      require(package.device->is_read_only(),"DLC no montado como solo lectura");
      enumerate(package.device->ResolvePath(""),"dlc/"+name,zones);devices.push_back(std::move(package.device));
    }
    require(!zones.empty(),"No se encontraron archivos .ff");
    std::sort(zones.begin(),zones.end(),[](const auto& a,const auto& b){return a.origin<b.origin||(a.origin==b.origin&&a.path<b.path);});
    uint64_t total=0;std::string inventory="{\"scope\":\"all-fastfiles-in-selected-base-update-dlc\",\"dlc_packages\":"+std::to_string(nd)+",\"zones\":[";
    for(size_t i=0;i<zones.size();++i){const auto& z=zones[i];
      require(z.entry->size()<=(uint64_t(16)<<30)-total,"Las zonas superan 16 GiB");total+=z.entry->size();
      if(i)inventory+=',';
      inventory+="{\"id\":"+std::to_string(i)+",\"origin\":"+quote(z.origin)+",\"path\":"+quote(z.path)+",\"bytes\":"+std::to_string(z.entry->size())+"}";
    }
    inventory+="]}";require(inventory.size()<=2*1024*1024,"Inventario demasiado grande");
    auto inv=env->NewStringUTF(inventory.c_str());sink.checked();
    env->CallVoidMethod(output,sink.inventory,inv,jint(zones.size()),jlong(total));env->DeleteLocalRef(inv);sink.checked();
    for(size_t i=0;i<zones.size();++i){const auto& z=zones[i];auto origin=env->NewStringUTF(z.origin.c_str());auto path=env->NewStringUTF(z.path.c_str());
      env->CallVoidMethod(output,sink.zone_start,jint(i),origin,path,jlong(z.entry->size()));env->DeleteLocalRef(origin);env->DeleteLocalRef(path);sink.checked();
      sink.copy(z.entry);env->CallVoidMethod(output,sink.zone_end);sink.checked();
    }
  }catch(const std::exception& e){

    if(!env->ExceptionCheck()){auto cls=env->FindClass("java/io/IOException");if(cls){env->ThrowNew(cls,e.what());env->DeleteLocalRef(cls);}}
  }
}
