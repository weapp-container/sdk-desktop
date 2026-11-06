#pragma once
#include "device_dbus_common.hpp"
#include "device_location_common.hpp"

namespace weapp::desktop::detail {
inline std::string geo_desktop_id(std::string id) {
  if (id.size() > 8 && id.compare(id.size() - 8, 8, ".desktop") == 0) id.resize(id.size() - 8);
  if (id.empty() || id.size() > 255 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
      })) throw std::invalid_argument("GeoClue requires a trusted host desktop-file ID");
  return id;
}
class GeoLocationSource final : public LocationSource {
  std::string desktop_id_;
  DbusConnection bus_;
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
      bus = dbus_private_system(cancelled.get());
      auto acquired = dbus_call(bus, "org.freedesktop.GeoClue2", "/org/freedesktop/GeoClue2/Manager",
          "org.freedesktop.GeoClue2.Manager", "GetClient", nullptr, G_VARIANT_TYPE("(o)"), cancelled.get());
      const char* path = nullptr; g_variant_get(acquired.get(), "(&o)", &path); client = path;
      auto set_property = [&](const char* name, GVariant* value) {
        dbus_call(bus, "org.freedesktop.GeoClue2", client, "org.freedesktop.DBus.Properties", "Set",
            g_variant_new("(ssv)", "org.freedesktop.GeoClue2.Client", name, value), G_VARIANT_TYPE("()"), cancelled.get());
      };
      set_property("DesktopId", g_variant_new_string(desktop_id_.c_str()));
      set_property("RequestedAccuracyLevel", g_variant_new_uint32(state->options.fuzzy ? 4 : state->options.high_accuracy ? 8 : 6));
      set_property("TimeThreshold", g_variant_new_uint32(1));
      dbus_call(bus, "org.freedesktop.GeoClue2", client, "org.freedesktop.GeoClue2.Client", "Start", nullptr,
          G_VARIANT_TYPE("()"), cancelled.get(), int(state->options.timeout_ms));
      state->announce_started();
      const auto first_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(state->options.timeout_ms);
      std::string previous;
      while (!state->cancelled) {
        auto active = dbus_property(bus, "org.freedesktop.GeoClue2", client, "org.freedesktop.GeoClue2.Client", "Active", cancelled.get());
        if (!g_variant_get_boolean(active.get())) throw std::runtime_error("OS GeoClue location authorization is inactive");
        auto location = dbus_property(bus, "org.freedesktop.GeoClue2", client, "org.freedesktop.GeoClue2.Client", "Location", cancelled.get());
        auto current = std::string(g_variant_get_string(location.get(), nullptr));
        if (current != "/" && current != previous) {
          auto reply = dbus_call(bus, "org.freedesktop.GeoClue2", current, "org.freedesktop.DBus.Properties", "GetAll",
              g_variant_new("(s)", "org.freedesktop.GeoClue2.Location"), G_VARIANT_TYPE("(a{sv})"), cancelled.get());
          DbusVariant fields(g_variant_get_child_value(reply.get(), 0), g_variant_unref);
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
      try { dbus_call(bus_, "org.freedesktop.GeoClue2", client_, "org.freedesktop.GeoClue2.Client", "Stop", nullptr, G_VARIANT_TYPE("()")); }
      catch (const std::exception&) {}  // DeleteClient or private disconnect is authoritative.
      try {
        dbus_call(bus_, "org.freedesktop.GeoClue2", "/org/freedesktop/GeoClue2/Manager",
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
      auto message = "GeoClue private connection close is pending: " + dbus_error(error, "unknown D-Bus error");
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
  if (!dbus_service_available("org.freedesktop.GeoClue2")) return {};
  return [desktop_id](const DeviceApiOptions&, const LocationOptions&) { return std::make_unique<GeoLocationSource>(desktop_id); };
}
}  // namespace weapp::desktop::detail
