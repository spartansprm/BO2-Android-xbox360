/**
 * @file        rex/os/switch/runtime_switch.cpp
 * @brief       libnx startup constants for the Switch executable
 *
 * This file is compiled directly into the final executable, not into a
 * library.
 *
 * Everything here replaces weak libnx symbols, and crt0 only references them
 * weakly. A weak reference does not pull an object out of a static library, so
 * if this ended up in rexcore.a the linker would silently ignore it and the
 * defaults would be used: no error, no warning, and the process starting in a
 * different state than expected. That is why it is added to the game target.
 */

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>

#include <cxxabi.h>

#include <switch.h>

extern "C" void RexSwitchCrashLog(const char* reason, const ThreadExceptionDump* ctx, u64 stack,
                                  u64 pc);
extern "C" NX_NORETURN void RexSwitchCrashEnd(Result result);

namespace {

/*
 * An uncaught exception. devkitA64's libstdc++ handler writes nothing
 * (rex_stderr.log came out empty when SetupVfs let a filesystem_error
 * escape), so the type and what() are recorded here; for std::filesystem
 * errors what() includes the path. It ends with error 2345-0104.
 */
[[noreturn]] void RexSwitchTerminate() {
  char reason[768] = "std::terminate without an active exception";
  if (std::exception_ptr current = std::current_exception()) {
    const std::type_info* type = abi::__cxa_current_exception_type();
    int status = -1;
    char* demangled = type ? abi::__cxa_demangle(type->name(), nullptr, nullptr, &status) : nullptr;
    const char* type_name = demangled ? demangled : (type ? type->name() : "?");
    try {
      std::rethrow_exception(current);
    } catch (const std::exception& e) {
      std::snprintf(reason, sizeof(reason), "std::terminate: %s: %s", type_name, e.what());
    } catch (...) {
      std::snprintf(reason, sizeof(reason), "std::terminate: %s", type_name);
    }
    std::free(demangled);
  }
  RexSwitchCrashLog(reason, nullptr, reinterpret_cast<u64>(__builtin_frame_address(0)),
                    reinterpret_cast<u64>(__builtin_return_address(0)));
  RexSwitchCrashEnd(MAKERESULT(Module_Libnx, 104));
}

__attribute__((constructor(101))) void RexSwitchInstallTerminate() {
  std::set_terminate(RexSwitchTerminate);
}

}  // namespace

extern "C" {

/*
 * Declared as an application instead of an applet. It goes with title takeover,
 * which was measured to be mandatory: in applet mode the process gets 400 MB, and
 * guest physical memory alone is 512.
 */
u32 __nx_applet_type = AppletType_Application;


size_t __nx_heap_size = 1024ull * 1024 * 1024;  /* see below */

/*
 * The exception handler stack is no longer declared here.
 *
 * libnx has a single one for the whole process, and with twenty-odd threads
 * faulting at the same time they overwrote each other. There are now eight
 * stack and dump sets in exception_handler_switch.cpp, which hands them out.
 */

}  // extern "C"
