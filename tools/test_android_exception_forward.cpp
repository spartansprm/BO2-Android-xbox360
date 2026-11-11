// SPDX-License-Identifier: GPL-3.0-only


#include <rex/exception_handler.h>
#include "fault_evidence.h"
#include <atomic>
#include <cassert>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
std::atomic<uint64_t> g_bo2zm_frames_game{123};
namespace {
volatile sig_atomic_t previous_calls=0,handled_calls=0;
char* page=nullptr;size_t page_size=0;
bool decline(rex::arch::Exception*,void*){return false;}
bool recover(rex::arch::Exception* ex,void*) {
  if(ex->code()!=rex::arch::Exception::Code::kAccessViolation||ex->fault_address()!=reinterpret_cast<uintptr_t>(page))return false;
  if(mprotect(page,page_size,PROT_READ|PROT_WRITE))_exit(91);
  handled_calls=handled_calls+1;return true;
}
void previous(int){previous_calls=previous_calls+1;}
void previous_info(int sig,siginfo_t* info,void* context){if(sig!=SIGSEGV||!info||!context)_exit(92);previous_calls=previous_calls+1;}
}
int main(int argc,char** argv) {
  assert(argc==2);rlimit limit{0,0};assert(setrlimit(RLIMIT_CORE,&limit)==0);
  using rex::arch::ExceptionHandler;
  struct sigaction action{},original{};action.sa_handler=previous;sigemptyset(&action.sa_mask);
  assert(sigaction(SIGILL,&action,&original)==0);ExceptionHandler::Install(decline,nullptr);raise(SIGILL);
  assert(previous_calls==1);ExceptionHandler::Uninstall(decline,nullptr);
  struct sigaction restored{};sigaction(SIGILL,nullptr,&restored);assert(restored.sa_handler==previous);sigaction(SIGILL,&original,nullptr);
  action={};action.sa_sigaction=previous_info;action.sa_flags=SA_SIGINFO;sigemptyset(&action.sa_mask);
  assert(sigaction(SIGSEGV,&action,&original)==0);
  page_size=size_t(sysconf(_SC_PAGESIZE));page=static_cast<char*>(mmap(nullptr,page_size,PROT_READ,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));assert(page!=MAP_FAILED);
  ExceptionHandler::Install(recover,nullptr);
  const std::filesystem::path files=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(files);
  bo2zm::android::fault::Start(files,"1f599c1e-c0b1-41d2-946d-edf338465449",nullptr);
  *static_cast<volatile char*>(page)=42;assert(handled_calls==1&&previous_calls==1);
  assert(std::filesystem::file_size(files/"game-native-fault.json")==0);
  raise(SIGSEGV);assert(previous_calls==2);
  bo2zm::android::fault::Stop();ExceptionHandler::Uninstall(recover,nullptr);
  std::ifstream input(files/"game-native-fault.json");std::string evidence((std::istreambuf_iterator<char>(input)),{});
  assert(evidence.size()==2048&&evidence.find("\"host_pc\":\"0x")!=std::string::npos&&evidence.find("\"guest_swaps_hex\":\"0x7b\"")!=std::string::npos);
  sigaction(SIGSEGV,&original,nullptr);munmap(page,page_size);
  const pid_t child=fork();assert(child>=0);
  if(!child) {
    action={};action.sa_handler=SIG_DFL;sigemptyset(&action.sa_mask);sigaction(SIGSEGV,&action,nullptr);
    ExceptionHandler::Install(decline,nullptr);alarm(2);
    auto* bad=static_cast<volatile char*>(mmap(nullptr,page_size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));*bad=1;_exit(93);
  }
  int status=0;assert(waitpid(child,&status,0)==child);assert(WIFSIGNALED(status)&&WTERMSIG(status)==SIGSEGV);
  std::cout<<"POSIX forwarding passed: handled memory fault resumes, plain/siginfo predecessors called, unhandled child terminates without retry loop, evidence excludes handled faults\n";
}
