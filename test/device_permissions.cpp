#include "device_apis_common.hpp"
#include <iostream>

using namespace weapp::desktop;
using namespace weapp::desktop::detail;

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
Value::Object object(const Json& value) {
  return std::get<Value::Object>(decode(value).data);
}
struct Fixture {
  std::filesystem::path directory = std::filesystem::temp_directory_path() /
      ("weapp-permission-test-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  Json application = {{"hostAppId", "test-host"}, {"tenantId", "tenant"},
      {"publisherId", "publisher"}, {"appId", "permissions"},
      {"environment", "test"}, {"version", "1"}, {"packRoot", std::string(64, 'a')}};
  Json account = {{"principalId", "alice"}, {"epoch", "1"}};
  std::vector<Json> completions;
  std::vector<Json> snapshots;
  std::string session = "session-1";
  Fixture() {
    std::filesystem::create_directories(directory / "user");
    std::filesystem::create_directories(directory / "native");
  }
  ~Fixture() { std::filesystem::remove_all(directory); }
  DeviceApiOptions options() const {
    DeviceApiOptions options;
    options.application = object(application);
    options.account = object(account);
    return options;
  }
  DeviceApis::Command command() {
    return [this](const std::string& name, const Value::Object& fields, Value* result) {
      if (name == "managedFileScope") {
        *result = decode(Json{{"root", (directory / "user").string()},
            {"auxiliaryRoot", (directory / "native").string()},
            {"application", application}, {"account", account}});
      } else if (name == "completeHostCall") {
        completions.push_back(encode(fields));
      } else if (name == "state") {
        *result = decode(Json{{"sessionId",session}});
      } else if (name == "capabilities") {
        *result = decode(Json{{"hostServices",{{"snapshotOwnerContext","1.0"}}}});
      } else if (name == "updateHostServiceSnapshots") {
        snapshots.push_back(encode(fields));
      } else {
        return Status::error("ERR_TEST_COMMAND", "unexpected command: " + name);
      }
      return Status{};
    };
  }
  Event call(const std::string& id, const std::string& api, Json payload = Json::object()) {
    Json context = {{"application", application}, {"account", account},
        {"sessionId", "session-1"}, {"callId", id}, {"api", api}};
    return {EventType::HostCall, object(Json{{"callId", id}, {"method", api},
        {"context", context}, {"payload", payload}})};
  }
  void expect_denied(const std::string& id) const {
    size_t count = 0;
    for (const auto& completion : completions) if (completion.at("callId") == id) {
      ++count;
      require(completion.contains("error"), "revoked call emitted success");
      require(completion.at("error").at("code") == "permission_denied", "wrong revocation error");
    }
    require(count == 1, "revoked call must settle once");
  }
};

// Only the OS consent callbacks are faked. Dispatch, owner queue, consent file,
// generation checks, failure delivery and retained-object cleanup are production
// DeviceBase code. This executable does not test hardware or native OS prompts.
class FakeDevices : public DeviceBase {
 public:
  std::vector<std::function<void(bool)>> prompts, os_callbacks;
  int mutations = 0, disposed = 0;
  std::map<std::string,std::function<void()>> retirement_cleanup;
  FakeDevices(Fixture& fixture) : DeviceBase(fixture.options(), fixture.command()) {
    add("test.sensitive");
    add("test.retained");
    add("test.failRetained");
    add("test.retire");
  }
  void reset() { reset_permissions(); }
  Status publish_snapshot() { return queue_snapshot_values({{"wx.getAppAuthorizeSetting",{{"cameraAuthorized","authorized"}}}}); }
  bool has_consent() const { return consents_.count("scope.contacts") != 0; }
  bool has_object(const std::string& id) const { return objects_.count(id) != 0; }
  void expect_empty_file() const {
    std::ifstream file(consents_file_);
    require(file.good() && Json::parse(file).empty(), "reset did not persist empty consent state");
  }
  void pump_checked() { require(bool(pump()), "device pump failed"); }
 protected:
  void request_permission(const std::string&, std::function<void(bool)> callback) override {
    prompts.push_back(std::move(callback));
  }
  bool platform_call(const std::string& call, const std::string& api,
      const Json& payload, const Json&) override {
    if (api == "test.retire") {
      auto id = payload.at("objectId").get<std::string>();
      retain_object(id, call, "Test", [this,id] { retirement_cleanup.at(id)(); });
      done(call, api); return true;
    }
    if (api == "test.failRetained") {
      retain_object("failed-object", call, "Test", [this] { ++disposed; });
      fail(call, api, "simulated native failure");
      return true;
    }
    if (api == "test.retained") {
      retain_object("retained-object", call, "Test", [this] { ++disposed; });
      permit("scope.contacts", api, call, [this, call, api] { done(call, api); });
      return true;
    }
    permit("scope.contacts", api, call, [this, call, api] {
      auto weak = weak_queue();
      const auto generation = permission_generation_;
      os_callbacks.push_back([this, weak, call, api, generation](bool granted) {
        queue_action(weak, [this, weak, call, api, generation, granted] {
          auto queue = weak.lock();
          if (!queue || queue->closed || !permission_call_is_current(call, generation)) return;
          if (!granted) { fail(call, api, "OS denied", "permission_denied"); return; }
          ++mutations;
          done(call, api);
        });
      });
    });
    return true;
  }
};

void queued_consent_cannot_regrant_after_reset() {
  Fixture fixture;
  FakeDevices devices(fixture);
  require(devices.dispatch(fixture.call("queued", "test.sensitive")), "call was not handled");
  devices.prompts.at(0)(true);  // Grant is queued, but has not reached the owner.
  devices.reset();
  devices.pump_checked();
  require(devices.os_callbacks.empty() && devices.mutations == 0, "stale consent started OS work");
  require(!devices.has_consent(), "stale consent was restored");
  devices.expect_empty_file();
  fixture.expect_denied("queued");
}
void late_os_callback_cannot_mutate_after_reset() {
  Fixture fixture;
  FakeDevices devices(fixture);
  devices.dispatch(fixture.call("late", "test.sensitive"));
  devices.prompts.at(0)(true);
  devices.pump_checked();
  require(devices.os_callbacks.size() == 1, "OS consent was not requested");
  devices.reset();
  devices.os_callbacks.at(0)(true);
  devices.pump_checked();
  require(devices.mutations == 0 && !devices.has_consent(), "late OS callback escaped reset");
  devices.expect_empty_file();
  fixture.expect_denied("late");
  // Even a reused external call ID cannot revive the prior generation.
  devices.dispatch(fixture.call("late", "test.sensitive"));
  devices.os_callbacks.at(0)(true);
  devices.pump_checked();
  require(devices.mutations == 0 && !devices.has_consent(), "old callback matched a reused call ID");
  devices.prompts.at(1)(true);
  devices.pump_checked();
  devices.os_callbacks.at(1)(true);
  devices.pump_checked();
  require(devices.mutations == 1 && devices.has_consent(), "fresh authorization did not recover");
}
void queued_success_becomes_one_failure_and_disposes_retained_object() {
  Fixture fixture;
  FakeDevices devices(fixture);
  devices.dispatch(fixture.call("grant", "wx.authorize", {{"scope", "scope.contacts"}}));
  devices.prompts.at(0)(true);
  devices.pump_checked();
  devices.dispatch(fixture.call("retained", "test.retained"));
  require(devices.has_object("retained-object"), "retained object was not installed");
  devices.reset();  // Complete is ahead of reset's Failure in the owner queue.
  devices.pump_checked();
  require(!devices.has_object("retained-object") && devices.disposed == 1,
      "revocation did not dispose retained native resources exactly once");
  fixture.expect_denied("retained");
}
void native_failure_disposes_retained_object() {
  Fixture fixture;
  FakeDevices devices(fixture);
  devices.dispatch(fixture.call("failure", "test.failRetained"));
  devices.pump_checked();
  require(!devices.has_object("failed-object") && devices.disposed == 1,
      "native failure leaked retained object resources");
  devices.retire();
  require(devices.disposed == 1, "retirement disposed failed object twice");
}
void retirement_retries_only_unacknowledged_cleanup() {
  Fixture fixture; FakeDevices devices(fixture);
  unsigned first = 0, second_attempts = 0, second = 0; bool fail = true;
  devices.retirement_cleanup["first"] = [&] { ++first; };
  devices.retirement_cleanup["second"] = [&] { ++second_attempts; if (fail) throw std::runtime_error("native close not acknowledged"); ++second; };
  devices.dispatch(fixture.call("first-call", "test.retire", {{"objectId", "first"}}));
  devices.dispatch(fixture.call("second-call", "test.retire", {{"objectId", "second"}}));
  auto failed = devices.retire();
  require(!failed && failed.code == "ERR_RESOURCE_CLEANUP" && first == 1 && second_attempts == 1 && second == 0, "retirement discarded or acknowledged failed cleanup");
  fail = false;
  require(bool(devices.retire()) && first == 1 && second_attempts == 2 && second == 1, "retirement repeated acknowledged cleanup or failed to recover");
  require(bool(devices.retire()) && first == 1 && second == 1, "successful retirement repeated native cleanup");
}

void queued_snapshot_freezes_owner_and_preserves_values_envelope() {
  Fixture fixture;FakeDevices devices(fixture);
  require(bool(devices.publish_snapshot()),"snapshot was not queued");
  fixture.session="replacement-session";
  fixture.account={{"principalId","bob"},{"epoch","2"}};
  devices.pump_checked();
  require(fixture.snapshots.size()==1,"snapshot was not delivered exactly once");
  const auto& update=fixture.snapshots.front();
  require(update.at("context").at("sessionId")=="session-1","snapshot was relabeled with a later owner");
  require(update.at("context").at("account").at("principalId")=="alice","snapshot lost its source account");
  require(update.at("context").at("application")==fixture.application,"snapshot lost its installed application");
  require(update.at("values").at("wx.getAppAuthorizeSetting").at("cameraAuthorized")=="authorized","snapshot values envelope was lost");
}

}  // namespace

int main() {
  try {
    queued_consent_cannot_regrant_after_reset();
    late_os_callback_cannot_mutate_after_reset();
    queued_success_becomes_one_failure_and_disposes_retained_object();
    native_failure_disposes_retained_object();
    retirement_retries_only_unacknowledged_cleanup();
    queued_snapshot_freezes_owner_and_preserves_values_envelope();
    std::cout << "DeviceBase permission lifecycle: 6 cases passed (fake OS callbacks; no hardware acceptance)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
