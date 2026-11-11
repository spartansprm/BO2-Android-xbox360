#include <rex/platform.h>

#if REX_PLATFORM_SWITCH

#include <cstdlib>

#include <rex/platform/exceptions.h>
#include <rex/platform/seh.h>

#include <switch.h>

namespace rex::platform {

static thread_local SehThreadState tls_seh_state;
static thread_local bool tls_seh_active = false;

SehThreadState& seh_thread_state() {
  return tls_seh_state;
}

int seh_filter(u32 /*code*/, void* /*exception_pointers*/) {
  // Only used on Windows.
  return 0;
}

[[noreturn]] void seh_rethrow() {
  // On POSIX the signal is raised again with the default handler, which
  // terminates the process. There are no signals here: it terminates directly.
  std::abort();
}

void seh_initialize() {
  // Nothing to install: the only fault entry point on Horizon is
  // __libnx_exception_handler, in exception_handler_switch.cpp, and it asks
  // here through RexSwitchSehHandleFault.
  g_seh_initialized.store(true);
}

bool& seh_active() {
  return tls_seh_active;
}

}  // namespace rex::platform

/*
 * Called by exception_handler_switch.cpp when no SDK handler resolved the fault.
 * Returns true if it redirected execution; currently never, for the reasons given
 * in the file header.
 */
extern "C" bool RexSwitchSehHandleFault(ThreadExceptionDump* ctx) {
  if (!ctx || !rex::platform::seh_active()) {
    return false;
  }

  // It is recorded in the thread's SEH state, as seh_posix.cpp would do
  // before throwing: code and faulting address.
  const uint32_t ec = ctx->esr >> 26;
  const bool data_miss = (ec & 0b111110) == 0b100100;
  auto& state = rex::platform::seh_thread_state();
  state.code = data_miss ? rex::SehException::ACCESS_VIOLATION
                               : rex::SehException::ILLEGAL_INSTRUCTION;
  state.info[0] = 0;
  state.info[1] = static_cast<uintptr_t>(ctx->far.x);

  return false;
}

#endif  // REX_PLATFORM_SWITCH
