#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <roapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Devices.Geolocation.h>
#include "device_location_common.hpp"

namespace weapp::desktop::detail {
using WinGeolocator = winrt::Windows::Devices::Geolocation::Geolocator;
struct WinLocationApartment {
  explicit WinLocationApartment(RO_INIT_TYPE type) { winrt::check_hresult(RoInitialize(type)); }
  ~WinLocationApartment() { RoUninitialize(); }
};
struct WinLocationCompletion {
  std::mutex mutex;
  std::condition_variable changed;
  bool terminal = false;
  void complete() { std::lock_guard<std::mutex> guard(mutex); terminal = true; changed.notify_all(); }
  bool completed() { std::lock_guard<std::mutex> guard(mutex); return terminal; }
  void wait() { std::unique_lock<std::mutex> guard(mutex); changed.wait_for(guard, std::chrono::milliseconds(25), [&] { return terminal; }); }
};
template<class Operation>
inline auto win_location_await(Operation operation, NativeLocation& state, long timeout_ms,
    const std::shared_ptr<WinLocationCompletion>& completion)
    -> std::optional<decltype(operation.GetResults())> {
  using winrt::Windows::Foundation::AsyncStatus;
  std::optional<decltype(operation.GetResults())> result;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  bool cancelling = false, timed_out = false;
  std::exception_ptr failure;
  // Completed is authoritative even if a later Status query fails. If handler
  // registration fails, Status remains a retryable fallback; retain the same
  // operation until either mechanism establishes its terminal state.
  try { operation.Completed([completion](const auto&, AsyncStatus status) { if (status != AsyncStatus::Started) completion->complete(); }); }
  catch (const winrt::hresult_error&) { state.cleanup_failed("Windows location completion handler unavailable; waiting for terminal status"); }
  while (!completion->completed()) {
    try { if (operation.Status() != AsyncStatus::Started) { completion->complete(); break; } }
    catch (const winrt::hresult_error&) { state.cleanup_failed("Windows location could not yet confirm terminal async status"); }
    if (state.cancelled || std::chrono::steady_clock::now() >= deadline) {
      if (!cancelling) { timed_out = !state.cancelled; cancelling = true; }
      try { operation.Cancel(); }
      catch (const winrt::hresult_error&) { state.cleanup_failed("Windows location cancellation is not yet acknowledged"); }
    }
    completion->wait();
  }
  try { if (!state.cancelled && !timed_out) result = operation.GetResults(); }
  catch (...) { failure = std::current_exception(); }
  // IAsyncInfo::Close is legal only after completion. A failed Close retains
  // this operation and apartment on the worker and retries instead of dropping
  // the final reference. Cancellation alone never reaches this boundary.
  while (true) {
    try { operation.Close(); break; }
    catch (const winrt::hresult_error& error) {
      if (error.code() == RO_E_CLOSED) break;
      state.cleanup_failed("Windows location async close is pending: " + winrt::to_string(error.message()));
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  state.cleanup_succeeded();
  if (failure) std::rethrow_exception(failure);
  if (timed_out) throw std::runtime_error("Windows location operation timed out");
  return result;
}
class WinLocationSource final : public LocationSource {
  // Destroy apartment initialization last, after all WinRT references.
  std::unique_ptr<WinLocationApartment> apartment_;
  std::shared_ptr<WinLocationCompletion> access_completion_ = std::make_shared<WinLocationCompletion>();
  winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Devices::Geolocation::GeolocationAccessStatus> access_{nullptr};
 public:
  WinLocationSource() : access_(WinGeolocator::RequestAccessAsync()) {}
  void run(const std::shared_ptr<NativeLocation>& state) override {
    using namespace winrt::Windows::Devices::Geolocation;
    try {
      // The worker is a new thread. Keep retrying apartment setup while the
      // UI-created permission operation remains alive; never discard it on an
      // initialization error before observing and closing its terminal state.
      while (!apartment_) {
        try { apartment_ = std::make_unique<WinLocationApartment>(RO_INIT_MULTITHREADED); }
        catch (const winrt::hresult_error&) { state->cleanup_failed("Windows location worker apartment initialization is pending"); std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
      }
      auto access = win_location_await(access_, *state, state->options.timeout_ms, access_completion_);
      if (!access) return;
      if (*access != GeolocationAccessStatus::Allowed) throw std::runtime_error("OS location permission denied");
      Geolocator locator;
      locator.DesiredAccuracy(state->options.high_accuracy ? PositionAccuracy::High : PositionAccuracy::Default);
      if (state->options.fuzzy) locator.DesiredAccuracyInMeters(winrt::box_value(uint32_t(1500)).as<winrt::Windows::Foundation::IReference<uint32_t>>());
      locator.ReportInterval(1000);
      while (!state->cancelled) {
        auto completion = std::make_shared<WinLocationCompletion>();
        auto operation = locator.GetGeopositionAsync(std::chrono::seconds(0), std::chrono::milliseconds(state->options.timeout_ms));
        auto position = win_location_await(operation, *state, state->options.timeout_ms, completion);
        if (!position) break;
        state->announce_started();
        auto coordinate = position->Coordinate();
        Json value = {{"latitude", coordinate.Latitude()}, {"longitude", coordinate.Longitude()},
            {"accuracy", coordinate.Accuracy()}, {"horizontalAccuracy", coordinate.Accuracy()}};
        if (auto altitude = coordinate.Altitude()) value["altitude"] = altitude.Value();
        if (auto accuracy = coordinate.AltitudeAccuracy()) value["verticalAccuracy"] = accuracy.Value();
        if (auto speed = coordinate.Speed(); speed && speed.Value() >= 0) value["speed"] = speed.Value();
        state->position(std::move(value));
        if (!state->options.continuous || !state->wait_for(std::chrono::milliseconds(1000))) break;
      }
    } catch (const winrt::hresult_error& error) { throw std::runtime_error("Windows location: " + winrt::to_string(error.message())); }
  }
};
inline LocationFactory win_location_factory() {
  try {
    auto apartment = std::make_shared<WinLocationApartment>(RO_INIT_SINGLETHREADED);
    if (!winrt::try_get_activation_factory<WinGeolocator>()) return {};
    return [apartment](const DeviceApiOptions& options, const LocationOptions&) -> std::unique_ptr<LocationSource> {
      auto parent = static_cast<HWND>(options.native_parent);
      if (!parent || !IsWindow(parent) || GetWindowThreadProcessId(parent, nullptr) != GetCurrentThreadId() ||
          !IsWindowVisible(parent) || GetAncestor(parent, GA_ROOT) != GetAncestor(GetForegroundWindow(), GA_ROOT))
        throw std::invalid_argument("OS location permission requires the host's foreground UI thread and attached window");
      try { return std::make_unique<WinLocationSource>(); }
      catch (const winrt::hresult_error& error) { throw std::runtime_error("Windows location permission: " + winrt::to_string(error.message())); }
    };
  } catch (const winrt::hresult_error&) { return {}; }
}
}  // namespace weapp::desktop::detail
