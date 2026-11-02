#pragma once
#include <gio/gio.h>
#include "device_location_common.hpp"

namespace weapp::desktop::detail {
using GeoVariant = std::unique_ptr<GVariant, decltype(&g_variant_unref)>;
using GeoBus = std::shared_ptr<GDBusConnection>;
inline std::string geo_error(GError* error, const char* fallback) {
  std::string message = error ? error->message : fallback;
  if (error) g_error_free(error); return message;
}
inline GeoVariant geo_call(const GeoBus& bus, const char* service, const std::string& path,
    const char* interface, const char* method, GVariant* arguments, const GVariantType* expected,
    GCancellable* cancelled = nullptr, int timeout_ms = 5000) {
  GError* error = nullptr;
  auto value = g_dbus_connection_call_sync(bus.get(), service, path.c_str(), interface, method,
      arguments, expected, G_DBUS_CALL_FLAGS_NONE, timeout_ms, cancelled, &error);
  if (!value) throw std::runtime_error(geo_error(error, "GeoClue service operation failed"));
  return GeoVariant(value, g_variant_unref);
}
inline GeoVariant geo_property(const GeoBus& bus, const std::string& path, const char* interface,
    const char* property, GCancellable* cancelled) {
  auto reply = geo_call(bus, "org.freedesktop.GeoClue2", path, "org.freedesktop.DBus.Properties", "Get",
      g_variant_new("(ss)", interface, property), G_VARIANT_TYPE("(v)"), cancelled);
  GVariant* value = nullptr; g_variant_get(reply.get(), "(v)", &value); return GeoVariant(value, g_variant_unref);
}
inline std::string geo_desktop_id(std::string id) {
  if (id.size() > 8 && id.compare(id.size() - 8, 8, ".desktop") == 0) id.resize(id.size() - 8);
  if (id.empty() || id.size() > 255 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
      })) throw std::invalid_argument("GeoClue requires a trusted host desktop-file ID");
  return id;
}
inline bool geo_service_available() {
  try {
    GError* error = nullptr;
    auto raw = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    if (!raw) { if (error) g_error_free(error); return false; }
    GeoBus bus(raw, [](GDBusConnection* value) { g_object_unref(value); });
    auto owner = geo_call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
        g_variant_new("(s)", "org.freedesktop.GeoClue2"), G_VARIANT_TYPE("(b)"), nullptr, 1000);
    gboolean present = FALSE; g_variant_get(owner.get(), "(b)", &present); if (present) return true;
    auto names = geo_call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListActivatableNames",
        nullptr, G_VARIANT_TYPE("(as)"), nullptr, 1000);
    GeoVariant array(g_variant_get_child_value(names.get(), 0), g_variant_unref);
    GVariantIter iterator; g_variant_iter_init(&iterator, array.get()); const char* name = nullptr;
    while (g_variant_iter_next(&iterator, "&s", &name)) if (std::string(name) == "org.freedesktop.GeoClue2") return true;
  } catch (const std::exception&) {}
  return false;
}
class GeoLocationSource final : public LocationSource {
  std::string desktop_id_;
  GeoBus bus_;
  std::string client_;
 public:
  explicit GeoLocationSource(std::string id) : desktop_id_(std::move(id)) {}
  void run(const std::shared_ptr<NativeLocation>& state) override {
    auto cancelled = std::shared_ptr<GCancellable>(g_cancellable_new(), [](GCancellable* value) { g_object_unref(value); });
    auto& bus = bus_; auto& client = client_;
    std::exception_ptr failure;
    try {
      { std::lock_guard<std::mutex> guard(state->mutex); state->interrupt = [cancelled] { g_cancellable_cancel(cancelled.get()); }; }
      if (state->cancelled) g_cancellable_cancel(cancelled.get());
      GError* error = nullptr;
      auto address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SYSTEM, cancelled.get(), &error);
      if (!address) throw std::runtime_error(geo_error(error, "system message bus unavailable"));
      auto address_owner = std::unique_ptr<gchar, decltype(&g_free)>(address, g_free);
      auto raw = g_dbus_connection_new_for_address_sync(address,
          GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
          nullptr, cancelled.get(), &error);
      if (!raw) throw std::runtime_error(geo_error(error, "private GeoClue connection unavailable"));
      bus = GeoBus(raw, [](GDBusConnection* value) { g_object_unref(value); });
      g_dbus_connection_set_exit_on_close(raw, FALSE);
      auto acquired = geo_call(bus, "org.freedesktop.GeoClue2", "/org/freedesktop/GeoClue2/Manager",
          "org.freedesktop.GeoClue2.Manager", "GetClient", nullptr, G_VARIANT_TYPE("(o)"), cancelled.get());
      const char* path = nullptr; g_variant_get(acquired.get(), "(&o)", &path); client = path;
      auto set_property = [&](const char* name, GVariant* value) {
        geo_call(bus, "org.freedesktop.GeoClue2", client, "org.freedesktop.DBus.Properties", "Set",
            g_variant_new("(ssv)", "org.freedesktop.GeoClue2.Client", name, value), G_VARIANT_TYPE("()"), cancelled.get());
      };
      set_property("DesktopId", g_variant_new_string(desktop_id_.c_str()));
      set_property("RequestedAccuracyLevel", g_variant_new_uint32(state->options.fuzzy ? 4 : state->options.high_accuracy ? 8 : 6));
      set_property("TimeThreshold", g_variant_new_uint32(1));
      geo_call(bus, "org.freedesktop.GeoClue2", client, "org.freedesktop.GeoClue2.Client", "Start", nullptr,
          G_VARIANT_TYPE("()"), cancelled.get(), int(state->options.timeout_ms));
      state->announce_started();
      const auto first_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(state->options.timeout_ms);
      std::string previous;
      while (!state->cancelled) {
        auto active = geo_property(bus, client, "org.freedesktop.GeoClue2.Client", "Active", cancelled.get());
        if (!g_variant_get_boolean(active.get())) throw std::runtime_error("OS GeoClue location authorization is inactive");
        auto location = geo_property(bus, client, "org.freedesktop.GeoClue2.Client", "Location", cancelled.get());
        auto current = std::string(g_variant_get_string(location.get(), nullptr));
        if (current != "/" && current != previous) {
          auto reply = geo_call(bus, "org.freedesktop.GeoClue2", current, "org.freedesktop.DBus.Properties", "GetAll",
              g_variant_new("(s)", "org.freedesktop.GeoClue2.Location"), G_VARIANT_TYPE("(a{sv})"), cancelled.get());
          GeoVariant fields(g_variant_get_child_value(reply.get(), 0), g_variant_unref);
          double latitude, longitude, accuracy;
          if (!g_variant_lookup(fields.get(), "Latitude", "d", &latitude) || !g_variant_lookup(fields.get(), "Longitude", "d", &longitude) ||
              !g_variant_lookup(fields.get(), "Accuracy", "d", &accuracy)) throw std::runtime_error("GeoClue position lacks required coordinates");
          Json value = {{"latitude", latitude}, {"longitude", longitude}, {"accuracy", accuracy}, {"horizontalAccuracy", accuracy}};
          double metadata;
          if (g_variant_lookup(fields.get(), "Altitude", "d", &metadata) && metadata != -G_MAXDOUBLE) value["altitude"] = metadata;
          if (g_variant_lookup(fields.get(), "Speed", "d", &metadata) && metadata >= 0) value["speed"] = metadata;
          state->position(std::move(value)); previous = current;
          if (!state->options.continuous) break;
        }
        if (previous.empty() && std::chrono::steady_clock::now() >= first_deadline) throw std::runtime_error("GeoClue location fix timed out");
        if (!state->wait_for(std::chrono::milliseconds(500))) break;
      }
    } catch (...) { failure = std::current_exception(); }
    { std::lock_guard<std::mutex> guard(state->mutex); state->interrupt = {}; }
    if (failure) std::rethrow_exception(failure);
  }
  bool cleanup(const std::shared_ptr<NativeLocation>& state) override {
    if (!bus_) return true;
    if (!g_dbus_connection_is_closed(bus_.get()) && !client_.empty()) {
      try { geo_call(bus_, "org.freedesktop.GeoClue2", client_, "org.freedesktop.GeoClue2.Client", "Stop", nullptr, G_VARIANT_TYPE("()")); }
      catch (const std::exception&) {}  // DeleteClient or private disconnect is authoritative.
      try {
        geo_call(bus_, "org.freedesktop.GeoClue2", "/org/freedesktop/GeoClue2/Manager",
            "org.freedesktop.GeoClue2.Manager", "DeleteClient", g_variant_new("(o)", client_.c_str()), G_VARIANT_TYPE("()"));
        client_.clear();
      } catch (const std::exception&) {
        // GeoClue Manager explicitly permits disconnecting the connection used
        // to communicate with it instead of DeleteClient. This source owns a
        // private connection, including any late GetClient/Start reply.
        // https://www.freedesktop.org/software/geoclue/docs/gdbus-org.freedesktop.GeoClue2.Manager.html
      }
    }
    GError* error = nullptr;
    if (!g_dbus_connection_is_closed(bus_.get()) && !g_dbus_connection_close_sync(bus_.get(), nullptr, &error)) {
      auto message = "GeoClue private connection close is pending: " + geo_error(error, "unknown D-Bus error");
      if (!g_dbus_connection_is_closed(bus_.get())) { state->cleanup_failed(message); return false; }
    }
    // A confirmed private disconnect also disposes the peer-owned client when
    // explicit deletion failed. Retain the same bus on an uncertain close so
    // cleanup can retry and stop/retirement cannot acknowledge early.
    bus_.reset(); client_.clear(); state->cleanup_succeeded(); return true;
  }
};
inline LocationFactory geo_location_factory(const DeviceApiOptions& options) {
  if (options.linux_desktop_id.empty()) return {};
  auto desktop_id = geo_desktop_id(options.linux_desktop_id);
  if (!geo_service_available()) return {};
  return [desktop_id](const DeviceApiOptions&, const LocationOptions&) { return std::make_unique<GeoLocationSource>(desktop_id); };
}
}  // namespace weapp::desktop::detail
