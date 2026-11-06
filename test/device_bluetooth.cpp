#include "device_bluetooth_common.hpp"
#include "core_host_services.hpp"
#include <iostream>
using namespace weapp::desktop;
using namespace weapp::desktop::detail;
using namespace weapp::desktop::detail::test;
namespace {
void require(bool value, const char* message) {if (!value) throw std::runtime_error(message);}
const std::string service = ble_uuid("180D"), characteristic = ble_uuid("2A37");
struct Control {
  std::atomic<bool> fail_command{false}, created{false}, allow_open{true}, acquired{false}, closed{false}, fail_cleanup{false};
  std::atomic<bool> allow_command{true}, command_waiting{false}, allow_connect{true}, connect_waiting{false}, late_acquired{false}, prepared_closed{false};
  std::atomic<unsigned> cleanup_attempts{0}, discovery_starts{0}, emit{0}, produced{0}, commands{0};
};
struct Prepared {
  std::shared_ptr<Control> control;
  explicit Prepared(std::shared_ptr<Control> state) : control(std::move(state)) {}
  ~Prepared() {control->prepared_closed = true;}
};
class FakeBluetooth final : public BleBackend {
  std::shared_ptr<Control> control_;
  std::shared_ptr<std::atomic<bool>> notification_;
 public:
  explicit FakeBluetooth(std::shared_ptr<Control> control) : control_(std::move(control)) {control_->created = true;}
  ~FakeBluetooth() override {control_->closed = true;}
  void open(const std::shared_ptr<NativeBle>&) override {
    while (!control_->allow_open) std::this_thread::sleep_for(std::chrono::milliseconds(1)); control_->acquired = true;
  }
  Json execute(const std::shared_ptr<NativeBle>& state, const std::shared_ptr<BleCommand>& command, const std::shared_ptr<void>& preparation) override {
    ++control_->commands;
    if (command->api == "wx.createBLEConnection") {
      require(bool(preparation), "connection preparation was lost"); control_->connect_waiting = true;
      while (!control_->allow_connect) std::this_thread::sleep_for(std::chrono::milliseconds(1)); control_->late_acquired = true;
      if (!state->stopping() && !command->cancelled()) state->connected("fixture-device", true);
      return Json::object();
    }
    if (command->api == "wx.stopBluetoothDevicesDiscovery") {state->cancel_discovery(); return Json::object();}
    if (command->api == "wx.closeBLEConnection") {state->cancel_device_notifications("fixture-device"); state->connected("fixture-device", false); return Json::object();}
    if (command->api == "wx.notifyBLECharacteristicValueChange" && !command->payload.at("state").get<bool>()) {if (notification_) *notification_ = false; return Json::object();}
    if (state->stopping() || command->cancelled()) return Json::object();
    if (command->api == "wx.getBluetoothAdapterState") {
      control_->command_waiting = true; if (control_->fail_command) throw std::system_error(std::error_code(77,std::generic_category()),"fake native failure"); while (!control_->allow_command) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      return {{"available", true}, {"discovering", bool(state->discovering)}};
    }
    if (command->api == "wx.startBluetoothDevicesDiscovery") {
      ++control_->discovery_starts; state->begin_discovery();
      state->found({{"deviceId", "fixture-device"}, {"addressType", "random"}, {"name", "fixture"}, {"advertisServiceUUIDs", Json::array({service})}}); ++control_->produced; return Json::object();
    }
    if (command->api == "wx.getBluetoothDevices") return {{"devices", state->devices()}};
    if (command->api == "wx.getConnectedBluetoothDevices") return {{"devices", state->devices(true)}};
    if (command->api == "wx.notifyBLECharacteristicValueChange") {notification_ = state->begin_notification(ble_characteristic_key(command->payload)); return Json::object();}
    if (command->api == "wx.getBLEDeviceServices") return {{"services", Json::array({{{"uuid", service}, {"isPrimary", true}}})}};
    if (command->api == "wx.getBLEDeviceCharacteristics") return {{"characteristics", Json::array({{{"uuid", characteristic}, {"properties", {{"notify", true}}}}})}};
    return Json::object();
  }
  void poll(const std::shared_ptr<NativeBle>& state) override {
    if (control_->emit.exchange(0) && notification_) {
      state->event("BLECharacteristicValueChange", {{"deviceId", "fixture-device"}, {"serviceId", service}, {"characteristicId", characteristic}, {"value", inline_binary({1,2,3})}}, false, notification_); ++control_->produced;
    }
  }
  bool cleanup(const std::shared_ptr<NativeBle>& state) override {
    ++control_->cleanup_attempts;
    if (control_->fail_cleanup) {state->cleanup_failed("fake Bluetooth cleanup pending"); return false;} return true;
  }
};
class TestBluetooth final : public DeviceBluetooth {
 public:
  bool auto_grant = true; std::function<void(bool)> pending_grant;
  TestBluetooth(DeviceApiOptions options, Command command, std::shared_ptr<std::shared_ptr<Control>> controls) : DeviceBluetooth(std::move(options), std::move(command)) {
    BlePlatform platform;
    platform.apis = {"wx.openBluetoothAdapter", "wx.closeBluetoothAdapter", "wx.getBluetoothAdapterState", "wx.startBluetoothDevicesDiscovery", "wx.stopBluetoothDevicesDiscovery", "wx.getBluetoothDevices", "wx.getConnectedBluetoothDevices", "wx.createBLEConnection", "wx.closeBLEConnection", "wx.getBLEDeviceServices", "wx.getBLEDeviceCharacteristics", "wx.readBLECharacteristicValue", "wx.writeBLECharacteristicValue", "wx.notifyBLECharacteristicValueChange"};
    platform.events = {"BluetoothAdapterStateChange", "BluetoothDeviceFound", "BLEConnectionStateChange", "BLECharacteristicValueChange"};
    platform.factory = [controls] {return std::make_unique<FakeBluetooth>(*controls);};
    platform.prepare = [controls](const DeviceApiOptions&, const std::string& api, const Json&, const Json& native_device) -> std::shared_ptr<void> {
      if (api != "wx.createBLEConnection") return {};
      require(native_device.at("addressType") == "random", "guest metadata replaced trusted native discovery"); return std::make_shared<Prepared>(*controls);
    };
    install_bluetooth_platform(std::move(platform));
  }
  void reset() {reset_permissions();}
  void saturate_terminal_queue() {for (unsigned index=0;index<1024;++index) require(queue_->push({DeviceQueued::NativeTerminalEvent,"","fixture-unsubscribed","",Json::object()}),"fixture could not saturate native terminal queue");}
 protected:
  void request_permission(const std::string&, std::function<void(bool)> grant) override {if (auto_grant) grant(true); else pending_grant = std::move(grant);}
  bool platform_call(const std::string& call, const std::string& api, const Json& payload, const Json&) override {return bluetooth_call(call, api, payload);}
};
struct Fixture {
  std::filesystem::path root = std::filesystem::temp_directory_path() / ("weapp-ble-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::thread::id owner = std::this_thread::get_id();
  Json app = {{"hostAppId", "host"}, {"tenantId", "tenant"}, {"publisherId", "publisher"}, {"appId", "bluetooth"}, {"environment", "develop"}, {"version", "1"}, {"packRoot", std::string(64,'a')}};
  Json account = {{"principalId", "alice"}, {"epoch", "1"}};
  std::shared_ptr<std::shared_ptr<Control>> controls = std::make_shared<std::shared_ptr<Control>>(std::make_shared<Control>());
  std::vector<std::shared_ptr<Control>> all_controls{*controls};
  std::map<std::string,Json> receipts; std::vector<Json> events; std::unique_ptr<TestBluetooth> devices;
  static Value::Object object(const Json& value) {return std::get<Value::Object>(decode(value).data);}
  explicit Fixture(bool consent = true) {
    std::filesystem::create_directories(root / "user"); std::filesystem::create_directories(root / "native");
    if (consent) {std::ofstream file(root / "native/consents.json"); file << R"({"scope.bluetooth":true})";}
    DeviceApiOptions options; options.application = object(app); options.account = object(account);
    devices = std::make_unique<TestBluetooth>(options, [this](const std::string& operation, const Value::Object& fields, Value* output) {
      require(std::this_thread::get_id() == owner, "Bluetooth worker entered host callback");
      if (operation == "managedFileScope") *output = decode(Json{{"root", (root / "user").string()}, {"auxiliaryRoot", (root / "native").string()}, {"application", app}, {"account", account}});
      else if (operation == "completeHostCall") {auto value = encode(fields); receipts[value.at("callId").get<std::string>()] = value;}
      else if (operation == "hostApiEvent") events.push_back(encode(fields));
      return Status{};
    }, controls);
    for (auto event : {"BluetoothDeviceFound", "BLEConnectionStateChange", "BLECharacteristicValueChange"}) call("subscribe-" + std::string(event), "wx.on" + std::string(event), {{"subscriptionId", event}, {"action", "subscribe"}}, true);
  }
  ~Fixture() {
    for (const auto& control : all_controls) {control->allow_open = true; control->allow_command = true; control->allow_connect = true; control->fail_cleanup = false;}
    if (devices) devices->retire(); devices.reset(); std::error_code error; std::filesystem::remove_all(root, error);
  }
  std::shared_ptr<Control> control() {return *controls;}
  void next() {*controls = std::make_shared<Control>(); all_controls.push_back(*controls);}
  void call(const std::string& id, const std::string& api, Json payload = Json::object(), bool retained = false) {
    devices->dispatch({EventType::HostCall, object(Json{{"callId", id}, {"method", api}, {"payload", payload}, {"retainUntilCancelled", retained}, {"context", {{"application", app}, {"account", account}, {"sessionId", "session"}, {"callId", id}, {"api", api}}}})});
  }
  void pump() {require(bool(devices->pump()), "Bluetooth owner pump failed");}
  template<class Predicate> void until(Predicate predicate, bool pump_owner = true) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {if (pump_owner) pump(); std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    require(predicate(), "Bluetooth fixture timed out");
  }
  void open() {call("open", "wx.openBluetoothAdapter"); until([&] {return receipts.count("open");}); require(receipts.at("open").contains("result"), "Bluetooth adapter did not open");}
  void discover() {call("discover", "wx.startBluetoothDevicesDiscovery"); until([&] {return receipts.count("discover") && !events.empty();}); events.clear();}
};
void validation_and_actual_catalog() {
  require(ble_uuid("180d") == service, "Bluetooth UUID normalization failed");
  bool rejected = false; try {ble_payload("wx.startBluetoothDevicesDiscovery", {{"services", Json::array({"bad"})}});} catch (const std::exception&) {rejected = true;} require(rejected, "invalid Bluetooth UUID accepted");
  Fixture fixture; configure_actual_core_catalog(*fixture.devices, fixture.app, fixture.account); fixture.open();
  fixture.call("unknown", "wx.createBLEConnection", {{"deviceId", "not-discovered"}}); fixture.until([&] {return fixture.receipts.count("unknown");}); require(fixture.receipts.at("unknown").contains("error") && !fixture.control()->connect_waiting, "unknown device acquired native permission");
}
void close_waits_for_late_native_acquisition() {
  Fixture fixture; fixture.open(); fixture.discover(); auto control = fixture.control(); control->allow_connect = false; control->fail_cleanup = true;
  fixture.call("connect", "wx.createBLEConnection", {{"deviceId", "fixture-device"}, {"addressType", "public"}}); fixture.until([&] {return control->connect_waiting.load();});
  fixture.devices->dispatch({EventType::HostCallCancelled, Fixture::object(Json{{"callId", "connect"}})}); fixture.call("close", "wx.closeBluetoothAdapter"); fixture.pump();
  require(!fixture.receipts.count("close"), "adapter close escaped pending native acquisition");
  control->allow_connect = true; fixture.until([&] {return control->late_acquired && control->prepared_closed && control->cleanup_attempts >= 2;});
  require(!control->closed && !fixture.receipts.count("close") && fixture.events.empty(), "late native BLE result escaped cancellation or cleanup barrier");
  control->fail_cleanup = false; fixture.until([&] {return fixture.receipts.count("close");}); require(control->closed && fixture.receipts.at("close").contains("result"), "close receipt preceded Bluetooth source destruction");
}
void stop_revokes_queued_discovery_start() {
  Fixture fixture; fixture.open(); auto control = fixture.control(); control->allow_command = false;
  fixture.call("blocked", "wx.getBluetoothAdapterState"); fixture.until([&] {return control->command_waiting.load();});
  fixture.call("start", "wx.startBluetoothDevicesDiscovery"); fixture.call("stop", "wx.stopBluetoothDevicesDiscovery"); control->allow_command = true;
  fixture.until([&] {return fixture.receipts.count("start") && fixture.receipts.count("stop");});
  require(control->discovery_starts == 0 && fixture.receipts.at("start").contains("error") && fixture.events.empty(), "queued discovery start escaped stop");
}
void notification_disable_revokes_queued_values() {
  Fixture fixture; fixture.open(); fixture.discover(); Json request = {{"deviceId", "fixture-device"}, {"serviceId", service}, {"characteristicId", characteristic}, {"state", true}};
  fixture.call("notify", "wx.notifyBLECharacteristicValueChange", request); fixture.until([&] {return fixture.receipts.count("notify");});
  auto produced = fixture.control()->produced.load(); fixture.control()->emit = 1; fixture.until([&] {return fixture.control()->produced > produced;}, false);
  request["state"] = false; fixture.call("disable", "wx.notifyBLECharacteristicValueChange", request); fixture.until([&] {return fixture.receipts.count("disable");}); require(fixture.events.empty(), "queued notification survived disable acceptance");
}
void pending_consent_reset_and_background() {
  Fixture consent(false); consent.devices->auto_grant = false; consent.call("pending", "wx.openBluetoothAdapter"); require(bool(consent.devices->pending_grant), "Bluetooth consent not requested");
  consent.call("close", "wx.closeBluetoothAdapter"); consent.devices->pending_grant(true); consent.until([&] {return consent.receipts.count("pending") && consent.receipts.count("close");}); require(!consent.control()->created, "late application consent reopened a closed adapter");
  Fixture fixture; fixture.open(); fixture.discover(); auto request = Json{{"deviceId", "fixture-device"}, {"serviceId", service}, {"characteristicId", characteristic}, {"state", true}};
  fixture.call("notify", "wx.notifyBLECharacteristicValueChange", request); fixture.until([&] {return fixture.receipts.count("notify");});
  auto produced = fixture.control()->produced.load(); fixture.control()->emit = 1; fixture.until([&] {return fixture.control()->produced > produced;}, false);
  fixture.devices->reset(); fixture.until([&] {return fixture.control()->closed.load();}); require(fixture.events.empty(), "queued notification survived application permission reset");
  fixture.next(); fixture.receipts.erase("open"); fixture.open(); require(bool(fixture.devices->set_foreground(false)), "Bluetooth background transition failed");
  fixture.until([&] {return fixture.control()->closed.load();}); fixture.call("hidden", "wx.openBluetoothAdapter"); fixture.until([&] {return fixture.receipts.count("hidden");}); require(fixture.receipts.at("hidden").contains("error"), "background application acquired Bluetooth");
}
void cancelled_owners_and_commands_keep_capacity() {
  Fixture fixture;
  for (unsigned index = 0; index < 8; ++index) {
    if (index) fixture.next(); auto control = fixture.control(); control->allow_open = false; auto id = "pending-" + std::to_string(index);
    fixture.call(id, "wx.openBluetoothAdapter"); fixture.until([&] {return control->created.load();}); fixture.devices->dispatch({EventType::HostCallCancelled, Fixture::object(Json{{"callId", id}})});
  }
  fixture.next(); fixture.call("overflow", "wx.openBluetoothAdapter"); fixture.until([&] {return fixture.receipts.count("overflow");}); require(fixture.receipts.at("overflow").contains("error") && !fixture.control()->created, "cancelled owners bypassed BLE capacity");
  fixture.call("close", "wx.closeBluetoothAdapter"); fixture.pump(); require(!fixture.receipts.count("close"), "adapter close did not retain every prior owner");
  for (const auto& control : fixture.all_controls) control->allow_open = true; fixture.until([&] {return fixture.receipts.count("close");});
  require(std::count_if(fixture.all_controls.begin(), fixture.all_controls.end(), [](const auto& control) {return control->closed.load();}) == 8, "close acknowledged before all cancelled owners drained");
  Fixture commands; commands.open(); auto control = commands.control(); control->allow_command = false;
  commands.call("blocked", "wx.getBluetoothAdapterState"); commands.until([&] {return control->command_waiting.load();});
  for (unsigned index = 0; index < 31; ++index) commands.call("queued-" + std::to_string(index), "wx.getBluetoothAdapterState");
  commands.call("command-overflow", "wx.getBluetoothAdapterState"); commands.until([&] {return commands.receipts.count("command-overflow");}); require(commands.receipts.at("command-overflow").contains("error"), "BLE command queue exceeded its bound");
  auto pending = commands.devices->retire(); require(!pending && pending.code == "ERR_RESOURCE_CLEANUP_PENDING", "pending BLE worker blocked or acknowledged retirement"); control->allow_command = true; commands.until([&] {return bool(commands.devices->retire());}, false); require(control->closed, "BLE retirement discarded worker ownership");
}
void saturated_failure_eventually_settles_call() {
  Fixture fixture; fixture.open(); fixture.devices->saturate_terminal_queue(); fixture.control()->fail_command = true;
  fixture.call("native-failure", "wx.getBluetoothAdapterState"); fixture.until([&] {return fixture.control()->closed.load();}, false);
  fixture.until([&] {return fixture.receipts.count("native-failure");});
  require(fixture.receipts.at("native-failure").contains("error") && fixture.receipts.at("native-failure").at("error").at("details").at("errCode") == 77, "saturated command failure was abandoned or lost its native code");
}
void saturated_success_eventually_settles_call() {
  Fixture fixture; fixture.open(); fixture.devices->saturate_terminal_queue();
  fixture.call("native-success", "wx.getBluetoothAdapterState"); fixture.until([&] {return fixture.control()->closed.load();}, false);
  fixture.until([&] {return fixture.receipts.count("native-success");});
  require(fixture.receipts.at("native-success").contains("error"), "saturated command receipt was abandoned");
  Fixture reopened; reopened.open(); reopened.devices->saturate_terminal_queue();
  reopened.call("open-again", "wx.openBluetoothAdapter"); reopened.until([&] {return reopened.control()->closed.load();}, false);
  reopened.until([&] {return reopened.receipts.count("open-again");});
  require(reopened.receipts.at("open-again").contains("error"), "saturated existing-adapter receipt was abandoned");
}

}
int main() {
  try {validation_and_actual_catalog(); close_waits_for_late_native_acquisition(); stop_revokes_queued_discovery_start(); notification_disable_revokes_queued_values(); pending_consent_reset_and_background(); cancelled_owners_and_commands_keep_capacity(); saturated_failure_eventually_settles_call(); saturated_success_eventually_settles_call();
    std::cout << "Bluetooth lifecycle: 8 groups passed (fake OS backend; actual core catalog; no radio/device acceptance)\n"; return 0;
  } catch (const std::exception& error) {std::cerr << error.what() << '\n'; return 1;}
}
