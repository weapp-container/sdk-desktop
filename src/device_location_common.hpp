#pragma once
#include "device_socket_common.hpp"
#include <cmath>
#include <optional>

namespace weapp::desktop::detail {
struct LocationOptions {
  bool continuous = false, fuzzy = false, high_accuracy = false;
  long timeout_ms = 30000;
};
inline LocationOptions location_options(const std::string& api, const Json& payload) {
  if (payload.value("type", "wgs84") != "wgs84")
    throw std::invalid_argument("native location supports WGS84 coordinates");
  LocationOptions result;
  result.continuous = api == "wx.startLocationUpdate";
  result.fuzzy = api == "wx.getFuzzyLocation";
  result.high_accuracy = !result.fuzzy && payload.value("isHighAccuracy", false);
  auto timeout = payload.value("timeout", int64_t(30000));
  if (timeout < 100 || timeout > 120000) throw std::invalid_argument("location timeout must be 100..120000 milliseconds");
  result.timeout_ms = long(timeout); return result;
}
inline Json location_position(Json value, bool fuzzy) {
  const auto latitude = value.at("latitude").get<double>(), longitude = value.at("longitude").get<double>();
  const auto accuracy = value.at("accuracy").get<double>();
  if (!std::isfinite(latitude) || !std::isfinite(longitude) || !std::isfinite(accuracy) ||
      std::abs(latitude) > 90 || std::abs(longitude) > 180 || accuracy < 0)
    throw std::runtime_error("native location returned invalid coordinates or accuracy");
  for (auto field : {"altitude", "verticalAccuracy", "horizontalAccuracy", "speed"})
    if (value.contains(field) && !std::isfinite(value.at(field).get<double>())) throw std::runtime_error("native location returned invalid metadata");
  if (fuzzy) return {{"latitude", std::round(latitude * 100) / 100}, {"longitude", std::round(longitude * 100) / 100}, {"accuracy", std::max(accuracy, 1500.0)}};
  return value;
}
struct NativeLocation {
  std::weak_ptr<DeviceQueue> queue;
  std::string call, api;
  LocationOptions options;
  std::atomic<bool> cancelled{false}, finished{false}, started{false};
  std::shared_ptr<std::atomic<bool>> delivery = std::make_shared<std::atomic<bool>>(true);
  std::mutex mutex;
  std::condition_variable changed;
  std::function<void()> interrupt;
  std::optional<Json> result;
  std::string cleanup_error;
  NativeLocation(std::weak_ptr<DeviceQueue> owner, std::string id, std::string name, LocationOptions request)
      : queue(std::move(owner)), call(std::move(id)), api(std::move(name)), options(request) {}
  void cancel() {
    std::function<void()> stop;
    { std::lock_guard<std::mutex> guard(mutex); cancelled = true; *delivery = false; stop = interrupt; }
    changed.notify_all(); if (stop) stop();
  }
  bool wait_for(std::chrono::milliseconds interval) {
    std::unique_lock<std::mutex> guard(mutex);
    changed.wait_for(guard, interval, [&] { return cancelled.load(); }); return !cancelled;
  }
  bool push(DeviceQueued item) {
    item.delivery_allowed = delivery;
    if (auto owner = queue.lock()) return owner->push(std::move(item)); return false;
  }
  void announce_started() {
    if (!options.continuous || cancelled || started.exchange(true)) return;
    if (!push({DeviceQueued::Complete, call, "", "", device_ok(api)}))
      throw std::runtime_error("native location completion queue is full");
  }
  void position(Json value) {
    value = location_position(std::move(value), options.fuzzy);
    if (cancelled) return;
    bool changed_position;
    { std::lock_guard<std::mutex> guard(mutex); changed_position = !result || *result != value; result = value; }
    if (options.continuous && changed_position && !push({DeviceQueued::NativeEvent, "", "LocationChange", "", std::move(value)}))
      throw std::runtime_error("native location ingress backpressure limit exceeded");
  }
  void cleanup_failed(const std::string& error) {
    std::lock_guard<std::mutex> guard(mutex); cleanup_error = socket_terminal_text(error, 512);
  }
  std::string cleanup_failure() { std::lock_guard<std::mutex> guard(mutex); return cleanup_error; }
  void cleanup_succeeded() { std::lock_guard<std::mutex> guard(mutex); cleanup_error.clear(); }
  // The worker calls this only after destroying its platform source.
  void finish(const std::string& error) {
    if (cancelled) push({DeviceQueued::Failure, call, "", "", device_failure(api, "native operation was retired", "cancelled")});
    else {
      if (!error.empty()) {
        auto message = socket_terminal_text(error, 512);
        push({DeviceQueued::Failure, call, "", "", device_failure(api, message)});
        if (options.continuous) push({DeviceQueued::NativeTerminalEvent, "", "LocationChangeError", "", {{"errMsg", message}}});
      } else if (!options.continuous) {
        std::optional<Json> position;
        { std::lock_guard<std::mutex> guard(mutex); position = result; }
        if (position) push({DeviceQueued::Complete, call, "", "", device_ok(api, std::move(*position))});
        else push({DeviceQueued::Failure, call, "", "", device_failure(api, "native location returned no position")});
      }
    }
    finished = true; changed.notify_all();
  }
};
class LocationSource {
 public:
  virtual ~LocationSource() = default;
  virtual void run(const std::shared_ptr<NativeLocation>&) = 0;
  // A failed attempt retains the source and all native handles on its worker.
  // Returning true acknowledges cleanup; destruction and receipts follow it.
  virtual bool cleanup(const std::shared_ptr<NativeLocation>&) { return true; }
};
using LocationFactory = std::function<std::unique_ptr<LocationSource>(const DeviceApiOptions&, const LocationOptions&)>;
class DeviceLocations : public DeviceSockets {
  LocationFactory location_factory_;
  std::vector<std::shared_ptr<NativeLocation>> locations_;
  std::shared_ptr<NativeLocation> watch_;
  struct Intent { std::shared_ptr<std::atomic<bool>> allowed; bool continuous; };
  std::map<std::string, Intent> intents_;
  struct Stop { std::string call; std::shared_ptr<NativeLocation> state; };
  std::vector<Stop> stops_;
  void reap_locations() {
    locations_.erase(std::remove_if(locations_.begin(), locations_.end(), [](const auto& state) { return state->finished.load(); }), locations_.end());
  }
  void start_location(const std::string& call, const std::string& api, const LocationOptions& options,
      const std::shared_ptr<std::atomic<bool>>& intent) {
    // Application consent may complete after the app went to the background.
    if (!foreground_) throw std::invalid_argument("location access requires a foreground application");
    reap_locations();
    if (options.continuous && watch_ && !watch_->cancelled && !watch_->finished) {
      if (watch_->options.high_accuracy != options.high_accuracy || watch_->options.timeout_ms != options.timeout_ms)
        throw std::invalid_argument("stop the active location watch before changing its options");
      if (!watch_->started) throw std::invalid_argument("location watch startup is already pending");
      DeviceQueued receipt{DeviceQueued::Complete, call, "", "", device_ok(api)};
      receipt.delivery_allowed = intent;
      if (!queue_->push(std::move(receipt))) throw std::runtime_error("native location completion queue is full");
      return;
    }
    if (locations_.size() >= 8) throw std::invalid_argument("native location concurrency limit exceeded");
    if (options.continuous && watch_) watch_->cancel();
    auto state = std::make_shared<NativeLocation>(weak_queue(), call, api, options);
    state->delivery = intent;
    auto ticket = std::make_shared<std::promise<std::unique_ptr<LocationSource>>>();
    auto prepared = ticket->get_future();
    workers_.reserve(workers_.size() + 1); locations_.reserve(locations_.size() + 1);
    // Start the waiting executor before owner-thread OS permission acquisition.
    workers_.push_back(std::async(std::launch::async, [state, prepared = std::move(prepared)]() mutable {
      std::string error;
      std::unique_ptr<LocationSource> source;
      try { source = prepared.get(); if (!source) throw std::runtime_error("native location source unavailable"); source->run(state); }
      catch (const std::exception& failure) { error = failure.what(); }
      catch (...) { error = "native location source failed"; }
      if (source) while (true) {
        try { if (source->cleanup(state)) break; }
        catch (const std::exception& failure) { state->cleanup_failed(failure.what()); }
        catch (...) { state->cleanup_failed("native location cleanup failed"); }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      source.reset(); state->cleanup_succeeded();
      state->finish(error);
    }));
    locations_.push_back(state);
    if (options.continuous) watch_ = state;
    calls_.at(call).cancel = [state] { state->cancel(); };
    try { ticket->set_value(location_factory_(options_, options)); }
    catch (...) { ticket->set_exception(std::current_exception()); }
  }
 protected:
  DeviceLocations(DeviceApiOptions options, Command command) : DeviceSockets(std::move(options), std::move(command)) {}
  void install_location_factory(LocationFactory factory) {
    if (!factory) return;
    location_factory_ = std::move(factory);
    for (auto api : {"wx.getLocation", "wx.getFuzzyLocation", "wx.startLocationUpdate", "wx.stopLocationUpdate"}) add(api);
    for (auto event : {"LocationChange", "LocationChangeError"}) add_event("wx.on" + std::string(event), "wx.off" + std::string(event), event);
  }
  bool location_call(const std::string& call, const std::string& api, const Json& payload) {
    if (!location_factory_) return false;
    if (api == "wx.stopLocationUpdate") {
      for (const auto& [pending, intent] : intents_) if (intent.continuous) {
        *intent.allowed = false;
        if (calls_.count(pending)) fail(pending, "wx.startLocationUpdate", "location update was stopped", "cancelled");
      }
      if (!watch_) done(call, api);
      else { watch_->cancel(); stops_.push_back({call, watch_}); }
      return true;
    }
    if (api != "wx.getLocation" && api != "wx.getFuzzyLocation" && api != "wx.startLocationUpdate") return false;
    if (!foreground_) throw std::invalid_argument("location access requires a foreground application");
    auto options = location_options(api, payload);
    auto intent = std::make_shared<std::atomic<bool>>(true);
    intents_[call] = {intent, options.continuous};
    calls_.at(call).cancel = [intent] { *intent = false; };
    permit(options.fuzzy ? "scope.userFuzzyLocation" : "scope.userLocation", api, call, [this, call, api, options, intent] {
      if (!intent->load()) { fail(call, api, "location request was retired", "cancelled"); return; }
      try { start_location(call, api, options, intent); }
      catch (const std::exception& error) { fail(call, api, error.what()); }
    });
    return true;
  }
  void cancel_locations() {
    for (const auto& [call, intent] : intents_) {
      *intent.allowed = false;
      if (!queue_->closed && calls_.count(call)) fail(call, calls_.at(call).context.value("api", "wx.getLocation"), "location request was retired", "cancelled");
    }
    for (auto& state : locations_) state->cancel();
    if (watch_) watch_->cancel();
  }
  void permissions_reset() override { cancel_locations(); DeviceSockets::permissions_reset(); }
  void retire_platform() override { cancel_locations(); DeviceSockets::retire_platform(); }
  Status foreground_changed(bool visible) override {
    if (!visible) cancel_locations(); return DeviceSockets::foreground_changed(visible);
  }
 public:
  ~DeviceLocations() override { retire(); }
  Status pump() override {
    auto status = DeviceSockets::pump(); if (!status) return status;
    for (auto item = intents_.begin(); item != intents_.end();)
      if (!calls_.count(item->first)) item = intents_.erase(item); else ++item;
    for (auto item = stops_.begin(); item != stops_.end();) {
      if (!calls_.count(item->call)) { item = stops_.erase(item); continue; }
      if (!item->state->finished) { ++item; continue; }
      done(item->call, "wx.stopLocationUpdate");
      item = stops_.erase(item);
    }
    reap_locations(); return status;
  }
};
}  // namespace weapp::desktop::detail
