#include <algorithm>
#include <condition_variable>
#include <cctype>
#include <deque>
#include <limits>
#include <mutex>
#include <regex>

#include "host_services_internal.hpp"

namespace weapp::desktop {
using detail::decode;
using detail::encode;
using detail::Json;
namespace {
Value::Object object(const Json& value) {
  return std::get<Value::Object>(decode(value).data);
}
Status invalid(const std::string& message) {
  return Status::error("ERR_INVALID_REQUEST", message);
}
Status cancelled() {
  return Status::error("ERR_CANCELLED",
                       "Native host service operation was cancelled");
}
std::string kind_name(HostConsentKind kind) {
  return kind == HostConsentKind::Phone ? "phone" : "profile";
}
bool decimal(const std::string& value, bool zero = false) {
  if (value.empty() || value.size() > 20 ||
      (value.size() > 1 && value.front() == '0') ||
      !std::all_of(value.begin(), value.end(),
                   [](char c) { return c >= '0' && c <= '9'; }))
    return false;
  if (value.size() == 20 && value > "18446744073709551615") return false;
  return zero || value != "0";
}
template <class T>
std::shared_future<T> immediate(T value) {
  std::promise<T> promise;
  auto result = promise.get_future().share();
  promise.set_value(std::move(value));
  return result;
}
Json selection(const HostApiContext& context) {
  return {{"miniAppId", context.application.app_id},
          {"packRoot", context.application.pack_root},
          {"expectedPrincipalId", context.account.principal_id},
          {"expectedPrincipalEpoch", context.account.epoch}};
}
HostIdentityState signed_out() {
  return {false,
          false,
          false,
          false,
          {{"hostLoggedIn", false},
           {"appLoggedIn", false},
           {"profileAuthorized", false},
           {"phoneAuthorized", false},
           {"authorizationVersion", "0"}}};
}
bool valid_bearer(const std::string& bearer) {
  return !bearer.empty() && bearer.size() <= 8192 &&
         std::all_of(bearer.begin(), bearer.end(),
                     [](unsigned char c) { return c >= 0x21 && c <= 0x7e; });
}
void require(bool condition, const char* message) {
  if (!condition) throw std::invalid_argument(message);
}
HostIdentityState identity(const Json& value) {
  for (const auto* field :
       {"hostLoggedIn", "appLoggedIn", "profileAuthorized", "phoneAuthorized"})
    require(value.contains(field) && value[field].is_boolean(),
            "Invalid backend identity state");
  return {value.at("hostLoggedIn"), value.at("appLoggedIn"),
          value.at("profileAuthorized"), value.at("phoneAuthorized"),
          object(value)};
}
HostLoginCode code(const Json& value) {
  const auto text = value.at("code").get<std::string>();
  const auto expires = value.at("expiresIn").get<double>();
  require(
      !text.empty() && text.size() <= 1024 && expires > 0 && expires <= 86400,
      "Invalid backend one-time code");
  return {text, expires};
}
HostAccountSwitch receipt(const Value& value) {
  auto j = encode(value);
  HostAccountSwitch out;
  if (j.is_null()) return out;
  out.operation_id = j.at("operationId").get<std::string>();
  out.epoch = j.at("epoch").get<std::string>();
  out.phase = j.at("phase").get<std::string>();
  if (j.contains("sessionId") && !j["sessionId"].is_null())
    out.session_id = j["sessionId"].get<std::string>();
  out.fields = object(j);
  return out;
}
Json load_options(const LoadOptions& options) {
  auto result = encode(options.options);
  if (!options.route.empty()) result["route"] = options.route;
  return result;
}
}  // namespace

namespace detail {
namespace {
std::string nonblank_text(const Json& value, const char* field) {
  if (!value.is_object() || !value.contains(field) || !value[field].is_string())
    return {};
  const auto text = value[field].get<std::string>();
  return std::any_of(text.begin(), text.end(), [](unsigned char c) {
           return !std::isspace(c);
         }) ? text : std::string{};
}
Json child_object(const Json& value, const char* field) {
  return value.is_object() && value.contains(field) && value[field].is_object()
             ? value[field] : Json::object();
}
}  // namespace
std::string host_error_category(const std::string& code) {
  auto normalized = code.compare(0, 4, "ERR_") == 0 ? code.substr(4) : code;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (normalized == "permission_denied" || normalized == "consent_required" ||
      normalized == "invalid_host_session" || normalized == "invalid_app_session" ||
      normalized == "host_identity_mismatch" || normalized == "stale_principal" ||
      normalized == "app_scope_mismatch" || normalized == "untrusted_pack")
    return "permission_denied";
  if (normalized == "timeout" || normalized == "cancelled" || normalized == "unsupported")
    return normalized;
  if (normalized == "unsupported_capability") return "unsupported";
  return "invalid_request";
}
HostBackendFailure parse_host_backend_error(const Json& response) {
  const auto nested = child_object(response, "error");
  auto code = nonblank_text(nested, "code");
  if (code.empty()) code = nonblank_text(response, "code");
  if (code.empty()) code = nonblank_text(response, "error");
  if (code.empty()) code = "ERR_HOST_SERVICES";
  Json fields = Json::object();
  // Non-null higher-priority fields win. Container fields never enter the
  // public API namespace, regardless of which envelope supplied them.
  for (const auto& source : {response, child_object(response, "details"),
                             child_object(nested, "details"), nested}) {
    if (!source.is_object()) continue;
    for (auto field = source.begin(); field != source.end(); ++field)
      if (field.key() != "code" && field.key() != "message" &&
          field.key() != "error" && field.key() != "details" && !field.value().is_null())
        fields[field.key()] = field.value();
  }
  auto message = nonblank_text(nested, "message");
  if (message.empty()) message = nonblank_text(response, "message");
  if (message.empty()) message = nonblank_text(fields, "errMsg");
  if (message.empty()) message = "Host services request failed";
  const auto category = host_error_category(code);
  auto status = category == "invalid_request" ? "ERR_HOST_SERVICES" : "ERR_" + category;
  if (code == "invalid_request" || code == "ERR_INVALID_REQUEST") status = "ERR_INVALID_REQUEST";
  std::transform(status.begin(), status.end(), status.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return {Status::error(status, message), code, object(fields)};
}
Json host_api_error_details(const std::string& api, const Status& status,
                            const Value::Object& api_details) {
  auto fields = encode(api_details);
  if (nonblank_text(fields, "errMsg").empty())
    fields["errMsg"] = api + ":fail " + status.message;
  if (!fields.contains("errCode") || fields["errCode"].is_null())
    fields["errCode"] = host_error_category(status.code) == "permission_denied" ? 2001 : -1;
  return fields;
}
}  // namespace detail

Value::Object HostApiContext::value() const {
  Json out = encode(wire);
  out["application"] = {{"hostAppId", application.host_app_id},
                        {"tenantId", application.tenant_id},
                        {"publisherId", application.publisher_id},
                        {"appId", application.app_id},
                        {"environment", application.environment},
                        {"version", application.version},
                        {"packRoot", application.pack_root}};
  out["account"] = {{"principalId", account.principal_id.empty()
                                        ? Json(nullptr)
                                        : Json(account.principal_id)},
                    {"epoch", account.epoch}};
  if (!session_id.empty()) out["sessionId"] = session_id;
  if (!call_id.empty()) out["callId"] = call_id;
  if (!api.empty()) out["api"] = api;
  if (!deadline_ns.empty()) out["deadlineNs"] = deadline_ns;
  if (!cancel_token.empty()) out["cancelToken"] = cancel_token;
  return object(out);
}
Status HostApiContext::parse(const Value::Object& input, HostApiContext& out) {
  try {
    const auto j = encode(input), a = j.at("application"), u = j.at("account");
    HostApiContext next;
    next.wire = input;
    auto text = [](const Json& parent, const char* key) {
      auto s = parent.at(key).get<std::string>();
      require(!s.empty() && s.size() <= 1024,
              "Missing trusted native identity field");
      return s;
    };
    next.application = {text(a, "hostAppId"),   text(a, "tenantId"),
                        text(a, "publisherId"), text(a, "appId"),
                        text(a, "environment"), text(a, "version"),
                        text(a, "packRoot")};
    require(std::regex_match(next.application.pack_root,
                             std::regex("[a-f0-9]{64}")),
            "Verified SHA-256 pack root required");
    require(next.application.environment == "develop" ||
                next.application.environment == "trial" ||
                next.application.environment == "release",
            "Invalid trusted environment");
    next.account = {u.at("principalId").is_null() ? "" : text(u, "principalId"),
                    text(u, "epoch")};
    require(decimal(next.account.epoch),
            "Account epoch must be an exact positive decimal u64");
    next.session_id = j.value("sessionId", "");
    next.call_id = j.value("callId", "");
    next.api = j.value("api", "");
    if (j.contains("deadlineNs") && !j["deadlineNs"].is_null()) {
      next.deadline_ns = j["deadlineNs"].get<std::string>();
      require(decimal(next.deadline_ns, true), "Invalid exact deadline");
    }
    if (j.contains("cancelToken") && !j["cancelToken"].is_null())
      next.cancel_token = j["cancelToken"].get<std::string>();
    out = std::move(next);
    return {};
  } catch (const std::exception& error) {
    return invalid(error.what());
  }
}

struct HostServicesClient::Impl {
  HostBackendOptions options;
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<std::function<void()>> jobs;
  std::vector<std::thread> workers;
  struct Active {
    HostCancellation cancellation;
    std::shared_ptr<std::atomic<bool>> done;
  };
  std::vector<Active> active;
  bool closed = false;
  explicit Impl(HostBackendOptions value) : options(std::move(value)) {
    for (int i = 0; i < 2; ++i)
      workers.emplace_back([this] {
        for (;;) {
          std::function<void()> job;
          {
            std::unique_lock<std::mutex> lock(mutex);
            ready.wait(lock, [this] { return closed || !jobs.empty(); });
            if (jobs.empty()) return;
            job = std::move(jobs.front());
            jobs.pop_front();
          }
          job();
        }
      });
  }
  ~Impl() {
    close();
    for (auto& worker : workers) worker.join();
  }
  void close() {
    std::vector<Active> cancel;
    {
      std::lock_guard<std::mutex> lock(mutex);
      closed = true;
      cancel = active;
    }
    for (auto& item : cancel)
      if (!item.done->load()) item.cancellation.cancel();
    ready.notify_all();
  }
  bool enqueue(std::function<void()> job, HostCancellation cancellation,
               std::shared_ptr<std::atomic<bool>> done) {
    std::lock_guard<std::mutex> lock(mutex);
    if (closed || jobs.size() >= 128) return false;
    // Keep only pending request cancellation sources; old completed tokens do
    // not retain platform handles because platform hooks hold weak references.
    active.erase(
        std::remove_if(active.begin(), active.end(),
                       [](const auto& item) { return item.done->load(); }),
        active.end());
    if (active.size() >= 256) return false;
    active.push_back({cancellation, std::move(done)});
    jobs.push_back(std::move(job));
    ready.notify_one();
    return true;
  }
  template <class T, class Convert>
  std::shared_future<HostResult<T>> post(const HostApiContext& context,
                                         const std::string& path, Json body,
                                         HostCancellation cancel,
                                         Convert convert) {
    auto promise = std::make_shared<std::promise<HostResult<T>>>();
    auto future = promise->get_future().share();
    try {
      HostApiContext validated;
      auto status = HostApiContext::parse(context.value(), validated);
      if (!status) throw std::invalid_argument(status.message);
      if (context.account.principal_id.empty()) {
        promise->set_value(
            {Status::error("ERR_PERMISSION_DENIED", "Host login required"),
             {},
             {}});
        return future;
      }
      const auto bearer = options.host_token(context);
      require(valid_bearer(bearer), "Valid native host bearer required");
      const auto endpoint = options.endpoint, bytes = body.dump();
      require(bytes.size() <= 262144, "Host request exceeds limit");
      auto done = std::make_shared<std::atomic<bool>>(false);
      auto job = [endpoint, path, bearer, bytes, cancel, convert, promise,
                  done]() mutable {
        HostResult<T> result;
        try {
          if (cancel.cancelled())
            result.status = cancelled();
          else {
            auto response =
                detail::host_http_post(endpoint, path, bearer, bytes, cancel,
                                       std::chrono::milliseconds(15000));
            result.status = response.status;
            if (result.status) {
              auto json = Json::parse(response.body);
              require(json.is_object(), "Backend response must be an object");
              if (response.http_status < 200 || response.http_status >= 300) {
                auto failure = detail::parse_host_backend_error(json);
                result.status = std::move(failure.status);
                result.backend_code = std::move(failure.backend_code);
                result.api_details = std::move(failure.api_details);
              } else
                result.value = convert(json);
            }
          }
        } catch (const std::exception& error) {
          result.status = invalid(error.what());
        }
        if (cancel.cancelled()) result.status = cancelled();
        done->store(true);
        promise->set_value(std::move(result));
      };
      if (!enqueue(std::move(job), cancel, done))
        promise->set_value(
            {Status::error("ERR_RESOURCE_LIMIT",
                           "Host service executor is closed or full"),
             {},
             {}});
    } catch (const std::exception& error) {
      promise->set_value({invalid(error.what()), {}, {}});
    }
    return future;
  }
};
HostServicesClient::HostServicesClient(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
HostServicesClient::~HostServicesClient() { close(); }
std::shared_ptr<HostServicesClient> HostServicesClient::create(
    HostBackendOptions options, Status& status) {
  if (!detail::host_http_available()) {
    status = Status::error("ERR_UNSUPPORTED",
                           "Native host HTTPS transport is not installed");
    return {};
  }
  // A deliberately small origin grammar avoids URL reinterpretation and bearer
  // forwarding. IPv6 is accepted only in bracket notation.
  const std::regex origin(
      R"(^(https|http)://(localhost|[A-Za-z0-9.-]+|\[[0-9A-Fa-f:]+\])(:[0-9]{1,5})?/?$)");
  std::smatch match;
  if (!std::regex_match(options.endpoint, match, origin) ||
      !options.host_token) {
    status =
        invalid("Configure an HTTPS origin and native host token provider");
    return {};
  }
  const auto host = match[2].str();
  if (match[1] != "https" &&
      !(options.allow_loopback_http &&
        (host == "localhost" || host == "127.0.0.1" || host == "[::1]"))) {
    status = invalid("HTTP requires explicit loopback development opt-in");
    return {};
  }
  if (match[3].matched) {
    auto port = std::stoi(match[3].str().substr(1));
    if (port < 1 || port > 65535) {
      status = invalid("Invalid host backend port");
      return {};
    }
  }
  if (options.endpoint.back() == '/') options.endpoint.pop_back();
  status = {};
  return std::shared_ptr<HostServicesClient>(
      new HostServicesClient(std::make_shared<Impl>(std::move(options))));
}
void HostServicesClient::close() { impl_->close(); }
std::shared_future<HostResult<HostIdentityState>>
HostServicesClient::identity_state(const HostApiContext& c,
                                   HostCancellation x) {
  if (c.account.principal_id.empty())
    return immediate(HostResult<HostIdentityState>{{}, signed_out(), {}});
  return impl_->post<HostIdentityState>(c, "/v1/host/identity-state",
                                        selection(c), x, identity);
}
std::shared_future<HostResult<HostLoginCode>> HostServicesClient::login(
    const HostApiContext& c, HostCancellation x) {
  return impl_->post<HostLoginCode>(c, "/v1/host/login-codes", selection(c), x,
                                    code);
}
std::shared_future<HostResult<Value::Object>> HostServicesClient::check_session(
    const HostApiContext& c, HostCancellation x) {
  return impl_->post<Value::Object>(c, "/v1/host/check-session", selection(c),
                                    x, object);
}
std::shared_future<HostResult<HostProfile>> HostServicesClient::profile(
    const HostApiContext& c, HostCancellation x) {
  return impl_->post<HostProfile>(
      c, "/v1/host/profile", selection(c), x, [](const Json& j) {
        HostProfile p;
        if (j.contains("nickName"))
          p.nick_name = j.at("nickName").get<std::string>();
        if (j.contains("avatarUrl"))
          p.avatar_url = j.at("avatarUrl").get<std::string>();
        return p;
      });
}
std::shared_future<HostResult<HostAuthorizationPolicy>>
HostServicesClient::authorization_policy(const HostApiContext& c,
                                         HostConsentKind kind,
                                         HostCancellation x) {
  auto body = selection(c);
  body["kind"] = kind_name(kind);
  return impl_->post<HostAuthorizationPolicy>(
      c, "/v1/host/authorization-policy", body, x, [kind](const Json& j) {
        auto purpose = j.at("purpose").get<std::string>(),
             version = j.at("permissionVersion").get<std::string>();
        require(j.at("kind") == kind_name(kind) && !purpose.empty() &&
                    purpose.size() <= 2048 && !version.empty() &&
                    version.size() <= 256,
                "Invalid registered authorization policy");
        return HostAuthorizationPolicy{kind, purpose, version};
      });
}
std::shared_future<HostResult<Value::Object>> HostServicesClient::consent(
    const HostApiContext& c, const HostAuthorizationPolicy& policy,
    bool granted, HostCancellation x) {
  auto body = selection(c);
  body["kind"] = kind_name(policy.kind);
  body["granted"] = granted;
  if (granted) {
    body["purpose"] = policy.purpose;
    body["permissionVersion"] = policy.permission_version;
  }
  return impl_->post<Value::Object>(c, "/v1/host/consents", body, x, object);
}
std::shared_future<HostResult<HostLoginCode>> HostServicesClient::phone_code(
    const HostApiContext& c, HostCancellation x) {
  return impl_->post<HostLoginCode>(c, "/v1/host/phone-codes", selection(c), x,
                                    code);
}
std::shared_future<HostResult<Value::Object>> HostServicesClient::logout_app(
    const HostApiContext& c, HostCancellation x) {
  return impl_->post<Value::Object>(c, "/v1/host/app-logout", selection(c), x,
                                    object);
}
std::shared_future<HostResult<HostPaymentOrder>>
HostServicesClient::payment_launch(const HostApiContext& c,
                                   const std::string& id, HostCancellation x) {
  if (!std::regex_match(id, std::regex("[A-Za-z0-9-]{1,128}")))
    return immediate(HostResult<HostPaymentOrder>{
        invalid("Invalid backend order ID"), {}, {}});
  return impl_->post<HostPaymentOrder>(
      c, "/v1/payments/orders/" + id + "/launch", selection(c), x,
      [id](const Json& j) {
        HostPaymentOrder order;
        order.order_id = j.at("orderId").get<std::string>();
        order.channel = j.at("channel").get<std::string>();
        order.product = j.at("product").get<std::string>();
        order.status = j.at("status").get<std::string>();
        require(order.order_id == id, "Backend order identity mismatch");
        if (j.contains("launch") && !j["launch"].is_null()) {
          order.launch = object(j["launch"]);
          order.kind = j["launch"].at("kind").get<std::string>();
        }
        return order;
      });
}
std::shared_future<HostResult<Value::Object>>
HostServicesClient::prepare_configuration(Value::Object config) {
  HostApiContext context;
  auto status = HostApiContext::parse(config, context);
  if (!status) return immediate(HostResult<Value::Object>{status, {}, {}});
  if (context.account.principal_id.empty()) {
    auto j = encode(config);
    j["syncValues"]["weappContainer.identity.getState"] =
        encode(signed_out().fields);
    if (!j.contains("apis")) j["apis"] = Json::array();
    if (std::find(j["apis"].begin(), j["apis"].end(),
                  "weappContainer.identity.getState") == j["apis"].end())
      j["apis"].push_back("weappContainer.identity.getState");
    return immediate(HostResult<Value::Object>{{}, object(j), {}});
  }
  // This transform runs on the bounded backend executor, with no Session/UI.
  return impl_->post<Value::Object>(
      context, "/v1/host/identity-state", selection(context), {},
      [config = std::move(config)](const Json& state) mutable {
        (void)identity(state);
        auto j = encode(config);
        if (!j.contains("syncValues")) j["syncValues"] = Json::object();
        j["syncValues"]["weappContainer.identity.getState"] = state;
        if (!j.contains("apis")) j["apis"] = Json::array();
        if (std::find(j["apis"].begin(), j["apis"].end(),
                      "weappContainer.identity.getState") == j["apis"].end())
          j["apis"].push_back("weappContainer.identity.getState");
        return object(j);
      });
}
std::shared_future<AccountCleanupResult> HostServicesClient::prepare_account(
    const AccountCleanupContext& cleanup, HostCancellation cancel) {
  HostApiContext context;
  auto status = HostApiContext::parse(
      {{"application", cleanup.application}, {"account", cleanup.account}},
      context);
  if (!status) return immediate(AccountCleanupResult{status, {}});
  if (context.account.principal_id.empty())
    return immediate(AccountCleanupResult{
        {}, {{"weappContainer.identity.getState", signed_out().fields}}});
  auto promise = std::make_shared<std::promise<AccountCleanupResult>>();
  auto future = promise->get_future().share();
  // Put the HTTP work itself on the bounded executor; do not queue a future
  // waiter there, which could deadlock both workers during rapid switches.
  try {
    const auto bearer = impl_->options.host_token(context),
               endpoint = impl_->options.endpoint,
               body = selection(context).dump();
    require(valid_bearer(bearer), "Valid native host bearer required");
    auto done = std::make_shared<std::atomic<bool>>(false);
    if (!impl_->enqueue(
            [promise, bearer, endpoint, body, cancel, done] {
              AccountCleanupResult result;
              try {
                auto response = detail::host_http_post(
                    endpoint, "/v1/host/identity-state", bearer, body, cancel,
                    std::chrono::milliseconds(15000));
                result.status = response.status;
                if (result.status) {
                  auto j = Json::parse(response.body);
                  if (response.http_status < 200 || response.http_status >= 300)
                    result.status = detail::parse_host_backend_error(j).status;
                  else {
                    (void)identity(j);
                    result.sync_values["weappContainer.identity.getState"] =
                        decode(j);
                  }
                }
              } catch (const std::exception& e) {
                result.status = invalid(e.what());
              }
              if (cancel.cancelled()) result.status = cancelled();
              done->store(true);
              promise->set_value(std::move(result));
            },
            cancel, done))
      promise->set_value(
          {Status::error("ERR_RESOURCE_LIMIT",
                         "Host service executor is closed or full"),
           {}});
  } catch (const std::exception& e) {
    promise->set_value({invalid(e.what()), {}});
  }
  return future;
}

std::shared_ptr<HostConsentPresenter> HostConsentPresenter::native(
    void* parent, Status& status) {
  return detail::host_consent_presenter(parent, status);
}

Status HostAccountManager::switch_account(
    const HostAccount& account,
    const std::map<std::string, LoadOptions>& launches,
    HostAccountSwitch& out) {
  if (!session_ || !decimal(account.epoch))
    return invalid("A session and exact positive account epoch are required");
  Json payload = {{"account",
                   {{"principalId", account.principal_id.empty()
                                        ? Json(nullptr)
                                        : Json(account.principal_id)},
                    {"epoch", account.epoch}}},
                  {"launches", Json::object()}};
  for (const auto& item : launches)
    payload["launches"][item.first] = {{"options", load_options(item.second)}};
  Value value;
  auto status =
      session_->host_services_command("switchAccount", object(payload), &value);
  if (status) try {
      out = receipt(value);
    } catch (const std::exception& e) {
      return invalid(e.what());
    }
  return status;
}
Status HostAccountManager::query(HostAccountSwitch& out,
                                 const std::string& id) {
  if (!session_) return invalid("Session required");
  Value value;
  Value::Object payload;
  if (!id.empty()) payload["operationId"] = id;
  auto status =
      session_->host_services_command("queryAccountSwitch", payload, &value);
  if (status) try {
      out = receipt(value);
    } catch (const std::exception& e) {
      return invalid(e.what());
    }
  return status;
}
Status HostAccountManager::cancel(const std::string& id) {
  return session_ ? session_->host_services_command("cancelAccountSwitch",
                                                    {{"operationId", id}})
                  : invalid("Session required");
}
Status HostAccountManager::retry(const LoadOptions& options) {
  return session_ ? session_->host_services_command(
                        "retryAccountSwitch",
                        {{"options", decode(load_options(options))}})
                  : invalid("Session required");
}
Status HostAccountManager::delete_local_data(const std::string& principal,
                                             uint64_t& count) {
  if (!session_) return invalid("Session required");
  Value result;
  auto status = session_->host_services_command(
      "deleteAccountData", {{"principalId", principal}}, &result);
  if (status) try {
      count = encode(result).at("deletedScopes").get<uint64_t>();
    } catch (const std::exception& e) {
      return invalid(e.what());
    }
  return status;
}
}  // namespace weapp::desktop
