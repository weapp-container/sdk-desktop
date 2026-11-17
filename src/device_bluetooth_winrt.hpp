#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <roapi.h>
#include <robuffer.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Storage.Streams.h>
#include "device_bluetooth_common.hpp"
#include <cstdio>

namespace weapp::desktop::detail {
namespace win_ble {
namespace bt = winrt::Windows::Devices::Bluetooth;
namespace adv = winrt::Windows::Devices::Bluetooth::Advertisement;
namespace gatt = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
namespace radio = winrt::Windows::Devices::Radios;
namespace streams = winrt::Windows::Storage::Streams;
using winrt::Windows::Foundation::AsyncStatus;
struct Apartment {
  explicit Apartment(RO_INIT_TYPE type) { winrt::check_hresult(RoInitialize(type)); }
  ~Apartment() { RoUninitialize(); }
};
struct Completion {
  std::mutex mutex; std::condition_variable changed; bool final = false;
  void complete() { std::lock_guard<std::mutex> guard(mutex); final = true; changed.notify_all(); }
  bool done() { std::lock_guard<std::mutex> guard(mutex); return final; }
  void wait() { std::unique_lock<std::mutex> guard(mutex); changed.wait_for(guard, std::chrono::milliseconds(25), [&] { return final; }); }
};
inline bool retired(const std::shared_ptr<NativeBle>& state, const std::shared_ptr<BleCommand>& command) {
  return state->stopping() || command && command->cancelled();
}
inline std::system_error error(HRESULT code, const std::string& message) {
  return std::system_error(int(code), std::system_category(), message);
}
template<class Result> struct Awaited {
  std::optional<Result> value; std::exception_ptr failure; bool cancelled = false, timed_out = false;
  void require() const {
    if (timed_out) throw error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), "Windows Bluetooth operation timed out");
    if (cancelled) throw error(HRESULT_FROM_WIN32(ERROR_CANCELLED), "Windows Bluetooth operation was retired");
    if (failure) std::rethrow_exception(failure);
    if (!value) throw std::runtime_error("Windows Bluetooth operation returned no result");
  }
};
// Keep the exact operation through native completion and IAsyncInfo::Close.
// A canceled successful acquisition still returns its value so the caller can
// Close that late device/session/service before acknowledging retirement.
template<class Operation> auto await(Operation operation, const std::shared_ptr<NativeBle>& state,
    const std::shared_ptr<BleCommand>& command, long timeout,
    const std::shared_ptr<Completion>& completion = std::make_shared<Completion>())
    -> Awaited<decltype(operation.GetResults())> {
  Awaited<decltype(operation.GetResults())> result;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
  try { operation.Completed([completion](const auto&, AsyncStatus status) { if (status != AsyncStatus::Started) completion->complete(); }); }
  catch (const winrt::hresult_error&) {} // Status remains an independent terminal proof.
  while (!completion->done()) {
    try { if (operation.Status() != AsyncStatus::Started) { completion->complete(); break; } }
    catch (const winrt::hresult_error&) {} // Retain uncertain operations; never infer completion.
    if (retired(state, command) || std::chrono::steady_clock::now() >= deadline) {
      if (!retired(state, command)) result.timed_out = true;
      try { operation.Cancel(); } catch (const winrt::hresult_error&) {}
    }
    completion->wait();
  }
  try { result.value.emplace(operation.GetResults()); } catch (...) { result.failure = std::current_exception(); }
  while (true) {
    try { operation.Close(); break; }
    catch (const winrt::hresult_error& failure) {
      if (failure.code() == RO_E_CLOSED) break;
      state->cleanup_failed("Windows Bluetooth async close pending: " + winrt::to_string(failure.message()));
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  result.cancelled = retired(state, command); return result;
}
// C++/WinRT event-removal projections are noexcept and discard the native
// HRESULT. Inspect that same generated interface ABI so a failed unregister
// retains its token for cleanup retry instead of reporting release success.
template<class Interface,class Object,class Remove> void checked_remove(const Object& object,Remove remove) {
  auto projected = object.template as<Interface>();
  auto abi = static_cast<winrt::impl::abi_t<Interface>*>(winrt::get_abi(projected));
  winrt::check_hresult(remove(abi));
}
template<class Remove> void revoke(bool& registered, Remove remove) {
  if (!registered) return;
  try {remove();}catch (const winrt::hresult_error& failure) {if (failure.code() != RO_E_CLOSED) throw;}
  registered = false;
}
template<class Closable> void close(Closable& object) {
  if (!object) return;
  try { object.Close(); } catch (const winrt::hresult_error& failure) { if (failure.code() != RO_E_CLOSED) throw; }
  object = nullptr;
}
inline std::string uuid(winrt::guid value) {
  auto text = winrt::to_string(winrt::to_hstring(value));
  if (text.size() == 38 && text.front() == '{' && text.back() == '}') text = text.substr(1, 36);
  return ble_uuid(std::move(text));
}
inline std::string device_id(uint64_t address) {
  char text[18]; std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
      unsigned((address >> 40) & 255), unsigned((address >> 32) & 255), unsigned((address >> 24) & 255),
      unsigned((address >> 16) & 255), unsigned((address >> 8) & 255), unsigned(address & 255)); return text;
}
inline uint64_t address(const std::string& id) {
  if (id.size() != 17) throw std::invalid_argument("invalid discovered Windows Bluetooth address");
  std::string digits;
  for (size_t index = 0; index < id.size(); ++index) {
    if (index % 3 == 2) { if (id[index] != ':') throw std::invalid_argument("invalid Bluetooth address separator"); }
    else { if (!std::isxdigit(static_cast<unsigned char>(id[index]))) throw std::invalid_argument("invalid Bluetooth address byte"); digits += id[index]; }
  }
  return std::stoull(digits, nullptr, 16);
}
inline std::vector<uint8_t> bytes(const streams::IBuffer& buffer) {
  if (!buffer || buffer.Length() > 1048576) throw std::invalid_argument("Windows Bluetooth buffer exceeds native bound");
  uint8_t* data = nullptr; winrt::check_hresult(buffer.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&data));
  if (!buffer.Length()) return {}; return {data, data + buffer.Length()};
}
inline streams::IBuffer buffer(const std::vector<uint8_t>& bytes) {
  streams::Buffer result(uint32_t(bytes.size())); result.Length(uint32_t(bytes.size()));
  uint8_t* output = nullptr; winrt::check_hresult(result.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&output));
  if (!bytes.empty()) std::copy(bytes.begin(), bytes.end(), output); return result;
}
inline void status(gatt::GattCommunicationStatus value, const winrt::Windows::Foundation::IReference<uint8_t>& protocol = nullptr) {
  if (value == gatt::GattCommunicationStatus::Success) return;
  if (value == gatt::GattCommunicationStatus::ProtocolError && protocol) throw std::system_error(int(protocol.Value()), std::system_category(), "Windows Bluetooth ATT protocol error");
  throw error(value == gatt::GattCommunicationStatus::AccessDenied ? E_ACCESSDENIED : HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED), "Windows Bluetooth GATT communication failed (status " + std::to_string(int(value)) + ")");
}
struct Reservations { std::mutex mutex; std::set<std::string> devices; };
struct Signal {
  std::mutex mutex; bool active = true, services_changed = false, announced = false;
  std::shared_ptr<std::atomic<bool>> delivery = std::make_shared<std::atomic<bool>>(true);
  std::vector<std::weak_ptr<std::atomic<bool>>> notification_deliveries;
  // Caller holds mutex. Invalidate already queued ingress as well as callbacks.
  void invalidate() {
    *delivery = false;
    for (auto& weak : notification_deliveries) if (auto token = weak.lock()) *token = false;
  }
};
struct Notification {
  gatt::GattCharacteristic characteristic{nullptr}; winrt::event_token token{}; bool registered = false, enabled = false;
  std::shared_ptr<Signal> signal = std::make_shared<Signal>();
  std::shared_ptr<std::atomic<bool>> delivery;
};
struct Connection {
  bt::BluetoothLEDevice device{nullptr}; gatt::GattSession session{nullptr};
  std::map<std::string,gatt::GattDeviceService> services;
  // Retain the entire returned collection before indexing by UUID: duplicate
  // UUIDs and exceptions must not abandon services not yet visited by the loop.
  gatt::GattDeviceServicesResult discovery{nullptr}; uint32_t services_closed = 0;
  std::map<std::string,gatt::GattCharacteristic> characteristics;
  std::map<std::string,Notification> notifications;
  std::shared_ptr<Signal> signal = std::make_shared<Signal>();
  winrt::event_token connection_token{}, mtu_token{}, services_token{};
  bool connection_registered = false, mtu_registered = false, services_registered = false, announced = false, maintaining = false;
};
struct Preparation {
  std::shared_ptr<Reservations> reservations; std::string device;
  winrt::Windows::Foundation::IAsyncOperation<bt::BluetoothLEDevice> operation{nullptr};
  std::shared_ptr<Completion> completion = std::make_shared<Completion>();
  // Allocate ownership before issuing a UI-thread operation.
  std::unique_ptr<Connection> connection = std::make_unique<Connection>();
};
inline void connection_event(const std::shared_ptr<NativeBle>& state, const std::string& id,
    bool connected, const std::shared_ptr<std::atomic<bool>>& delivery = {}) {
  {std::lock_guard<std::mutex> guard(state->mutex); if (connected) state->connected_ids.insert(id); else state->connected_ids.erase(id);}
  state->event("BLEConnectionStateChange",{{"deviceId",id},{"connected",connected}},false,delivery);
}
struct ScanSignal {
  std::mutex mutex; bool expected = false; std::set<std::string> seen;
  std::shared_ptr<Completion> terminal = std::make_shared<Completion>();
};
}  // namespace win_ble

class WinBluetoothBackend final : public BleBackend {
  // Apartment is declared first and destroyed after all native references.
  std::unique_ptr<win_ble::Apartment> apartment_;
  win_ble::bt::BluetoothAdapter adapter_{nullptr}; win_ble::radio::Radio radio_{nullptr};
  winrt::event_token radio_token_{}; bool radio_registered_ = false;
  win_ble::adv::BluetoothLEAdvertisementWatcher watcher_{nullptr};
  winrt::event_token received_token_{}, stopped_token_{}; bool received_registered_ = false, stopped_registered_ = false;
  std::shared_ptr<win_ble::ScanSignal> scan_;
  std::map<std::string,std::unique_ptr<win_ble::Connection>> connections_;
  std::shared_ptr<win_ble::Reservations> reservations_;
  void apartment() {
    if (!apartment_) apartment_ = std::make_unique<win_ble::Apartment>(RO_INIT_MULTITHREADED);
  }
  void stop_scan(const std::shared_ptr<NativeBle>& state) {
    try {
    state->cancel_discovery(); if (!watcher_) return;
    {std::lock_guard<std::mutex> guard(scan_->mutex); scan_->expected = true;}
    using S = win_ble::adv::BluetoothLEAdvertisementWatcherStatus;
    auto status = watcher_.Status();
    if (status == S::Started || status == S::Stopping) watcher_.Stop();
    while (status != S::Created && status != S::Stopped && status != S::Aborted && !scan_->terminal->done()) {
      scan_->terminal->wait(); status = watcher_.Status();
    }
    win_ble::revoke(received_registered_,[&] {win_ble::checked_remove<win_ble::adv::IBluetoothLEAdvertisementWatcher>(watcher_,[&](auto* abi) {return abi->remove_Received(received_token_);});});
    win_ble::revoke(stopped_registered_,[&] {win_ble::checked_remove<win_ble::adv::IBluetoothLEAdvertisementWatcher>(watcher_,[&](auto* abi) {return abi->remove_Stopped(stopped_token_);});});
    watcher_ = nullptr; scan_.reset();
    } catch (...) {state->fault("Windows Bluetooth scanner cleanup remains pending");throw;}
  }
  void close_connection(const std::shared_ptr<NativeBle>& state, const std::string& id) {
    auto found = connections_.find(id); if (found == connections_.end()) return; auto& connection = *found->second;
    try {
    {std::lock_guard<std::mutex> guard(connection.signal->mutex); connection.signal->active = false; connection.signal->invalidate();}
    std::exception_ptr failure;
    auto release = [&](auto action) {try {action();}catch (...) {if (!failure) failure = std::current_exception();}};
    for (auto entry = connection.notifications.begin(); entry != connection.notifications.end();) {
      auto& notification = entry->second;
      {std::lock_guard<std::mutex> guard(notification.signal->mutex); notification.signal->active = false; if (notification.delivery) *notification.delivery = false;}
      release([&] {win_ble::revoke(notification.registered,[&] {win_ble::checked_remove<win_ble::gatt::IGattCharacteristic>(notification.characteristic,[&](auto* abi) {return abi->remove_ValueChanged(notification.token);});});});
      if (notification.registered) ++entry;else entry = connection.notifications.erase(entry);
    }
    release([&] {if (connection.device) win_ble::revoke(connection.connection_registered,[&] {win_ble::checked_remove<win_ble::bt::IBluetoothLEDevice>(connection.device,[&](auto* abi) {return abi->remove_ConnectionStatusChanged(connection.connection_token);});});});
    release([&] {if (connection.device) win_ble::revoke(connection.services_registered,[&] {win_ble::checked_remove<win_ble::bt::IBluetoothLEDevice>(connection.device,[&](auto* abi) {return abi->remove_GattServicesChanged(connection.services_token);});});});
    release([&] {if (connection.session) win_ble::revoke(connection.mtu_registered,[&] {win_ble::checked_remove<win_ble::gatt::IGattSession>(connection.session,[&](auto* abi) {return abi->remove_MaxPduSizeChanged(connection.mtu_token);});});});
    if (connection.session && connection.maintaining) release([&] {
      try { connection.session.MaintainConnection(false); } catch (const winrt::hresult_error& error) {if (error.code() != RO_E_CLOSED) throw;}
      connection.maintaining = false;
    });
    connection.characteristics.clear();
    release([&] {
      if (connection.discovery) {
        auto services = connection.discovery.Services();
        while (connection.services_closed < services.Size()) {
          auto service = services.GetAt(connection.services_closed);win_ble::close(service);++connection.services_closed;
        }
        connection.discovery = nullptr;connection.services.clear();
      }
    });
    release([&] {win_ble::close(connection.session);if (!connection.session) {connection.maintaining = false;connection.mtu_registered = false;}});
    release([&] {win_ble::close(connection.device);if (!connection.device) {connection.connection_registered = false;connection.services_registered = false;}});
    if (failure) std::rethrow_exception(failure);
    if (connection.announced) {connection.announced = false; state->connected(id,false);}
    connections_.erase(found);
    {std::lock_guard<std::mutex> guard(reservations_->mutex);reservations_->devices.erase(id);}
    } catch (...) {state->fault("Windows Bluetooth connection cleanup remains pending");throw;}
  }
  win_ble::Connection& connection(const std::shared_ptr<NativeBle>& state, const std::string& id) {
    auto found = connections_.find(id); if (found == connections_.end()) throw std::invalid_argument("Bluetooth device has no owned GATT connection");
    bool changed; {std::lock_guard<std::mutex> guard(found->second->signal->mutex); changed = found->second->signal->services_changed;}
    if (changed) {close_connection(state,id);throw std::invalid_argument("Bluetooth connection retired or service database changed; reconnect the device");}
    return *found->second;
  }
  void characteristics(const std::shared_ptr<NativeBle>& state, const std::shared_ptr<BleCommand>& command, win_ble::Connection& connection, const std::string& service) {
    auto found = connection.services.find(service); if (found == connection.services.end()) throw std::invalid_argument("Bluetooth service is not part of this connection");
    auto result = win_ble::await(found->second.GetCharacteristicsAsync(win_ble::bt::BluetoothCacheMode::Uncached),state,command,command->timeout_ms);
    if (result.value) {
      win_ble::status(result.value->Status(),result.value->ProtocolError());
      std::set<std::string> seen;
      for (auto characteristic : result.value->Characteristics()) {
        auto key = service + "/" + win_ble::uuid(characteristic.Uuid());
        if (!seen.insert(key).second) {
          {std::lock_guard<std::mutex> guard(connection.signal->mutex);connection.signal->services_changed = true;connection.signal->invalidate();}
          throw std::invalid_argument("device exposes ambiguous duplicate characteristic UUIDs");
        }
        if (!connection.characteristics.count(key) && connection.characteristics.size() >= 256) throw std::invalid_argument("Windows Bluetooth characteristic limit exceeded");
        connection.characteristics.insert_or_assign(key, characteristic);
      }
    }
    result.require();
  }
  win_ble::gatt::GattCharacteristic characteristic(const std::shared_ptr<NativeBle>& state, const std::shared_ptr<BleCommand>& command, win_ble::Connection& connection) {
    auto service = command->payload.at("serviceId").get<std::string>(), id = command->payload.at("characteristicId").get<std::string>(), key = service + "/" + id;
    if (!connection.characteristics.count(key)) characteristics(state,command,connection,service);
    auto found = connection.characteristics.find(key); if (found == connection.characteristics.end()) throw std::invalid_argument("Bluetooth characteristic was not discovered in this service"); return found->second;
  }
  static bool property(win_ble::gatt::GattCharacteristicProperties value, win_ble::gatt::GattCharacteristicProperties flag) {return (uint32_t(value) & uint32_t(flag)) != 0;}
 public:
  explicit WinBluetoothBackend(std::shared_ptr<win_ble::Reservations> reservations) : reservations_(std::move(reservations)) {}
  void open(const std::shared_ptr<NativeBle>& state) override {
    try {
    apartment();
    auto adapter = win_ble::await(win_ble::bt::BluetoothAdapter::GetDefaultAsync(),state,nullptr,10000);adapter.require();adapter_ = *adapter.value;
    if (!adapter_ || !adapter_.IsLowEnergySupported()) throw win_ble::error(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),"Windows has no available Bluetooth LE adapter");
    auto radio = win_ble::await(adapter_.GetRadioAsync(),state,nullptr,10000);radio.require();radio_ = *radio.value;
    if (!radio_ || radio_.State() != win_ble::radio::RadioState::On) throw win_ble::error(HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_AVAILABLE),"Windows Bluetooth radio is unavailable or disabled");
    std::weak_ptr<NativeBle> weak = state;
    radio_token_ = radio_.StateChanged([weak](const auto& sender,const auto&) {
      if (auto owner = weak.lock(); owner && !owner->stopping()) {
        try {bool available = sender.State() == win_ble::radio::RadioState::On; owner->event("BluetoothAdapterStateChange",{{"available",available},{"discovering",available && owner->discovering.load()}},!available);if (!available) owner->fault("OS disabled the Windows Bluetooth radio");}
        catch (const winrt::hresult_error& failure) {owner->fault("Windows Bluetooth radio status: " + winrt::to_string(failure.message()));}
        catch (...) {owner->fault("Windows Bluetooth radio status callback failed");}
      }
    });radio_registered_ = true;
    state->event("BluetoothAdapterStateChange",{{"available",true},{"discovering",false}});
    }catch (const winrt::hresult_error& failure) {throw win_ble::error(failure.code(),"Windows Bluetooth: " + winrt::to_string(failure.message()));}
  }
  Json execute(const std::shared_ptr<NativeBle>& state, const std::shared_ptr<BleCommand>& command, const std::shared_ptr<void>& preparation) override;
  void poll(const std::shared_ptr<NativeBle>& state) override {
    std::vector<std::string> changed;
    for (const auto& [id,connection] : connections_) {std::lock_guard<std::mutex> guard(connection->signal->mutex);if (connection->signal->services_changed) changed.push_back(id);}
    for (const auto& id : changed) close_connection(state,id);
  }
  bool cleanup(const std::shared_ptr<NativeBle>& state) override {
    std::string failure;
    try {stop_scan(state);}catch (const winrt::hresult_error& error) {failure = winrt::to_string(error.message());}catch (const std::exception& error) {failure = error.what();}catch (...) {failure = "Windows Bluetooth scanner release failed";}
    std::vector<std::string> ids;for (const auto& [id,_] : connections_) ids.push_back(id);
    for (const auto& id : ids) try {close_connection(state,id);}catch (const winrt::hresult_error& error) {failure = winrt::to_string(error.message());}catch (const std::exception& error) {failure = error.what();}catch (...) {failure = "Windows Bluetooth connection release failed";}
    try {if (radio_) win_ble::revoke(radio_registered_,[&] {win_ble::checked_remove<win_ble::radio::IRadio>(radio_,[&](auto* abi) {return abi->remove_StateChanged(radio_token_);});});}
    catch (const winrt::hresult_error& error) {failure = winrt::to_string(error.message());}
    if (!failure.empty()) {state->cleanup_failed(failure);return false;}
    radio_ = nullptr;adapter_ = nullptr;state->cleanup_succeeded();return true;
  }
};

namespace win_ble {
inline std::string service_data_uuid(const std::vector<uint8_t>& bytes, size_t length) {
  static const char* digits = "0123456789ABCDEF"; std::string text;
  for (size_t index = length; index > 0; --index) {text += digits[bytes[index-1] >> 4];text += digits[bytes[index-1] & 15];}
  if (length == 16) {text.insert(8,"-");text.insert(13,"-");text.insert(18,"-");text.insert(23,"-");}
  return ble_uuid(std::move(text));
}
inline Json advertisement(const adv::BluetoothLEAdvertisementReceivedEventArgs& args) {
  auto advertisement = args.Advertisement(); Json services = Json::array(), service_data = Json::object(), manufacturers = Json::object();
  for (auto value : advertisement.ServiceUuids()) {if (services.size() >= 64) break;services.push_back(uuid(value));}
  for (auto section : advertisement.DataSections()) {
    size_t prefix = section.DataType() == 0x16 ? 2 : section.DataType() == 0x20 ? 4 : section.DataType() == 0x21 ? 16 : 0;
    if (!prefix) continue;auto raw = bytes(section.Data());if (raw.size() < prefix || raw.size() > 1650) continue;
    service_data[service_data_uuid(raw,prefix)] = inline_binary(std::vector<uint8_t>(raw.begin()+prefix,raw.end()));
  }
  std::vector<uint8_t> manufacturer_data;
  for (auto section : advertisement.ManufacturerData()) {
    auto raw = bytes(section.Data());if (raw.size() > 1650 || manufacturers.size() >= 16) continue;
    char company[5];std::snprintf(company,sizeof(company),"%04X",unsigned(section.CompanyId()));manufacturers[company] = inline_binary(raw);
    if (manufacturer_data.empty()) {manufacturer_data.push_back(uint8_t(section.CompanyId()));manufacturer_data.push_back(uint8_t(section.CompanyId() >> 8));manufacturer_data.insert(manufacturer_data.end(),raw.begin(),raw.end());}
  }
  auto type = args.BluetoothAddressType();
  Json result = {{"deviceId",device_id(args.BluetoothAddress())},{"name",winrt::to_string(advertisement.LocalName())},
      {"localName",winrt::to_string(advertisement.LocalName())},{"RSSI",args.RawSignalStrengthInDBm()},
      {"addressType",type == bt::BluetoothAddressType::Random ? "random" : "public"},
      {"advertisServiceUUIDs",std::move(services)},{"serviceData",std::move(service_data)},
      {"manufacturerData",std::move(manufacturers)},{"advertisData",inline_binary(manufacturer_data)}};
  auto kind = args.AdvertisementType();result["connectable"] = kind == adv::BluetoothLEAdvertisementType::ConnectableUndirected || kind == adv::BluetoothLEAdvertisementType::ConnectableDirected;
  return result;
}
}  // namespace win_ble

inline Json WinBluetoothBackend::execute(const std::shared_ptr<NativeBle>& state, const std::shared_ptr<BleCommand>& command, const std::shared_ptr<void>& preparation) {
  const auto& api = command->api;const auto& payload = command->payload;
  try {
    apartment();
    // Owner-created consent acquisition must always drain, including after the
    // caller was canceled or the adapter faulted before this command ran.
    if (api == "wx.createBLEConnection") {
      auto prepared = std::static_pointer_cast<win_ble::Preparation>(preparation);
      if (!prepared || !prepared->operation) throw std::invalid_argument("Windows Bluetooth requires owner-thread device preparation");
      const auto id = payload.at("deviceId").get<std::string>();
      try {connections_.emplace(id,std::move(prepared->connection));}
      catch (...) {
        auto failure = std::current_exception();
        auto acquired = win_ble::await(prepared->operation,state,command,command->timeout_ms,prepared->completion);
        // The map allocation failed before acquiring any native child objects.
        // Retain this exact late device on the worker until Close succeeds.
        if (acquired.value) {
          auto device = *acquired.value;
          while (device) try {win_ble::close(device);}catch (...) {state->cleanup_failed("Windows Bluetooth late device close pending");std::this_thread::sleep_for(std::chrono::milliseconds(50));}
        }
        {std::lock_guard<std::mutex> guard(reservations_->mutex);reservations_->devices.erase(id);}
        std::rethrow_exception(failure);
      }
      auto& connection = *connections_.at(id);
      try {
        auto acquired = win_ble::await(prepared->operation,state,command,command->timeout_ms,prepared->completion);
        if (acquired.value) connection.device = *acquired.value;
        acquired.require();if (!connection.device) throw win_ble::error(E_ACCESSDENIED,"OS did not grant Bluetooth device access");
        auto session = win_ble::await(win_ble::gatt::GattSession::FromDeviceIdAsync(connection.device.BluetoothDeviceId()),state,command,command->timeout_ms);
        if (session.value) connection.session = *session.value;session.require();
        if (!connection.session) throw std::runtime_error("Windows Bluetooth GATT session is unavailable");
        connection.session.MaintainConnection(true);connection.maintaining = true;
        // FromBluetoothAddressAsync alone is not a connected-device receipt.
        // Uncached service discovery forces a real connection attempt.
        auto discovered = win_ble::await(connection.device.GetGattServicesAsync(win_ble::bt::BluetoothCacheMode::Uncached),state,command,command->timeout_ms);
        if (discovered.value) {
          connection.discovery = *discovered.value;
          win_ble::status(discovered.value->Status(),discovered.value->ProtocolError());
          for (auto service : discovered.value->Services()) {
            auto key = win_ble::uuid(service.Uuid());
            if (connection.services.size() >= 256) throw std::invalid_argument("Windows Bluetooth primary service limit exceeded");
            auto inserted = connection.services.emplace(key,service);
            if (!inserted.second) throw std::invalid_argument("device exposes ambiguous duplicate primary service UUIDs");
          }
        }
        discovered.require();
        std::weak_ptr<NativeBle> weak = state;auto signal = connection.signal;auto intent = command->delivery;
        connection.connection_token = connection.device.ConnectionStatusChanged([weak,signal,intent,id](const auto& sender,const auto&) {
          std::lock_guard<std::mutex> guard(signal->mutex);if (!signal->active || signal->services_changed || !intent->load()) return;
          if (auto owner = weak.lock();owner && !owner->stopping()) try {
            const bool connected = sender.ConnectionStatus() == win_ble::bt::BluetoothConnectionStatus::Connected;
            if (!connected) {signal->services_changed = true;signal->invalidate();}
            if (signal->announced) win_ble::connection_event(owner,id,connected,connected ? signal->delivery : std::shared_ptr<std::atomic<bool>>{});
          } catch (const winrt::hresult_error& error) {owner->fault("Windows Bluetooth status: " + winrt::to_string(error.message()));}
          catch (...) {owner->fault("Windows Bluetooth status callback failed");}
        });connection.connection_registered = true;
        connection.services_token = connection.device.GattServicesChanged([signal](const auto&,const auto&) {
          std::lock_guard<std::mutex> guard(signal->mutex);if (signal->active) {signal->services_changed = true;signal->invalidate();}
        });connection.services_registered = true;
        connection.mtu_token = connection.session.MaxPduSizeChanged([weak,signal,intent,id](const auto& sender,const auto&) {
          std::lock_guard<std::mutex> guard(signal->mutex);if (!signal->active || signal->services_changed || !signal->announced || !intent->load()) return;
          if (auto owner = weak.lock();owner && !owner->stopping()) try {owner->event("BLEMTUChange",{{"deviceId",id},{"mtu",sender.MaxPduSize()}},false,signal->delivery);}
          catch (const winrt::hresult_error& error) {owner->fault("Windows Bluetooth MTU: " + winrt::to_string(error.message()));}
          catch (...) {owner->fault("Windows Bluetooth MTU callback failed");}
        });connection.mtu_registered = true;
        {
          std::lock_guard<std::mutex> guard(signal->mutex);
          if (win_ble::retired(state,command) || signal->services_changed) throw win_ble::error(HRESULT_FROM_WIN32(ERROR_CANCELLED),"Bluetooth connection caller retired");
          if (connection.device.ConnectionStatus() != win_ble::bt::BluetoothConnectionStatus::Connected) throw win_ble::error(HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED),"native GATT connection was not established");
          connection.announced = true;signal->announced = true;win_ble::connection_event(state,id,true,signal->delivery);
        }
        return Json::object();
      } catch (...) {
        auto failure = std::current_exception();
        try {close_connection(state,id);}catch (const std::exception& cleanup) {state->fault(std::string("Windows Bluetooth connection cleanup pending: ") + cleanup.what());throw;}
        std::rethrow_exception(failure);
      }
    }
    // Explicit release operations remain effective when their result is canceled.
    if (api == "wx.stopBluetoothDevicesDiscovery") {stop_scan(state);return Json::object();}
    if (api == "wx.closeBLEConnection") {close_connection(state,payload.at("deviceId").get<std::string>());return Json::object();}
    const bool disabling = api == "wx.notifyBLECharacteristicValueChange" && !payload.at("state").get<bool>();
    if (win_ble::retired(state,command) && !disabling) return Json::object();
    if (api == "wx.getBluetoothAdapterState") return {{"available",radio_ && radio_.State() == win_ble::radio::RadioState::On},{"discovering",state->discovering.load()}};
    if (api == "wx.getBluetoothDevices") return {{"devices",state->devices()}};
    if (api == "wx.getConnectedBluetoothDevices") {
      auto filters = payload.value("services",Json::array());Json devices = Json::array();
      for (const auto& [id,value] : connections_) {
        if (!value->device || value->device.ConnectionStatus() != win_ble::bt::BluetoothConnectionStatus::Connected) continue;
        if (!filters.empty() && std::none_of(filters.begin(),filters.end(),[&](const Json& service) {return value->services.count(service.get<std::string>()) != 0;})) continue;
        devices.push_back(state->device(id));
      }
      return {{"devices",std::move(devices)}};
    }
    if (api == "wx.startBluetoothDevicesDiscovery") {
      if (payload.value("interval",int64_t(0)) != 0 || payload.contains("powerLevel")) throw std::invalid_argument("Windows scan interval/power overrides are not supported");
      stop_scan(state);if (win_ble::retired(state,command)) return Json::object();
      watcher_ = win_ble::adv::BluetoothLEAdvertisementWatcher();scan_ = std::make_shared<win_ble::ScanSignal>();auto scan = scan_;
      watcher_.ScanningMode(win_ble::adv::BluetoothLEScanningMode::Active);
      const auto filters = payload.value("services",Json::array());const bool duplicates = payload.value("allowDuplicatesKey",false);std::weak_ptr<NativeBle> weak = state;auto intent = command->delivery;
      received_token_ = watcher_.Received([weak,scan,filters,duplicates,intent](const auto&,const win_ble::adv::BluetoothLEAdvertisementReceivedEventArgs& args) {
        auto owner = weak.lock();if (!owner || owner->stopping() || !owner->discovering) return;
        try {
          auto type = args.BluetoothAddressType();if (type != win_ble::bt::BluetoothAddressType::Public && type != win_ble::bt::BluetoothAddressType::Random) return;
          auto value = win_ble::advertisement(args);
          if (!filters.empty() && std::none_of(filters.begin(),filters.end(),[&](const Json& filter) {auto& services = value.at("advertisServiceUUIDs");return std::find(services.begin(),services.end(),filter) != services.end();})) return;
          // Hold the watcher generation through enqueue. An old callback can
          // never resume after a replacement scan and borrow its delivery gate.
          std::lock_guard<std::mutex> guard(scan->mutex);if (scan->expected || !intent->load()) return;
          if (!duplicates) {auto id = value.at("deviceId").get<std::string>();if (scan->seen.count(id) || scan->seen.size() >= 256) return;scan->seen.insert(std::move(id));}
          owner->found(std::move(value));
        } catch (const winrt::hresult_error& failure) {owner->fault("Windows Bluetooth advertisement: " + winrt::to_string(failure.message()));}
        catch (const std::exception& failure) {owner->fault(std::string("Windows Bluetooth advertisement: ") + failure.what());}
        catch (...) {owner->fault("Windows Bluetooth advertisement callback failed");}
      });received_registered_ = true;
      stopped_token_ = watcher_.Stopped([weak,scan](const auto&,const win_ble::adv::BluetoothLEAdvertisementWatcherStoppedEventArgs& args) {
        scan->terminal->complete();bool expected;{std::lock_guard<std::mutex> guard(scan->mutex);expected = scan->expected;}
        if (auto owner = weak.lock();owner && !owner->stopping() && !expected) {
          owner->cancel_discovery();
          try {owner->fault("Windows Bluetooth scan stopped by OS (error " + std::to_string(int(args.Error())) + ")");}
          catch (...) {owner->fault("Windows Bluetooth scan stopped by OS");}
        }
      });stopped_registered_ = true;
      state->begin_discovery();
      try {watcher_.Start();if (win_ble::retired(state,command)) stop_scan(state);}
      catch (...) {auto failure = std::current_exception();try {stop_scan(state);}catch (const std::exception& cleanup) {state->fault(std::string("Windows Bluetooth scanner cleanup pending: ") + cleanup.what());throw;}std::rethrow_exception(failure);}
      return Json::object();
    }
    const auto id = payload.at("deviceId").get<std::string>();auto& owned = connection(state,id);
    if (api == "wx.getBLEMTU") return {{"mtu",owned.session.MaxPduSize()}};
    if (api == "wx.getBLEDeviceServices") {
      Json services = Json::array();for (const auto& [uuid,_] : owned.services) services.push_back({{"uuid",uuid},{"isPrimary",true}});return {{"services",std::move(services)}};
    }
    if (api == "wx.getBLEDeviceCharacteristics") {
      auto service = payload.at("serviceId").get<std::string>();characteristics(state,command,owned,service);Json values = Json::array();
      using P = win_ble::gatt::GattCharacteristicProperties;
      for (const auto& [key,characteristic] : owned.characteristics) if (key.rfind(service + "/",0) == 0) {
        auto flags = characteristic.CharacteristicProperties();values.push_back({{"uuid",win_ble::uuid(characteristic.Uuid())},{"properties",{{"read",property(flags,P::Read)},{"write",property(flags,P::Write)},{"writeNoResponse",property(flags,P::WriteWithoutResponse)},{"notify",property(flags,P::Notify)},{"indicate",property(flags,P::Indicate)}}}});
      }
      return {{"characteristics",std::move(values)}};
    }
    auto characteristic = this->characteristic(state,command,owned);auto flags = characteristic.CharacteristicProperties();using P = win_ble::gatt::GattCharacteristicProperties;
    if (api == "wx.readBLECharacteristicValue") {
      if (!property(flags,P::Read)) throw std::invalid_argument("Bluetooth characteristic is not readable");
      auto result = win_ble::await(characteristic.ReadValueAsync(win_ble::bt::BluetoothCacheMode::Uncached),state,command,command->timeout_ms);result.require();win_ble::status(result.value->Status(),result.value->ProtocolError());
      auto value = win_ble::bytes(result.value->Value());
      {std::lock_guard<std::mutex> guard(owned.signal->mutex);
        if (!owned.signal->active || owned.signal->services_changed) throw win_ble::error(HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED),"Bluetooth connection changed during read");
        state->event("BLECharacteristicValueChange",{{"deviceId",id},{"serviceId",payload.at("serviceId")},{"characteristicId",payload.at("characteristicId")},{"value",inline_binary(value)}},false,command->delivery);
      }return Json::object();
    }
    if (api == "wx.writeBLECharacteristicValue") {
      bool no_response = payload.value("writeType","write") == "writeNoResponse";
      if (!property(flags,no_response ? P::WriteWithoutResponse : P::Write)) throw std::invalid_argument("Bluetooth characteristic does not support this writeType");
      auto value = binary_bytes(payload.at("value"));auto mtu = owned.session.MaxPduSize();
      if (value.size() > 512 || no_response && (mtu < 3 || value.size() > size_t(mtu - 3))) throw std::invalid_argument("Bluetooth write exceeds native attribute/negotiated MTU limit");
      auto result = win_ble::await(characteristic.WriteValueWithResultAsync(win_ble::buffer(value),no_response ? win_ble::gatt::GattWriteOption::WriteWithoutResponse : win_ble::gatt::GattWriteOption::WriteWithResponse),state,command,command->timeout_ms);result.require();win_ble::status(result.value->Status(),result.value->ProtocolError());return Json::object();
    }
    if (api == "wx.notifyBLECharacteristicValueChange") {
      const auto key = ble_characteristic_key(payload),characteristic_key = payload.at("serviceId").get<std::string>() + "/" + payload.at("characteristicId").get<std::string>();
      const bool enabled = payload.at("state").get<bool>();
      auto type = payload.value("type", "");
      if (enabled && type != "" && type != "notification" && type != "indication") throw std::invalid_argument("Bluetooth notification type must be notification or indication");
      const bool indication = type == "indication" || type.empty() && !property(flags,P::Notify);
      if (enabled && !property(flags,indication ? P::Indicate : P::Notify)) throw std::invalid_argument("Bluetooth characteristic does not support the requested subscription type");
      try {
        auto existing = owned.notifications.find(characteristic_key);
        if (existing != owned.notifications.end()) {
          {std::lock_guard<std::mutex> guard(existing->second.signal->mutex);existing->second.signal->active = false;if (existing->second.delivery) *existing->second.delivery = false;}
          win_ble::revoke(existing->second.registered,[&] {win_ble::checked_remove<win_ble::gatt::IGattCharacteristic>(existing->second.characteristic,[&](auto* abi) {return abi->remove_ValueChanged(existing->second.token);});});
          owned.notifications.erase(existing);
        }
        auto descriptor = win_ble::gatt::GattClientCharacteristicConfigurationDescriptorValue::None;
        if (enabled) {
          descriptor = indication ? win_ble::gatt::GattClientCharacteristicConfigurationDescriptorValue::Indicate : win_ble::gatt::GattClientCharacteristicConfigurationDescriptorValue::Notify;
          // Reserve the tracked map slot and delivery gates before installing
          // ValueChanged. No allocation after native registration can orphan it.
          auto& notification = owned.notifications.try_emplace(characteristic_key).first->second;
          notification.characteristic = characteristic;notification.delivery = state->begin_notification(key);
          auto signal = notification.signal;auto delivery = notification.delivery;auto intent = command->delivery;auto connection_signal = owned.signal;
          {std::lock_guard<std::mutex> guard(connection_signal->mutex);
            if (!connection_signal->active || connection_signal->services_changed) throw std::invalid_argument("Bluetooth connection was retired during notification setup");
            auto& gates = connection_signal->notification_deliveries;
            gates.erase(std::remove_if(gates.begin(),gates.end(),[](const auto& gate) {return gate.expired();}),gates.end());gates.push_back(delivery);
          }
          std::weak_ptr<NativeBle> weak = state;auto service = payload.at("serviceId").get<std::string>(),uuid = payload.at("characteristicId").get<std::string>();
          notification.token = characteristic.ValueChanged([weak,signal,connection_signal,delivery,intent,id,service,uuid](const auto&,const win_ble::gatt::GattValueChangedEventArgs& event) {
            std::lock_guard<std::mutex> guard(signal->mutex);if (!signal->active || !delivery->load() || !intent->load()) return;
            std::lock_guard<std::mutex> connection_guard(connection_signal->mutex);if (!connection_signal->active || connection_signal->services_changed) return;
            if (auto owner = weak.lock();owner && !owner->stopping()) try {owner->event("BLECharacteristicValueChange",{{"deviceId",id},{"serviceId",service},{"characteristicId",uuid},{"value",inline_binary(win_ble::bytes(event.CharacteristicValue()))}},false,delivery);}
            catch (const winrt::hresult_error& error) {owner->fault("Windows Bluetooth notification: " + winrt::to_string(error.message()));}
            catch (const std::exception& error) {owner->fault(std::string("Windows Bluetooth notification: ") + error.what());}
            catch (...) {owner->fault("Windows Bluetooth notification callback failed");}
          });notification.registered = true;
        } else state->cancel_notification(key);
        // Disabling is a resource release even when its JS result is canceled.
        auto result = win_ble::await(characteristic.WriteClientCharacteristicConfigurationDescriptorWithResultAsync(descriptor),state,enabled ? command : nullptr,command->timeout_ms);
        if (win_ble::retired(state,command)) {state->cancel_notification(key);close_connection(state,id);return Json::object();}
        result.require();win_ble::status(result.value->Status(),result.value->ProtocolError());
        if (enabled) owned.notifications.at(characteristic_key).enabled = true;else owned.notifications.erase(characteristic_key);
      } catch (...) {
        auto failure = std::current_exception();state->cancel_notification(key);
        // A failed CCCD write has an uncertain peripheral state. Closing our
        // exact session is the authoritative release, including late success.
        close_connection(state,id);std::rethrow_exception(failure);
      }
      return Json::object();
    }
    throw std::invalid_argument("Windows Bluetooth command is not implemented");
  } catch (const winrt::hresult_error& failure) {throw win_ble::error(failure.code(),"Windows Bluetooth: " + winrt::to_string(failure.message()));}
}

inline BlePlatform win_bluetooth_platform() {
  try {
    auto apartment = std::make_shared<win_ble::Apartment>(RO_INIT_SINGLETHREADED);
    using winrt::Windows::Foundation::Metadata::ApiInformation;
    if (!winrt::try_get_activation_factory<win_ble::bt::BluetoothAdapter>() ||
        !ApiInformation::IsTypePresent(L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattSession") ||
        !ApiInformation::IsPropertyPresent(L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementReceivedEventArgs",L"BluetoothAddressType")) return {};
    auto reservations = std::make_shared<win_ble::Reservations>();BlePlatform result;
    result.apis = {"wx.openBluetoothAdapter","wx.closeBluetoothAdapter","wx.getBluetoothAdapterState","wx.startBluetoothDevicesDiscovery","wx.stopBluetoothDevicesDiscovery","wx.getBluetoothDevices","wx.getConnectedBluetoothDevices","wx.createBLEConnection","wx.closeBLEConnection","wx.getBLEDeviceServices","wx.getBLEDeviceCharacteristics","wx.readBLECharacteristicValue","wx.writeBLECharacteristicValue","wx.notifyBLECharacteristicValueChange","wx.getBLEMTU"};
    result.events = {"BluetoothAdapterStateChange","BluetoothDeviceFound","BLEConnectionStateChange","BLECharacteristicValueChange","BLEMTUChange"};
    result.factory = [reservations] {return std::make_unique<WinBluetoothBackend>(reservations);};
    result.prepare = [apartment,reservations](const DeviceApiOptions& options,const std::string& api,const Json& payload,const Json& device) -> std::shared_ptr<void> {
      if (api != "wx.createBLEConnection") return {};
      auto parent = static_cast<HWND>(options.native_parent);
      if (!parent || !IsWindow(parent) || GetWindowThreadProcessId(parent,nullptr) != GetCurrentThreadId() || !IsWindowVisible(parent) || GetAncestor(parent,GA_ROOT) != GetAncestor(GetForegroundWindow(),GA_ROOT)) throw std::invalid_argument("Windows Bluetooth device access requires the host's foreground UI thread and attached window");
      const auto id = payload.at("deviceId").get<std::string>();
      if (device.at("deviceId") != id) throw std::invalid_argument("Windows Bluetooth metadata belongs to another discovered device");
      auto type = device.value("addressType","");if (type != "public" && type != "random") throw std::invalid_argument("native Bluetooth address type was not discovered");
      auto prepared = std::make_shared<win_ble::Preparation>();prepared->reservations = reservations;prepared->device = id;
      {std::lock_guard<std::mutex> guard(reservations->mutex);
        if (reservations->devices.size() >= 32) throw std::invalid_argument("Windows Bluetooth connection limit exceeded");
        if (!reservations->devices.insert(id).second) throw std::invalid_argument("Bluetooth connection is already owned or acquiring; close it before reconnecting");
      }
      try {prepared->operation = win_ble::bt::BluetoothLEDevice::FromBluetoothAddressAsync(win_ble::address(id),type == "random" ? win_ble::bt::BluetoothAddressType::Random : win_ble::bt::BluetoothAddressType::Public);}
      catch (const winrt::hresult_error& failure) {{std::lock_guard<std::mutex> guard(reservations->mutex);reservations->devices.erase(id);}throw win_ble::error(failure.code(),"Windows Bluetooth permission: " + winrt::to_string(failure.message()));}
      catch (...) {std::lock_guard<std::mutex> guard(reservations->mutex);reservations->devices.erase(id);throw;}
      return prepared;
    };return result;
  } catch (const winrt::hresult_error&) {return {};}
}
}  // namespace weapp::desktop::detail
