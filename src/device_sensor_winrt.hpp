#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <roapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Devices.Sensors.h>
#include <winrt/Windows.Graphics.Display.h>
#include "device_sensor_common.hpp"

namespace weapp::desktop::detail {
namespace win_sensor {
namespace sensors = winrt::Windows::Devices::Sensors;
using winrt::Windows::Graphics::Display::DisplayOrientations;
// WinRT's sensor configuration belongs to the app's physical source. Keep a
// process lease until checked native removal/restoration succeeds; a retiring
// SDK owner cannot reset a newly created owner's requested interval.
struct SourceLease {
  inline static std::mutex mutex;
  inline static std::set<SensorKind> active;
  SensorKind kind;
  explicit SourceLease(SensorKind value) : kind(value) {
    std::lock_guard<std::mutex> guard(mutex);
    if (!active.insert(kind).second) throw std::runtime_error("Windows sensor is already owned by another SDK session");
  }
  ~SourceLease() { std::lock_guard<std::mutex> guard(mutex); active.erase(kind); }
};
struct Apartment {
  Apartment() { winrt::check_hresult(RoInitialize(RO_INIT_MULTITHREADED)); }
  ~Apartment() { RoUninitialize(); }
};
template<class Action> void release_step(Action action) {
  try { action(); }
  catch (const winrt::hresult_error& error) { if (error.code() != RO_E_CLOSED) throw; }
}
template<class Sensor, class Interface> struct Registration {
  Sensor sensor{nullptr};
  winrt::event_token token{};
  bool registered = false, interval_dirty = false, transform_dirty = false;
  uint32_t original_interval = 0;
  DisplayOrientations original = DisplayOrientations::None;
  template<class Project> void open(const std::shared_ptr<NativeSensor>& state, Project project) {
    if (state->stopping()) return;
    sensor = Sensor::GetDefault();
    if (!sensor) throw std::runtime_error("Windows has no integrated " + sensor_name(state->kind) + " sensor");
    if (state->stopping()) return;
    original = sensor.ReadingTransform(); transform_dirty = true;
    // Keep the public axes fixed in portrait, independent of a tablet's
    // natural orientation and later host-window rotation.
    sensor.ReadingTransform(DisplayOrientations::Portrait);
    original_interval = sensor.ReportInterval(); interval_dirty = true;
    sensor.ReportInterval(std::max(state->interval, sensor.MinimumReportInterval()));
    auto weak = std::weak_ptr<NativeSensor>(state);
    token = sensor.ReadingChanged([weak, project](const auto&, const auto& arguments) {
      if (auto owner = weak.lock(); owner && !owner->stopping()) {
        try { owner->sample(project(arguments.Reading())); }
        catch (const winrt::hresult_error& error) { owner->fault("Windows sensor reading: " + winrt::to_string(error.message()), error.code() == E_ACCESSDENIED ? "permission_denied" : "invalid_request", int32_t(error.code())); }
        catch (const std::exception& error) { owner->fault(error.what()); }
        catch (...) { owner->fault("Windows sensor reading failed"); }
      }
    });
    registered = true;
    state->announce_started(); state->wait();
  }
  void cleanup() {
    if (!sensor) return;
    if (registered) {
      // The projected ReadingChanged(token) is noexcept and discards HRESULT.
      // Call the same SDK ABI directly and consume only acknowledged steps.
      release_step([&] {
        auto projected = sensor.template as<Interface>();
        auto abi = static_cast<winrt::impl::abi_t<Interface>*>(winrt::get_abi(projected));
        winrt::check_hresult(abi->remove_ReadingChanged(token));
      });
      registered = false;
    }
    if (interval_dirty) { release_step([&] { sensor.ReportInterval(original_interval); }); interval_dirty = false; }
    if (transform_dirty) { release_step([&] { sensor.ReadingTransform(original); }); transform_dirty = false; }
    sensor = nullptr;
  }
};
class Source final : public SensorSource {
  // Last destroyed: every registration and WinRT reference lives on this MTA.
  std::unique_ptr<Apartment> apartment_;
  std::unique_ptr<SourceLease> lease_;
  Registration<sensors::Accelerometer, sensors::IAccelerometer> accelerometer_;
  Registration<sensors::Compass, sensors::ICompass> compass_;
  Registration<sensors::Gyrometer, sensors::IGyrometer> gyrometer_;
 public:
  void run(const std::shared_ptr<NativeSensor>& state) override {
    try {
      if (state->stopping()) return;
      apartment_ = std::make_unique<Apartment>();
      lease_ = std::make_unique<SourceLease>(state->kind);
      if (state->kind == SensorKind::Accelerometer)
        accelerometer_.open(state, [](const auto& reading) { return sensor_vector(reading.AccelerationX(), reading.AccelerationY(), reading.AccelerationZ()); });
      else if (state->kind == SensorKind::Gyroscope)
        gyrometer_.open(state, [](const auto& reading) { return sensor_vector(reading.AngularVelocityX(), reading.AngularVelocityY(), reading.AngularVelocityZ(), true); });
      else compass_.open(state, [](const auto& reading) {
        std::string accuracy;
        switch (reading.HeadingAccuracy()) {
          case sensors::MagnetometerAccuracy::High: accuracy = "high"; break;
          case sensors::MagnetometerAccuracy::Approximate: accuracy = "medium"; break;
          case sensors::MagnetometerAccuracy::Unreliable: accuracy = "unreliable"; break;
          default: break;
        }
        return sensor_heading(reading.HeadingMagneticNorth(), accuracy);
      });
    } catch (const winrt::hresult_error& error) {
      state->fault("Windows sensor: " + winrt::to_string(error.message()), error.code() == E_ACCESSDENIED ? "permission_denied" : "invalid_request", int32_t(error.code()));
    }
  }
  bool cleanup(const std::shared_ptr<NativeSensor>& state) override {
    try { accelerometer_.cleanup(); compass_.cleanup(); gyrometer_.cleanup(); return true; }
    catch (const winrt::hresult_error& error) { state->cleanup_failed("Windows sensor cleanup pending: " + winrt::to_string(error.message())); return false; }
  }
};
} // namespace win_sensor
inline SensorFactory win_sensor_factory() {
  // Hardware is queried on each start; absent hardware is an operation failure,
  // not a reason to remove Windows' implemented API from the host catalog.
  // These three WinRT classes have no RequestAccessAsync contract. Actual OS
  // denial is preserved; no fictitious wx authorization scope is introduced.
  return [](SensorKind) { return std::make_unique<win_sensor::Source>(); };
}
} // namespace weapp::desktop::detail
