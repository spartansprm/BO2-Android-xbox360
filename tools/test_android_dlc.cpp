// SPDX-License-Identifier: GPL-3.0-only
#include "dlc_package.h"
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <thread>
#include <source_location>
using namespace rex::system;
using rex::X_RESULT;using rex::X_STATUS;
static void require(bool ok,const std::source_location& where=std::source_location::current()) {
  if(!ok)throw std::runtime_error("DLC check at "+std::to_string(where.line()));
}
int main(int argc,char** argv)try {
  require(argc==3);rex::InitLoggingEarly();const uint32_t title=0x415608C3;
  int fd=open(argv[1],O_RDONLY);require(fd>=0);
  auto package=bo2zm::android::OpenDlcPackage(fd,"ABCDEF0123456789","\\Device\\TestDLC",title);
  require(package.markers==std::vector<std::string>{"dlczm0.id"});require(package.content.license==0xA);
  require(package.header_license==0xA&&!package.mtx);
  require(package.content.data.display_name()==u"Authored DLC");
  require(package.device->is_read_only());close(fd);
  rex::filesystem::VirtualFileSystem vfs;require(vfs.RegisterDevice(std::move(package.device)));
  require(vfs.RegisterSymbolicLink("dlc:",package.content.device_path));
  auto* entry=vfs.ResolvePath("dlc:\\zm_test.ff");require(entry&&entry->size()==9000);
  rex::filesystem::File* file=nullptr;require(entry->Open(rex::filesystem::FileAccess::kFileReadData,&file)==0);
  std::vector<uint8_t> bytes(9000);size_t got=0;require(file->ReadSync(bytes,0,&got)==0&&got==9000);
  for(size_t i=0;i<bytes.size();++i)require(bytes[i]==uint8_t(i*17+9));
  require(file->WriteSync({bytes.data(),1},0,&got)==X_STATUS_ACCESS_DENIED);file->Destroy();
  xam::ReadOnlyContentCatalog catalog;require(catalog.Register(package.content));
  auto duplicate=package.content;duplicate.data.set_file_name("abcdef0123456789");require(!catalog.Register(duplicate));
  require(catalog.Find(title,XContentType::kMarketplaceContent,"abcdef0123456789")->license==0xA);
  require(!catalog.Find(title+1,XContentType::kMarketplaceContent,"ABCDEF0123456789"));
  require(catalog.List(title,XContentType::kSavedGame).empty());
  for(const auto* name:{"../ABC","a/b","a\\b","..",""}){auto p=package.content;p.data.set_file_name(name);require(!catalog.Register(p));}
  auto invalid=package.content;invalid.data.xuid=123;require(!catalog.Register(invalid));
  invalid=package.content;invalid.data.content_type=XContentType::kAvatarItem;require(!catalog.Register(invalid));
  for(unsigned i=1;i<32;++i){auto p=package.content;p.data.set_file_name("pack"+std::to_string(i));require(catalog.Register(p));}
  invalid=package.content;invalid.data.set_file_name("extra");require(!catalog.Register(invalid));
  std::thread reader([&]{for(unsigned i=0;i<2000;++i)require(catalog.List(title,XContentType::kMarketplaceContent).size()==32);});
  for(unsigned i=0;i<2000;++i)require(catalog.Find(title,XContentType::kMarketplaceContent,"abcdef0123456789").has_value());reader.join();

  xam::ContentManager manager(nullptr,argv[2]);
  require(manager.RegisterReadOnlyContent(package.content.data,package.content.device_path,package.content.license));
  require(manager.ListContent(1,0x1234,XContentType::kMarketplaceContent,title).empty());
  require(manager.ListContent(1,0,XContentType::kMarketplaceContent,title).size()==1);
  require(manager.ContentExists(0x1234,package.content.data));
  require(manager.CreateContent("dlc",0,package.content.data)==X_ERROR_ALREADY_EXISTS);
  require(manager.DeleteContent(0,package.content.data)==X_ERROR_ACCESS_DENIED);
  require(manager.UnmountAndDeleteContent(0,package.content.data)==X_ERROR_ACCESS_DENIED);
  require(manager.SetContentThumbnail(0,package.content.data,{1})==X_ERROR_ACCESS_DENIED);
  require(manager.WriteContentHeaderFile(0,package.content.data,0xFFFF)==X_ERROR_ACCESS_DENIED);
  xam::XCONTENT_AGGREGATE_DATA data{};
  require(manager.ReadContentHeaderFile("abcdef0123456789",0,title,XContentType::kMarketplaceContent,data)==X_ERROR_SUCCESS);
  require(data.file_name()==package.content.data.file_name());
  fd=open(argv[1],O_RDONLY);require(fd>=0);bool rejected=false;
  try{bo2zm::android::OpenDlcPackage(fd,"test","\\Device\\BadDLC",title+1);}catch(const std::exception&){rejected=true;}require(rejected);close(fd);


  require(bo2zm::android::DlcGameLicense(1,false)==0xFFFFFFFFu&&bo2zm::android::DlcGameLicense(1,true)==1);
  require(bo2zm::android::DlcGameLicense(16,false)==16&&bo2zm::android::DlcGameLicense(0x508,true)==0x508);
  std::cout<<"DLC tests passed: FD lifetime, marker/file reads, license metadata, trial-license mapping, XAM enumeration, identity isolation, mutation refusal and concurrent lookup\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
