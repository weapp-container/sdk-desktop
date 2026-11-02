#pragma once
#include "weapp/desktop.hpp"

namespace weapp::desktop {
// Host configuration only. The runtime, not guest options, supplies application
// and account to calls. Network origins are explicit host policy; empty denies.
struct DeviceApiOptions {
  std::string private_root;
  // Populated from the core managedFileScope lease; never guest paths.
  std::string managed_scope_root, auxiliary_root;
  Value::Object application, account;
  std::vector<std::string> allowed_network_origins;
  void* native_parent = nullptr;
  // Installed host desktop-file ID used by GeoClue authorization. Empty keeps
  // Linux location routes disabled; guest request parameters cannot set it.
  std::string linux_desktop_id;
};

// Reusable with an owned Session or an external/Flutter native session. Commands
// require the creating thread. OS callbacks only enqueue data for pump().
class DeviceApis {
 public:
  using Command = std::function<Status(const std::string&, const Value::Object&, Value*)>;
  static std::unique_ptr<DeviceApis> create(const DeviceApiOptions&, Command, Status&);
  virtual ~DeviceApis() = default;
  virtual Value::Object configuration() const = 0;
  virtual bool dispatch(const Event&) = 0;
  virtual Status pump() = 0;
  virtual Status retire() = 0;
  virtual Status set_native_parent(void*) = 0;
  virtual Status set_foreground(bool) = 0;
  virtual Status update_components(const Value::Array&) = 0;
};
}  // namespace weapp::desktop
