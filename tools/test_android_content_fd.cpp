// SPDX-License-Identifier: GPL-3.0-only

#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <thread>
#include <iostream>
#include <stdexcept>
#include <source_location>
#include "content_mount.h"

using rex::X_STATUS;
void require(bool ok,const std::source_location& loc=std::source_location::current()) { if (!ok) throw std::runtime_error("Native FD container assertion at line "+std::to_string(loc.line())); }
int main(int argc,char**argv) {
  require(argc==5); rex::InitLoggingEarly();
  int h=open(argv[1],O_RDONLY), a=open(argv[2],O_RDONLY), b=open(argv[3],O_RDONLY), patch=open(argv[4],O_RDONLY);
  require(h>=0&&a>=0&&b>=0&&patch>=0);
  auto base=std::make_unique<rex::filesystem::StfsContainerDevice>("\\Device\\Svod", "[descriptors]");
  const int fds[]={a,b}; require(base->InitializeFromFileDescriptors(h,fds));
  require(base->is_read_only());
  auto update=std::make_unique<rex::filesystem::StfsContainerDevice>("\\Device\\Update", "[descriptors]");
  require(update->InitializeFromFileDescriptors(patch,{})); require(update->is_read_only());
  rex::filesystem::VirtualFileSystem vfs;
  require(vfs.RegisterDevice(std::move(base)));require(vfs.RegisterDevice(std::move(update)));
  require(vfs.RegisterSymbolicLink("game:","\\Device\\Svod"));
  require(vfs.RegisterSymbolicLink("update:","\\Device\\Update"));
  auto* retained=vfs.ResolvePath("game:\\default_mp.xex");require(retained);
  bool refused=false;
  try{bo2zm::android::MountContent(vfs,h,{fds,1},patch);}catch(const std::exception&){refused=true;}
  require(refused&&vfs.ResolvePath("game:\\default_mp.xex")==retained);
  bo2zm::android::MountContent(vfs,h,fds,patch);
  auto* mounted=vfs.ResolvePath("game:\\default_mp.xex");require(mounted&&mounted!=retained);
  require(vfs.ResolvePath("d:\\default_mp.xex")==mounted);
  require(vfs.ResolvePath("\\Device\\Harddisk0\\Partition1\\default_mp.xex")==mounted);
  require(vfs.ResolvePath("\\Device\\Harddisk0\\PartitionUpdate\\default_mp.xexp")==vfs.ResolvePath("update:\\default_mp.xexp"));
  auto reject=[&](int header,std::span<const int> parts) {
    rex::filesystem::StfsContainerDevice invalid("\\Device\\Invalid","[descriptors]");
    require(!invalid.InitializeFromFileDescriptors(header,parts));
  };
  reject(h,{fds,1});
  int writable=open(argv[1],O_RDWR);require(writable>=0);reject(writable,fds);close(writable);
  int pipe_fds[2];require(pipe(pipe_fds)==0);reject(pipe_fds[0],fds);close(pipe_fds[0]);close(pipe_fds[1]);
  const auto folder=std::filesystem::path(argv[1]).parent_path();
  for(const char* name:{"bad-parent.stfs","cyclic-chain.stfs","truncated.stfs"}) {
    int bad=open((folder/name).c_str(),O_RDONLY);require(bad>=0);reject(bad,{});close(bad);
  }
  int cycle=open((folder/"cyclic-svod.data").c_str(),O_RDONLY);require(cycle>=0);
  const int cyclic[]={cycle,b};reject(h,cyclic);close(cycle);
  unsigned char magic=0;require(pread(h,&magic,1,0)==1&&magic=='L');
  close(h);close(a);close(b);close(patch);
  auto* entry=vfs.ResolvePath("game:\\DEFAULT_MP.XEX"); require(entry && entry->size()==9000);
  auto* delta=vfs.ResolvePath("update:\\default_mp.xexp");require(delta && delta->size()==9000);
  auto check=[](rex::filesystem::Entry* e) {
    rex::filesystem::File* raw=nullptr; require(e->Open(rex::filesystem::FileAccess::kFileReadData,&raw)==0);
    std::vector<uint8_t> data(9000);size_t got=0;
    require(raw->ReadSync(data,0,&got)==0 && got==9000);
    require(std::string(data.begin(),data.begin()+4)=="XEX2");
    for(size_t i=160;i<data.size();++i) require(data[i]==uint8_t(i*17+9));
    std::atomic<bool> correct{true};
    auto work=[&](size_t offset) {for(int i=0;i<2000;++i) {
      uint8_t bytes[111];size_t n=0;
      if(raw->ReadSync(bytes,offset,&n)!=0||n!=111) {correct=false;return;}
      for(size_t j=0;j<n;++j) if(bytes[j]!=uint8_t((offset+j)*17+9))correct=false;
    }};
    std::thread t1(work,2000),t2(work,7000);t1.join();t2.join();require(correct);
    uint8_t byte=0;got=77;require(raw->ReadSync({&byte,1},9000,&got)==X_STATUS_END_OF_FILE&&got==0);
    require(raw->WriteSync({&byte,1},0,&got)==X_STATUS_ACCESS_DENIED);raw->Destroy();
  };
  check(entry);check(delta);check(retained);
  rex::filesystem::File* denied=nullptr;rex::filesystem::FileAction action;
  auto status=vfs.OpenFile(nullptr,"game:\\default_mp.xex",rex::filesystem::FileDisposition::kOpen,
      rex::filesystem::FileAccess::kFileWriteData,false,true,&denied,&action);
  require(status==0 && denied && !(denied->file_access()&rex::filesystem::FileAccess::kFileWriteData));
  denied->Destroy();require(vfs.ResolvePath("game:") && vfs.ResolvePath("update:"));
  std::cout<<"SDK SVOD/STFS descriptor mounts: exact cross-hash/fragment reads, case folding, concurrent pread, EOF, read-only, ownership and corrupt-input rejection passed\n";
}
