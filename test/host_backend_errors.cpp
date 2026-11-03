#include <algorithm>
#include <cctype>
#include <iostream>
#include <type_traits>

#include "host_services_internal.hpp"

using namespace weapp::desktop;
using namespace weapp::desktop::detail;
namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
Json fields(const HostBackendFailure& value, const std::string& api = "wx.login") {
  return host_api_error_details(api, value.status, value.api_details);
}
void envelope_precedence_and_private_namespaces() {
  const Json response = {
      {"code", "envelope-code"}, {"message", "envelope message"},
      {"errCode", 10}, {"errMsg", "envelope:fail"},
      {"requestId", "request-1"}, {"collision", "envelope"},
      {"details", {{"collision", "envelope-details"}, {"envelopeDetails", true},
                   {"nullable", "retained"}, {"code", "excluded"}}},
      {"error", {{"code", "consent_required"}, {"message", "consent required"},
                 {"errCode", 4017}, {"errMsg", "login:fail provider original"},
                 {"collision", "nested"}, {"nullable", nullptr},
                 {"extension", {{"items", Json::array({1, "two", false})}}},
                 {"details", {{"collision", "nested-details"},
                              {"provider", "identity-provider"}, {"errCode", 12}}}}}};
  auto parsed = parse_host_backend_error(response);
  require(parsed.backend_code == "consent_required" &&
              parsed.status.code == "ERR_PERMISSION_DENIED" &&
              parsed.status.message == "consent required", "nested classification was lost");
  auto projected = fields(parsed);
  require(projected["errCode"] == 4017 && projected["errMsg"] == "login:fail provider original",
          "nested public API errors were overwritten");
  require(projected["requestId"] == "request-1" && projected["provider"] == "identity-provider" &&
              projected["envelopeDetails"] == true && projected["collision"] == "nested" &&
              projected["nullable"] == "retained", "four-layer extension precedence was lost");
  for (const auto* key : {"code", "message", "error", "details"})
    require(!projected.contains(key), "container fields entered public API details");
  projected["extension"]["items"][0] = "receiver modification";
  require(encode(parsed.api_details)["extension"]["items"][0] == 1,
          "projected API details share mutable storage");
  const auto plain = parse_host_backend_error({{"error", "invalid_app_session"},
      {"message", "expired"}, {"errCode", "E_EXPIRED"},
      {"errMsg", "login:fail expired"}, {"traceId", "plain-trace"}});
  require(plain.status.code == "ERR_PERMISSION_DENIED" && plain.backend_code == "invalid_app_session" &&
              fields(plain)["errCode"] == "E_EXPIRED" && fields(plain)["traceId"] == "plain-trace",
          "string envelope or string errCode was lost");
}
void fallback_and_malformed_fields() {
  for (const auto& bad : {Json::object(), Json(nullptr), Json(false), Json(4017), Json(""), Json(" \t\n")}) {
    const auto parsed = parse_host_backend_error({{"error", bad}});
    require(parsed.backend_code == "ERR_HOST_SERVICES" &&
                parsed.status.message == "Host services request failed" &&
                host_error_category(parsed.status.code) == "invalid_request",
            "malformed object error became a type error or a classification");
  }
  const auto fallback = parse_host_backend_error({{"errMsg", "backend unavailable"}});
  require(fallback.status.message == "backend unavailable", "public errMsg fallback was lost");
  const auto nested = parse_host_backend_error({{"error", {{"details", {
      {"errCode", 4017}, {"errMsg", "checkSession:fail provider"}}}}}});
  require(nested.backend_code == "ERR_HOST_SERVICES" &&
              nested.status.message == "checkSession:fail provider" && fields(nested)["errCode"] == 4017,
          "details-only object errors were lost");
  const auto nulls = parse_host_backend_error({{"error", "invalid_app_session"},
      {"message", "expired"}, {"errCode", nullptr}, {"errMsg", nullptr}});
  require(fields(nulls)["errCode"] == 2001 && fields(nulls)["errMsg"] == "wx.login:fail expired",
          "null API fields prevented classified defaults");
  const auto blank = parse_host_backend_error({{"code", "ERR_CANCELLED"},
      {"message", false}, {"error", {{"code", nullptr}, {"message", " \n"}}},
      {"details", {{"errMsg", ""}, {"errCode", 0}, {"flag", false}}}});
  require(blank.status.code == "ERR_CANCELLED" && fields(blank)["errCode"] == 0 &&
              fields(blank)["flag"] == false && fields(blank)["errMsg"] == "wx.login:fail Host services request failed",
          "blank text or falsy public values were mishandled");
}
void canonical_classification_and_aggregate_compatibility() {
  for (const std::string wire : {"invalid_request", "permission_denied", "timeout", "cancelled", "unsupported"}) {
    auto upper = wire;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    for (const auto& code : {wire, "ERR_" + upper}) {
      const auto parsed = parse_host_backend_error({{"error", {{"code", code},
          {"message", "classified backend failure"}, {"errCode", 4017},
          {"errMsg", "login:fail provider original"}, {"providerId", "identity-provider"}}}});
      require(host_error_category(parsed.status.code) == wire, "canonical error category changed");
      const auto projected = fields(parsed);
      require(projected["errCode"] == 4017 && projected["errMsg"] == "login:fail provider original" &&
                  projected["providerId"] == "identity-provider", "classification rewrote public provider fields");
    }
  }
  for (const std::string alias : {"consent_required", "invalid_host_session", "invalid_app_session",
                                  "host_identity_mismatch", "stale_principal", "app_scope_mismatch", "untrusted_pack"}) {
    auto upper = alias;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    for (const auto& code : {alias, "ERR_" + upper}) {
      const auto parsed = parse_host_backend_error({{"error", code}, {"message", "authorization rejected"}});
      require(host_error_category(parsed.status.code) == "permission_denied" && fields(parsed)["errCode"] == 2001,
              "authorization alias lost its permission category");
    }
  }
  for (const auto* alias : {"unsupported_capability", "ERR_UNSUPPORTED_CAPABILITY"}) {
    const auto parsed = parse_host_backend_error({{"error", alias}, {"errCode", 4017}});
    require(parsed.status.code == "ERR_UNSUPPORTED" && host_error_category(alias) == "unsupported" &&
                fields(parsed)["errCode"] == 4017, "core unsupported alias lost its category");
  }
  static_assert(std::is_aggregate<HostResult<bool>>::value);
  HostResult<bool> previous_shape{Status::error("ERR_TIMEOUT", "previous initializer"), false, "old-code"};
  require(previous_shape.backend_code == "old-code" && previous_shape.api_details.empty(),
          "appending API fields broke three-field aggregate initialization");
}
}  // namespace
int main() {
  try {
    envelope_precedence_and_private_namespaces();
    fallback_and_malformed_fields();
    canonical_classification_and_aggregate_compatibility();
    std::cout << "Desktop backend error parser/projection: 3 C++ groups passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
