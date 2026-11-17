#pragma once
#include "device_apis_common.hpp"
namespace weapp::desktop::detail::test {
class CoreSession {
  weapp_container_sdk_session* session_ = nullptr;
  static Json consume(weapp_container_sdk_buffer& buffer) {
    auto result = Json::parse(buffer.data, buffer.data + buffer.len);
    weapp_container_sdk_buffer_free(&buffer); return result;
  }
 public:
  CoreSession() {
    const std::string config = R"({"engine":"quickjs","gpuRequired":false})";
    weapp_container_sdk_buffer buffer{};
    auto code = weapp_container_sdk_session_new(reinterpret_cast<const uint8_t*>(config.data()), config.size(), &session_, &buffer);
    auto result = consume(buffer);
    if (code != WEAPP_CONTAINER_SDK_OK || !session_) throw std::runtime_error("core test session: " + result.dump());
  }
  ~CoreSession() {
    if (session_) { weapp_container_sdk_buffer buffer{}; weapp_container_sdk_session_destroy(&session_, &buffer); weapp_container_sdk_buffer_free(&buffer); }
  }
  Json configure(const Json& config) {
    const std::string operation = "configureHostServices", payload = config.dump();
    weapp_container_sdk_buffer buffer{};
    weapp_container_sdk_session_command(session_, reinterpret_cast<const uint8_t*>(operation.data()), operation.size(),
        reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), &buffer);
    return consume(buffer);
  }
};
inline void configure_actual_core_catalog(const DeviceApis& devices, const Json& application, const Json& account) {
  auto configuration = encode(devices.configuration());
  configuration["application"] = application; configuration["account"] = account;
  CoreSession core;
  auto accepted = core.configure(configuration);
  if (!accepted.value("ok", false)) throw std::runtime_error("native device configuration rejected by core: " + accepted.dump());
  auto unknown = configuration;
  unknown["apis"].push_back("object:SocketTask.offOpen");
  if (core.configure(unknown).value("ok", false)) throw std::runtime_error("core accepted an API absent from its locked catalog");
  if (!core.configure(configuration).value("ok", false)) throw std::runtime_error("rejected catalog update corrupted the previous configuration");
}
}
