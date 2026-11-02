#pragma once
#include "device_bluetooth_common.hpp"
#include "device_dbus_common.hpp"
namespace weapp::desktop::detail {
inline std::string bluez_string(GVariant* fields, const char* name, const std::string& fallback = "") {
  DbusVariant value(g_variant_lookup_value(fields, name, nullptr), g_variant_unref);
  if (!value) return fallback;
  if (!g_variant_is_of_type(value.get(), G_VARIANT_TYPE_STRING) && !g_variant_is_of_type(value.get(), G_VARIANT_TYPE_OBJECT_PATH)) throw std::runtime_error("BlueZ property is not a string");
  return g_variant_get_string(value.get(), nullptr);
}
inline bool bluez_bool(GVariant* fields, const char* name, bool fallback = false) {
  gboolean value = fallback; g_variant_lookup(fields, name, "b", &value); return value;
}
inline Json bluez_strings(GVariant* fields, const char* name, size_t limit = 64) {
  DbusVariant array(g_variant_lookup_value(fields, name, G_VARIANT_TYPE("as")), g_variant_unref); Json values = Json::array();
  if (array) {GVariantIter iterator; g_variant_iter_init(&iterator, array.get()); const char* value;
    while (g_variant_iter_next(&iterator, "&s", &value)) {if (values.size() >= limit) throw std::runtime_error("BlueZ string list limit exceeded"); values.push_back(value);}}
  return values;
}
inline std::vector<uint8_t> bluez_bytes(GVariant* value) {
  gsize size = 0; auto raw = static_cast<const uint8_t*>(g_variant_get_fixed_array(value, &size, 1));
  if (size > 1048576) throw std::runtime_error("BlueZ value exceeds one MiB");
  return size ? std::vector<uint8_t>(raw, raw + size) : std::vector<uint8_t>();
}
struct BluezObject {std::string path, interface; DbusVariant fields;};
inline std::vector<BluezObject> bluez_objects(const DbusConnection& bus) {
  auto reply = dbus_call(bus, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", nullptr, G_VARIANT_TYPE("(a{oa{sa{sv}}})"));
  DbusVariant objects(g_variant_get_child_value(reply.get(), 0), g_variant_unref); std::vector<BluezObject> result;
  GVariantIter iterator; g_variant_iter_init(&iterator, objects.get()); const char* path; GVariant* interfaces;
  size_t count = 0;
  while (g_variant_iter_next(&iterator, "{&o@a{sa{sv}}}", &path, &interfaces)) {
    DbusVariant owner(interfaces, g_variant_unref); if (++count > 4096) throw std::runtime_error("BlueZ object cache limit exceeded");
    for (auto name : {"org.bluez.Adapter1", "org.bluez.Device1", "org.bluez.GattService1", "org.bluez.GattCharacteristic1"}) {
      auto fields = g_variant_lookup_value(interfaces, name, G_VARIANT_TYPE("a{sv}"));
      if (fields) result.push_back({path, name, DbusVariant(fields, g_variant_unref)});
    }
  }
  return result;
}
struct BluezExclusiveLeases {
  std::mutex mutex; std::set<std::string> paths;
  static BluezExclusiveLeases& instance() {static BluezExclusiveLeases leases; return leases;}
};
class BluezBackend final : public BleBackend {
  DbusConnection bus_;
  GMainContext* context_ = nullptr;
  guint properties_signal_ = 0, interfaces_signal_ = 0;
  std::weak_ptr<NativeBle> state_;
  struct SignalOwner {BluezBackend* owner; std::atomic<unsigned> subscriptions{0};};
  std::shared_ptr<SignalOwner> signals_ = std::make_shared<SignalOwner>();
  using SignalTicket = std::shared_ptr<SignalOwner>;
  static void signal_released(gpointer user) {auto ticket = static_cast<SignalTicket*>(user); --(*ticket)->subscriptions; delete ticket;}
  void unsubscribe_signals() {
    if (properties_signal_) {g_dbus_connection_signal_unsubscribe(bus_.get(), properties_signal_); properties_signal_ = 0;}
    if (interfaces_signal_) {g_dbus_connection_signal_unsubscribe(bus_.get(), interfaces_signal_); interfaces_signal_ = 0;}
  }
  bool signals_released() {
    for (unsigned count = 0; count < 64 && context_ && g_main_context_pending(context_); ++count) g_main_context_iteration(context_, FALSE);
    return signals_->subscriptions == 0;
  }
  std::string adapter_;
  bool scanning_ = false, duplicate_ = false;
  Json service_filter_ = Json::array();
  std::map<std::string,std::string> devices_;
  std::set<std::string> dirty_;
  struct Characteristic {std::string device, service, uuid, path; Json flags; std::shared_ptr<std::atomic<bool>> notification; bool native_notifying=false;};
  struct Device {std::string id,path; bool connected=false; std::map<std::string,std::string> services;};
  std::map<std::string,Device> connections_;
  std::map<std::string,Characteristic> characteristics_;
  std::map<std::string,Json> advertisements_;
  DbusVariant properties(const std::string& path, const char* interface) {
    auto reply = dbus_call(bus_, "org.bluez", path, "org.freedesktop.DBus.Properties", "GetAll", g_variant_new("(s)", interface), G_VARIANT_TYPE("(a{sv})"));
    return DbusVariant(g_variant_get_child_value(reply.get(), 0), g_variant_unref);
  }
  bool property_bool(const std::string& path, const char* interface, const char* field) {
    auto value = dbus_property(bus_, "org.bluez", path, interface, field); return g_variant_get_boolean(value.get());
  }
  void call(const std::string& path, const char* interface, const char* method, GVariant* arguments = nullptr, int timeout = 5000) {
    dbus_call(bus_, "org.bluez", path, interface, method, arguments, G_VARIANT_TYPE("()"), nullptr, timeout);
  }
  void publish_device(const std::shared_ptr<NativeBle>& state, const std::string& path) {
    auto fields = properties(path, "org.bluez.Device1");
    if (bluez_string(fields.get(), "Adapter") != adapter_) return;
    auto id = bluez_string(fields.get(), "Address"); if (id.empty()) return;
    Json uuids = Json::array(); for (const auto& value : bluez_strings(fields.get(), "UUIDs")) uuids.push_back(ble_uuid(value.get<std::string>()));
    bool matches = service_filter_.empty(); for (const auto& expected : service_filter_) for (const auto& uuid : uuids) if (expected == uuid) matches = true;
    if (!matches) return;
    Json device = {{"deviceId", id}, {"name", bluez_string(fields.get(), "Name")}, {"localName", bluez_string(fields.get(), "Name")}, {"advertisServiceUUIDs", uuids}};
    gint16 rssi; if (g_variant_lookup(fields.get(), "RSSI", "n", &rssi)) device["RSSI"] = rssi;
    DbusVariant manufacturer(g_variant_lookup_value(fields.get(), "ManufacturerData", G_VARIANT_TYPE("a{qv}")), g_variant_unref);
    if (manufacturer && g_variant_n_children(manufacturer.get())) {
      // One manufacturer record is the WeChat advertisement payload: LE
      // company identifier followed by its actual native bytes.
      DbusVariant entry(g_variant_get_child_value(manufacturer.get(), 0), g_variant_unref);
      guint16 company; GVariant* wrapped; g_variant_get(entry.get(), "(qv)", &company, &wrapped); DbusVariant bytes(wrapped, g_variant_unref);
      auto payload = bluez_bytes(bytes.get()); payload.insert(payload.begin(), {uint8_t(company & 255), uint8_t(company >> 8)}); device["advertisData"] = inline_binary(payload);
    }
    auto previous = advertisements_.find(id); if (!duplicate_ && previous != advertisements_.end() && previous->second == device) return;
    if (devices_.size() >= 256 && !devices_.count(id)) return;
    devices_[id] = path; advertisements_[id] = device; state->found(std::move(device));
  }
  void changed(const std::string& path, GVariant* parameters) {
    auto state = state_.lock(); if (!state || state->stopping()) return;
    const char* interface; GVariant* values; GVariant* invalidated;
    g_variant_get(parameters, "(&s@a{sv}@as)", &interface, &values, &invalidated);
    DbusVariant fields(values, g_variant_unref), unused(invalidated, g_variant_unref);
    if (std::string(interface) == "org.bluez.Adapter1" && path == adapter_) {
      if (!bluez_bool(values, "Powered", true)) state->fault("native Bluetooth adapter was powered off");
    } else if (std::string(interface) == "org.bluez.Device1") {
      if (path.compare(0, adapter_.size() + 1, adapter_ + "/") == 0 && dirty_.size() < 256) dirty_.insert(path);
      for (auto& [id, connection] : connections_) if (connection.path == path && connection.connected) {
        const bool disconnected = !bluez_bool(values, "Connected", true);
        const bool unresolved = !bluez_bool(values, "ServicesResolved", true);
        if (disconnected || unresolved) {
          // Revoke already queued values before announcing native loss. Keep
          // the exact lease until explicit Disconnect and Connected=false proof.
          state->cancel_device_notifications(id); connection.connected = false; state->connected(id, false);
          if (unresolved && !disconnected) state->fault("BlueZ Bluetooth service database changed; reconnect the device");
        }
      }
    } else if (std::string(interface) == "org.bluez.GattCharacteristic1") {
      for (const auto& [key, characteristic] : characteristics_) if (characteristic.path == path && characteristic.notification && characteristic.notification->load()) {
        DbusVariant value(g_variant_lookup_value(values, "Value", G_VARIANT_TYPE("ay")), g_variant_unref);
        if (value) state->event("BLECharacteristicValueChange", {{"deviceId", characteristic.device}, {"serviceId", characteristic.service}, {"characteristicId", characteristic.uuid}, {"value", inline_binary(bluez_bytes(value.get()))}}, false, characteristic.notification);
      }
    }
  }
  static void property_signal(GDBusConnection*, const gchar*, const gchar* path, const gchar*, const gchar*, GVariant* parameters, gpointer user) {
    auto self = (*static_cast<SignalTicket*>(user))->owner;
    try {self->changed(path, parameters);} catch (const std::exception& error) {if (auto state = self->state_.lock()) state->fault(error.what());}
  }
  static void added_signal(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* parameters, gpointer user) {
    auto self = (*static_cast<SignalTicket*>(user))->owner;
    try {const char* path; GVariant* interfaces; g_variant_get(parameters, "(&o@a{sa{sv}})", &path, &interfaces); DbusVariant fields(interfaces, g_variant_unref);
      DbusVariant device(g_variant_lookup_value(interfaces, "org.bluez.Device1", G_VARIANT_TYPE("a{sv}")), g_variant_unref);
      if (device && self->dirty_.size() < 256 && std::string(path).compare(0, self->adapter_.size() + 1, self->adapter_ + "/") == 0) self->dirty_.insert(path);
    } catch (const std::exception& error) {if (auto state = self->state_.lock()) state->fault(error.what());}
  }
  Device& connected_device(const Json& payload) {
    auto found = connections_.find(payload.at("deviceId").get<std::string>());
    if (found == connections_.end() || !found->second.connected) throw std::invalid_argument("Bluetooth device is not connected by this owner"); return found->second;
  }
  Characteristic& characteristic(const Json& payload) {
    connected_device(payload); auto found = characteristics_.find(ble_characteristic_key(payload));
    if (found == characteristics_.end()) throw std::invalid_argument("Bluetooth characteristic was not discovered by this owner"); return found->second;
  }
  bool has_flag(const Characteristic& characteristic, const std::string& flag) {
    return std::find(characteristic.flags.begin(), characteristic.flags.end(), flag) != characteristic.flags.end();
  }
  void stop_notification(Characteristic& characteristic) {
    if (characteristic.notification) *characteristic.notification = false;
    if (characteristic.native_notifying) {call(characteristic.path, "org.bluez.GattCharacteristic1", "StopNotify"); characteristic.native_notifying = false;}
  }
  bool disconnect(const std::shared_ptr<NativeBle>& state, const std::string& id) {
    auto found = connections_.find(id); if (found == connections_.end()) return true;
    auto& device = found->second;
    // Host opted into exclusive target-device ownership before these global
    // methods were registered. Disconnect also cancels a pending Connect per
    // the official Device1 contract. Never use it for an unleased device.
    try {
      if (g_dbus_connection_is_closed(bus_.get())) throw std::runtime_error("BlueZ connection closed before exclusive device cleanup was confirmed");
      try {call(device.path, "org.bluez.Device1", "Disconnect");}
      catch (const std::exception&) {if (property_bool(device.path, "org.bluez.Device1", "Connected")) throw;}
      if (property_bool(device.path, "org.bluez.Device1", "Connected")) throw std::runtime_error("BlueZ exclusive device disconnect is pending");
      for (auto item = characteristics_.begin(); item != characteristics_.end();) if (item->second.device == id) {if (item->second.notification) *item->second.notification = false; item = characteristics_.erase(item);} else ++item;
      if (device.connected) state->connected(id, false);
      {auto& leases = BluezExclusiveLeases::instance(); std::lock_guard<std::mutex> guard(leases.mutex); leases.paths.erase(device.path);}
      connections_.erase(found); return true;
    } catch (const std::exception& error) {state->cleanup_failed(error.what()); return false;}
  }
  void await_disconnect(const std::shared_ptr<NativeBle>& state, const std::string& id) {
    while (!disconnect(state, id)) std::this_thread::sleep_for(std::chrono::milliseconds(50)); state->cleanup_succeeded();
  }
  void stop_discovery(const std::shared_ptr<NativeBle>& state) {
    state->cancel_discovery(); if (scanning_) {call(adapter_, "org.bluez.Adapter1", "StopDiscovery"); scanning_ = false;}
  }
 public:
  void open(const std::shared_ptr<NativeBle>& state) override {
    state_ = state; context_ = g_main_context_new(); g_main_context_push_thread_default(context_); bus_ = dbus_private_system();
    for (const auto& object : bluez_objects(bus_)) if (object.interface == "org.bluez.Adapter1" && bluez_bool(object.fields.get(), "Powered")) {adapter_ = object.path; break;}
    if (adapter_.empty()) throw std::runtime_error("no powered BlueZ adapter is available");
    signals_->owner = this;
    auto property_ticket = new SignalTicket(signals_); ++signals_->subscriptions;
    properties_signal_ = g_dbus_connection_signal_subscribe(bus_.get(), "org.bluez", "org.freedesktop.DBus.Properties", "PropertiesChanged", nullptr, nullptr, G_DBUS_SIGNAL_FLAGS_NONE, property_signal, property_ticket, signal_released);
    auto interfaces_ticket = new SignalTicket(signals_); ++signals_->subscriptions;
    interfaces_signal_ = g_dbus_connection_signal_subscribe(bus_.get(), "org.bluez", "org.freedesktop.DBus.ObjectManager", "InterfacesAdded", nullptr, nullptr, G_DBUS_SIGNAL_FLAGS_NONE, added_signal, interfaces_ticket, signal_released);
  }
  Json execute(const std::shared_ptr<NativeBle>& state, const std::shared_ptr<BleCommand>& command, const std::shared_ptr<void>&) override {
    const auto& api = command->api; const auto& payload = command->payload;
    if (api == "wx.stopBluetoothDevicesDiscovery") {stop_discovery(state); return Json::object();}
    if (api == "wx.closeBLEConnection") {await_disconnect(state, payload.at("deviceId").get<std::string>()); return Json::object();}
    if (api == "wx.notifyBLECharacteristicValueChange" && !payload.at("state").get<bool>()) {stop_notification(characteristic(payload)); return Json::object();}
    if (state->stopping() || command->cancelled()) return Json::object();
    if (api == "wx.getBluetoothAdapterState") return {{"available", property_bool(adapter_, "org.bluez.Adapter1", "Powered")}, {"discovering", bool(state->discovering)}};
    if (api == "wx.getBluetoothDevices") return {{"devices", state->devices()}};
    if (api == "wx.getConnectedBluetoothDevices") {
      Json result = Json::array(); auto filters = payload.value("services", Json::array());
      for (const auto& device : state->devices(true)) {
        auto found = connections_.find(device.at("deviceId").get<std::string>()); if (found == connections_.end() || !found->second.connected) continue;
        bool matches = filters.empty();
        if (!matches) for (const auto& object : bluez_objects(bus_)) if (object.interface == "org.bluez.GattService1" && bluez_string(object.fields.get(), "Device") == found->second.path)
          for (const auto& expected : filters) if (ble_uuid(bluez_string(object.fields.get(), "UUID")) == expected) matches = true;
        if (matches) result.push_back(device);
      }
      return {{"devices", result}};
    }
    if (api == "wx.startBluetoothDevicesDiscovery") {
      if (scanning_) throw std::invalid_argument("Bluetooth discovery is already active; stop before changing its filters");
      dirty_.clear(); advertisements_.clear();
      service_filter_ = payload.value("services", Json::array()); duplicate_ = payload.value("allowDuplicatesKey", false);
      GVariantBuilder filter; g_variant_builder_init(&filter, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&filter, "{sv}", "Transport", g_variant_new_string("le")); g_variant_builder_add(&filter, "{sv}", "DuplicateData", g_variant_new_boolean(duplicate_));
      GVariantBuilder uuids; g_variant_builder_init(&uuids, G_VARIANT_TYPE("as")); for (const auto& uuid : service_filter_) g_variant_builder_add(&uuids, "s", uuid.get_ref<const std::string&>().c_str());
      g_variant_builder_add(&filter, "{sv}", "UUIDs", g_variant_builder_end(&uuids));
      call(adapter_, "org.bluez.Adapter1", "SetDiscoveryFilter", g_variant_new("(a{sv})", &filter));
      state->begin_discovery(); scanning_ = true;
      try {call(adapter_, "org.bluez.Adapter1", "StartDiscovery", nullptr, int(command->timeout_ms));}
      catch (...) {stop_discovery(state); throw;}
      if (state->stopping() || command->cancelled()) {stop_discovery(state); return Json::object();}
      state->event("BluetoothAdapterStateChange", {{"available", true}, {"discovering", true}}); return Json::object();
    }
    if (api == "wx.createBLEConnection") {
      if (!state->options.linux_bluetooth_exclusive) throw std::invalid_argument("BlueZ connections require trusted exclusive host ownership");
      auto id = payload.at("deviceId").get<std::string>(); if (connections_.count(id)) throw std::invalid_argument("Bluetooth device is already owned; close it before reconnecting");
      auto path = devices_.at(id); if (connections_.size() >= 8) throw std::invalid_argument("Bluetooth connection limit exceeded");
      if (property_bool(path, "org.bluez.Device1", "Connected")) throw std::invalid_argument("BlueZ device is already connected by another owner");
      connections_.emplace(id, Device{id, path, false, {}});
      try {auto& leases = BluezExclusiveLeases::instance(); std::lock_guard<std::mutex> guard(leases.mutex); if (!leases.paths.insert(path).second) throw std::invalid_argument("BlueZ device already has an exclusive SDK lease");}
      catch (...) {connections_.erase(id); throw;}
      try {
        call(path, "org.bluez.Device1", "Connect", nullptr, int(command->timeout_ms));
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(command->timeout_ms);
        while (!state->stopping() && !command->cancelled()) {
          if (!property_bool(path, "org.bluez.Device1", "Connected")) throw std::runtime_error("BlueZ device disconnected during service discovery");
          if (property_bool(path, "org.bluez.Device1", "ServicesResolved")) {connections_.at(id).connected = true; state->connected(id, true); return Json::object();}
          if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("BlueZ service discovery timed out");
          poll(state); std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        await_disconnect(state, id); return Json::object();
      } catch (...) {auto failure = std::current_exception(); await_disconnect(state, id); std::rethrow_exception(failure);}
    }
    if (api == "wx.getBLEDeviceServices") {
      auto& device = connected_device(payload); Json services = Json::array();
      for (const auto& object : bluez_objects(bus_)) if (object.interface == "org.bluez.GattService1" && bluez_string(object.fields.get(), "Device") == device.path) {
        if (services.size() >= 64) throw std::runtime_error("Bluetooth service limit exceeded"); auto uuid = ble_uuid(bluez_string(object.fields.get(), "UUID"));
        if (device.services.count(uuid) && device.services.at(uuid) != object.path) throw std::runtime_error("duplicate Bluetooth service UUID is ambiguous");
        device.services[uuid] = object.path; services.push_back({{"uuid", uuid}, {"isPrimary", bluez_bool(object.fields.get(), "Primary")}});
      }
      return {{"services", services}};
    }
    if (api == "wx.getBLEDeviceCharacteristics") {
      auto& device = connected_device(payload); auto service = payload.at("serviceId").get<std::string>(); auto found = device.services.find(service);
      if (found == device.services.end()) throw std::invalid_argument("Bluetooth service was not discovered by this owner"); Json result = Json::array();
      for (const auto& object : bluez_objects(bus_)) if (object.interface == "org.bluez.GattCharacteristic1" && bluez_string(object.fields.get(), "Service") == found->second) {
        if (result.size() >= 256) throw std::runtime_error("Bluetooth characteristic limit exceeded");
        auto uuid = ble_uuid(bluez_string(object.fields.get(), "UUID")); auto key = ble_characteristic_key({{"deviceId", device.id}, {"serviceId", service}, {"characteristicId", uuid}});
        auto existing = characteristics_.find(key); if (existing != characteristics_.end() && existing->second.path != object.path) throw std::runtime_error("duplicate Bluetooth characteristic UUID is ambiguous");
        if (existing == characteristics_.end() && characteristics_.size() >= 256) throw std::runtime_error("Bluetooth characteristic limit exceeded");
        if (existing == characteristics_.end()) existing = characteristics_.emplace(key, Characteristic{device.id, service, uuid, object.path, bluez_strings(object.fields.get(), "Flags"), {}, false}).first;
        auto& current = existing->second;
        result.push_back({{"uuid", uuid}, {"properties", {{"read", has_flag(current, "read")}, {"write", has_flag(current, "write")}, {"writeNoResponse", has_flag(current, "write-without-response")}, {"notify", has_flag(current, "notify")}, {"indicate", has_flag(current, "indicate")}}}});
      }
      return {{"characteristics", result}};
    }
    if (api == "wx.readBLECharacteristicValue") {
      auto& current = characteristic(payload); if (!has_flag(current, "read")) throw std::invalid_argument("Bluetooth characteristic is not readable");
      GVariantBuilder options; g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
      auto reply = dbus_call(bus_, "org.bluez", current.path, "org.bluez.GattCharacteristic1", "ReadValue", g_variant_new("(a{sv})", &options), G_VARIANT_TYPE("(ay)"), nullptr, int(command->timeout_ms));
      DbusVariant bytes(g_variant_get_child_value(reply.get(), 0), g_variant_unref);
      if (!state->stopping() && !command->cancelled()) state->event("BLECharacteristicValueChange", {{"deviceId", current.device}, {"serviceId", current.service}, {"characteristicId", current.uuid}, {"value", inline_binary(bluez_bytes(bytes.get()))}}, false, command->delivery);
      return Json::object();
    }
    if (api == "wx.writeBLECharacteristicValue") {
      auto& current = characteristic(payload); auto type = payload.value("writeType", "write");
      if (!has_flag(current, type == "writeNoResponse" ? "write-without-response" : "write")) throw std::invalid_argument("Bluetooth characteristic does not support requested write type");
      auto bytes = binary_bytes(payload.at("value")); if (bytes.size() > 512) throw std::invalid_argument("BlueZ GATT writes are limited to 512 bytes");
      GVariantBuilder options; g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}")); g_variant_builder_add(&options, "{sv}", "type", g_variant_new_string(type == "writeNoResponse" ? "command" : "request"));
      call(current.path, "org.bluez.GattCharacteristic1", "WriteValue", g_variant_new("(@aya{sv})", g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, bytes.data(), bytes.size(), 1), &options), int(command->timeout_ms)); return Json::object();
    }
    if (api == "wx.notifyBLECharacteristicValueChange") {
      auto& current = characteristic(payload); if (current.native_notifying) throw std::invalid_argument("Bluetooth notifications are already enabled");
      if (!has_flag(current, "notify") && !has_flag(current, "indicate")) throw std::invalid_argument("Bluetooth characteristic does not notify or indicate");
      current.notification = state->begin_notification(ble_characteristic_key(payload)); current.native_notifying = true;
      try {call(current.path, "org.bluez.GattCharacteristic1", "StartNotify", nullptr, int(command->timeout_ms));}
      catch (...) {stop_notification(current); throw;}
      if (state->stopping() || command->cancelled() || !current.notification->load()) stop_notification(current);
      return Json::object();
    }
    throw std::invalid_argument("BlueZ Bluetooth operation is unsupported");
  }
  void poll(const std::shared_ptr<NativeBle>& state) override {
    for (unsigned count = 0; count < 64 && g_main_context_pending(context_); ++count) g_main_context_iteration(context_, FALSE);
    if (g_dbus_connection_is_closed(bus_.get())) throw std::runtime_error("BlueZ private connection was closed");
    if (state->discovering) {auto dirty = std::move(dirty_); dirty_.clear(); for (const auto& path : dirty) {try {publish_device(state, path);} catch (const std::exception& error) {state->fault(error.what()); break;}}}
  }
  bool cleanup(const std::shared_ptr<NativeBle>& state) override {
    if (!bus_) {if (context_) {g_main_context_pop_thread_default(context_); g_main_context_unref(context_); context_ = nullptr;} return true;}
    if (g_dbus_connection_is_closed(bus_.get()) && !connections_.empty()) {
      // Keep the exact exclusive device leases while reconnecting the control
      // transport. A lost D-Bus peer does not prove the global ACL is closed.
      unsubscribe_signals(); if (!signals_released()) return false;
      bus_ = dbus_private_system(); scanning_ = false;
    }
    if (!connections_.empty()) {auto id = connections_.begin()->first; if (!disconnect(state, id)) return false;}
    if (!connections_.empty()) return false;
    try {stop_discovery(state);} catch (const std::exception& error) {state->cleanup_failed(error.what());}
    for (auto& [key, current] : characteristics_) {try {stop_notification(current);} catch (const std::exception& error) {state->cleanup_failed(error.what()); return false;}}
    unsubscribe_signals(); if (!signals_released()) return false;
    GError* error = nullptr;
    if (!g_dbus_connection_is_closed(bus_.get()) && !g_dbus_connection_close_sync(bus_.get(), nullptr, &error)) {
      auto message = dbus_error(error, "BlueZ private connection close is pending"); if (!g_dbus_connection_is_closed(bus_.get())) {state->cleanup_failed(message); return false;}
    }
    // The private peer owns its discovery/notification sessions. Exclusive
    // device ACL cleanup was acknowledged separately above; disconnecting this
    // private peer is never substituted for a global device disconnect.
    bus_.reset(); characteristics_.clear(); scanning_ = false;
    if (context_) {while (g_main_context_pending(context_)) g_main_context_iteration(context_, FALSE); g_main_context_pop_thread_default(context_); g_main_context_unref(context_); context_ = nullptr;}
    state->cleanup_succeeded(); return true;
  }
};
inline BlePlatform bluez_bluetooth_platform(const DeviceApiOptions& options) {
  if (!dbus_service_available("org.bluez")) return {};
  BlePlatform platform;
  platform.apis = {"wx.openBluetoothAdapter", "wx.closeBluetoothAdapter", "wx.getBluetoothAdapterState", "wx.startBluetoothDevicesDiscovery", "wx.stopBluetoothDevicesDiscovery", "wx.getBluetoothDevices"};
  platform.events = {"BluetoothAdapterStateChange", "BluetoothDeviceFound"};
  if (options.linux_bluetooth_exclusive) {
    for (auto api : {"wx.getConnectedBluetoothDevices", "wx.createBLEConnection", "wx.closeBLEConnection", "wx.getBLEDeviceServices", "wx.getBLEDeviceCharacteristics", "wx.readBLECharacteristicValue", "wx.writeBLECharacteristicValue", "wx.notifyBLECharacteristicValueChange"}) platform.apis.push_back(api);
    platform.events.push_back("BLEConnectionStateChange"); platform.events.push_back("BLECharacteristicValueChange");
  }
  platform.factory = [] {return std::make_unique<BluezBackend>();}; return platform;
}
}  // namespace weapp::desktop::detail
