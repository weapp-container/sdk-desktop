#pragma once
#include <gio/gio.h>
#include "device_apis_common.hpp"
namespace weapp::desktop::detail {
using DbusVariant = std::unique_ptr<GVariant, decltype(&g_variant_unref)>;
using DbusConnection = std::shared_ptr<GDBusConnection>;
inline std::string dbus_error(GError* error, const char* fallback) {
  std::string message = error ? error->message : fallback;
  if (error) g_error_free(error); return message;
}
inline DbusVariant dbus_call(const DbusConnection& bus, const char* service, const std::string& path,
    const char* interface, const char* method, GVariant* arguments, const GVariantType* expected,
    GCancellable* cancelled = nullptr, int timeout_ms = 5000) {
  GError* error = nullptr;
  auto value = g_dbus_connection_call_sync(bus.get(), service, path.c_str(), interface, method,
      arguments, expected, G_DBUS_CALL_FLAGS_NONE, timeout_ms, cancelled, &error);
  if (!value) throw std::runtime_error(dbus_error(error, "native D-Bus service operation failed"));
  return DbusVariant(value, g_variant_unref);
}
inline DbusVariant dbus_property(const DbusConnection& bus, const char* service, const std::string& path,
    const char* interface, const char* property, GCancellable* cancelled = nullptr) {
  auto reply = dbus_call(bus, service, path, "org.freedesktop.DBus.Properties", "Get",
      g_variant_new("(ss)", interface, property), G_VARIANT_TYPE("(v)"), cancelled);
  GVariant* value = nullptr; g_variant_get(reply.get(), "(v)", &value); return DbusVariant(value, g_variant_unref);
}
inline DbusConnection dbus_private_system(GCancellable* cancelled = nullptr) {
  GError* error = nullptr;
  auto address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SYSTEM, cancelled, &error);
  if (!address) throw std::runtime_error(dbus_error(error, "system message bus unavailable"));
  auto address_owner = std::unique_ptr<gchar, decltype(&g_free)>(address, g_free);
  auto raw = g_dbus_connection_new_for_address_sync(address,
      GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
      nullptr, cancelled, &error);
  if (!raw) throw std::runtime_error(dbus_error(error, "private native D-Bus connection unavailable"));
  g_dbus_connection_set_exit_on_close(raw, FALSE);
  return DbusConnection(raw, [](GDBusConnection* value) {g_object_unref(value);});
}
inline bool dbus_service_available(const std::string& name) {
  try {
    GError* error = nullptr; auto raw = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    if (!raw) {if (error) g_error_free(error); return false;}
    DbusConnection bus(raw, [](GDBusConnection* value) {g_object_unref(value);});
    auto owner = dbus_call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
        g_variant_new("(s)", name.c_str()), G_VARIANT_TYPE("(b)"), nullptr, 1000);
    gboolean present = FALSE; g_variant_get(owner.get(), "(b)", &present); if (present) return true;
    auto names = dbus_call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListActivatableNames",
        nullptr, G_VARIANT_TYPE("(as)"), nullptr, 1000);
    DbusVariant array(g_variant_get_child_value(names.get(), 0), g_variant_unref);
    GVariantIter iterator; g_variant_iter_init(&iterator, array.get()); const char* value = nullptr;
    while (g_variant_iter_next(&iterator, "&s", &value)) if (name == value) return true;
  } catch (const std::exception&) {}
  return false;
}
}  // namespace weapp::desktop::detail
