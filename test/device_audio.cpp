#include "device_audio_wave.hpp"
#include <iostream>
using namespace weapp::desktop;
using namespace weapp::desktop::detail;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Control {
  std::atomic<bool> created{false}, allow_open{true}, opened{false}, fail_open{false}, fail_command{false}, closed{false}, fail_cleanup{false}, block_command{false}, entered{false}, block_reset{false}, resetting{false};
  std::atomic<unsigned> cleanup_attempts{0}, executed{0}, emit{0}, produced{0};
  std::atomic<size_t> resource_bytes{0};
};
class ControlledBackend final : public AudioBackend {
  std::shared_ptr<Control> control_; uint64_t generation_ = 0;
 public:
  explicit ControlledBackend(std::shared_ptr<Control> value) : control_(std::move(value)) { control_->created = true; }
  ~ControlledBackend() override { control_->closed = true; }
  void open(const std::shared_ptr<NativeAudio>&) override {
    while (!control_->allow_open) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (control_->fail_open) throw AudioFailure("native playback denied", "permission_denied", -2147024891LL);
    control_->opened = true;
  }
  void poll(const std::shared_ptr<NativeAudio>& state) override {
    if (generation_ != state->generation) {
      control_->resetting = true;
      while (control_->block_reset) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      generation_ = state->generation;
      state->sample(generation_, {{"paused", true}, {"currentTime", 0}, {"duration", 0}, {"buffered", 0}});
    }
    if (control_->emit.exchange(0)) { state->sample(generation_, {{"currentTime", 2.5}, {"duration", 10.0}, {"buffered", 8.0}, {"paused", false}}, "TimeUpdate"); ++control_->produced; }
  }
  Json execute(const std::shared_ptr<NativeAudio>& state, const std::shared_ptr<AudioCommand>& command) override {
    control_->entered = true;
    while (control_->block_command && state->current(command->generation) && !command->cancelled) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!state->current(command->generation) || command->cancelled) throw AudioFailure("retired native operation", "cancelled");
    if (control_->fail_command) throw AudioFailure("native media failed", "invalid_request", -1, {{"errMsg", "original media error"}, {"nativeMediaError", 3}, {"nativeErrorMessage", "raw message"}});
    ++control_->executed; control_->resource_bytes = command->resource.size(); return {{"nativeSequence", control_->executed.load()}};
  }
  bool cleanup(const std::shared_ptr<NativeAudio>&) override { ++control_->cleanup_attempts; return !control_->fail_cleanup; }
};
class TestAudio final : public DeviceAudio {
 public:
  TestAudio(DeviceApiOptions options, Command command, std::shared_ptr<Control> control) : DeviceAudio(std::move(options), std::move(command)) {
    install_audio_factory([control](const auto&) { return std::make_unique<ControlledBackend>(control); });
  }
  void reset() { reset_permissions(); }
  void saturate() { while (queue_->push({DeviceQueued::NativeTerminalEvent, "", "unsubscribed", "", Json::object()})) {} }
 protected:
  void request_permission(const std::string&, std::function<void(bool)> callback) override { callback(false); }
  bool platform_call(const std::string& call, const std::string& api, const Json& payload, const Json&) override { return audio_call(call, api, payload); }
};
struct Fixture {
  std::filesystem::path root = std::filesystem::temp_directory_path() / ("weapp-audio-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::thread::id owner = std::this_thread::get_id();
  Json app = {{"hostAppId", "host"}, {"tenantId", "tenant"}, {"publisherId", "publisher"}, {"appId", "audio"}, {"environment", "develop"}, {"version", "1"}, {"packRoot", std::string(64, 'a')}};
  Json account = {{"principalId", "alice"}, {"epoch", "1"}};
  std::shared_ptr<Control> control = std::make_shared<Control>();
  std::map<std::string, Json> receipts; std::vector<Json> events, resource_contexts;
  bool resource_support = true, inconsistent_resource = false; size_t resource_reads = 0;
  std::unique_ptr<TestAudio> devices;
  static Value::Object object(const Json& value) { return std::get<Value::Object>(decode(value).data); }
  Fixture() {
    std::filesystem::create_directories(root / "user"); std::filesystem::create_directories(root / "native");
    DeviceApiOptions options; options.application = object(app); options.account = object(account);
    devices = std::make_unique<TestAudio>(options, [this](const std::string& operation, const Value::Object& fields, Value* output) {
      require(std::this_thread::get_id() == owner, "audio callback entered runtime off owner thread");
      if (operation == "managedFileScope") *output = decode(Json{{"root", (root / "user").string()}, {"auxiliaryRoot", (root / "native").string()}, {"application", app}, {"account", account}});
      else if (operation == "completeHostCall") { auto value = encode(fields); receipts[value.at("callId").get<std::string>()] = value; }
      else if (operation == "hostApiEvent") events.push_back(encode(fields));
      else if (operation == "capabilities") *output = decode(resource_support ? Json{{"hostServices", {{"apiResourceChunks", "1.0"}}}} : Json::object());
      else if (operation == "hostApiResource") {
        auto request = encode(fields); resource_contexts.push_back(request.at("context")); ++resource_reads;
        require(request.size() == 3 && request.at("length") == 1048576, "native resource request selected an untrusted file or exceeded chunk limit");
        size_t total = 1048580, offset = request.at("offset"); auto length = std::min<size_t>(total - offset, 1048576);
        *output = decode(Json{{"totalBytes", total + ((inconsistent_resource && offset) ? 1 : 0)}, {"bytes", std::vector<uint8_t>(length, 7)}});
      }
      return Status{};
    }, control);
  }
  ~Fixture() {
    control->allow_open = true; control->block_command = false; control->block_reset = false; control->fail_cleanup = false;
    if (devices) { auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3); while (!devices->retire() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    devices.reset(); std::error_code error; std::filesystem::remove_all(root, error);
  }
  void call(const std::string& id, const std::string& api, Json payload = Json::object(), bool retained = false) {
    devices->dispatch({EventType::HostCall, object(Json{{"callId", id}, {"method", api}, {"payload", payload}, {"retainUntilCancelled", retained}, {"context", {{"application", app}, {"account", account}, {"sessionId", "session"}, {"callId", id}, {"api", api}}}})});
  }
  void member(const std::string& id, const std::string& name, Json extra = Json::object()) { extra["objectId"] = "audio"; call(id, "object:InnerAudioContext." + name, extra); }
  void set(const std::string& id, const std::string& name, Json value) { member(id, name, {{"action", "set"}, {"value", value}}); }
  void pump() { require(bool(devices->pump()), "audio owner pump failed"); }
  template<class Predicate> void until(Predicate predicate, bool pump_owner = true) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) { if (pump_owner) pump(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    require(predicate(), "audio fixture timed out");
  }
  void create() { call("create", "wx.createInnerAudioContext", {{"objectId", "audio"}}, true); until([&] { return receipts.count("create"); }); require(receipts.at("create").contains("result"), "native audio was not created"); }
  void subscribe() { call("listen", "object:InnerAudioContext.onTimeUpdate", {{"subscriptionId", "time"}, {"objectId", "audio"}, {"action", "subscribe"}}, true); }
};
std::vector<uint8_t> wave(uint16_t codec = 1, uint16_t bits = 16) {
  std::vector<uint8_t> bytes(48); auto put16 = [&](size_t p, uint16_t v) { bytes[p] = uint8_t(v); bytes[p + 1] = uint8_t(v >> 8); };
  auto put32 = [&](size_t p, uint32_t v) { put16(p, uint16_t(v)); put16(p + 2, uint16_t(v >> 16)); };
  std::memcpy(bytes.data(), "RIFF", 4); put32(4, 40); std::memcpy(bytes.data() + 8, "WAVEfmt ", 8); put32(16, 16); put16(20, codec); put16(22, 1); put32(24, 8000); put32(28, 8000 * bits / 8); put16(32, bits / 8); put16(34, bits); std::memcpy(bytes.data() + 36, "data", 4); put32(40, 4); return bytes;
}
void format_and_catalog() {
  validate_audio_wave(wave()); validate_audio_wave(wave(3, 32));
  for (unsigned kind = 0; kind < 8; ++kind) {
    auto bytes = wave();
    if (kind == 0) bytes.resize(20); if (kind == 1) bytes[4] = 255; if (kind == 2) bytes[20] = 6; if (kind == 3) bytes[32] = 0;
    if (kind == 4) bytes[24] = 0; if (kind == 5) std::memcpy(bytes.data() + 36, "LIST", 4); if (kind == 6) bytes[40] = 255; if (kind == 7) bytes[34] = 12;
    bool rejected = false; try { validate_audio_wave(bytes); } catch (...) { rejected = true; } require(rejected, "unsafe or invalid WAVE accepted");
  }
  bool rejected = false; try { audio_property("src", "https://host/song.wav"); } catch (...) { rejected = true; } require(rejected, "unconfigured remote audio accepted");
  Fixture fixture; auto apis = encode(fixture.devices->configuration()).at("apis"); unsigned audio_count = 0;
  for (const auto& api : apis) if (api.get<std::string>().find("InnerAudioContext") != std::string::npos) ++audio_count;
  require(audio_count == 36, "audio catalog advertises missing or unavailable properties");
}
void destroy_waits_and_revokes() {
  Fixture f; f.create(); f.subscribe(); f.control->emit = 1; f.until([&] { return f.control->produced == 1; }, false);
  f.control->fail_cleanup = true; f.member("destroy", "destroy"); f.until([&] { return f.control->cleanup_attempts >= 2; });
  require(f.events.empty() && !f.receipts.count("destroy") && !f.control->closed, "destroy preceded exact cleanup or leaked queued event");
  f.member("late", "play"); f.until([&] { return f.receipts.count("late"); }); require(f.receipts.at("late").contains("error"), "destroyed object accepted work");
  f.control->fail_cleanup = false; f.until([&] { return f.receipts.count("destroy"); }); require(f.control->closed, "destroy acknowledged before backend release");
}
void queued_receipt_fencing() {
  Fixture f; f.create(); f.member("old", "play"); f.until([&] { return f.control->executed == 1; }, false); f.pump();
  f.set("source", "src", "wxfile://user/new.wav"); f.until([&] { return f.receipts.count("old") && f.receipts.count("source"); });
  require(f.receipts.at("old").contains("error") && f.receipts.at("source").contains("result"), "source replacement let an already queued old receipt escape");
  f.member("cancel", "play"); f.until([&] { return f.control->executed == 3; }, false); f.pump();
  f.devices->dispatch({EventType::HostCallCancelled, Fixture::object({{"callId", "cancel"}})}); f.pump(); require(!f.receipts.count("cancel"), "cancelled queued audio receipt delivered");
  Fixture preferences; preferences.create(); preferences.control->block_command = true; preferences.set("volume", "volume", 0.2);
  preferences.until([&] { return preferences.control->entered.load(); }, false);
  preferences.devices->dispatch({EventType::HostCallCancelled, Fixture::object({{"callId", "volume"}})}); preferences.control->block_command = false;
  preferences.member("snapshot", "pause"); preferences.until([&] { return preferences.receipts.count("snapshot"); });
  require(preferences.receipts.at("snapshot").at("result").at("properties").at("volume") == 1.0, "cancelled preference contaminated confirmed native settings");
}
void background_and_reset() {
  Fixture f; f.create(); f.subscribe(); f.control->emit = 1; f.until([&] { return f.control->produced == 1; }, false);
  f.control->block_reset = true; require(bool(f.devices->set_foreground(false)), "audio background rejected"); f.until([&] { return f.control->resetting.load(); }, false); f.pump(); require(f.events.empty(), "old source event survived background");
  f.member("hidden", "play"); f.until([&] { return f.receipts.count("hidden"); }); require(f.receipts.at("hidden").contains("error"), "background audio playback accepted");
  f.control->block_reset = false; require(bool(f.devices->set_foreground(true)), "audio foreground rejected"); f.set("again", "src", "wxfile://user/a.wav"); f.until([&] { return f.receipts.count("again"); });
  f.devices->reset(); f.member("stable", "pause"); f.until([&] { return f.receipts.count("stable"); }); require(f.receipts.at("stable").contains("result"), "source reset destroyed stable public context");
}
void late_acquire_and_raw_errors() {
  Fixture f; f.control->allow_open = false; f.call("pending", "wx.createInnerAudioContext", {{"objectId", "audio"}}, true); f.until([&] { return f.control->created.load(); }, false);
  f.control->fail_cleanup = true; f.member("destroy", "destroy"); f.control->allow_open = true; f.until([&] { return f.control->cleanup_attempts >= 2; }); require(!f.receipts.count("destroy"), "late acquire treated as cleaned");
  f.control->fail_cleanup = false; f.until([&] { return f.receipts.count("destroy"); }); require(f.control->closed, "late decoder outlived destroy");
  Fixture denied; denied.devices->saturate(); denied.control->fail_open = true; denied.call("denied", "wx.createInnerAudioContext", {{"objectId", "audio"}}, true); denied.until([&] { return denied.receipts.count("denied"); });
  require(denied.receipts.at("denied").at("error").at("details").at("errCode") == -2147024891LL, "raw Windows playback error lost under backpressure");
  Fixture decode; decode.create(); decode.control->fail_command = true; decode.member("decode", "play"); decode.until([&] { return decode.receipts.count("decode"); });
  auto detail = decode.receipts.at("decode").at("error").at("details");
  require(detail.at("errCode") == -1 && detail.at("nativeMediaError") == 3 && detail.at("errMsg") == "original media error" && detail.at("nativeErrorMessage") == "raw message", "native media public failure fields were rewritten or dropped");
}
void budget_backpressure_retirement() {
  Fixture f; f.create(); f.control->block_command = true; for (unsigned i = 0; i < 33; ++i) f.member("command" + std::to_string(i), "play");
  f.until([&] { return f.receipts.count("command32"); }); require(f.receipts.at("command32").contains("error"), "audio commands exceeded retained budget"); f.control->block_command = false;
  f.until([&] { return f.receipts.count("command31"); }); f.subscribe(); f.devices->saturate(); f.control->emit = 1; f.until([&] { return f.control->closed.load(); }, false); f.pump(); require(f.events.empty(), "overflowing decoder leaked stale samples");
  Fixture retiring; retiring.create(); retiring.control->fail_cleanup = true; auto before = std::chrono::steady_clock::now(); auto status = retiring.devices->retire();
  require(!status && status.code == "ERR_RESOURCE_CLEANUP_PENDING" && std::chrono::steady_clock::now() - before < std::chrono::milliseconds(100), "audio retirement blocked or forged native release");
  retiring.control->fail_cleanup = false; retiring.until([&] { return bool(retiring.devices->retire()); }, false); require(retiring.control->closed, "audio retirement preceded backend destruction");
}
void package_chunks_and_negotiation() {
  Fixture f; f.create(); f.set("package", "src", "/audio.wav"); f.until([&] { return f.receipts.count("package"); });
  if (!f.receipts.at("package").contains("result") || f.resource_reads != 2 || f.control->resource_bytes != 1048580) std::cerr << f.receipts.at("package").dump() << " reads=" << f.resource_reads << " resource=" << f.control->resource_bytes << "\n";
  require(f.receipts.at("package").contains("result") && f.resource_reads == 2 && f.control->resource_bytes == 1048580, "package audio did not completely read bounded chunks");
  for (const auto& context : f.resource_contexts) require(context.at("callId") == "package" && context.at("application") == f.app && context.at("account") == f.account, "audio resource dropped exact trusted context");
  Fixture legacy; legacy.create(); legacy.resource_support = false; legacy.set("legacy", "src", "audio.wav"); legacy.until([&] { return legacy.receipts.count("legacy"); }); require(legacy.receipts.at("legacy").contains("error") && !legacy.resource_reads, "old runtime silently accepted package source");
  Fixture corrupt; corrupt.create(); corrupt.inconsistent_resource = true; corrupt.set("bad", "src", "audio.wav"); corrupt.until([&] { return corrupt.receipts.count("bad"); }); require(corrupt.receipts.at("bad").contains("error") && !corrupt.control->executed, "inconsistent resource bytes reached native decoder");
}
}
int main() {
  try { format_and_catalog(); destroy_waits_and_revokes(); queued_receipt_fencing(); background_and_reset(); late_acquire_and_raw_errors(); budget_backpressure_retirement(); package_chunks_and_negotiation();
    std::cout << "Audio lifecycle: 7 groups passed (controlled backend, no hardware or runtime acceptance)\n"; return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
