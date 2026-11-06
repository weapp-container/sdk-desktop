#pragma once
#include "device_bluetooth_common.hpp"
#include <cmath>

namespace weapp::desktop::detail {
enum class SensorKind { Accelerometer, Compass, Gyroscope };
inline std::string sensor_name(SensorKind kind) {
  switch (kind) {
    case SensorKind::Accelerometer: return "Accelerometer";
    case SensorKind::Compass: return "Compass";
    case SensorKind::Gyroscope: return "Gyroscope";
  }
  throw std::invalid_argument("unknown native sensor");
}
inline uint32_t sensor_interval(SensorKind kind, const Json& payload) {
  auto interval = payload.value("interval", "normal");
  if (kind == SensorKind::Compass && interval != "normal")
    throw std::invalid_argument("compass does not expose a configurable sampling interval");
  if (interval == "normal") return 200;
  if (interval == "ui") return 60;
  if (interval == "game") return 20;
  throw std::invalid_argument("sensor interval must be normal, ui or game");
}
inline Json sensor_vector(double x, double y, double z, bool degrees = false) {
  constexpr double radians = 3.14159265358979323846 / 180.0;
  const auto scale = degrees ? radians : 1.0;
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
    throw std::runtime_error("native sensor returned a non-finite vector");
  return {{"x", x * scale}, {"y", y * scale}, {"z", z * scale}};
}
inline Json sensor_heading(double direction, const std::string& accuracy) {
  if (!std::isfinite(direction) || direction < 0 || direction > 360)
    throw std::runtime_error("native compass returned an invalid heading");
  Json value = {{"direction", direction == 360 ? 0.0 : direction}};
  // An unknown OS accuracy is omitted instead of inventing a measurement.
  if (!accuracy.empty()) value["accuracy"] = accuracy;
  return value;
}
struct NativeSensor {
  std::weak_ptr<DeviceQueue> queue;
  SensorKind kind;
  uint32_t interval;
  std::string call, api;
  std::atomic<bool> cancelled{false}, failed{false}, started{false}, finished{false};
  std::shared_ptr<std::atomic<bool>> delivery = std::make_shared<std::atomic<bool>>(true);
  std::mutex mutex;
  std::condition_variable changed;
  std::string error, cleanup_error, error_code = "invalid_request";
  int64_t native_error = 0;
  bool settled = false; // owner thread only; retained even when ingress is full
  NativeSensor(std::weak_ptr<DeviceQueue> owner, SensorKind type, uint32_t period, std::string id)
      : queue(std::move(owner)), kind(type), interval(period), call(std::move(id)), api("wx.start" + sensor_name(type)) {}
  bool stopping() const { return cancelled || failed; }
  void cancel() { cancelled = true; *delivery = false; changed.notify_all(); }
  void fault(const std::string& message, const std::string& code = "invalid_request", int64_t native = 0) {
    { std::lock_guard<std::mutex> guard(mutex);
      if (!failed) { error = socket_terminal_text(message, 512); error_code = code; native_error = native; }
      failed = true;
    }
    *delivery = false; changed.notify_all();
  }
  void announce_started() { if (!stopping()) started = true; }
  void wait() { std::unique_lock<std::mutex> guard(mutex); changed.wait(guard, [&] { return stopping(); }); }
  bool sample(Json value) {
    if (stopping() || !started) return false;
    DeviceQueued item{DeviceQueued::NativeEvent, "", sensor_name(kind) + "Change", "", std::move(value)};
    item.delivery_allowed = delivery;
    if (auto owner = queue.lock(); owner && owner->push(std::move(item))) return true;
    fault("native sensor ingress backpressure limit exceeded"); return false;
  }
  void cleanup_failed(const std::string& message) { std::lock_guard<std::mutex> guard(mutex); cleanup_error = socket_terminal_text(message, 512); }
  // Owner pump retries this exact settlement after draining ingress; a full
  // queue cannot discard a startup failure or manufacture a successful stop.
  bool settle() {
    if (settled) return true;
    if (!started && !finished) return false;
    DeviceQueued item;
    item.id = call;
    if (stopping() || !started) {
      std::lock_guard<std::mutex> guard(mutex);
      item.kind = DeviceQueued::Failure;
      item.value = device_failure(api, cancelled ? "native sensor was retired" : error.empty() ? "native sensor returned no source" : error,
          cancelled ? "cancelled" : error_code, native_error);
    } else {
      item.kind = DeviceQueued::Complete; item.value = device_ok(api); item.delivery_allowed = delivery;
    }
    if (auto owner = queue.lock(); owner && owner->push(std::move(item))) { settled = true; return true; }
    return false;
  }
};
class SensorSource {
 public:
  virtual ~SensorSource() = default;
  virtual void run(const std::shared_ptr<NativeSensor>&) = 0;
  // False/exception retains this exact source, registration and native owner.
  virtual bool cleanup(const std::shared_ptr<NativeSensor>&) = 0;
};
using SensorFactory = std::function<std::unique_ptr<SensorSource>(SensorKind)>;
class DeviceSensors : public DeviceBluetooth {
  SensorFactory sensor_factory_;
  std::vector<std::shared_ptr<NativeSensor>> sensors_;
  struct Stop { std::string call, api; std::vector<std::shared_ptr<NativeSensor>> owners; };
  std::vector<Stop> sensor_stops_;
  void reap_sensors() {
    sensors_.erase(std::remove_if(sensors_.begin(), sensors_.end(), [](const auto& state) { return state->finished && state->settled; }), sensors_.end());
  }
  void cancel_sensors() { for (auto& state : sensors_) state->cancel(); }
 protected:
  DeviceSensors(DeviceApiOptions options, Command command) : DeviceBluetooth(std::move(options), std::move(command)) {}
  void install_sensor_factory(SensorFactory factory) {
    if (!factory) return;
    sensor_factory_ = std::move(factory);
    for (auto api : {"wx.startAccelerometer", "wx.stopAccelerometer", "wx.startCompass", "wx.stopCompass", "wx.startGyroscope", "wx.stopGyroscope"}) add(api);
    add_event("wx.onAccelerometerChange", "wx.offAccelerometerChange", "AccelerometerChange");
    add_event("wx.onCompassChange", "wx.offCompassChange", "CompassChange");
    add_event("wx.onGyroscopeChange", "wx.offGyroscopeChange", "GyroscopeChange");
  }
  bool sensor_call(const std::string& call, const std::string& api, const Json& payload) {
    if (!sensor_factory_) return false;
    for (auto kind : {SensorKind::Accelerometer, SensorKind::Compass, SensorKind::Gyroscope}) {
      auto name = sensor_name(kind);
      if (api == "wx.stop" + name) {
        Stop stop{call, api, {}};
        // Include every earlier owner, including cancelled or faulted sources
        // whose native removal is still pending, rather than only the latest.
        for (auto& state : sensors_) if (state->kind == kind) { state->cancel(); stop.owners.push_back(state); }
        sensor_stops_.push_back(std::move(stop)); return true;
      }
      if (api != "wx.start" + name) continue;
      if (!foreground_) throw std::invalid_argument("sensor access requires a foreground application");
      auto interval = sensor_interval(kind, payload);
      reap_sensors();
      for (const auto& state : sensors_) if (state->kind == kind && state->stopping() && !state->finished)
        throw std::invalid_argument("earlier native sensor cleanup is still pending");
      for (const auto& state : sensors_) if (state->kind == kind && !state->stopping() && !state->finished) {
        if (!state->started) throw std::invalid_argument("native sensor startup is already pending");
        if (state->interval != interval) throw std::invalid_argument("stop the active sensor before changing its interval");
        // Duplicate starts share the physical source but have their own
        // receipt lease, so stopping before delivery also revokes this result.
        auto duplicate = std::make_shared<NativeSensor>(weak_queue(), kind, interval, call);
        duplicate->delivery = state->delivery; duplicate->started = true; duplicate->finished = true;
        duplicates_.push_back(duplicate); return true;
      }
      if (sensors_.size() >= 8) throw std::invalid_argument("native sensor owner limit exceeded");
      auto state = std::make_shared<NativeSensor>(weak_queue(), kind, interval, call); auto factory = sensor_factory_;
      workers_.reserve(workers_.size() + 1); sensors_.reserve(sensors_.size() + 1);
      workers_.push_back(std::async(std::launch::async, [state, factory] {
        std::unique_ptr<SensorSource> source;
        try { source = factory(state->kind); if (!source) throw std::runtime_error("native sensor source unavailable"); source->run(state); }
        catch (const std::system_error& error) { state->fault(error.what(), "invalid_request", error.code().value()); }
        catch (const std::exception& error) { state->fault(error.what()); }
        catch (...) { state->fault("native sensor source failed"); }
        *state->delivery = false;
        if (source) while (true) {
          try { if (source->cleanup(state)) break; }
          catch (const std::exception& error) { state->cleanup_failed(error.what()); }
          catch (...) { state->cleanup_failed("native sensor cleanup failed"); }
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        source.reset(); state->finished = true; state->changed.notify_all();
      }));
      sensors_.push_back(state); calls_.at(call).cancel = [state] { state->cancel(); }; return true;
    }
    return false;
  }
  std::vector<std::shared_ptr<NativeSensor>> duplicates_;
  void permissions_reset() override { cancel_sensors(); DeviceBluetooth::permissions_reset(); }
  void retire_platform() override { cancel_sensors(); DeviceBluetooth::retire_platform(); }
  Status foreground_changed(bool visible) override { if (!visible) cancel_sensors(); return DeviceBluetooth::foreground_changed(visible); }
 public:
  ~DeviceSensors() override { retire(); }
  Status pump() override {
    auto status = DeviceBluetooth::pump(); if (!status || queue_->closed) return status;
    for (auto& state : sensors_) { if (!calls_.count(state->call)) state->settled = true; else state->settle(); }
    for (auto& state : duplicates_) { if (!calls_.count(state->call)) state->settled = true; else state->settle(); }
    duplicates_.erase(std::remove_if(duplicates_.begin(), duplicates_.end(), [](const auto& state) { return state->settled; }), duplicates_.end());
    for (auto item = sensor_stops_.begin(); item != sensor_stops_.end();) {
      if (!calls_.count(item->call)) { item = sensor_stops_.erase(item); continue; }
      if (std::any_of(item->owners.begin(), item->owners.end(), [](const auto& state) { return !state->finished; })) { ++item; continue; }
      if (!queue_->push({DeviceQueued::Complete, item->call, "", "", device_ok(item->api)})) { ++item; continue; }
      item = sensor_stops_.erase(item);
    }
    reap_sensors(); return status;
  }
};
} // namespace weapp::desktop::detail
