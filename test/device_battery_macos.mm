#include "device_apis_common.hpp"
#include "core_host_services.hpp"

#include <iostream>

using namespace weapp::desktop;
using namespace weapp::desktop::detail;

namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

struct Fixture {
  std::filesystem::path root = std::filesystem::temp_directory_path() /
      ("weapp-battery-macos-test-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Json application = {
      {"hostAppId", "host"}, {"tenantId", "tenant"}, {"publisherId", "publisher"},
      {"appId", "battery"}, {"environment", "develop"}, {"version", "1"},
      {"packRoot", std::string(64, 'a')}};
  Json account = {{"principalId", "alice"}, {"epoch", "1"}};
  std::thread::id owner = std::this_thread::get_id();
  std::vector<Json> completions;
  std::unique_ptr<DeviceApis> devices;

  static Value::Object object(const Json& value) {
    return std::get<Value::Object>(decode(value).data);
  }

  Fixture() {
    std::filesystem::create_directories(root / "user");
    std::filesystem::create_directories(root / "native");

    DeviceApiOptions options;
    options.application = object(application);
    options.account = object(account);

    Status status;
    devices = DeviceApis::create(
        options,
        [this](const std::string& operation, const Value::Object& fields, Value* output) {
          require(std::this_thread::get_id() == owner,
                  "native callback changed owner thread");
          if (operation == "managedFileScope") {
            *output = decode(Json{{"root", (root / "user").string()},
                                  {"auxiliaryRoot", (root / "native").string()},
                                  {"application", application}, {"account", account}});
          } else if (operation == "completeHostCall") {
            completions.push_back(encode(fields));
          } else {
            throw std::runtime_error("unexpected native command: " + operation);
          }
          return Status{};
        },
        status);
    require(bool(status) && devices != nullptr, "macOS device APIs could not be created");
  }

  ~Fixture() {
    if (devices) {
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (!devices->retire() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      devices.reset();
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }

  void pump() { require(bool(devices->pump()), "macOS device owner pump failed"); }

  void call(const std::string& call_id, const std::string& api) {
    auto context = Json{{"application", application},
                        {"account", account},
                        {"sessionId", "battery-session"},
                        {"callId", call_id},
                        {"api", api}};
    require(devices->dispatch({EventType::HostCall,
                               object(Json{{"callId", call_id},
                                           {"method", api},
                                           {"payload", Json::object()},
                                           {"context", context}})}),
            "battery host call was not dispatched");
  }
};

void native_snapshot_and_async_call() {
  Fixture fixture;
  const auto configuration = encode(fixture.devices->configuration());
  require(std::find(configuration.at("apis").begin(), configuration.at("apis").end(),
                    Json("wx.getBatteryInfoSync")) != configuration.at("apis").end(),
          "macOS battery sync API is absent from the native catalog");
  require(configuration.at("syncValues").contains("wx.getBatteryInfoSync"),
          "macOS battery sync snapshot was not published");
  const auto& snapshot = configuration.at("syncValues").at("wx.getBatteryInfoSync");
  require(snapshot.is_object() && snapshot.at("level").is_number_integer() &&
              snapshot.at("level").get<int>() >= 0 && snapshot.at("level").get<int>() <= 100,
          "macOS battery level has an invalid native shape");
  require(snapshot.at("isCharging").is_boolean(),
          "macOS battery charging state has an invalid native shape");
  detail::test::configure_actual_core_catalog(*fixture.devices, fixture.application,
                                               fixture.account);

  fixture.call("battery", "wx.getBatteryInfo");
  fixture.pump();
  require(fixture.completions.size() == 1 && fixture.completions.front().at("callId") == "battery",
          "macOS battery async call did not complete on the owner thread");
  const auto& result = fixture.completions.front().at("result");
  require(result.at("level") == snapshot.at("level") &&
              result.at("isCharging") == snapshot.at("isCharging"),
          "macOS battery async result diverged from the native sync snapshot");
}

}  // namespace

int main() {
  try {
    native_snapshot_and_async_call();
    std::cout << "macOS battery provider: sync snapshot and async owner dispatch passed "
                 "(native IOKit; no runtime or SDK-chain acceptance)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
