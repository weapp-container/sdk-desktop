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
struct HostBackendFailure {
  Status status;
  std::string backend_code;
  Value::Object api_details;
};
// These functions share the exact HTTP/parser and JS completion boundary.
HostBackendFailure parse_host_backend_error(const Json&);
std::string host_error_category(const std::string&);
Json host_api_error_details(const std::string& api, const Status&,
                            const Value::Object&);
}  // namespace weapp::desktop::detail
