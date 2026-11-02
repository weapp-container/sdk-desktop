#pragma once
#include "device_sensor_common.hpp"

namespace weapp::desktop::detail {
struct AudioFailure : std::runtime_error {
  std::string category;
  int64_t native;
  Json public_fields;
  AudioFailure(std::string message, std::string code = "invalid_request", int64_t raw = 0, Json fields = Json::object())
      : std::runtime_error(std::move(message)), category(std::move(code)), native(raw), public_fields(std::move(fields)) {}
};
inline Json audio_defaults() {
  return {{"src", ""}, {"autoplay", false}, {"loop", false}, {"startTime", 0.0}, {"volume", 1.0}, {"playbackRate", 1.0},
      {"paused", true}, {"currentTime", 0.0}, {"duration", 0.0}, {"buffered", 0.0}};
}
inline void audio_property(const std::string& member, const Json& value) {
  if (member == "src") {
    if (!value.is_string() || value.get_ref<const std::string&>().size() > 4096) throw std::invalid_argument("audio src must be a bounded managed file path");
    const auto& path = value.get_ref<const std::string&>();
    if (path.find('\0') != std::string::npos || (path.find(":") != std::string::npos && path.rfind("wxfile://user/", 0) != 0)) throw std::invalid_argument("audio src requires an application-managed file or verified package resource; remote audio is unavailable");
  } else if (member == "autoplay" || member == "loop") {
    if (!value.is_boolean()) throw std::invalid_argument("audio boolean property requires true or false");
  } else if (member == "volume" || member == "playbackRate" || member == "startTime") {
    if (!value.is_number() || !std::isfinite(value.get<double>())) throw std::invalid_argument("audio property requires a finite number");
    const auto number = value.get<double>();
    if ((member == "volume" && (number < 0 || number > 1)) || (member == "playbackRate" && (number < 0.5 || number > 2)) ||
        (member == "startTime" && (number < 0 || static_cast<long double>(number) * 10000000.L >= 9223372036854775808.L)))
      throw std::invalid_argument("audio property is outside its supported range");
  } else throw std::invalid_argument("audio property is read-only or unavailable");
}
struct AudioCommand {
  std::string call, api, member;
  std::vector<uint8_t> resource;
  Json payload, result = Json::object(), failure;
  uint64_t generation = 0;
  std::atomic<bool> cancelled{false}, finished{false};
  bool settled = false;
  std::shared_ptr<std::atomic<bool>> delivery;
  void cancel() { cancelled = true; if (delivery) *delivery = false; }
};
struct NativeAudio {
  std::weak_ptr<DeviceQueue> queue;
  std::string object, call;
  std::atomic<bool> cancelled{false}, failed{false}, ready{false}, finished{false};
  std::atomic<uint64_t> generation{0};
  std::mutex mutex;
  std::condition_variable changed;
  Json properties = audio_defaults(), failure;
  std::deque<std::shared_ptr<AudioCommand>> commands;
  std::vector<std::weak_ptr<std::atomic<bool>>> receipt_tokens;
  size_t pending = 0;
  bool settled = false, error_sent = false; // runtime owner thread only
  std::shared_ptr<std::atomic<bool>> delivery = std::make_shared<std::atomic<bool>>(true);
  std::shared_ptr<std::atomic<bool>> source_delivery = std::make_shared<std::atomic<bool>>(true);
  NativeAudio(std::weak_ptr<DeviceQueue> owner, std::string id, std::string creator)
      : queue(std::move(owner)), object(std::move(id)), call(std::move(creator)) {}
  bool stopping() const { return cancelled || failed; }
  void cancel() {
    cancelled = true; *delivery = false;
    { std::lock_guard<std::mutex> guard(mutex); *source_delivery = false; for (auto& token : receipt_tokens) if (auto value = token.lock()) *value = false; for (auto& command : commands) command->cancel(); }
    changed.notify_all();
  }
  void fault(std::string message, std::string category = "invalid_request", int64_t native = 0) {
    { std::lock_guard<std::mutex> guard(mutex);
      if (!failed) failure = device_failure("wx.createInnerAudioContext", socket_terminal_text(message, 512), category, native);
      failed = true; *source_delivery = false; for (auto& token : receipt_tokens) if (auto value = token.lock()) *value = false;
    }
    *delivery = false; changed.notify_all();
  }
  Json snapshot() { std::lock_guard<std::mutex> guard(mutex); return properties; }
  std::shared_ptr<std::atomic<bool>> source_token() { std::lock_guard<std::mutex> guard(mutex); return source_delivery; }
  bool current(uint64_t value) const { return !stopping() && generation == value; }
  uint64_t retire_source() {
    std::lock_guard<std::mutex> guard(mutex);
    *source_delivery = false; for (auto& token : receipt_tokens) if (auto value = token.lock()) *value = false; receipt_tokens.clear();
    source_delivery = std::make_shared<std::atomic<bool>>(true); auto value = ++generation;
    changed.notify_all(); return value;
  }
  bool sample(uint64_t value, Json patch, const std::string& event = "TimeUpdate", Json extra = Json::object()) {
    std::shared_ptr<std::atomic<bool>> token;
    { std::lock_guard<std::mutex> guard(mutex);
      if (!current(value)) return false;
      for (auto key : {"paused", "currentTime", "duration", "buffered"}) if (patch.contains(key)) properties[key] = patch.at(key);
      extra["properties"] = properties; token = source_delivery;
    }
    DeviceQueued item{DeviceQueued::NativeEvent, "", event, object, std::move(extra)}; item.delivery_allowed = token;
    if (auto owner = queue.lock(); owner && owner->push(std::move(item))) return true;
    fault("audio event ingress backpressure limit exceeded"); return false;
  }
  bool enqueue(const std::shared_ptr<AudioCommand>& command) {
    std::lock_guard<std::mutex> guard(mutex);
    if (stopping() || pending >= 32) return false;
    ++pending; commands.push_back(command);
    receipt_tokens.erase(std::remove_if(receipt_tokens.begin(), receipt_tokens.end(), [](const auto& token) { return token.expired(); }), receipt_tokens.end());
    receipt_tokens.push_back(command->delivery); changed.notify_all(); return true;
  }
  std::shared_ptr<AudioCommand> next() {
    std::unique_lock<std::mutex> guard(mutex);
    changed.wait_for(guard, std::chrono::milliseconds(20), [&] { return stopping() || !commands.empty(); });
    if (stopping() || commands.empty()) return {};
    auto command = commands.front(); commands.pop_front(); return command;
  }
  void abandon_commands() {
    std::lock_guard<std::mutex> guard(mutex);
    for (auto& command : commands) { command->failure = device_failure(command->api, "audio owner was retired", "cancelled"); command->finished = true; }
    commands.clear();
  }
};
class AudioBackend {
 public:
  virtual ~AudioBackend() = default;
  virtual void open(const std::shared_ptr<NativeAudio>&) = 0;
  virtual void poll(const std::shared_ptr<NativeAudio>&) = 0;
  virtual Json execute(const std::shared_ptr<NativeAudio>&, const std::shared_ptr<AudioCommand>&) = 0;
  virtual bool cleanup(const std::shared_ptr<NativeAudio>&) = 0;
};
using AudioFactory = std::function<std::unique_ptr<AudioBackend>(const std::filesystem::path&)>;
class DeviceAudio : public DeviceSensors {
  AudioFactory audio_factory_;
  std::map<std::string, std::shared_ptr<NativeAudio>> audio_;
  std::vector<std::shared_ptr<NativeAudio>> audio_owners_;
  struct Pending { std::shared_ptr<NativeAudio> owner; std::shared_ptr<AudioCommand> command; };
  std::vector<Pending> audio_commands_;
  struct Destroy { std::string call, api; std::shared_ptr<NativeAudio> owner; };
  std::vector<Destroy> audio_destroy_;
  void cancel_audio() { for (auto& state : audio_owners_) state->cancel(); }
 protected:
  DeviceAudio(DeviceApiOptions options, Command command) : DeviceSensors(std::move(options), std::move(command)) {}
  void install_audio_factory(AudioFactory factory) {
    if (!factory) return;
    audio_factory_ = std::move(factory);
    for (auto api : {"wx.createInnerAudioContext", "object:InnerAudioContext.src", "object:InnerAudioContext.autoplay", "object:InnerAudioContext.loop", "object:InnerAudioContext.startTime", "object:InnerAudioContext.volume", "object:InnerAudioContext.playbackRate", "object:InnerAudioContext.paused", "object:InnerAudioContext.currentTime", "object:InnerAudioContext.duration", "object:InnerAudioContext.buffered", "object:InnerAudioContext.play", "object:InnerAudioContext.pause", "object:InnerAudioContext.stop", "object:InnerAudioContext.seek", "object:InnerAudioContext.destroy"}) add(api);
    add_event("object:InnerAudioContext.onCanplay", "object:InnerAudioContext.offCanplay", "Canplay");
    add_event("object:InnerAudioContext.onEnded", "object:InnerAudioContext.offEnded", "Ended");
    add_event("object:InnerAudioContext.onError", "object:InnerAudioContext.offError", "Error");
    add_event("object:InnerAudioContext.onPause", "object:InnerAudioContext.offPause", "Pause");
    add_event("object:InnerAudioContext.onPlay", "object:InnerAudioContext.offPlay", "Play");
    add_event("object:InnerAudioContext.onSeeked", "object:InnerAudioContext.offSeeked", "Seeked");
    add_event("object:InnerAudioContext.onSeeking", "object:InnerAudioContext.offSeeking", "Seeking");
    add_event("object:InnerAudioContext.onStop", "object:InnerAudioContext.offStop", "Stop");
    add_event("object:InnerAudioContext.onTimeUpdate", "object:InnerAudioContext.offTimeUpdate", "TimeUpdate");
    add_event("object:InnerAudioContext.onWaiting", "object:InnerAudioContext.offWaiting", "Waiting");
  }
  bool audio_call(const std::string& call, const std::string& api, const Json& payload) {
    if (!audio_factory_) return false;
    if (api == "wx.createInnerAudioContext") {
      if (!foreground_) throw std::invalid_argument("audio creation requires a foreground application");
      if (audio_owners_.size() >= 8) throw std::invalid_argument("native audio context limit exceeded");
      auto id = payload.at("objectId").get<std::string>();
      auto state = std::make_shared<NativeAudio>(weak_queue(), id, call); auto factory = audio_factory_; auto root = root_;
      workers_.reserve(workers_.size() + 1); audio_owners_.reserve(audio_owners_.size() + 1);
      retain_object(id, call, "InnerAudioContext", [state] { state->cancel(); });
      audio_[id] = state; audio_owners_.push_back(state);
      try { workers_.push_back(std::async(std::launch::async, [state, factory, root] {
        std::unique_ptr<AudioBackend> backend;
        try { backend = factory(root); if (!backend) throw AudioFailure("native audio backend is unavailable", "unsupported"); backend->open(state); if (!state->stopping()) state->ready = true;
          while (!state->stopping()) {
            backend->poll(state); if (state->stopping()) break;
            auto command = state->next(); if (!command) continue;
            try {
              if (command->cancelled || !state->current(command->generation)) throw AudioFailure("audio command was retired", "cancelled");
              command->result = backend->execute(state, command);
              if (command->cancelled || !state->current(command->generation)) throw AudioFailure("audio command was retired", "cancelled");
              if (command->payload.value("action", "") == "set") {
                std::lock_guard<std::mutex> guard(state->mutex);
                if (state->current(command->generation) && !command->cancelled) state->properties[command->member] = command->payload.at("value");
              }
            } catch (const AudioFailure& error) { command->failure = device_failure(command->api, error.what(), error.category, error.native); command->failure["details"].update(error.public_fields); }
              catch (const std::system_error& error) { command->failure = device_failure(command->api, error.what(), "invalid_request", error.code().value()); }
              catch (const std::exception& error) { command->failure = device_failure(command->api, error.what()); }
              catch (...) { command->failure = device_failure(command->api, "native audio command failed"); }
            command->finished = true;
          }
        } catch (const AudioFailure& error) { state->fault(error.what(), error.category, error.native); }
          catch (const std::system_error& error) { state->fault(error.what(), "invalid_request", error.code().value()); }
          catch (const std::exception& error) { state->fault(error.what()); }
          catch (...) { state->fault("native audio backend failed"); }
        state->abandon_commands();
        if (backend) while (true) {
          try { if (backend->cleanup(state)) break; } catch (...) { /* Exact backend and native handles remain owned for retry. */ }
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        backend.reset(); state->finished = true; state->changed.notify_all();
      })); } catch (...) { state->cancel(); state->finished = true; state->settled = true; audio_.erase(id); audio_owners_.pop_back(); objects_.erase(id); throw; } return true;
    }
    if (api.rfind("object:InnerAudioContext.", 0) != 0) return false;
    require_object(payload, "InnerAudioContext"); auto id = payload.at("objectId").get<std::string>();
    auto found = audio_.find(id); if (found == audio_.end() || found->second->stopping()) throw std::invalid_argument("audio owner is retired"); auto state = found->second;
    auto member = api.substr(api.find_last_of('.') + 1);
    if (member == "destroy") {
      state->cancel(); cancelled(state->call); objects_.erase(id); audio_.erase(found); audio_destroy_.push_back({call, api, state}); return true;
    }
    const bool setting = payload.value("action", "") == "set";
    if (setting) {
      audio_property(member, payload.at("value"));
      if (member == "src") { if (!foreground_) throw std::invalid_argument("audio source acquisition requires a foreground application"); const auto& value = payload.at("value").get_ref<const std::string&>(); if (value.rfind("wxfile://user/", 0) == 0) (void)managed_path(root_, value); }
    } else if (member != "play" && member != "pause" && member != "stop" && member != "seek") throw std::invalid_argument("audio property reads use the synchronous facade snapshot");
    if (!foreground_ && (member == "play" || member == "seek")) throw std::invalid_argument("audio playback requires a foreground application");
    auto command = std::make_shared<AudioCommand>(); command->call = call; command->api = api; command->member = member; command->payload = payload;
    command->delivery = std::make_shared<std::atomic<bool>>(true);
    // Capacity is checked before accepting a new source or local preference.
    { std::lock_guard<std::mutex> guard(state->mutex); if (state->pending >= 32) throw std::invalid_argument("native audio command limit exceeded"); }
    if (setting && member == "src") state->retire_source();
    command->generation = state->generation;
    if (setting && member == "src") {
      const auto& source = payload.at("value").get_ref<const std::string&>();
      if (!source.empty() && source.rfind("wxfile://user/", 0) != 0) {
        // Only the runtime selects a file from this exact pending setter. No
        // native path/hash field is accepted. These calls stay on owner_.
        Value capabilities; auto status = command_("capabilities", {}, &capabilities);
        if (!status) throw std::invalid_argument("package audio requires a runtime with hostApiResource support");
        auto caps = encode(capabilities);
        if (!caps.contains("hostServices") || caps.at("hostServices").value("apiResourceChunks", "") != "1.0")
          throw std::invalid_argument("package audio resource protocol is unavailable in this runtime");
        constexpr size_t limit = 16711680, chunk = 1048576;
        size_t total = 0;
        do {
          Value output; auto fields = std::get<Value::Object>(decode(Json{{"context", calls_.at(call).context}, {"offset", command->resource.size()}, {"length", chunk}}).data);
          auto read = command_("hostApiResource", fields, &output);
          if (!read) throw std::invalid_argument("verified package audio read failed: " + read.message);
          auto result = encode(output);
          auto integer = [](const Json& value, uint64_t maximum) {
            if (!value.is_number()) throw std::invalid_argument("package audio count is not numeric");
            auto number = value.get<double>();
            if (!std::isfinite(number) || number < 0 || number > double(maximum) || std::floor(number) != number) throw std::invalid_argument("package audio count is not a bounded exact integer");
            return uint64_t(number);
          };
          if (!result.contains("totalBytes") || !result.contains("bytes") || !result.at("bytes").is_array()) throw std::invalid_argument("invalid package audio chunk response");
          auto count = integer(result.at("totalBytes"), limit);
          if (!count || count > limit || (total && total != count)) throw std::invalid_argument("inconsistent or oversized package audio");
          total = size_t(count); const auto& bytes = result.at("bytes");
          if (bytes.empty() || bytes.size() > chunk || bytes.size() > total - command->resource.size()) throw std::invalid_argument("invalid package audio chunk progress");
          for (const auto& byte : bytes) command->resource.push_back(uint8_t(integer(byte, 255)));
        } while (command->resource.size() < total);
      }
    }
    if (!state->enqueue(command)) throw std::invalid_argument("native audio owner is unavailable");
    audio_commands_.push_back({state, command}); calls_.at(call).cancel = [command, state] { command->cancel(); state->changed.notify_all(); }; return true;
  }
  void permissions_reset() override { for (auto& state : audio_owners_) if (!state->stopping()) state->retire_source(); DeviceSensors::permissions_reset(); }
  void retire_platform() override { cancel_audio(); DeviceSensors::retire_platform(); }
  Status foreground_changed(bool visible) override { if (!visible) for (auto& state : audio_owners_) if (!state->stopping()) state->retire_source(); return DeviceSensors::foreground_changed(visible); }
 public:
  ~DeviceAudio() override { retire(); }
  Status pump() override {
    auto status = DeviceSensors::pump(); if (!status || queue_->closed) return status;
    for (auto& state : audio_owners_) {
      if (!calls_.count(state->call)) { state->settled = true; state->cancel(); }
      if (!state->settled && (state->ready || state->failed || state->finished)) {
        DeviceQueued item; item.id = state->call;
        if (state->stopping() || !state->ready) { item.kind = DeviceQueued::Failure; std::lock_guard<std::mutex> guard(state->mutex); item.value = state->failure.is_null() ? device_failure("wx.createInnerAudioContext", "audio owner was retired", "cancelled") : state->failure; }
        else { item.kind = DeviceQueued::Complete; item.value = device_ok("wx.createInnerAudioContext", {{"objectId", state->object}, {"properties", state->snapshot()}}); item.delivery_allowed = state->delivery; }
        if (queue_->push(std::move(item))) state->settled = true;
      }
      if (state->failed && state->ready && !state->error_sent) {
        Json detail; { std::lock_guard<std::mutex> guard(state->mutex); detail = state->failure.at("details"); }
        if (queue_->push({DeviceQueued::NativeTerminalEvent, "", "Error", state->object, detail})) state->error_sent = true;
      }
    }
    for (auto item = audio_commands_.begin(); item != audio_commands_.end();) {
      auto& state = item->owner; auto& command = item->command;
      if (!calls_.count(command->call)) { command->settled = true; command->cancel(); }
      if (command->finished && !command->settled) {
        DeviceQueued receipt; receipt.id = command->call;
        if (!command->failure.is_null()) { receipt.kind = DeviceQueued::Failure; receipt.value = command->failure; }
        else if (!state->current(command->generation) || command->cancelled) { receipt.kind = DeviceQueued::Failure; receipt.value = device_failure(command->api, "audio source was retired", "cancelled"); }
        else { receipt.kind = DeviceQueued::Complete; auto result = command->result; result["properties"] = state->snapshot(); receipt.value = device_ok(command->api, std::move(result)); receipt.delivery_allowed = command->delivery; }
        if (queue_->push(std::move(receipt))) command->settled = true;
      }
      if (command->finished && command->settled) { std::lock_guard<std::mutex> guard(state->mutex); --state->pending; item = audio_commands_.erase(item); } else ++item;
    }
    for (auto item = audio_destroy_.begin(); item != audio_destroy_.end();) {
      if (!calls_.count(item->call)) { item = audio_destroy_.erase(item); continue; }
      if (item->owner->finished && queue_->push({DeviceQueued::Complete, item->call, "", "", device_ok(item->api)})) item = audio_destroy_.erase(item); else ++item;
    }
    for (auto item = audio_.begin(); item != audio_.end();) if (item->second->finished) { objects_.erase(item->first); item = audio_.erase(item); } else ++item;
    audio_owners_.erase(std::remove_if(audio_owners_.begin(), audio_owners_.end(), [](const auto& state) { return state->finished && state->settled; }), audio_owners_.end());
    return status;
  }
};
} // namespace weapp::desktop::detail
