#include "device_socket_common.hpp"
#include <iostream>
using namespace weapp::desktop;
using namespace weapp::desktop::detail;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct WorkerControl {
  std::atomic<bool> opened{false}, close_seen{false}, release{false}, native_closed{false};
  std::atomic<bool> hold_cancelled{false};
  std::atomic<size_t> drained{0};
};
struct TestRoot {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("weapp-socket-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  TestRoot() { std::filesystem::create_directories(path / "user"); std::filesystem::create_directories(path / "native"); }
  ~TestRoot() { std::error_code error; std::filesystem::remove_all(path, error); }
};
std::shared_ptr<WorkerControl> control;
void fake_socket(std::shared_ptr<NativeSocket> socket, SocketOptions options) {
  auto state = control;
  if (socket->opened(options, "")) state->opened = true;
  while (!socket->cancelled) {
    if (auto command = socket->take()) {
      if (command->kind == SocketCommand::Close) {
        state->close_seen = true;
        while (!state->release && !socket->cancelled) std::this_thread::yield();
        break;
      }
      socket->delivered(command);
    }
    std::this_thread::yield();
  }
  while (state->hold_cancelled && !state->release) std::this_thread::yield();
  state->native_closed = true;  // Fake OS resource destruction precedes receipt.
  socket->finish(1000, "");
  ++state->drained;
}
class TestDevices : public DeviceSockets {
 public:
  TestDevices(DeviceApiOptions options, Command command) : DeviceSockets(std::move(options), std::move(command)) { install_socket_runner(fake_socket); }
 protected:
  void request_permission(const std::string&, std::function<void(bool)>) override {}
  bool platform_call(const std::string& call, const std::string& api, const Json& payload, const Json&) override { return socket_call(call, api, payload); }
};
class CoreSession {
  weapp_container_sdk_session* session_ = nullptr;
  static Json consume(weapp_container_sdk_buffer& buffer) {
    auto result = Json::parse(buffer.data, buffer.data + buffer.len);
    weapp_container_sdk_buffer_free(&buffer); return result;
  }
 public:
  CoreSession() {
    const std::string config = R"({"engine":"quickjs","gpuRequired":false})";
    weapp_container_sdk_buffer buffer{};
    auto code = weapp_container_sdk_session_new(reinterpret_cast<const uint8_t*>(config.data()), config.size(), &session_, &buffer);
    auto result = consume(buffer);
    if (code != WEAPP_CONTAINER_SDK_OK || !session_) throw std::runtime_error("core test session: " + result.dump());
  }
  ~CoreSession() {
    if (session_) { weapp_container_sdk_buffer buffer{}; weapp_container_sdk_session_destroy(&session_, &buffer); weapp_container_sdk_buffer_free(&buffer); }
  }
  Json configure(const Json& config) {
    const std::string operation = "configureHostServices", payload = config.dump();
    weapp_container_sdk_buffer buffer{};
    weapp_container_sdk_session_command(session_, reinterpret_cast<const uint8_t*>(operation.data()), operation.size(),
        reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), &buffer);
    return consume(buffer);
  }
};
void configure_actual_core_catalog(const TestDevices& devices, const Json& application, const Json& account) {
  auto configuration = encode(devices.configuration());
  configuration["application"] = application; configuration["account"] = account;
  CoreSession core;
  auto accepted = core.configure(configuration);
  if (!accepted.value("ok", false)) throw std::runtime_error("native socket configuration rejected by core: " + accepted.dump());
  auto unknown = configuration;
  unknown["apis"].push_back("object:SocketTask.offOpen");
  require(!core.configure(unknown).value("ok", false), "core accepted an API absent from its locked catalog");
  require(core.configure(configuration).value("ok", false), "rejected catalog update corrupted the previous configuration");
}
void close_receipt_waits_for_worker_and_delivery_stays_on_owner() {
  control = std::make_shared<WorkerControl>();
  TestRoot fixture; const auto& root = fixture.path;
  const Json app = {{"hostAppId", "host"}, {"tenantId", "tenant"}, {"publisherId", "publisher"}, {"appId", "socket"}, {"environment", "develop"}, {"version", "1"}, {"packRoot", std::string(64, 'a')}};
  const Json account = {{"principalId", "alice"}, {"epoch", "1"}};
  auto object = [](const Json& json) { return std::get<Value::Object>(decode(json).data); };
  DeviceApiOptions options; options.application = object(app); options.account = object(account); options.allowed_network_origins = {"https://example.com"};
  auto owner = std::this_thread::get_id();
  std::vector<Json> completions;
  {
    TestDevices devices(options, [&](const std::string& name, const Value::Object& fields, Value* output) {
      require(std::this_thread::get_id() == owner, "worker entered the host command callback");
      if (name == "managedFileScope") *output = decode(Json{{"root", (root / "user").string()}, {"auxiliaryRoot", (root / "native").string()}, {"application", app}, {"account", account}});
      else if (name == "completeHostCall") {
        auto result = encode(fields);
        if (result.at("callId") == "close") require(control->native_closed, "close receipt preceded native resource drain");
        completions.push_back(result);
      }
      return Status{};
    });
    struct Release { ~Release() { control->release = true; } } release_on_failure;
    configure_actual_core_catalog(devices, app, account);
    auto call = [&](const char* id, const char* api, Json payload) {
      return devices.dispatch({EventType::HostCall, object(Json{{"callId", id}, {"method", api}, {"payload", payload}, {"context", {{"application", app}, {"account", account}, {"sessionId", "session"}, {"callId", id}, {"api", api}}}})});
    };
    call("connect", "wx.connectSocket", {{"url", "wss://example.com/"}, {"objectId", "socket"}});
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!control->opened && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    require(control->opened, "fake OS socket did not open");
    devices.pump();
    call("close", "object:SocketTask.close", {{"objectId", "socket"}});
    while (!control->close_seen && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    devices.pump();
    bool premature = false; for (const auto& completion : completions) if (completion.at("callId") == "close") premature = true;
    control->release = true;
    require(!premature, "close settled while OS resources were still live");
    while (!control->native_closed && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    while (completions.size() < 2 && std::chrono::steady_clock::now() < deadline) { devices.pump(); std::this_thread::yield(); }
    require(completions.size() == 2 && control->native_closed, "drained close receipt was not delivered");
    // Cancelled factories keep their slots until the fake OS destruction
    // barrier is released. Their queued factory completions must be discarded.
    control->hold_cancelled = true; control->release = false;
    for (size_t index = 0; index < 8; ++index) {
      auto id = "cancel-" + std::to_string(index), object_id = "retiring-" + std::to_string(index);
      call(id.c_str(), "wx.connectSocket", {{"url", "wss://example.com/"}, {"objectId", object_id}});
      devices.dispatch({EventType::HostCallCancelled, object(Json{{"callId", id}})});
    }
    call("overflow", "wx.connectSocket", {{"url", "wss://example.com/"}, {"objectId", "ninth"}});
    devices.pump();
    bool bounded = false;
    for (const auto& completion : completions) {
      require(completion.at("callId").get<std::string>().rfind("cancel-", 0) != 0, "cancelled factory completed after cancellation");
      if (completion.at("callId") == "overflow") bounded = completion.contains("error");
    }
    control->release = true;
    require(bounded, "cancelled workers released concurrency before native drain");
    require(bool(devices.retire()), "retirement did not join the socket worker");
    require(control->drained == 9, "retirement returned before every fake native resource drained");
  }
}
}  // namespace
int main() {
  try {
    close_receipt_waits_for_worker_and_delivery_stays_on_owner();
    std::cout << "Socket lifecycle: 1 group passed (fake OS transport; no native network acceptance)\n"; return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
