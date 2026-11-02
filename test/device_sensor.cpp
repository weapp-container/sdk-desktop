#include "device_sensor_common.hpp"
#include <iostream>
using namespace weapp::desktop;
using namespace weapp::desktop::detail;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Control {
  std::atomic<bool> created{false}, allow_acquire{true}, acquired{false}, announced{false}, closed{false}, fail_cleanup{false}, fail_start{false};
  std::atomic<unsigned> cleanup_attempts{0}, emit{0}, produced{0};
};
class FakeSource final : public SensorSource {
  std::shared_ptr<Control> control_;
 public:
  explicit FakeSource(std::shared_ptr<Control> control) : control_(std::move(control)) { control_->created = true; }
  ~FakeSource() override { control_->closed = true; }
  void run(const std::shared_ptr<NativeSensor>& state) override {
    while (!control_->allow_acquire) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    control_->acquired = true;
    if (control_->fail_start) throw std::system_error(std::error_code(5, std::generic_category()), "native start denied");
    state->announce_started(); control_->announced = true;
    while (!state->stopping()) {
      if (control_->emit.exchange(0)) { state->sample(state->kind == SensorKind::Compass ? sensor_heading(270, "high") : sensor_vector(1, 2, 3)); ++control_->produced; }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  bool cleanup(const std::shared_ptr<NativeSensor>& state) override {
    ++control_->cleanup_attempts;
    if (control_->fail_cleanup) { state->cleanup_failed("native removal pending"); return false; }
    return true;
  }
};
class TestSensors final : public DeviceSensors {
 public:
  TestSensors(DeviceApiOptions options, Command command, std::shared_ptr<std::shared_ptr<Control>> control)
      : DeviceSensors(std::move(options), std::move(command)) {
    install_sensor_factory([control](SensorKind) { return std::make_unique<FakeSource>(*control); });
  }
  void reset() { reset_permissions(); }
  void saturate() { unsigned count = 0; while (queue_->push({DeviceQueued::NativeTerminalEvent, "", "unsubscribed", "", Json::object()})) ++count; require(count >= 512, "fixture ingress was already unexpectedly full"); }
 protected:
  void request_permission(const std::string&, std::function<void(bool)> callback) override { callback(false); }
  bool platform_call(const std::string& call, const std::string& api, const Json& payload, const Json&) override { return sensor_call(call, api, payload); }
};
struct Fixture {
  std::filesystem::path root = std::filesystem::temp_directory_path() / ("weapp-sensor-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::thread::id owner = std::this_thread::get_id();
  Json app = {{"hostAppId", "host"}, {"tenantId", "tenant"}, {"publisherId", "publisher"}, {"appId", "sensors"}, {"environment", "develop"}, {"version", "1"}, {"packRoot", std::string(64, 'a')}};
  Json account = {{"principalId", "alice"}, {"epoch", "1"}};
  std::shared_ptr<std::shared_ptr<Control>> controls = std::make_shared<std::shared_ptr<Control>>(std::make_shared<Control>());
  std::vector<std::shared_ptr<Control>> all_controls{*controls};
  std::map<std::string, Json> receipts; std::vector<Json> events;
  std::unique_ptr<TestSensors> devices;
  static Value::Object object(const Json& value) { return std::get<Value::Object>(decode(value).data); }
  Fixture() {
    std::filesystem::create_directories(root / "user"); std::filesystem::create_directories(root / "native");
    DeviceApiOptions options; options.application = object(app); options.account = object(account);
    devices = std::make_unique<TestSensors>(options, [this](const std::string& operation, const Value::Object& fields, Value* output) {
      require(std::this_thread::get_id() == owner, "sensor callback entered runtime off its owner thread");
      if (operation == "managedFileScope") *output = decode(Json{{"root", (root / "user").string()}, {"auxiliaryRoot", (root / "native").string()}, {"application", app}, {"account", account}});
      else if (operation == "completeHostCall") { auto value = encode(fields); receipts[value.at("callId").get<std::string>()] = value; }
      else if (operation == "hostApiEvent") events.push_back(encode(fields));
      return Status{};
    }, controls);
    for (auto name : {"Accelerometer", "Compass", "Gyroscope"}) call("subscribe-" + std::string(name), "wx.on" + std::string(name) + "Change", {{"subscriptionId", name}, {"action", "subscribe"}}, true);
  }
  ~Fixture() {
    for (auto& control : all_controls) { control->allow_acquire = true; control->fail_cleanup = false; }
    if (devices) { auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3); while (!devices->retire() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    devices.reset(); std::error_code error; std::filesystem::remove_all(root, error);
  }
  std::shared_ptr<Control> control() { return *controls; }
  void next() { *controls = std::make_shared<Control>(); all_controls.push_back(*controls); }
  void call(const std::string& id, const std::string& api, Json payload = Json::object(), bool retained = false) {
    devices->dispatch({EventType::HostCall, object(Json{{"callId", id}, {"method", api}, {"payload", payload}, {"retainUntilCancelled", retained}, {"context", {{"application", app}, {"account", account}, {"sessionId", "session"}, {"callId", id}, {"api", api}}}})});
  }
  void pump() { require(bool(devices->pump()), "sensor owner pump failed"); }
  template<class Predicate> void until(Predicate predicate, bool pump_owner = true) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) { if (pump_owner) pump(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    require(predicate(), "sensor fixture timed out");
  }
  void start(const std::string& name = "Accelerometer") { call("start", "wx.start" + name); until([&] { return receipts.count("start"); }); require(receipts.at("start").contains("result"), "sensor did not start"); }
};
void projection_and_catalog() {
  require(path_utf8(std::filesystem::u8path("资料/測試.bin")) == "资料/測試.bin", "C++20 managed path lost UTF-8 bytes");
  auto radians = sensor_vector(180, -90, 0, true); require(std::abs(radians["x"].get<double>() - 3.141592653589793) < 1e-10 && radians["y"].get<double>() < 0, "gyroscope was not converted to radians");
  require(sensor_vector(1, -1, .5)["z"] == .5, "accelerometer g units changed");
  require(sensor_heading(360, "")["direction"] == 0 && !sensor_heading(90, "").contains("accuracy"), "unknown compass accuracy was invented");
  require(sensor_interval(SensorKind::Gyroscope, {{"interval", "game"}}) == 20, "sampling period mapping failed");
  bool rejected = false; try { sensor_interval(SensorKind::Accelerometer, {{"interval", "fastest"}}); } catch (...) { rejected = true; } require(rejected, "unknown interval accepted");
  rejected = false; try { sensor_vector(NAN, 0, 0); } catch (...) { rejected = true; } require(rejected, "non-finite native vector accepted");
  Fixture fixture; auto apis = encode(fixture.devices->configuration()).at("apis");
  for (auto name : {"Accelerometer", "Compass", "Gyroscope"}) for (auto prefix : {"wx.start", "wx.stop", "wx.on", "wx.off"}) {
    auto api = std::string(prefix) + name + ((std::string(prefix) == "wx.on" || std::string(prefix) == "wx.off") ? "Change" : "");
    require(std::find(apis.begin(), apis.end(), Json(api)) != apis.end(), "installed sensor API missing");
  }
}
void stop_drains_and_revokes_values() {
  Fixture fixture; fixture.start(); fixture.control()->emit = 1; fixture.until([&] { return fixture.control()->produced == 1; }, false);
  fixture.control()->fail_cleanup = true; fixture.call("stop", "wx.stopAccelerometer");
  fixture.until([&] { return fixture.control()->cleanup_attempts >= 2; });
  require(fixture.events.empty() && !fixture.receipts.count("stop") && !fixture.control()->closed, "stop escaped exact native cleanup or delivered stale value");
  fixture.call("restart", "wx.startAccelerometer"); fixture.until([&] { return fixture.receipts.count("restart"); }); require(fixture.receipts.at("restart").contains("error"), "new source raced pending old cleanup");
  fixture.control()->fail_cleanup = false; fixture.until([&] { return fixture.receipts.count("stop"); }); require(fixture.control()->closed && fixture.receipts.at("stop").contains("result"), "stop did not wait for source destruction");
}
void late_acquire_and_cancelled_receipt() {
  Fixture fixture; fixture.control()->allow_acquire = false; fixture.call("pending", "wx.startGyroscope"); fixture.until([&] { return fixture.control()->created.load(); });
  fixture.call("duplicate", "wx.startGyroscope"); fixture.until([&] { return fixture.receipts.count("duplicate"); }); require(fixture.receipts.at("duplicate").contains("error"), "pending native acquisition acknowledged active sensor");
  fixture.call("stop", "wx.stopGyroscope"); fixture.pump(); require(!fixture.receipts.count("stop"), "pending acquisition counted as drained");
  fixture.control()->allow_acquire = true; fixture.until([&] { return fixture.receipts.count("stop") && fixture.receipts.count("pending"); }); require(fixture.control()->closed && fixture.receipts.at("pending").contains("error"), "late acquire survived stop");
  Fixture queued; queued.call("queued", "wx.startCompass"); queued.until([&] { return queued.control()->announced.load(); }, false); queued.pump();
  queued.devices->dispatch({EventType::HostCallCancelled, Fixture::object({{"callId", "queued"}})}); queued.pump(); queued.until([&] { return queued.control()->closed.load(); }); require(!queued.receipts.count("queued"), "cancelled queued startup receipt reached guest");
}
void duplicate_and_changed_interval() {
  Fixture fixture; fixture.start(); fixture.call("same", "wx.startAccelerometer"); fixture.call("different", "wx.startAccelerometer", {{"interval", "game"}});
  fixture.until([&] { return fixture.receipts.count("same") && fixture.receipts.count("different"); });
  require(fixture.receipts.at("same").contains("result") && fixture.receipts.at("different").contains("error"), "duplicate starts created or reconfigured source");
}
void reset_and_background() {
  Fixture fixture; fixture.start("Compass"); fixture.control()->emit = 1; fixture.until([&] { return fixture.control()->produced == 1; }, false); fixture.devices->reset(); fixture.until([&] { return fixture.control()->closed.load(); }); require(fixture.events.empty(), "permission reset did not fence queued compass sample");
  fixture.next(); fixture.receipts.erase("start"); fixture.start(); require(bool(fixture.devices->set_foreground(false)), "background transition failed"); fixture.until([&] { return fixture.control()->closed.load(); });
  fixture.call("hidden", "wx.startGyroscope"); fixture.until([&] { return fixture.receipts.count("hidden"); }); require(fixture.receipts.at("hidden").contains("error"), "background source was acquired");
}
void saturation_and_retirement() {
  Fixture startup; startup.devices->saturate(); startup.control()->fail_start = true; startup.call("failure", "wx.startGyroscope"); startup.until([&] { return startup.control()->closed.load(); }, false); startup.until([&] { return startup.receipts.count("failure"); });
  require(startup.receipts.at("failure").at("error").at("details").at("errCode") == 5, "native startup error was lost under pressure");
  Fixture pressure; pressure.start(); pressure.devices->saturate(); pressure.control()->emit = 1; pressure.until([&] { return pressure.control()->closed.load(); }, false); pressure.pump(); require(pressure.events.empty(), "overflowing sensor continued producing");
  Fixture retirement; retirement.start(); retirement.control()->fail_cleanup = true; auto before = std::chrono::steady_clock::now(); auto pending = retirement.devices->retire();
  require(!pending && pending.code == "ERR_RESOURCE_CLEANUP_PENDING" && std::chrono::steady_clock::now() - before < std::chrono::milliseconds(100), "retirement blocked or falsely acknowledged cleanup");
  retirement.until([&] { return retirement.control()->cleanup_attempts >= 2; }, false); require(!retirement.control()->closed, "uncertain cleanup discarded native source"); retirement.control()->fail_cleanup = false;
  retirement.until([&] { return bool(retirement.devices->retire()); }, false); require(retirement.control()->closed, "retirement preceded exact native release");
}
}
int main() {
  try { projection_and_catalog(); stop_drains_and_revokes_values(); late_acquire_and_cancelled_receipt(); duplicate_and_changed_interval(); reset_and_background(); saturation_and_retirement();
    std::cout << "Sensor lifecycle: 6 groups passed (controlled OS sources, no hardware or runtime acceptance)\n"; return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
