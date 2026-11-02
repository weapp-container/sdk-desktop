#pragma once
#include "device_location_common.hpp"
#include <cctype>
#include <system_error>

namespace weapp::desktop::detail {
inline std::string ble_uuid(std::string value) {
  if (value.size() == 4) value = "0000" + value + "-0000-1000-8000-00805F9B34FB";
  else if (value.size() == 8) value += "-0000-1000-8000-00805F9B34FB";
  if (value.size() != 36) throw std::invalid_argument("Bluetooth UUID must contain 4, 8 or 36 characters");
  for (size_t index = 0; index < value.size(); ++index) {
    const bool separator = index == 8 || index == 13 || index == 18 || index == 23;
    auto c = static_cast<unsigned char>(value[index]);
    if (separator ? c != '-' : !std::isxdigit(c)) throw std::invalid_argument("invalid Bluetooth UUID");
    value[index] = char(std::toupper(c));
  }
  return value;
}
inline std::string ble_characteristic_key(const Json& payload) {
  return Json::array({payload.at("deviceId"), payload.at("serviceId"), payload.at("characteristicId")}).dump();
}
struct BleCommand {
  std::string call, api;
  Json payload;
  long timeout_ms = 10000;
  std::shared_ptr<std::atomic<bool>> delivery = std::make_shared<std::atomic<bool>>(true);
  bool cancelled() const { return !delivery->load(); }
};
struct BlePending {
  std::shared_ptr<BleCommand> command;
  std::future<std::shared_ptr<void>> preparation;
};
struct NativeBle {
  DeviceApiOptions options;
  std::weak_ptr<DeviceQueue> queue;
  std::string call;
  std::atomic<bool> cancelled{false}, finished{false}, opened{false}, discovering{false}, failed{false};
  std::shared_ptr<std::atomic<bool>> delivery = std::make_shared<std::atomic<bool>>(true);
  std::mutex mutex;
  std::condition_variable changed;
  std::map<std::string, Json> known_devices;
  std::set<std::string> connected_ids;
  std::deque<BlePending> commands;
  size_t outstanding = 0;
  std::shared_ptr<std::atomic<bool>> discovery_delivery = std::make_shared<std::atomic<bool>>(false);
  std::string error, cleanup_error;
  std::map<std::string,Json> undelivered_failures;
  std::map<std::string,std::shared_ptr<std::atomic<bool>>> notification_deliveries;
  NativeBle(DeviceApiOptions host, std::weak_ptr<DeviceQueue> owner, std::string id)
      : options(std::move(host)), queue(std::move(owner)), call(std::move(id)) {}
  bool stopping() const { return cancelled || failed; }
  void fault(const std::string& message) {
    { std::lock_guard<std::mutex> guard(mutex); if (error.empty()) error = socket_terminal_text(message, 512); }
    failed = true; changed.notify_all();
  }
  std::string failure() { std::lock_guard<std::mutex> guard(mutex); return error; }
  void cleanup_failed(const std::string& message) { std::lock_guard<std::mutex> guard(mutex); cleanup_error = socket_terminal_text(message, 512); }
  void cleanup_succeeded() { std::lock_guard<std::mutex> guard(mutex); cleanup_error.clear(); }
  bool push(DeviceQueued item) {
    if (!item.delivery_allowed) item.delivery_allowed = delivery;
    if (auto owner = queue.lock()) return owner->push(std::move(item)); return false;
  }
  void fail_call(const std::string& id, const std::string& api, const std::string& message, int64_t native = 0) {
    auto failure = device_failure(api, message, "invalid_request", native);
    if (!push({DeviceQueued::Failure, id, "", "", failure})) {
      {std::lock_guard<std::mutex> guard(mutex); undelivered_failures[id] = std::move(failure);}
      fault("native Bluetooth failure completion queue is full");
    }
  }
  void flush_failures() {
    std::map<std::string,Json> pending;
    {std::lock_guard<std::mutex> guard(mutex); pending.swap(undelivered_failures);}
    for (auto& [id, value] : pending) if (!push({DeviceQueued::Failure, id, "", "", value})) {
      std::lock_guard<std::mutex> guard(mutex); undelivered_failures.emplace(id, std::move(value));
    }
  }
  bool failures_drained() {std::lock_guard<std::mutex> guard(mutex); return undelivered_failures.empty();}
  bool event(const std::string& name, Json value, bool terminal = false, std::shared_ptr<std::atomic<bool>> token = {}) {
    if (cancelled) return false;
    DeviceQueued item{terminal ? DeviceQueued::NativeTerminalEvent : DeviceQueued::NativeEvent, "", name, "", std::move(value)}; item.delivery_allowed = std::move(token);
    if (!push(std::move(item))) {
      fault("native Bluetooth ingress backpressure limit exceeded"); return false;
    }
    return true;
  }
  std::shared_ptr<std::atomic<bool>> begin_notification(const std::string& key) {
    std::lock_guard<std::mutex> guard(mutex); if (!notification_deliveries.count(key) && notification_deliveries.size() >= 256) throw std::invalid_argument("Bluetooth notification limit exceeded"); auto& token = notification_deliveries[key]; if (token) *token = false;
    token = std::make_shared<std::atomic<bool>>(true); return token;
  }
  void cancel_notification(const std::string& key) {
    std::lock_guard<std::mutex> guard(mutex); auto found = notification_deliveries.find(key); if (found != notification_deliveries.end()) *found->second = false;
  }
  void cancel_device_notifications(const std::string& id) {
    std::lock_guard<std::mutex> guard(mutex);
    for (auto& [key, token] : notification_deliveries) if (Json::parse(key).at(0) == id) *token = false;
  }
  void begin_discovery() {
    std::lock_guard<std::mutex> guard(mutex);
    *discovery_delivery = false; discovery_delivery = std::make_shared<std::atomic<bool>>(true); discovering = true;
  }
  void cancel_discovery() {
    std::lock_guard<std::mutex> guard(mutex); *discovery_delivery = false; discovering = false;
  }
  bool found(Json device) {
    if (stopping() || !discovering) return false;
    std::shared_ptr<std::atomic<bool>> current;
    {
      std::lock_guard<std::mutex> guard(mutex);
      if (!discovering || !discovery_delivery->load()) return false;
      auto id = device.at("deviceId").get<std::string>();
      if (id.empty() || id.size() > 256 || device.dump().size() > 65536) throw std::runtime_error("native Bluetooth advertisement exceeded its bound");
      if (!known_devices.count(id) && known_devices.size() >= 256) return true;
      known_devices[id] = device; current = discovery_delivery;
    }
    DeviceQueued item{DeviceQueued::NativeEvent, "", "BluetoothDeviceFound", "", {{"devices", Json::array({std::move(device)})}}};
    item.delivery_allowed = current;
    if (!push(std::move(item))) { fault("native Bluetooth discovery backpressure limit exceeded"); return false; }
    return true;
  }
  void connected(const std::string& id, bool value) {
    { std::lock_guard<std::mutex> guard(mutex); if (value) connected_ids.insert(id); else connected_ids.erase(id); }
    event("BLEConnectionStateChange", {{"deviceId", id}, {"connected", value}});
  }
  bool contains_device(const std::string& id) {
    std::lock_guard<std::mutex> guard(mutex); return known_devices.count(id) != 0;
  }
  Json device(const std::string& id) {std::lock_guard<std::mutex> guard(mutex); return known_devices.at(id);}
  Json devices(bool connected_only = false) {
    std::lock_guard<std::mutex> guard(mutex); Json result = Json::array();
    for (const auto& [id, value] : known_devices) if (!connected_only || connected_ids.count(id)) result.push_back(value);
    return result;
  }
  std::shared_ptr<std::promise<std::shared_ptr<void>>> reserve(const std::shared_ptr<BleCommand>& command) {
    auto ticket = std::make_shared<std::promise<std::shared_ptr<void>>>(); auto future = ticket->get_future();
    { std::lock_guard<std::mutex> guard(mutex);
      if (stopping() || outstanding >= 32) throw std::invalid_argument("native Bluetooth command capacity is unavailable");
      commands.push_back({command, std::move(future)}); ++outstanding;
    }
    changed.notify_all(); return ticket;
  }
  std::optional<BlePending> take() {
    std::lock_guard<std::mutex> guard(mutex); if (commands.empty()) return {};
    auto item = std::move(commands.front()); commands.pop_front(); return item;
  }
  void command_finished() { std::lock_guard<std::mutex> guard(mutex); --outstanding; }
  void wait() { std::unique_lock<std::mutex> guard(mutex); changed.wait_for(guard, std::chrono::milliseconds(10), [&] { return stopping() || !commands.empty(); }); }
  void cancel() {
    cancelled = true; *delivery = false;
    { std::lock_guard<std::mutex> guard(mutex); *discovery_delivery = false; discovering = false; for (auto& [key, token] : notification_deliveries) *token = false; for (auto& pending : commands) *pending.command->delivery = false; }
    changed.notify_all();
  }
};
class BleBackend {
 public:
  virtual ~BleBackend() = default;
  virtual void open(const std::shared_ptr<NativeBle>&) = 0;
  // Called even after cancellation when an owner-thread preparation acquired
  // an async operation. Drain that operation and close late native results.
  // Stop/disconnect/notification-disable commands must also perform their
  // release action even when their guest result has been cancelled.
  virtual Json execute(const std::shared_ptr<NativeBle>&, const std::shared_ptr<BleCommand>&, const std::shared_ptr<void>& preparation) = 0;
  virtual void poll(const std::shared_ptr<NativeBle>&) {}
  // False or an exception retains this backend and handles for another attempt.
  virtual bool cleanup(const std::shared_ptr<NativeBle>&) { return true; }
};
struct BlePlatform {
  std::vector<std::string> apis, events;
  std::function<std::unique_ptr<BleBackend>()> factory;
  std::function<std::shared_ptr<void>(const DeviceApiOptions&, const std::string&, const Json&, const Json& native_device)> prepare;
};
inline Json ble_payload(const std::string& api, Json payload) {
  auto timeout = payload.value("timeout", int64_t(10000));
  if (timeout < 100 || timeout > 60000) throw std::invalid_argument("Bluetooth timeout must be 100..60000 milliseconds");
  payload["timeout"] = timeout;
  if (api == "wx.openBluetoothAdapter" && payload.value("mode", "central") != "central") throw std::invalid_argument("native Bluetooth supports central mode only");
  if (payload.contains("services")) {
    auto& services = payload.at("services"); if (!services.is_array() || services.size() > 16) throw std::invalid_argument("Bluetooth service filter limit exceeded");
    for (auto& value : services) value = ble_uuid(value.get<std::string>());
  }
  for (auto field : {"serviceId", "characteristicId"}) if (payload.contains(field)) payload[field] = ble_uuid(payload[field].get<std::string>());
  if (api == "wx.notifyBLECharacteristicValueChange" && !payload.at("state").is_boolean()) throw std::invalid_argument("Bluetooth notification state must be boolean");
  if (api == "wx.writeBLECharacteristicValue") {
    auto type = payload.value("writeType", "write");
    if (type != "write" && type != "writeNoResponse") throw std::invalid_argument("unsupported Bluetooth writeType");
    auto bytes = binary_bytes(payload.at("value"));
    if (bytes.size() > 1048576) throw std::invalid_argument("Bluetooth write exceeds one MiB");
  }
  return payload;
}
class DeviceBluetooth : public DeviceLocations {
  BlePlatform bluetooth_platform_;
  std::shared_ptr<NativeBle> bluetooth_;
  std::vector<std::shared_ptr<NativeBle>> bluetooth_owners_;
  struct Intent {std::shared_ptr<std::atomic<bool>> delivery; std::string api, characteristic, device; bool notification_enable=false;};
  std::map<std::string, Intent> bluetooth_intents_;
  struct Stop {std::string call; std::vector<std::shared_ptr<NativeBle>> states;};
  std::vector<Stop> bluetooth_stops_;
  void reap_bluetooth() {
    bluetooth_owners_.erase(std::remove_if(bluetooth_owners_.begin(), bluetooth_owners_.end(), [](const auto& state) { return state->finished.load() && state->failures_drained(); }), bluetooth_owners_.end());
  }
  void open_bluetooth(const std::string& call, const std::shared_ptr<std::atomic<bool>>& intent) {
    if (!intent->load() || !foreground_) { fail(call, "wx.openBluetoothAdapter", "Bluetooth request was retired", "cancelled"); return; }
    if (bluetooth_ && !bluetooth_->stopping() && !bluetooth_->finished) {
      if (!bluetooth_->opened) throw std::invalid_argument("Bluetooth adapter startup is already pending");
      DeviceQueued result{DeviceQueued::Complete, call, "", "", device_ok("wx.openBluetoothAdapter")}; result.delivery_allowed = intent;
      if (!bluetooth_->push(std::move(result))) bluetooth_->fail_call(call, "wx.openBluetoothAdapter", "native Bluetooth completion queue is full"); return;
    }
    reap_bluetooth(); if (bluetooth_owners_.size() >= 8) throw std::invalid_argument("native Bluetooth owner limit exceeded");
    auto state = std::make_shared<NativeBle>(options_, weak_queue(), call); state->delivery = intent;
    auto factory = bluetooth_platform_.factory;
    workers_.reserve(workers_.size() + 1); bluetooth_owners_.reserve(bluetooth_owners_.size() + 1);
    workers_.push_back(std::async(std::launch::async, [state, factory] {
      std::unique_ptr<BleBackend> source; std::string error; int64_t native_error = 0;
      auto execute = [&](BlePending pending) {
        auto command = pending.command;
        try {
          auto preparation = pending.preparation.get();
          // Preparation may own an OS permission operation even if the guest
          // cancelled before this worker reached the command.
          auto result = source->execute(state, command, preparation);
          if (!state->stopping() && !command->cancelled()) {
            DeviceQueued receipt{DeviceQueued::Complete, command->call, "", "", device_ok(command->api, std::move(result))}; receipt.delivery_allowed = command->delivery;
            if (!state->push(std::move(receipt))) state->fail_call(command->call, command->api, "native Bluetooth completion queue is full");
          }
        } catch (const std::system_error& failure) {
          if (!state->cancelled && !command->cancelled()) state->fail_call(command->call, command->api, failure.what(), failure.code().value());
        } catch (const std::exception& failure) {
          if (!state->cancelled && !command->cancelled()) state->fail_call(command->call, command->api, failure.what());
        } catch (...) {
          if (!state->cancelled && !command->cancelled()) state->fail_call(command->call, command->api, "native Bluetooth command failed");
        }
        state->command_finished();
      };
      try {
        source = factory(); if (!source) throw std::runtime_error("native Bluetooth backend is unavailable");
        source->open(state);
        if (!state->stopping()) {
          state->opened = true;
          if (!state->push({DeviceQueued::Complete, state->call, "", "", device_ok("wx.openBluetoothAdapter")})) throw std::runtime_error("native Bluetooth completion queue is full");
        }
        while (!state->stopping()) {
          if (auto pending = state->take()) execute(std::move(*pending));
          else {source->poll(state); state->wait();}
        }
        error = state->failure();
      } catch (const std::system_error& failure) {error = failure.what(); native_error = failure.code().value();}
      catch (const std::exception& failure) {error = failure.what();}
      catch (...) {error = "native Bluetooth backend failed";}
      if (!error.empty()) state->fault(error);
      // UI-created operations retain their ownership even on source faults.
      // Execute their cancellation/drain path before releasing the backend.
      if (source) {
        while (auto pending = state->take()) execute(std::move(*pending));
        while (true) {
          try {if (source->cleanup(state)) break;}
          catch (const std::exception& failure) {state->cleanup_failed(failure.what());}
          catch (...) {state->cleanup_failed("native Bluetooth cleanup failed");}
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
      }
      source.reset(); state->cleanup_succeeded(); state->opened = false;
      if (!state->cancelled && !error.empty()) {
        state->fail_call(state->call, "wx.openBluetoothAdapter", error, native_error);
        state->event("BluetoothAdapterStateChange", {{"available", false}, {"discovering", false}, {"errMsg", socket_terminal_text(error, 512)}, {"errCode", native_error}}, true);
      }
      state->finished = true; state->changed.notify_all();
    }));
    bluetooth_ = state; bluetooth_owners_.push_back(state); calls_.at(call).cancel = [state] {state->cancel();};
  }
  void cancel_bluetooth() {
    for (const auto& [call, intent] : bluetooth_intents_) {
      *intent.delivery = false;
      if (!queue_->closed && calls_.count(call)) fail(call, intent.api, "native Bluetooth owner was retired", "cancelled");
    }
    for (const auto& state : bluetooth_owners_) state->cancel();
  }
 protected:
  DeviceBluetooth(DeviceApiOptions options, Command command) : DeviceLocations(std::move(options), std::move(command)) {}
  void install_bluetooth_platform(BlePlatform platform) {
    if (!platform.factory) return;
    bluetooth_platform_ = std::move(platform);
    for (const auto& api : bluetooth_platform_.apis) add(api);
    for (const auto& event : bluetooth_platform_.events) add_event("wx.on" + event, "wx.off" + event, event);
  }
  bool bluetooth_call(const std::string& call, const std::string& api, const Json& request) {
    if (!bluetooth_platform_.factory || std::find(bluetooth_platform_.apis.begin(), bluetooth_platform_.apis.end(), api) == bluetooth_platform_.apis.end()) return false;
    if (api == "wx.closeBluetoothAdapter") {
      cancel_bluetooth();
      reap_bluetooth(); if (bluetooth_owners_.empty()) done(call, api); else bluetooth_stops_.push_back({call, bluetooth_owners_});
      return true;
    }
    if (!foreground_) throw std::invalid_argument("Bluetooth requires a foreground application");
    auto payload = ble_payload(api, request); auto intent = std::make_shared<std::atomic<bool>>(true);
    bluetooth_intents_[call] = {intent, api, api == "wx.notifyBLECharacteristicValueChange" ? ble_characteristic_key(payload) : "", payload.value("deviceId", ""), api == "wx.notifyBLECharacteristicValueChange" && payload.at("state").get<bool>()}; calls_.at(call).cancel = [intent] {*intent = false;};
    if (api == "wx.openBluetoothAdapter") {
      permit("scope.bluetooth", api, call, [this, call, intent] {try {open_bluetooth(call, intent);} catch (const std::exception& error) {fail(call, "wx.openBluetoothAdapter", error.what());}}); return true;
    }
    if (!bluetooth_ || !bluetooth_->opened || bluetooth_->stopping() || bluetooth_->finished) throw std::invalid_argument("Bluetooth adapter is not open");
    if (payload.contains("deviceId") && !bluetooth_->contains_device(payload.at("deviceId").get<std::string>())) throw std::invalid_argument("Bluetooth device was not discovered by this application");
    calls_.at(call).permission_generation = permission_generation_;
    if (api == "wx.stopBluetoothDevicesDiscovery") {
      for (const auto& [pending, operation] : bluetooth_intents_) if (operation.api == "wx.startBluetoothDevicesDiscovery") {
        *operation.delivery = false;
        if (calls_.count(pending)) fail(pending, operation.api, "Bluetooth discovery was stopped", "cancelled");
      }
      bluetooth_->cancel_discovery();
    }
    if (api == "wx.notifyBLECharacteristicValueChange" && !payload.at("state").get<bool>()) {
      auto key = ble_characteristic_key(payload);
      for (const auto& [pending, operation] : bluetooth_intents_) if (operation.notification_enable && operation.characteristic == key) {
        *operation.delivery = false; if (calls_.count(pending)) fail(pending, operation.api, "Bluetooth notifications were disabled", "cancelled");
      }
      bluetooth_->cancel_notification(key);
    }
    if (api == "wx.closeBLEConnection") {
      auto id = payload.at("deviceId").get<std::string>(); bluetooth_->cancel_device_notifications(id);
      for (const auto& [pending, operation] : bluetooth_intents_) if (operation.notification_enable && operation.device == id) {
        *operation.delivery = false; if (calls_.count(pending)) fail(pending, operation.api, "Bluetooth connection was closed", "cancelled");
      }
    }
    auto command = std::make_shared<BleCommand>(); command->call = call; command->api = api; command->payload = payload; command->timeout_ms = long(payload.at("timeout").get<int64_t>()); command->delivery = intent;
    auto ticket = bluetooth_->reserve(command);
    try {ticket->set_value(bluetooth_platform_.prepare ? bluetooth_platform_.prepare(options_, api, payload, payload.contains("deviceId") ? bluetooth_->device(payload.at("deviceId").get<std::string>()) : Json::object()) : nullptr);}
    catch (...) {ticket->set_exception(std::current_exception());}
    return true;
  }
  void permissions_reset() override {cancel_bluetooth(); DeviceLocations::permissions_reset();}
  void retire_platform() override {cancel_bluetooth(); DeviceLocations::retire_platform();}
  Status foreground_changed(bool visible) override {if (!visible) cancel_bluetooth(); return DeviceLocations::foreground_changed(visible);}
 public:
  ~DeviceBluetooth() override {retire();}
  Status pump() override {
    auto status = DeviceLocations::pump(); if (!status) return status;
    for (const auto& state : bluetooth_owners_) state->flush_failures();
    for (auto item = bluetooth_intents_.begin(); item != bluetooth_intents_.end();) if (!calls_.count(item->first)) item = bluetooth_intents_.erase(item); else ++item;
    for (auto item = bluetooth_stops_.begin(); item != bluetooth_stops_.end();) {
      if (!calls_.count(item->call)) {item = bluetooth_stops_.erase(item); continue;}
      if (!std::all_of(item->states.begin(), item->states.end(), [](const auto& state) {return state->finished.load();})) {++item; continue;}
      done(item->call, "wx.closeBluetoothAdapter"); item = bluetooth_stops_.erase(item);
    }
    reap_bluetooth(); return status;
  }
};
}  // namespace weapp::desktop::detail
