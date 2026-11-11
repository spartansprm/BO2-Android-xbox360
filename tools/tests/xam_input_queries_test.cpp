#include <rex/input/user_queries.h>

#include <array>
#include <cassert>
#include <cstring>
#include <deque>
#include <iostream>
#include <vector>

using namespace rex;
using namespace rex::input;

struct Ports {
  std::array<bool, kMaxGuestUsers> connected{};
  std::array<std::deque<X_INPUT_KEYSTROKE>, kMaxGuestUsers> queues;
  std::array<X_RESULT, kMaxGuestUsers> errors{};
  std::vector<uint32_t> queried;

  void Push(uint32_t port, uint16_t key, uint16_t flags = X_INPUT_KEYSTROKE_KEYDOWN) {
    connected[port] = true;
    X_INPUT_KEYSTROKE event{};
    event.virtual_key = key;
    event.flags = flags;
    event.user_index = 91;
    queues[port].push_back(event);
  }
  X_RESULT Read(uint32_t port, uint32_t, X_INPUT_KEYSTROKE* output) {
    assert(port < kMaxGuestUsers);
    queried.push_back(port);
    if (!connected[port]) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (errors[port]) return errors[port];
    if (queues[port].empty()) return X_ERROR_EMPTY;
    *output = queues[port].front();
    queues[port].pop_front();
    return X_ERROR_SUCCESS;
  }
};

void ExplicitAndAny() {
  Ports ports;
  UserKeystrokes keys;
  auto read = [&](auto port, auto flags, auto* output) { return ports.Read(port, flags, output); };
  ports.Push(0, 0x5800);
  ports.Push(1, 0x5814);
  X_INPUT_KEYSTROKE event{};
  assert(keys.Get(0, 0, &event, read) == X_ERROR_SUCCESS);
  assert(event.user_index == 0 && event.virtual_key == 0x5800);
  assert(ports.queues[1].size() == 1 && ports.queried == std::vector<uint32_t>{0});
  assert(keys.Get(0xFF, 0, &event, read) == X_ERROR_SUCCESS);
  assert(event.user_index == 1 && event.virtual_key == 0x5814);
  ports.Push(1, 0x5801, X_INPUT_KEYSTROKE_KEYUP);
  assert(keys.Get(1, 0, &event, read) == X_ERROR_SUCCESS);
  assert(event.user_index == 1 && event.flags == X_INPUT_KEYSTROKE_KEYUP);
  ports.Push(1, 0x5814);
  assert(keys.Get(0, kAnyInputUserFlag, &event, read) == X_ERROR_SUCCESS && event.user_index == 1);
  ports.Push(1, 0x5814);
  assert(keys.Get(UINT32_MAX, 0, &event, read) == X_ERROR_SUCCESS && event.user_index == 1);
  ports.Push(1, 0x5814);
  assert(keys.Get(0x1FF, 0, &event, read) == X_ERROR_SUCCESS && event.user_index == 1);
}

void FairQueuesAndEx() {
  Ports ports;
  UserKeystrokes keys;
  auto read = [&](auto port, auto flags, auto* output) { return ports.Read(port, flags, output); };
  const uint16_t sequence[] = {X_INPUT_KEYSTROKE_KEYDOWN,
                              X_INPUT_KEYSTROKE_KEYDOWN | X_INPUT_KEYSTROKE_REPEAT,
                              X_INPUT_KEYSTROKE_KEYUP};
  for (const auto flags : sequence) {
    ports.Push(0, 0x5800, flags);
    ports.Push(1, 0x5814, flags);
  }
  X_INPUT_KEYSTROKE event{};
  for (int i = 0; i < 6; ++i) {
    be_u32 user = 0xFF;

    if (i % 2) {
      assert(keys.GetEx(&user, 0, &event, read) == X_ERROR_SUCCESS);
      assert(uint32_t(user) == 1);
      const auto* stored = reinterpret_cast<const uint8_t*>(&user);
      assert(stored[0] == 0 && stored[1] == 0 && stored[2] == 0 && stored[3] == 1);
    } else assert(keys.Get(0xFF, 0, &event, read) == X_ERROR_SUCCESS);
    assert(event.user_index == i % 2 && event.flags == sequence[i / 2]);
    assert(event.virtual_key == (i % 2 ? 0x5814 : 0x5800));
  }
  assert(ports.queues[0].empty() && ports.queues[1].empty());
}

void ResultsAndInvalidArguments() {
  Ports ports;
  UserKeystrokes keys;
  auto read = [&](auto port, auto flags, auto* output) { return ports.Read(port, flags, output); };
  X_INPUT_KEYSTROKE event{};
  std::memset(&event, 0xA5, sizeof(event));
  const auto original = event;
  be_u32 user = 0xFF;
  assert(keys.GetEx(&user, 0, &event, read) == X_ERROR_DEVICE_NOT_CONNECTED);
  assert(uint32_t(user) == 0xFF && std::memcmp(&event, &original, sizeof(event)) == 0);
  ports.connected[0] = true;
  assert(keys.GetEx(&user, 0, &event, read) == X_ERROR_EMPTY);
  assert(uint32_t(user) == 0xFF && std::memcmp(&event, &original, sizeof(event)) == 0);
  ports.errors[0] = X_ERROR_FUNCTION_FAILED;
  assert(keys.GetEx(&user, 0, &event, read) == X_ERROR_FUNCTION_FAILED);
  assert(uint32_t(user) == 0xFF);
  ports.Push(1, 0x5814);
  assert(keys.GetEx(&user, 0, &event, read) == X_ERROR_SUCCESS && uint32_t(user) == 1);
  ports.queried.clear();
  assert(keys.Get(4, 0, &event, read) == X_ERROR_DEVICE_NOT_CONNECTED);
  assert(keys.Get(0, 2, &event, read) == X_ERROR_DEVICE_NOT_CONNECTED);
  assert(keys.Get(0, 0, nullptr, read) == X_ERROR_BAD_ARGUMENTS);
  assert(keys.GetEx(nullptr, 0, &event, read) == X_ERROR_BAD_ARGUMENTS);
  assert(keys.GetEx(&user, 0, nullptr, read) == X_ERROR_BAD_ARGUMENTS);
  assert(ports.queried.empty());
}

void DeviceContexts() {


  Ports ports;
  ports.connected[1] = true;
  ports.Push(1, 0x5814);
  auto connected = [&](uint32_t port) { return ports.connected[port]; };
  uint32_t context = 0xDEADBEEF;
  assert(ConnectedUserContext(1, context, connected) == X_E_SUCCESS && context == 0);
  assert(ports.queues[1].size() == 1);
  context = 0xDEADBEEF;
  assert(ConnectedUserContext(0xFF, context, connected) == X_E_SUCCESS && context == 0);
  assert(ConnectedUserContext(0, context, connected) == X_E_DEVICE_NOT_CONNECTED && context == 0);
  ports.connected[0] = true;
  assert(ConnectedUserContext(0, context, connected) == X_E_SUCCESS && context == 0);
  assert(ConnectedUserContext(0xFF, context, connected) == X_E_SUCCESS && context == 0);
  assert(ConnectedUserContext(4, context, connected) == X_E_DEVICE_NOT_CONNECTED && context == 0);
  ports.connected = {};
  assert(ConnectedUserContext(UINT32_MAX, context, connected) == X_E_DEVICE_NOT_CONNECTED && context == 0);
  static_assert(X_E_DEVICE_NOT_CONNECTED == 0x8007048Fu);
  static_assert(X_ERROR_DEVICE_NOT_CONNECTED == 0x48Fu);
}

int main() {
  ExplicitAndAny();
  FairQueuesAndEx();
  ResultsAndInvalidArguments();
  DeviceContexts();
  std::cout << "XAM explicit/any-user queues, event ABI and device-context fixtures passed\n";
}
