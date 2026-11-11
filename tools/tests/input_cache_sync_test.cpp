#include <rex/input/input_system.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

using namespace rex;
using namespace rex::input;
using namespace std::chrono_literals;
extern bool input_vibration;

void Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

enum class Call { Enumeration, Caps, State, Vibration, Key, Attach };
X_RESULT Query(InputSystem& system, Call call, uint32_t user = 0);

struct Monitor {
  std::mutex mutex;
  std::condition_variable cv;
  Call target = Call::Enumeration;
  bool armed = false, entered = false, released = false;
  std::atomic<int> active{0}, overlaps{0}, calls{0}, destroyed{0};
  std::atomic<int> callbacks{0};
  std::atomic<bool> ui_attach{false}, ui_destroy{false};
  InputSystem* system = nullptr;

  void Arm(Call call) {
    std::lock_guard lock(mutex);
    target = call;
    armed = true;
    entered = released = false;
  }
  void Wait() {
    std::unique_lock lock(mutex);
    Check(cv.wait_for(lock, 3s, [&] { return entered; }), "holder never entered callback");
  }
  void Release() {
    std::lock_guard lock(mutex);
    released = true;
    cv.notify_all();
  }
  struct Entry {
    Monitor& monitor;
    Entry(Monitor& m, Call call) : monitor(m) {
      ++m.calls;
      if (m.active.fetch_add(1) != 0) ++m.overlaps;
      std::unique_lock lock(m.mutex);
      m.cv.notify_all();
      if (m.armed && m.target == call && !m.entered) {
        m.entered = true;
        m.cv.notify_all();
        Check(m.cv.wait_for(lock, 3s, [&] { return m.released; }), "held callback timed out");
        m.armed = false;
      }
    }
    ~Entry() { --monitor.active; }
  };
};

DeviceId Id(uint64_t value) { return static_cast<DeviceId>(value); }

class Driver final : public InputDriver {
 public:
  explicit Driver(Monitor& monitor) : InputDriver(nullptr, 0), monitor_(monitor) {
    SetDevices({101, 202});
  }
  ~Driver() override {
    Check(monitor_.active == 0, "driver destroyed during query");
    if (monitor_.ui_destroy) UIQuery();
    ++monitor_.destroyed;
  }
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void SetDevices(std::initializer_list<uint64_t> ids, bool fallback = true) {
    std::lock_guard lock(config_mutex_);
    devices_.clear();

    if (fallback) {
      devices_.push_back({Id(9000), 0, std::string(96, 's'), std::string(160, 'S'), true});
    }
    for (auto id : ids) {
      devices_.push_back({Id(id), 0, std::string(128, 'n') + std::to_string(id),
                          std::string(192, 'g') + std::to_string(id), false});
    }
  }
  void EnumerateDevices(std::vector<DeviceInfo>& out) override {
    Monitor::Entry entry(monitor_, Call::Enumeration);
    std::lock_guard lock(config_mutex_);
    out.insert(out.end(), devices_.begin(), devices_.end());
  }
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t, X_INPUT_CAPABILITIES* out) override {
    Monitor::Entry entry(monitor_, Call::Caps);
    *out = {};
    out->sub_type = uint8_t(uint64_t(id));
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out) override {
    Monitor::Entry entry(monitor_, Call::State);
    if (is_active()) ++monitor_.callbacks;
    *out = {};
    out->packet_number = uint32_t(uint64_t(id));
    if (id != Id(9000)) {
      out->gamepad.buttons = id == Id(101) ? X_INPUT_GAMEPAD_A : X_INPUT_GAMEPAD_B;
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) override {
    Monitor::Entry entry(monitor_, Call::Vibration);
    if (id != Id(9000)) {
      last_vibration_id = uint64_t(id);
      last_left = vibration->left_motor_speed;
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t, X_INPUT_KEYSTROKE* out) override {
    Monitor::Entry entry(monitor_, Call::Key);
    *out = {};
    out->virtual_key = uint16_t(uint64_t(id));
    out->user_index = 91;
    return X_ERROR_SUCCESS;
  }
  void OnWindowAvailable(rex::ui::Window*) override {
    Monitor::Entry entry(monitor_, Call::Attach);
    if (monitor_.ui_attach) UIQuery();
  }
  std::atomic<uint64_t> last_vibration_id{0};
  std::atomic<uint16_t> last_left{0};

 private:
  void UIQuery() {


    auto ui = std::async(std::launch::async, [&] {
      for (Call call : {Call::Caps, Call::State, Call::Vibration, Call::Key}) {
        Check(Query(*monitor_.system, call) == X_ERROR_DEVICE_NOT_CONNECTED,
              "query was not rejected during UI lifecycle callback");
      }
      (void)monitor_.system->window();
    });
    Check(ui.wait_for(2s) == std::future_status::ready, "UI lifecycle query deadlocked");
    ui.get();
  }
  Monitor& monitor_;
  std::mutex config_mutex_;
  std::vector<DeviceInfo> devices_;
};

X_RESULT Query(InputSystem& system, Call call, uint32_t user) {
  X_INPUT_CAPABILITIES caps{};
  X_INPUT_STATE state{};
  X_INPUT_VIBRATION vibration{};
  X_INPUT_KEYSTROKE key{};
  switch (call) {
    case Call::Caps: {
      const auto result = system.GetCapabilities(user, 0, &caps);
      if (user == 1 && result == X_ERROR_SUCCESS) {
        Check(caps.sub_type == 202, "remaining player's capabilities changed identity");
      }
      return result;
    }
    case Call::State: {
      const auto result = system.GetState(user, &state);
      if (user == 1 && result == X_ERROR_SUCCESS) {
        Check(uint16_t(state.gamepad.buttons) == X_INPUT_GAMEPAD_B,
              "remaining player's input changed identity");
      }
      return result;
    }
    case Call::Vibration: return system.SetState(user, &vibration);
    case Call::Key: {
      const auto result = system.GetKeystroke(user, 0, &key);
      if (user == 1 && result == X_ERROR_SUCCESS) {
        Check(uint16_t(key.virtual_key) == 202 && key.user_index == 1,
              "remaining player's keys changed identity");
      }
      return result;
    }
    default: return system.GetCapabilities(user, 0, &caps);
  }
}

struct Fixture {
  Monitor monitor;
  InputSystem system{nullptr};
  Driver* driver;
  Fixture() {
    monitor.system = &system;
    auto owned = std::make_unique<Driver>(monitor);
    driver = owned.get();
    system.AddDriver(std::move(owned));
    system.SetDeviceAssignment(std::make_unique<SlotAssignment>());
    Check(Query(system, Call::State) == X_ERROR_SUCCESS, "initial poll failed");
  }
};

bool Overlap(Call held, bool negative = false) {
  Fixture f;
  f.monitor.Arm(held);
  if (held == Call::Enumeration) std::this_thread::sleep_for(60ms);
  auto holder = std::async(std::launch::async, [&] { return Query(f.system, held); });
  f.monitor.Wait();


  std::this_thread::sleep_for(60ms);
  std::vector<std::future<X_RESULT>> competitors;
  std::atomic<int> started{0};
  for (Call call : {Call::Caps, Call::State, Call::Vibration, Call::Key}) {
    competitors.push_back(std::async(std::launch::async, [&, call] {
      ++started;
      return Query(f.system, call, 1);
    }));
    if (negative) break;
  }
  while (started != (negative ? 1 : 4)) std::this_thread::yield();
  {
    std::unique_lock lock(f.monitor.mutex);
    f.monitor.cv.wait_for(lock, 150ms, [&] { return f.monitor.overlaps != 0; });
  }
  bool overlap = f.monitor.overlaps != 0;


  if (negative && overlap) {
    Check(competitors.front().get() == X_ERROR_SUCCESS, "negative competitor failed");
    competitors.clear();
  }
  f.monitor.Release();
  Check(holder.get() == X_ERROR_SUCCESS, "held query failed");
  for (auto& result : competitors) Check(result.get() == X_ERROR_SUCCESS, "competing query failed");
  return overlap || f.monitor.overlaps != 0;
}

template<class Mutation>
void PausedMutation(Mutation mutation) {
  Fixture f;
  f.monitor.Arm(Call::State);
  auto holder = std::async(std::launch::async, [&] { return Query(f.system, Call::State); });
  f.monitor.Wait();
  std::promise<void> started;
  auto changed = std::async(std::launch::async, [&] {
    started.set_value();
    mutation(f);
  });
  started.get_future().wait();
  Check(changed.wait_for(100ms) == std::future_status::timeout, "mutation overtook in-flight query");
  f.monitor.Release();
  Check(holder.get() == X_ERROR_SUCCESS, "paused query failed");
  Check(changed.wait_for(2s) == std::future_status::ready, "mutation did not finish");
  changed.get();
}

void Lifecycle() {
  PausedMutation([](Fixture& f) {
    f.system.SetDeviceAssignment(std::make_unique<SharedAssignment>());
    Check(Query(f.system, Call::Caps, 1) == X_ERROR_DEVICE_NOT_CONNECTED, "assignment not replaced");
  });
  PausedMutation([](Fixture& f) {
    f.system.SetActiveCallback([] { return false; });
    const int before = f.monitor.callbacks;
    Check(Query(f.system, Call::State) == X_ERROR_SUCCESS, "callback replacement query failed");
    Check(f.monitor.callbacks == before, "old active callback still used");
  });
  PausedMutation([](Fixture& f) {
    f.system.AddDriver(std::make_unique<Driver>(f.monitor));
  });
  PausedMutation([](Fixture& f) {
    f.monitor.ui_attach = true;
    f.system.AttachWindow(nullptr);
    Check(Query(f.system, Call::State) == X_ERROR_SUCCESS, "attachment did not resume queries");
  });
  PausedMutation([](Fixture& f) {
    f.monitor.ui_destroy = true;
    f.system.Shutdown();
    Check(f.monitor.destroyed == 1, "shutdown did not destroy driver");
    const int before = f.monitor.calls;
    for (Call call : {Call::Caps, Call::State, Call::Vibration, Call::Key}) {
      Check(Query(f.system, call) == X_ERROR_DEVICE_NOT_CONNECTED, "late shutdown query accepted");
    }
    Check(f.monitor.calls == before, "late query reached retired driver");
    f.system.Shutdown();
    Check(f.monitor.destroyed == 1, "repeated shutdown destroyed twice");
  });

  Monitor monitor;
  monitor.ui_destroy = true;
  {
    InputSystem system(nullptr);
    monitor.system = &system;
    system.SetDeviceAssignment(std::make_unique<SlotAssignment>());
    system.AddDriver(std::make_unique<Driver>(monitor));
    Check(Query(system, Call::State) == X_ERROR_SUCCESS, "destructor fixture poll failed");
  }
  Check(monitor.destroyed == 1, "direct destructor did not retire driver");
}

void RoutingAndChurn() {
  Fixture f;
  input_vibration = true;
  for (uint32_t user = 0; user < 2; ++user) {
    X_INPUT_STATE state{};
    Check(f.system.GetState(user, &state) == X_ERROR_SUCCESS, "two-port state failed");
    Check(uint16_t(state.gamepad.buttons) == (user ? X_INPUT_GAMEPAD_B : X_INPUT_GAMEPAD_A),
          "physical ports merged or synthetic stole slot");
    X_INPUT_CAPABILITIES caps{};
    Check(f.system.GetCapabilities(user, 0, &caps) == X_ERROR_SUCCESS, "two-port caps failed");
    Check(caps.sub_type == (user ? 202 : 101), "active device capability selection changed");
    X_INPUT_KEYSTROKE key{};
    Check(f.system.GetKeystroke(user, 0, &key) == X_ERROR_SUCCESS, "two-port key failed");
    Check(key.user_index == user, "backend index leaked to guest");
    X_INPUT_VIBRATION vibration{};
    vibration.left_motor_speed = uint16_t(120 + user);
    Check(f.system.SetState(user, &vibration) == X_ERROR_SUCCESS, "two-port vibration failed");
    Check(f.driver->last_vibration_id == (user ? 202 : 101), "vibration crossed ports");
    Check(f.driver->last_left == 120 + user, "vibration payload changed");
  }
  Check(Query(f.system, Call::Caps, 4) == X_ERROR_DEVICE_NOT_CONNECTED, "SDK port limit changed");
  input_vibration = false;
  X_INPUT_VIBRATION vibration{};
  vibration.left_motor_speed = 321;
  Check(f.system.SetState(1, &vibration) == X_ERROR_SUCCESS, "muted vibration failed");
  Check(f.driver->last_left == 0, "default vibration suppression changed");

  for (int round = 0; round < 6; ++round) {
    f.driver->SetDevices({202}, false);
    std::this_thread::sleep_for(60ms);
    X_INPUT_STATE state{};
    Check(f.system.GetState(1, &state) == X_ERROR_SUCCESS, "remaining player was renumbered");
    Check(uint16_t(state.gamepad.buttons) == X_INPUT_GAMEPAD_B, "wrong player after disconnect");
    for (Call call : {Call::Caps, Call::State, Call::Vibration, Call::Key}) {
      Check(Query(f.system, call) == X_ERROR_DEVICE_NOT_CONNECTED, "removed port still connected");
    }

    f.driver->SetDevices({202, uint64_t(303 + round)});
    std::this_thread::sleep_for(60ms);
    std::vector<std::future<void>> workers;
    for (Call call : {Call::Caps, Call::State, Call::Vibration, Call::Key}) {
      workers.push_back(std::async(std::launch::async, [&, call] {
        for (int poll = 0; poll < 100; ++poll) {
          const uint32_t user = uint32_t(poll % 2);
          const auto result = Query(f.system, call, user);


          Check(result == X_ERROR_SUCCESS || (user == 0 && result == X_ERROR_DEVICE_NOT_CONNECTED),
                "churn query failed or remaining player disconnected");
          std::this_thread::sleep_for(1ms);
        }
      }));
    }
    workers.push_back(std::async(std::launch::async, [&] {
      for (int change = 0; change < 30; ++change) {
        f.system.SetDeviceAssignment(std::make_unique<SlotAssignment>());
        std::this_thread::sleep_for(2ms);
      }
    }));
    workers.push_back(std::async(std::launch::async, [&] {
      for (int change = 0; change < 4; ++change) {
        f.driver->SetDevices({202}, false);
        std::this_thread::sleep_for(20ms);
        f.driver->SetDevices({202, uint64_t(303 + round)});
        std::this_thread::sleep_for(20ms);
      }
    }));
    for (auto& worker : workers) worker.get();
    std::this_thread::sleep_for(60ms);
    X_INPUT_CAPABILITIES caps{};
    Check(f.system.GetCapabilities(0, 0, &caps) == X_ERROR_SUCCESS, "reconnect caps failed");
    Check(caps.sub_type == uint8_t(303 + round), "reconnect did not use vacant port");
  }
  Check(f.monitor.overlaps == 0, "concurrent driver transactions during churn");
}

int main(int argc, char**) {
  if (Overlap(Call::Enumeration, argc > 1)) {
    std::cout << "DETECTED overlapping refresh/query transactions\n";
    return 3;
  }
  if (argc > 1) {
    std::cerr << "negative control failed to detect overlap\n";
    return 1;
  }
  for (Call held : {Call::Caps, Call::State, Call::Vibration, Call::Key}) {
    Check(!Overlap(held), "query callback overlapped cache refresh");
  }
  Lifecycle();
  RoutingAndChurn();
  std::cout << "PASS full-query exclusion, lifecycle/UI quiescence, assignment/callback replacement, "
               "two-port routing and reconnect churn\n";
}
