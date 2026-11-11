// XAM input selection over explicit guest ports. Key results are Win32 errors;
// device-context queries use HRESULTs and do not establish profile sign-in.
#pragma once

#include <mutex>

#include <rex/input/device.h>
#include <rex/input/input.h>
#include <rex/system/xtypes.h>

namespace rex::input {
inline constexpr uint32_t kAnyInputUserFlag = 1u << 30;

constexpr bool AnyInputUser(uint32_t user, uint32_t flags = 0) {
  return (user & 0xFF) == 0xFF || (flags & kAnyInputUserFlag);
}

class UserKeystrokes {
 public:
  // Read supplies GetKeystroke(port, flags, output). Rotate only after an
  // actual event, so a held/repeating P1 cannot starve a pending P2 press.
  template <class Read>
  X_RESULT Get(uint32_t user, uint32_t flags, X_INPUT_KEYSTROKE* output, Read read) {
    if (!output) return X_ERROR_BAD_ARGUMENTS;
    if ((flags & 0xFF) && !(flags & X_INPUT_FLAG_GAMEPAD)) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (!AnyInputUser(user, flags)) {
      if (user >= kMaxGuestUsers) return X_ERROR_DEVICE_NOT_CONNECTED;
      return ReadPort(user, flags, output, read);
    }
    // The backend calls here are native input operations, not guest calls.
    std::lock_guard lock(mutex_);
    bool connected = false;
    X_RESULT error = X_ERROR_SUCCESS;
    for (uint32_t offset = 0; offset < kMaxGuestUsers; ++offset) {
      const auto port = (next_ + offset) % kMaxGuestUsers;
      const auto result = ReadPort(port, flags, output, read);
      if (result == X_ERROR_SUCCESS) {
        next_ = (port + 1) % kMaxGuestUsers;
        return result;
      }
      if (result == X_ERROR_EMPTY) connected = true;
      else if (result != X_ERROR_DEVICE_NOT_CONNECTED && error == X_ERROR_SUCCESS) error = result;
    }
    if (error != X_ERROR_SUCCESS) return error;
    return connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  template <class Read>
  X_RESULT GetEx(be_u32* user, uint32_t flags, X_INPUT_KEYSTROKE* output, Read read) {
    if (!user) return X_ERROR_BAD_ARGUMENTS;
    const auto result = Get(uint32_t(*user), flags, output, read);
    // XSUCCEEDED is an NTSTATUS test; positive Win32 errors are not success.
    if (result == X_ERROR_SUCCESS) *user = output->user_index;
    return result;
  }

 private:
  template <class Read>
  static X_RESULT ReadPort(uint32_t port, uint32_t flags, X_INPUT_KEYSTROKE* output, Read& read) {
    X_INPUT_KEYSTROKE event{};
    const auto result = read(port, flags, &event);
    if (result == X_ERROR_SUCCESS) {
      event.user_index = uint8_t(port);
      *output = event;
    }
    return result;
  }

  std::mutex mutex_;
  uint32_t next_ = 0;
};

// Connected supplies a capability query for one gamepad port. No key queues
// are drained. Preserve the opaque context as zero on success and failure;
// its encoding is unknown and is not established to be a guest port index.
template <class Connected>
X_HRESULT ConnectedUserContext(uint32_t user, uint32_t& context, Connected connected) {
  context = 0;
  if (!AnyInputUser(user)) {
    if (user >= kMaxGuestUsers || !connected(user)) return X_E_DEVICE_NOT_CONNECTED;
    return X_E_SUCCESS;
  }
  for (uint32_t port = 0; port < kMaxGuestUsers; ++port) {
    if (!connected(port)) continue;
    return X_E_SUCCESS;
  }
  return X_E_DEVICE_NOT_CONNECTED;
}
}  // namespace rex::input
