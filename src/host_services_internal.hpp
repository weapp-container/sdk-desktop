#pragma once
#include "internal.hpp"
#include "weapp/host_services.hpp"

namespace weapp::desktop::detail {
struct HostHttpResponse {
  Status status;
  int http_status = 0;
  std::string body;
};
HostHttpResponse host_http_post(const std::string& endpoint,
                                const std::string& path,
                                const std::string& bearer,
                                const std::string& body, HostCancellation,
                                std::chrono::milliseconds timeout);
bool host_http_available();
std::shared_ptr<HostConsentPresenter> host_consent_presenter(void*, Status&);
}  // namespace weapp::desktop::detail
