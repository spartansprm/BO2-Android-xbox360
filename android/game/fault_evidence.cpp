// SPDX-License-Identifier: GPL-3.0-only
#include "fault_evidence.h"
#include <rex/exception_handler.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <link.h>
#include <fcntl.h>
#include <unistd.h>

extern std::atomic<uint64_t> g_bo2zm_frames_game;
namespace bo2zm::android::fault {
namespace {
int output=-1;uintptr_t base=0;char run_id[37]{};
std::atomic_flag writing=ATOMIC_FLAG_INIT;
uint64_t events=0;
static_assert(std::atomic<uint64_t>::is_always_lock_free);
struct Record {
  char data[2048];size_t length=0;
  Record(){std::memset(data,' ',sizeof(data));}
  void Text(const char* value){while(*value&&length<sizeof(data))data[length++]=*value++;}
  void Hex(uint64_t value) {
    Text("\"0x");bool started=false;
    for(int shift=60;shift>=0;shift-=4){unsigned n=(value>>shift)&15;if(n||started||!shift){started=true;char c[2]={"0123456789abcdef"[n],0};Text(c);}}
    Text("\"");
  }
};
bool Observe(rex::arch::Exception* ex,void*) {
  if(output<0||writing.test_and_set(std::memory_order_acquire))return false;
  Record record;record.Text("{\"schema\":1,\"run_id\":\"");record.Text(run_id);
  record.Text("\",\"kind\":\"unhandled_by_memory_callbacks\",\"host_pc\":");record.Hex(ex->pc());
  record.Text(",\"events_hex\":");record.Hex(++events);
  record.Text(",\"guest_swaps_hex\":");record.Hex(g_bo2zm_frames_game.load(std::memory_order_relaxed));
  if(ex->code()==rex::arch::Exception::Code::kAccessViolation) {
    record.Text(",\"host_address\":");const auto address=ex->fault_address();record.Hex(address);
    if(address>=base&&address-base<=UINT32_MAX){record.Text(",\"guest_address\":");record.Hex(address-base);}
  }
#if REX_ARCH_ARM64
  record.Text(",\"host_lr\":");record.Hex(ex->thread_context()->x[30]);
#endif
  record.Text("}");


  (void)::pwrite(output,record.data,sizeof(record.data),0);
  writing.clear(std::memory_order_release);
  return false;
}
std::string Hex(uintptr_t value){Record r;r.Hex(value);return {r.data,r.length};}
}
void Start(const std::filesystem::path& files,const std::string& run,const void* virtual_base) {
  if(output>=0||run.size()!=36||run.find_first_not_of("0123456789abcdef-")!=std::string::npos)
    throw std::runtime_error("Invalid native fault evidence setup");
  std::string modules="{\"run_id\":\""+run+"\",\"modules\":[";
  dl_iterate_phdr([](dl_phdr_info* info,size_t,void* data) {
    std::string_view path=info->dlpi_name?info->dlpi_name:"";
    auto name=path.substr(path.find_last_of('/')==std::string_view::npos?0:path.find_last_of('/')+1);
    if(name!="libbo2game.so"&&name!="librexruntime.so"&&name!="libSDL3.so")return 0;
    auto& text=*static_cast<std::string*>(data);if(text.back()!='[')text+=',';
    text+="{\"library\":\""+std::string(name)+"\",\"load_bias\":"+Hex(info->dlpi_addr)+"}";return 0;
  },&modules);
  modules+="]}";std::ofstream stream(files/"game-native-modules.json");stream<<modules;stream.close();
  if(!stream)throw std::runtime_error("Cannot save native module locations");
  int fd=::open((files/"game-native-fault.json").c_str(),O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC,0600);
  if(fd<0)throw std::runtime_error("Cannot create native fault evidence");
  std::memcpy(run_id,run.c_str(),37);base=reinterpret_cast<uintptr_t>(virtual_base);events=0;writing.clear();output=fd;
  rex::arch::ExceptionHandler::Install(Observe,nullptr);
}
void Stop(){if(output>=0){rex::arch::ExceptionHandler::Uninstall(Observe,nullptr);::close(output);output=-1;}}
}
