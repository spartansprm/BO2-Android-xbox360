#include <rex/input/device_assignment.h>
#include <rex/input/switch/npad_sources.h>

#include <algorithm>
#include <bit>
#include <cassert>
#include <iostream>

using namespace rex::input;
using namespace rex::input::nx;

DeviceId Identity(size_t source) { return DeviceId(0x4E580000u + source + 1); }

std::vector<DeviceInfo> InitiallyConnected(std::initializer_list<size_t> connected) {
  std::vector<DeviceInfo> devices;
  for (const auto source : kNpadEnumerationOrder) {
    if (std::find(connected.begin(), connected.end(), source) == connected.end()) continue;
    DeviceInfo device;
    device.id = Identity(source);
    device.ordinal = uint32_t(devices.size());
    devices.push_back(device);
  }
  return devices;
}

std::vector<DeviceId> User(const SlotAssignment& assignment, uint32_t user) {
  std::vector<DeviceId> devices;
  assignment.DevicesForUser(user, devices);
  return devices;
}

int main() {
  static_assert(kMaxGuestUsers == 4 && kNpadSourceCount == 5);
  uint64_t all = 0;
  for (const auto source : kNpadEnumerationOrder) {
    const auto mask = NpadSourceMask(source);
    assert(std::popcount(mask) == 1 && !(all & mask));
    all |= mask;
  }
  assert(all == (uint64_t(1) << 32 | 0xFu));
  assert(NpadSourceMask(kNpadHandheldSource) == uint64_t(1) << 32);
  assert(NpadSourceMask(0) == 1 && NpadSourceMask(3) == 8);
  assert(NpadSourceMask(kNpadSourceCount) == 0 && NpadSourceMask(SIZE_MAX) == 0);

  SlotAssignment assignment;
  assignment.OnDevicesChanged(InitiallyConnected({kNpadHandheldSource}));
  assert(User(assignment, 0) == std::vector{Identity(kNpadHandheldSource)});
  assert(User(assignment, 1).empty());
  assignment.OnDevicesChanged(InitiallyConnected({0}));
  assert(User(assignment, 0) == std::vector{Identity(0)});
  assert(User(assignment, 1).empty());


  auto devices = InitiallyConnected({kNpadHandheldSource, 0});
  assignment.OnDevicesChanged(devices);
  assert(User(assignment, 0) == std::vector{Identity(kNpadHandheldSource)});
  assert(User(assignment, 1) == std::vector{Identity(0)});


  devices.erase(devices.begin());
  assignment.OnDevicesChanged(devices);
  assert(User(assignment, 0).empty());
  assert(User(assignment, 1) == std::vector{Identity(0)});

  assignment.OnDevicesChanged(InitiallyConnected({0, 1}));
  assert(User(assignment, 0) == std::vector{Identity(0)});
  assert(User(assignment, 1) == std::vector{Identity(1)});
  assignment.OnDevicesChanged(InitiallyConnected({kNpadHandheldSource, 0, 1, 2, 3}));
  assert(User(assignment, 0) == std::vector{Identity(kNpadHandheldSource)});
  assert(User(assignment, 3) == std::vector{Identity(2)});
  assert(User(assignment, 4).empty());
  std::cout << "Independent Switch sources and guest-port fixtures passed\n";
}
