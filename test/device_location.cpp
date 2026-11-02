#include "device_location_common.hpp"
#include "core_host_services.hpp"
#include <iostream>
using namespace weapp::desktop;
using namespace weapp::desktop::detail;
using namespace weapp::desktop::detail::test;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Control {
  std::atomic<bool> created{false}, waiting{false}, allow_acquire{true}, acquired{false}, hold_cleanup{false}, cleanup_waiting{false}, release{true}, closed{false}, fail_cleanup{false};
  std::atomic<unsigned> emit{0}, produced{0};
  std::atomic<unsigned> cleanup_attempts{0};
};
class FakeLocation final : public LocationSource {
  std::shared_ptr<Control> control_;
 public:
  explicit FakeLocation(std::shared_ptr<Control> control) : control_(std::move(control)) { control_->created = true; }
  ~FakeLocation() override { control_->closed = true; }
  void run(const std::shared_ptr<NativeLocation>& state) override {
    control_->waiting = true;
    // Model a permission provider whose acquisition completes after Cancel.
    while (!control_->allow_acquire) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    control_->acquired = true;
    auto position = [&] {
      auto sequence = control_->produced.load();
      state->position({{"latitude", 31.12345 + sequence * 0.001}, {"longitude", 121.54321}, {"accuracy", 5.0}, {"altitude", 8.0}});
      ++control_->produced;
    };
    if (!state->cancelled) {
      state->announce_started(); position();
      while (state->options.continuous && !state->cancelled) {
        if (control_->emit.exchange(0)) position();
        state->wait_for(std::chrono::milliseconds(5));
      }
    }
  }
  bool cleanup(const std::shared_ptr<NativeLocation>& state) override {
    control_->cleanup_waiting = true; ++control_->cleanup_attempts;
    if ((control_->hold_cleanup && !control_->release) || control_->fail_cleanup) {
      state->cleanup_failed("fake native cleanup pending"); return false;
    }
    return true;
  }
};
class TestLocations final : public DeviceLocations {
 public:
  bool auto_grant = true;
  std::function<void(bool)> pending_grant;
  std::vector<std::string> requested_scopes;
  TestLocations(DeviceApiOptions options, Command command, std::shared_ptr<std::shared_ptr<Control>> control)
      : DeviceLocations(std::move(options), std::move(command)) {
    install_location_factory([control](const DeviceApiOptions&, const LocationOptions&) { return std::make_unique<FakeLocation>(*control); });
  }
  void reset() { reset_permissions(); }
 protected:
  void request_permission(const std::string& scope, std::function<void(bool)> grant) override {
    requested_scopes.push_back(scope);
    if (auto_grant) grant(true); else pending_grant = std::move(grant);
  }
  bool platform_call(const std::string& call, const std::string& api, const Json& payload, const Json&) override { return location_call(call, api, payload); }
};
struct Harness {
  const std::thread::id owner = std::this_thread::get_id();
  std::filesystem::path root = std::filesystem::temp_directory_path() /
      ("weapp-location-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Json app = {{"hostAppId", "host"}, {"tenantId", "tenant"}, {"publisherId", "publisher"}, {"appId", "location"}, {"environment", "develop"}, {"version", "1"}, {"packRoot", std::string(64, 'a')}};
  Json account = {{"principalId", "alice"}, {"epoch", "1"}};
  std::shared_ptr<std::shared_ptr<Control>> controls = std::make_shared<std::shared_ptr<Control>>(std::make_shared<Control>());
  std::vector<std::shared_ptr<Control>> all_controls{*controls};
  std::map<std::string, Json> receipts;
  std::vector<Json> events;
  std::unique_ptr<TestLocations> devices;
  static Value::Object object(const Json& json) { return std::get<Value::Object>(decode(json).data); }
  explicit Harness(bool consent = true) {
    std::filesystem::create_directories(root / "user"); std::filesystem::create_directories(root / "native");
    if (consent) { std::ofstream file(root / "native/consents.json"); file << R"({"scope.userLocation":true,"scope.userFuzzyLocation":true})"; }
    DeviceApiOptions options; options.application = object(app); options.account = object(account);
    devices = std::make_unique<TestLocations>(options, [&](const std::string& operation, const Value::Object& fields, Value* output) {
      require(std::this_thread::get_id() == owner, "native source entered host callback from its worker");
      if (operation == "managedFileScope") *output = decode(Json{{"root", (root / "user").string()}, {"auxiliaryRoot", (root / "native").string()}, {"application", app}, {"account", account}});
      else if (operation == "completeHostCall") {
        auto value = encode(fields); auto call = value.at("callId").get<std::string>();
        if (call == "stop" && (*controls)->created) require((*controls)->closed, "stop receipt preceded native source destruction");
        receipts[call] = value;
      } else if (operation == "hostApiEvent") events.push_back(encode(fields));
      return Status{};
    }, controls);
    call("subscribe", "wx.onLocationChange", {{"subscriptionId", "locations"}, {"action", "subscribe"}}, true);
  }
  ~Harness() {
    for (const auto& control : all_controls) { control->allow_acquire = true; control->release = true; control->fail_cleanup = false; }
    if (devices) devices->retire();
    devices.reset(); std::error_code error; std::filesystem::remove_all(root, error);
  }
  std::shared_ptr<Control> control() { return *controls; }
  void next_control() { *controls = std::make_shared<Control>(); all_controls.push_back(*controls); }
  void call(const std::string& id, const std::string& api, Json payload = Json::object(), bool retained = false) {
    devices->dispatch({EventType::HostCall, object(Json{{"callId", id}, {"method", api}, {"payload", payload}, {"retainUntilCancelled", retained},
        {"context", {{"application", app}, {"account", account}, {"sessionId", "session"}, {"callId", id}, {"api", api}}}})});
  }
  void pump() { require(bool(devices->pump()), "native owner pump failed"); }
  template<class Predicate> void until(Predicate predicate, bool pump_owner = true) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) { if (pump_owner) pump(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    require(predicate(), "native location fixture timed out");
  }
};
void inputs_and_fuzzy_privacy() {
  auto value = location_position({{"latitude", 31.12345}, {"longitude", 121.54321}, {"accuracy", 5.0}, {"altitude", 8.0}}, true);
  require(value.at("latitude") == 31.12 && value.at("longitude") == 121.54 && value.at("accuracy") == 1500.0 && !value.contains("altitude"), "fuzzy location exposed precise metadata");
  bool rejected = false;
  try { location_options("wx.getLocation", {{"type", "gcj02"}}); } catch (const std::exception&) { rejected = true; }
  require(rejected, "unsupported coordinate system was silently mislabeled");
  rejected = false;
  try { location_position({{"latitude", 91}, {"longitude", 0}, {"accuracy", 5}}, false); } catch (const std::exception&) { rejected = true; }
  require(rejected, "invalid provider coordinates were published");
  Harness fuzzy(false); fuzzy.call("fuzzy", "wx.getFuzzyLocation");
  fuzzy.until([&] { return fuzzy.receipts.count("fuzzy"); });
  require(fuzzy.devices->requested_scopes == std::vector<std::string>{"scope.userFuzzyLocation"}, "fuzzy location reused precise application consent");
  const auto& projected = fuzzy.receipts.at("fuzzy").at("result");
  require(projected.at("latitude") == 31.12 && projected.at("longitude") == 121.54 && projected.at("accuracy") == 1500.0 && !projected.contains("altitude"), "dispatcher bypassed fuzzy projection");
}
void stop_waits_for_cleanup_and_revokes_queued_positions() {
  Harness fixture; configure_actual_core_catalog(*fixture.devices, fixture.app, fixture.account);
  fixture.call("start", "wx.startLocationUpdate");
  fixture.until([&] { return fixture.receipts.count("start") && !fixture.events.empty(); });
  fixture.call("same-options", "wx.startLocationUpdate");
  fixture.call("changed-options", "wx.startLocationUpdate", {{"isHighAccuracy", true}});
  fixture.until([&] { return fixture.receipts.count("same-options") && fixture.receipts.count("changed-options"); });
  require(fixture.receipts.at("same-options").contains("result") && fixture.receipts.at("changed-options").contains("error"), "active watch silently accepted different options");
  fixture.events.clear(); auto control = fixture.control();
  control->hold_cleanup = true; control->release = false; control->emit = 1;
  fixture.until([&] { return control->produced >= 2; }, false);
  fixture.call("stop", "wx.stopLocationUpdate");
  fixture.until([&] { return control->cleanup_waiting.load(); }); fixture.pump();
  require(!fixture.receipts.count("stop") && fixture.events.empty(), "stop acknowledged or delivered queued location before cleanup");
  control->release = true; fixture.until([&] { return fixture.receipts.count("stop"); });
  require(fixture.receipts.at("stop").contains("result") && control->closed && fixture.events.empty(), "drained stop did not revoke stale positions");
}
void late_acquisition_cannot_escape_stop() {
  Harness fixture; auto control = fixture.control(); control->allow_acquire = false; control->hold_cleanup = true; control->release = false;
  fixture.call("start", "wx.startLocationUpdate"); fixture.until([&] { return control->waiting.load(); });
  fixture.call("duplicate", "wx.startLocationUpdate"); fixture.until([&] { return fixture.receipts.count("duplicate"); });
  require(fixture.receipts.at("duplicate").contains("error"), "pending watch startup was acknowledged as active");
  fixture.call("stop", "wx.stopLocationUpdate"); fixture.pump(); require(!fixture.receipts.count("stop"), "pending permission acquisition was treated as drained");
  control->allow_acquire = true; fixture.until([&] { return control->acquired.load() && control->cleanup_waiting.load(); });
  require(!fixture.receipts.count("stop") && fixture.events.empty(), "late native acquisition escaped cancellation");
  control->release = true; fixture.until([&] { return fixture.receipts.count("stop") && fixture.receipts.count("start"); });
  require(fixture.receipts.at("start").contains("error") && fixture.events.empty(), "cancelled start was completed successfully");
}
void reset_and_foreground_revoke_delivery() {
  Harness fixture; fixture.call("start", "wx.startLocationUpdate");
  fixture.until([&] { return fixture.receipts.count("start") && !fixture.events.empty(); }); fixture.events.clear();
  auto old = fixture.control(); old->emit = 1; fixture.until([&] { return old->produced >= 2; }, false);
  fixture.devices->reset(); fixture.until([&] { return old->closed.load(); }); fixture.pump();
  require(fixture.events.empty(), "queued location survived permission reset");
  fixture.next_control(); fixture.call("fresh", "wx.startLocationUpdate");
  fixture.until([&] { return fixture.receipts.count("fresh") && !fixture.events.empty(); });
  fixture.events.clear(); require(bool(fixture.devices->set_foreground(false)), "background transition failed");
  fixture.until([&] { return fixture.control()->closed.load(); });
  fixture.call("hidden", "wx.getLocation"); fixture.until([&] { return fixture.receipts.count("hidden"); });
  require(fixture.receipts.at("hidden").contains("error") && fixture.events.empty(), "background access acquired a new location");
}
void pending_consent_and_cleanup_failures() {
  Harness consent(false); consent.devices->auto_grant = false;
  consent.call("pending", "wx.getLocation"); require(bool(consent.devices->pending_grant), "application permission was not requested");
  require(bool(consent.devices->set_foreground(false)), "background transition failed");
  consent.devices->pending_grant(true); consent.until([&] { return consent.receipts.count("pending"); });
  require(!consent.control()->waiting && consent.receipts.at("pending").contains("error"), "late application consent started native acquisition in background");
  Harness stopped(false); stopped.devices->auto_grant = false;
  stopped.call("pending-start", "wx.startLocationUpdate"); stopped.call("stop", "wx.stopLocationUpdate");
  stopped.devices->pending_grant(true); stopped.until([&] { return stopped.receipts.count("pending-start") && stopped.receipts.count("stop"); });
  require(!stopped.control()->created && stopped.receipts.at("pending-start").contains("error"), "late application consent restarted a stopped location watch");
  Harness cleanup; cleanup.call("start", "wx.startLocationUpdate"); cleanup.until([&] { return cleanup.receipts.count("start"); });
  cleanup.control()->fail_cleanup = true; cleanup.call("stop", "wx.stopLocationUpdate");
  cleanup.until([&] { return cleanup.control()->cleanup_attempts >= 2; });
  require(!cleanup.control()->closed && !cleanup.receipts.count("stop"), "failed native cleanup discarded its resource or acknowledged stop");
  cleanup.control()->fail_cleanup = false; cleanup.until([&] { return cleanup.receipts.count("stop"); });
  require(cleanup.receipts.at("stop").contains("result") && cleanup.control()->closed && bool(cleanup.devices->retire()), "recovered cleanup could not complete retirement");
}
void cancellation_retains_capacity_until_native_drain() {
  Harness fixture;
  for (unsigned index = 0; index < 8; ++index) {
    if (index) fixture.next_control();
    fixture.control()->allow_acquire = false;
    auto call = "pending-" + std::to_string(index);
    fixture.call(call, "wx.getLocation");
    fixture.devices->dispatch({EventType::HostCallCancelled, Harness::object(Json{{"callId", call}})});
  }
  fixture.next_control(); fixture.call("overflow", "wx.getLocation");
  fixture.until([&] { return fixture.receipts.count("overflow"); });
  require(fixture.receipts.at("overflow").contains("error") && !fixture.control()->created, "cancelled location owners bypassed the worker limit");
  for (const auto& control : fixture.all_controls) control->allow_acquire = true;
  require(bool(fixture.devices->retire()), "native location retirement failed");
  require(std::count_if(fixture.all_controls.begin(), fixture.all_controls.end(), [](const auto& control) { return control->closed.load(); }) == 8,
      "retirement returned before every cancelled native owner drained");
}
}
int main() {
  try {
    inputs_and_fuzzy_privacy(); stop_waits_for_cleanup_and_revokes_queued_positions(); late_acquisition_cannot_escape_stop(); reset_and_foreground_revoke_delivery(); pending_consent_and_cleanup_failures(); cancellation_retains_capacity_until_native_drain();
    std::cout << "Location lifecycle: 6 groups passed (fake OS sources; actual core configuration; no device acceptance)\n"; return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
