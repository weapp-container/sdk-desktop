#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#include "native_test_host.hpp"

#include "host_services_internal.hpp"
using namespace weapp::desktop;
using namespace weapp::desktop::detail;
namespace {
void require(bool value, const std::string& message) {
  if (!value) throw std::runtime_error(message);
}
void ok(const Status& status) {
  require(bool(status), status.code + ": " + status.message);
}
Value::Object object(const Json& value) {
  return std::get<Value::Object>(decode(value).data);
}
void control(const Json& input, const std::string& path, const Json& value) {
  auto response =
      host_http_post(input.at("endpoint"), path, input.at("control"),
                     value.dump(), {}, std::chrono::seconds(3));
  ok(response.status);
  require(response.http_status == 200,
          "Reference fixture control failed: " + response.body);
}
struct ControlledConsent final : HostConsentPresenter {
  bool grant = false;
  HostResult<bool> forced_error;
  size_t presentations = 0;
  std::thread::id owner = std::this_thread::get_id();
  HostConsentRequest last;
  void present(const HostConsentRequest& request, HostCancellation cancellation,
               std::function<void(HostResult<bool>)> complete) override {
    require(owner == std::this_thread::get_id(),
            "Consent escaped the UI owner thread");
    require(!cancellation.cancelled(), "Cancelled consent was presented");
    last = request;
    ++presentations;
    if (!forced_error) complete(forced_error);
    else complete({{}, grant, {}});
  }
};
struct ReferenceChannel final : HostPaymentChannel {
  Json input;
  bool confirm = true;
  HostApiError forced_error;
  size_t launches = 0;
  std::vector<std::future<void>> workers;
  explicit ReferenceChannel(Json value) : input(std::move(value)) {}
  ~ReferenceChannel() {
    for (auto& worker : workers) worker.get();
  }
  bool supports(const std::string& channel, const std::string& product,
                const std::string& kind) const override {
    return channel == "reference" && product == "app" && kind == "reference";
  }
  void launch(const HostPaymentOrder& order, const HostApiContext&,
              HostCancellation cancellation,
              std::function<void(HostApiError)> complete) override {
    ++launches;
    const auto confirm_now = confirm;
    workers.push_back(std::async(
        std::launch::async,
        [input = input, id = order.order_id, cancellation, complete,
         confirm_now, forced_error = forced_error] {
          if (cancellation.cancelled()) {
            complete(
                {Status::error("ERR_CANCELLED", "Reference launch cancelled"),
                 {}});
            return;
          }
          if (!forced_error.status.code.empty()) {
            complete(forced_error);
            return;
          }
          try {
            if (confirm_now)
              control(input, "/fixture/confirm", {{"orderId", id}});
            complete({});
          } catch (const std::exception& e) {
            complete({Status::error("ERR_REFERENCE_TEST", e.what()), {}});
          }
        }));
  }
};
struct Fixture {
  Json input;
  std::shared_ptr<HostServicesClient> backend;
  std::shared_ptr<HostServices> services;
  std::shared_ptr<ControlledConsent> consent =
      std::make_shared<ControlledConsent>();
  std::shared_ptr<ReferenceChannel> payments;
  std::shared_ptr<Session> session;
  std::vector<Json> messages;
  size_t unrelated_events = 0;
  size_t host_token_requests = 0;
  Fixture(const std::string& pack, Json native)
      : input(std::move(native)),
        payments(std::make_shared<ReferenceChannel>(input)) {
    Status status;
    HostBackendOptions backend_options;
    backend_options.endpoint = input.at("endpoint");
    backend_options.allow_loopback_http = true;
    backend_options.host_token = [this](const HostApiContext& context) {
      ++host_token_requests;
      return input.at("tokens")
          .at(context.account.principal_id)
          .get<std::string>();
    };
    backend = HostServicesClient::create(std::move(backend_options), status);
    ok(status);
    require(bool(backend), "Backend was not constructed");
    auto preflight =
        backend->prepare_configuration(object(input.at("configuration"))).get();
    ok(preflight.status);
    require(
        encode(
            preflight.value)["syncValues"]["weappContainer.identity.getState"]
                            ["appLoggedIn"] == true,
        "Preflight did not reflect actual backend app session");
    Config config;
    config.metrics = {2, 1, 1, 2, 1, 0};
    config.host_services = preflight.value;
    session = Session::create(config, status);
    ok(status);
    HostServicesOptions services_options;
    services_options.backend = backend;
    services_options.consent = consent;
    services_options.payments = {payments};
    services_options.allow_reference_payments = true;
    services_options.payment_confirmation_timeout = std::chrono::seconds(1);
    services = HostServices::create(std::move(services_options), status);
    ok(status);
    ok(services->install(session));
    session->on_event([this](const Event& event) {
      if (event.type == EventType::Message)
        messages.push_back(encode(event.fields).at("message"));
      else
        ++unrelated_events;
    });
    ok(session->load(pack));
    wait("start", 0);
  }
  ~Fixture() {
    if (session) {
      session->on_event({});
      auto status = session->close();
      for (int i = 0;
           i < 3000 && session->state().status != SessionState::Closed; ++i) {
        (void)session->pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      if (session->state().status != SessionState::Closed) std::terminate();
    }
    backend->close();
  }
  void pump() { ok(session->pump()); }
  Json wait(const std::string& kind, size_t begin) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
      pump();
      for (size_t n = begin; n < messages.size(); ++n)
        if (messages[n].value("kind", "") == kind) return messages[n];
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Json observed = Json::array();
    for (size_t n = begin; n < messages.size(); ++n)
      observed.push_back(messages[n]);
    throw std::runtime_error("Missing real JS result: " + kind +
                             " observed=" + observed.dump());
  }
  size_t send(const std::string& op, const Json& payload = Json::object()) {
    const auto begin = messages.size();
    ok(session->send_message(decode(Json{{"op", op}, {"payload", payload}})));
    pump();
    return begin;
  }
  HostApiContext context() {
    HostApiContext context;
    ok(services->current_context(context));
    return context;
  }
  template <class T>
  HostResult<T> await(const std::shared_future<HostResult<T>>& future) {
    for (int i = 0; i < 5000 && future.wait_for(std::chrono::seconds(0)) !=
                                    std::future_status::ready;
         ++i) {
      pump();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(
        future.wait_for(std::chrono::seconds(0)) == std::future_status::ready,
        "Native future was not settled by pump");
    return future.get();
  }
  Json state() { return wait("state", send("state")).at("value"); }
};
void transport_guards(const Json& input) {
  Status status;
  for (const auto* endpoint :
       {"http://example.com", "https://host/path", "https://user@host",
        "https://host/?q=1", "https://host/#fragment", "http://127.0.0.2:80",
        "https://host:65536"}) {
    HostBackendOptions options;
    options.endpoint = endpoint;
    options.allow_loopback_http = true;
    options.host_token = [](const auto&) { return "unused"; };
    require(!HostServicesClient::create(options, status) && !status,
            "Invalid bearer origin was accepted");
  }
  HostApiContext context;
  ok(HostApiContext::parse(object(input.at("configuration")), context));
  auto wire = encode(context.value());
  wire["account"]["epoch"] = 9007199254740992.0;
  HostApiContext rejected;
  require(!HostApiContext::parse(object(wire), rejected),
          "Lossy epoch number accepted");
  wire = encode(context.value());
  wire["account"]["epoch"] = "18446744073709551615";
  wire["deadlineNs"] = "18446744073709551615";
  wire["componentTarget"] = {
      {"pageId", "1"}, {"nodeId", "phone"}, {"incarnation", "3"}};
  ok(HostApiContext::parse(object(wire), rejected));
  require(encode(rejected.value()) == wire,
          "Full accepted context was not preserved");
  for (const auto* token : {"redirect-fixture", "oversized-fixture",
                            "challenge-fixture", "slow-fixture"}) {
    HostBackendOptions options;
    options.endpoint = input.at("endpoint");
    options.allow_loopback_http = true;
    options.host_token = [token](const auto&) { return token; };
    auto client = HostServicesClient::create(options, status);
    ok(status);
    HostCancellation cancellation;
    auto future = client->identity_state(context, cancellation);
    if (std::string(token) == "slow-fixture") cancellation.cancel();
    auto response = future.get();
    require(!response,
            "Native transport accepted redirect/oversize/auth/cancel result");
    if (std::string(token) == "slow-fixture")
      require(response.status.code == "ERR_CANCELLED",
              "Cancellation was not preserved");
    client->close();
  }
  HostBackendOptions wrong;
  wrong.endpoint = input.at("endpoint");
  wrong.allow_loopback_http = true;
  wrong.host_token = [&input](const auto&) {
    return input.at("tokens").at("alice").get<std::string>();
  };
  auto mismatch = HostServicesClient::create(wrong, status);
  ok(status);
  context.account = {"bob", "2"};
  auto result = mismatch->identity_state(context).get();
  require(!result && result.backend_code == "host_identity_mismatch",
          "Cross-account host bearer was accepted");
  std::cout << "host-services transport guards: passed\n";
}
Json backend_error(const std::string& code) {
  return {{"code", "envelope-code"}, {"message", "envelope message"},
          {"errCode", 10}, {"errMsg", "envelope:fail"},
          {"requestId", "request-1"}, {"collision", "envelope"},
          {"details", {{"collision", "envelope-details"}, {"envelopeDetails", true},
                       {"retained", "non-null"}}},
          {"error", {{"code", code}, {"message", "classified backend failure"},
                     {"errCode", 4017}, {"errMsg", "provider:fail original"},
                     {"collision", "nested"}, {"retained", nullptr},
                     {"backendCode", "provider-annotation"},
                     {"extension", Json::array({1, "two", false})},
                     {"details", {{"collision", "nested-details"},
                                  {"provider", "identity-provider"}, {"errCode", 12}}}}}};
}
void arm_error(const Json& input, const std::string& path, const Json& body, int skip = 0) {
  control(input, "/fixture/backend-error", {{"path", path}, {"body", body}, {"skip", skip}});
}
void public_fields(const Json& fields) {
  require(fields.at("errCode") == 4017 && fields.at("errMsg") == "provider:fail original" &&
              fields.at("requestId") == "request-1" && fields.at("envelopeDetails") == true &&
              fields.at("provider") == "identity-provider" && fields.at("collision") == "nested" &&
              fields.at("retained") == "non-null" && fields.at("backendCode") == "provider-annotation" &&
              fields.at("extension") == Json::array({1, "two", false}),
          "Actual native HTTP/QuickJS error lost public fields: " + fields.dump());
}
void js_backend_failure(Fixture& f, const std::string& op, const std::string& path,
                        const std::string& backend, const std::string& category,
                        int skip = 0) {
  arm_error(f.input, path, backend_error(backend), skip);
  const auto begin = f.send(op, op == "profile" ? Json{{"desc", "Display registered profile"}} :
                               op == "payment" ? Json{{"orderId", f.input.at("orders").at("pending")}} : Json::object());
  const auto promise = f.wait(op + "-reject", begin);
  const auto callback = f.wait(op + "-failure", begin);
  require(promise.at("error") == callback.at("error"), "callback and Promise received different API failures");
  const auto& error = promise.at("error");
  public_fields(error);
  const std::map<std::string, std::string> public_codes = {
      {"invalid_request", "ERR_INVALID_REQUEST"}, {"permission_denied", "ERR_PERMISSION_DENIED"},
      {"timeout", "ERR_TIMEOUT"}, {"cancelled", "ERR_CANCELLED"},
      {"unsupported", "ERR_UNSUPPORTED_CAPABILITY"}};
  require(error.at("code") == public_codes.at(category) && error.at("message") == "classified backend failure",
          "Actual JS error lost its independent container classification: " + error.dump());
  std::vector<std::string> order;
  for (size_t n = begin; n < f.messages.size(); ++n) {
    const auto kind = f.messages[n].value("kind", "");
    if (kind.rfind(op + "-", 0) == 0) order.push_back(kind);
  }
  require(order == std::vector<std::string>{op + "-failure", op + "-complete", op + "-reject"},
          "Backend failure did not settle fail/complete/Promise exactly once in order");
}
void backend_error_projection(const std::string& pack, const Json& input) {
  Fixture f(pack, input);
  for (const std::string category : {"invalid_request", "permission_denied", "timeout", "cancelled", "unsupported"}) {
    auto upper = category;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    for (const auto& code : {category, "ERR_" + upper})
      js_backend_failure(f, "login", "/v1/host/login-codes", code, category);
  }
  for (const std::string alias : {"consent_required", "invalid_host_session", "invalid_app_session", "host_identity_mismatch", "stale_principal"}) {
    auto upper = alias;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    for (const auto& code : {alias, "ERR_" + upper})
      js_backend_failure(f, "check", "/v1/host/check-session", code, "permission_denied");
  }
  for (const auto* alias : {"unsupported_capability", "ERR_UNSUPPORTED_CAPABILITY"})
    js_backend_failure(f, "login", "/v1/host/login-codes", alias, "unsupported");
  js_backend_failure(f, "info", "/v1/host/profile", "ERR_UNSUPPORTED", "unsupported");
  js_backend_failure(f, "profile", "/v1/host/authorization-policy", "ERR_TIMEOUT", "timeout");
  f.consent->grant = true;
  js_backend_failure(f, "profile", "/v1/host/consents", "ERR_CANCELLED", "cancelled");
  js_backend_failure(f, "login", "/v1/host/identity-state", "ERR_PERMISSION_DENIED", "permission_denied");
  js_backend_failure(f, "logout", "/v1/host/app-logout", "ERR_INVALID_REQUEST", "invalid_request");
  const auto payment_path = "/v1/payments/orders/" + input.at("orders").at("pending").get<std::string>() + "/launch";
  js_backend_failure(f, "payment", payment_path, "ERR_UNSUPPORTED", "unsupported");
  f.payments->confirm = false;
  js_backend_failure(f, "payment", payment_path, "ERR_TIMEOUT", "timeout", 1);
  arm_error(input, "/v1/host/login-codes", {{"error", "invalid_app_session"},
      {"message", "expired"}, {"errCode", "TOKEN_EXPIRED"}, {"errMsg", "login:fail expired"},
      {"traceId", "plain-native-trace"}});
  const auto plain_begin = f.send("login");
  const auto plain_error = f.wait("login-reject", plain_begin).at("error");
  require(plain_error == f.wait("login-failure", plain_begin).at("error") &&
              plain_error.at("code") == "ERR_PERMISSION_DENIED" &&
              plain_error.at("errCode") == "TOKEN_EXPIRED" &&
              plain_error.at("errMsg") == "login:fail expired" &&
              plain_error.at("traceId") == "plain-native-trace",
          "Actual native HTTP string error or string errCode was lost");
  for (const auto* original_code : {"invalid_app_session", "ERR_INVALID_APP_SESSION"}) {
    const Json original = {{"error", original_code}, {"message", "expired before refresh"},
        {"errCode", "TOKEN_EXPIRED"}, {"errMsg", "checkSession:fail expired"},
        {"traceId", "original-business-error"}};
    auto refresh_error = backend_error("ERR_TIMEOUT");
    refresh_error["error"]["errCode"] = 9999;
    refresh_error["error"]["errMsg"] = "identity refresh:fail secondary timeout";
    control(input, "/fixture/backend-error", {{"failures", Json::array({
        {{"path", "/v1/host/check-session"}, {"body", original}},
        {{"path", "/v1/host/identity-state"}, {"body", refresh_error}}})}});
    const auto begin = f.send("check");
    const auto failure = f.wait("check-reject", begin).at("error");
    require(failure == f.wait("check-failure", begin).at("error") &&
                failure.at("code") == "ERR_PERMISSION_DENIED" &&
                failure.at("message") == "expired before refresh" &&
                failure.at("errCode") == "TOKEN_EXPIRED" &&
                failure.at("errMsg") == "checkSession:fail expired" &&
                failure.at("traceId") == "original-business-error" &&
                !failure.contains("requestId"),
            "Best-effort identity refresh replaced the original business failure");
    std::vector<std::string> settled;
    for (size_t n = begin; n < f.messages.size(); ++n) {
      const auto kind = f.messages[n].value("kind", "");
      if (kind.rfind("check-", 0) == 0) settled.push_back(kind);
    }
    require(settled == std::vector<std::string>{"check-failure", "check-complete", "check-reject"},
            "Double backend failure changed the one-settlement boundary");
  }
  // The public native future also retains fields; revocation's second request
  // must forward its own failure rather than keep the first request's metadata.
  arm_error(input, "/v1/host/identity-state", backend_error("ERR_CANCELLED"));
  const auto refresh = f.await(f.services->refresh(f.context()));
  require(!refresh && refresh.status.code == "ERR_CANCELLED" && refresh.backend_code == "ERR_CANCELLED",
          "Native refresh lost backend classification");
  public_fields(encode(refresh.api_details));
  arm_error(input, "/v1/host/consents", backend_error("ERR_UNSUPPORTED"));
  const auto revoke = f.await(f.services->revoke(f.context(), HostConsentKind::Profile));
  require(!revoke && revoke.status.code == "ERR_UNSUPPORTED", "Native revoke lost first-request error");
  public_fields(encode(revoke.api_details));
  arm_error(input, "/v1/host/identity-state", backend_error("ERR_TIMEOUT"));
  const auto revoke_refresh = f.await(f.services->revoke(f.context(), HostConsentKind::Profile));
  require(!revoke_refresh && revoke_refresh.status.code == "ERR_TIMEOUT" && revoke_refresh.backend_code == "ERR_TIMEOUT",
          "Native revoke lost the identity refresh error");
  public_fields(encode(revoke_refresh.api_details));
  arm_error(input, "/v1/host/identity-state", backend_error("ERR_PERMISSION_DENIED"));
  const auto preflight = f.backend->prepare_configuration(object(input.at("configuration"))).get();
  require(!preflight && preflight.status.code == "ERR_PERMISSION_DENIED", "Configuration preflight lost category");
  public_fields(encode(preflight.api_details));
  arm_error(input, "/v1/host/identity-state", backend_error("ERR_PERMISSION_DENIED"));
  const auto config = input.at("configuration");
  const auto cleanup = f.backend->prepare_account({"error-preflight", object(config.at("application")), object(config.at("account"))}).get();
  require(!cleanup.status && cleanup.status.code == "ERR_PERMISSION_DENIED" &&
              cleanup.status.message == "classified backend failure", "Account preflight reclassified object error");
  // Native presenter errors use the same independent API namespace.
  f.consent->forced_error = {Status::error("ERR_CANCELLED", "Native consent cancelled"), false, {},
                             object(Json{{"errCode", "NATIVE_CANCEL"}, {"errMsg", "profile:fail native cancel"}, {"extension", 17}})};
  const auto begin = f.send("profile", {{"desc", "Display registered profile"}});
  const auto decision = f.wait("profile-reject", begin).at("error");
  require(decision["code"] == "ERR_CANCELLED" && decision["errCode"] == "NATIVE_CANCEL" &&
              decision["errMsg"] == "profile:fail native cancel" && decision["extension"] == 17,
          "Native consent decision lost public API fields");
  ok(f.await(f.services->revoke(f.context(), HostConsentKind::Phone)).status);
  std::cout << "host-services real HTTP errors: 29 object callback/Promise routes, string error, 2 double failures, native futures and preflight passed (no component presentation or merchant acceptance)\n";
}
void identity_and_payment(const std::string& pack, const Json& input) {
  Fixture f(pack, input);
  require(f.state()["appLoggedIn"] == true,
          "Synchronous state did not start from reference backend");
  auto begin = f.send("login");
  auto login = f.wait("login-promise", begin);
  std::vector<std::string> order;
  for (size_t n = begin; n < f.messages.size(); ++n) {
    auto kind = f.messages[n].value("kind", "");
    if (kind.rfind("login-", 0) == 0) order.push_back(kind);
  }
  require(order == std::vector<std::string>{"login-callback", "login-complete",
                                            "login-promise"},
          "Callback/complete/Promise settlement order changed");
  control(input, "/fixture/exchange", {{"code", login.at("value").at("code")}});
  auto refreshed = f.await(f.services->refresh(f.context()));
  ok(refreshed.status);
  require(refreshed.value.app_logged_in, "Exchanged app login was not visible");
  f.wait("check-promise", f.send("check"));
  auto failure = f.wait("info-reject", f.send("info"));
  require(failure["error"]["errCode"] == 2001,
          "Unapproved profile disclosure was accepted");
  failure = f.wait("profile-reject", f.send("profile"));
  require(failure["error"].value("errMsg", "").find("non-empty desc") !=
                  std::string::npos &&
              f.consent->presentations == 0,
          "Facade did not reject missing profile description before native UI");
  f.consent->grant = false;
  failure =
      f.wait("profile-reject",
             f.send("profile",
                    {{"desc", "Guest description is not registered policy"}}));
  require(failure["error"]["errCode"] == 2001,
          "Consent denial code was lost: " + failure.dump());
  require(f.state()["profileAuthorized"] == false,
          "Denied consent changed sync grant");
  f.consent->grant = true;
  auto profile =
      f.wait("profile-promise",
             f.send("profile",
                    {{"desc", "Guest description is not registered policy"}}));
  require(profile["value"]["userInfo"]["nickName"] == "alice",
          "Profile did not come from reference host identity");
  require(f.consent->last.policy.purpose == "Display the authorized profile" &&
              f.consent->last.policy.permission_version == "profile-v1",
          "Prompt used guest-supplied purpose/version");
  Value components;
  ok(f.session->host_services_command("hostComponents", {}, &components));
  std::string phone_node;
  for (const auto& node : encode(components))
    if (node.value("tag", "") == "button")
      phone_node = node.at("nodeId").get<std::string>();
  require(!phone_node.empty(),
          "Actual rendered getPhoneNumber button was not bound");
  const auto phone_begin = f.messages.size();
  ok(f.session->host_services_command("authorizePhoneButton",
                                      {{"nodeId", phone_node}}));
  auto phone = f.wait("phone-event", phone_begin);
  require(phone["value"]["code"].is_string(),
          "Phone authorization did not return a one-time code");
  require(
      f.consent->last.context.api == "weappContainer.identity.authorizePhone",
      "Phone consent did not use the runtime's internal button authorization "
      "route");
  auto state = f.state();
  require(
      state["profileAuthorized"] == true && state["phoneAuthorized"] == true,
      "Independent grants were not reflected");
  auto revoke =
      f.await(f.services->revoke(f.context(), HostConsentKind::Profile));
  ok(revoke.status);
  state = f.state();
  require(
      state["profileAuthorized"] == false && state["phoneAuthorized"] == true,
      "Revocation crossed authorization kind boundary");
  bool observed = false;
  for (const auto& message : f.messages)
    if (message.value("kind", "") == "identity") observed = true;
  require(observed, "Actual JS identity subscription did not receive updates");
  std::cout
      << "host-services real backend login, consent and revocation: passed\n";
  const auto confirmed = input.at("orders").at("confirmed");
  auto pay =
      f.wait("payment-promise", f.send("payment", {{"orderId", confirmed}}));
  require(pay["value"]["serverConfirmed"] == true && f.payments->launches == 1,
          "Payment was not confirmed by signed backend notification");
  f.wait("payment-reject", f.send("payment", {{"orderId", confirmed},
                                              {"paySign", "untrusted-jsapi"}}));
  require(f.payments->launches == 1,
          "Guest payment signature reached a channel");
  f.wait("payment-reject",
         f.send("payment", {{"orderId", input.at("orders").at("foreign")}}));
  require(f.payments->launches == 1, "Foreign order reached a channel");
  f.payments->confirm = false;
  failure = f.wait(
      "payment-reject",
      f.send("payment", {{"orderId", input.at("orders").at("pending")}}));
  require(failure["error"]
                  .value("errMsg", "")
                  .find("server payment confirmation") != std::string::npos,
          "Native channel launch was treated as a paid trade");
  f.payments->confirm = false;
  f.payments->forced_error = {
      Status::error("ERR_PAYMENT_DECLINED", "Reference channel declined"),
      {{"errCode", 7001},
       {"errMsg", "wx.requestPayment:fail merchant declined"},
       {"extension", "preserved"},
       {"numericCode", 7001.0}}};
  begin = f.messages.size();
  auto channel_failure = f.wait(
      "payment-failure",
      f.send("payment", {{"orderId", input.at("orders").at("pending")}}));
  require(channel_failure["error"]["errCode"] == 7001,
          "Payment callback lost original numeric errCode");
  require(channel_failure["error"]["errMsg"] ==
              "wx.requestPayment:fail merchant declined",
          "Payment callback rewrote original errMsg");
  require(channel_failure["error"]["extension"] == "preserved",
          "Payment callback lost public extension field");
  auto channel_promise = f.wait("payment-reject", begin);
  require(channel_promise["error"]["errCode"] == 7001,
          "Payment Promise lost original numeric errCode");
  require(channel_promise["error"]["errMsg"] ==
              "wx.requestPayment:fail merchant declined",
          "Payment Promise rewrote original errMsg");
  const auto channel_complete = f.wait("payment-complete", begin);
  require(channel_complete.value("kind", "") == "payment-complete",
          "Payment complete callback did not settle after channel failure");
  std::cout
      << "host-services signed reference payment and pending result: passed\n";
  const auto before_logout = f.context();
  const auto logout_begin = f.send("logout");
  f.wait("start", logout_begin);
  require(f.context().session_id != before_logout.session_id,
          "Application logout reused old JS owner");
  for (size_t n = logout_begin; n < f.messages.size(); ++n)
    require(f.messages[n].value("kind", "").rfind("logout-", 0) != 0,
            "Logout continued in retired JS owner");
  state = f.state();
  require(state["appLoggedIn"] == false && state["hostLoggedIn"] == true,
          "App logout changed host account");
  f.wait("check-reject", f.send("check"));
  const auto old = f.context();
  control(input, "/fixture/delay", Json::object());
  begin = f.send("login");
  HostAccountManager manager(f.session);
  HostAccountSwitch switched;
  ok(manager.switch_account({"bob", "2"}, {}, switched));
  require(!switched.operation_id.empty(), "Typed switch lost operation ID");
  f.wait("start", begin);
  auto now = f.context();
  require(now.account.principal_id == "bob" && now.account.epoch == "2" &&
              now.session_id != old.session_id,
          "Controller did not replace internal identity owner");
  state = f.state();
  require(
      state["profileAuthorized"] == false && state["phoneAuthorized"] == false,
      "New identity inherited grants");
  const auto credentials_before_stale = f.host_token_requests;
  auto stale = f.await(f.services->refresh(old));
  require(!stale && stale.status.code == "ERR_CANCELLED",
          "Old native refresh overwrote new identity snapshot");
  auto stale_revoke = f.await(f.services->revoke(old, HostConsentKind::Phone));
  require(!stale_revoke && stale_revoke.status.code == "ERR_CANCELLED",
          "Old native context revoked a retired identity");
  require(f.host_token_requests == credentials_before_stale,
          "Retired native operation fetched credentials or reached backend");
  ok(manager.query(switched));
  require(switched.phase == "complete",
          "Typed switch query did not reach terminal state");
  for (size_t n = begin; n < f.messages.size(); ++n)
    require(f.messages[n].value("kind", "").rfind("login-", 0) != 0,
            "Late login settled into replacement JS");
  begin = f.messages.size();
  ok(manager.switch_account({"", "3"}, {}, switched));
  f.wait("start", begin);
  state = f.state();
  require(state["hostLoggedIn"] == false && state["appLoggedIn"] == false,
          "Host sign-out synthesized an identity");
  failure = f.wait("login-reject", f.send("login"));
  require(failure["error"]["errCode"] == 2001,
          "Signed-out login did not reject");
  require(f.unrelated_events > 0,
          "Installing services replaced application event observer");
  begin = f.messages.size();
  ok(manager.switch_account({"dave", "4"}, {}, switched));
  for (int n = 0; n < 3; ++n) f.pump();
  auto closing = f.session->close();
  require(closing || closing.code == "ERR_RESOURCE_CLEANUP_PENDING",
          "Closing failed while backend preflight was cancelled");
  const auto close_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (f.session->state().status != SessionState::Closed &&
         std::chrono::steady_clock::now() < close_deadline) {
    f.pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(f.session->state().status == SessionState::Closed,
          "Cancelled backend preflight prevented close");
  for (size_t n = begin; n < f.messages.size(); ++n)
    require(f.messages[n].value("kind", "") != "start",
            "Close started a replacement JS owner");
  std::cout << "host-services actual FFI/QuickJS identity and reference "
               "payment lifecycle: passed\n";
}
}  // namespace
int main(int argc, char** argv) {
  std::cout.setf(std::ios::unitbuf);
  try {
    require(weapp::desktop::test::initialize_native_host(),
            "Native SDK test host requires an initialized platform display");
    require(argc == 3 || (argc == 4 && std::string(argv[3]) == "--backend-errors-only"),
            "Expected authored pack and private native fixture input, optionally --backend-errors-only");
    const bool errors_only = argc == 4;
    std::ifstream stream(argv[2]);
    Json input;
    stream >> input;
    transport_guards(input);
    if (errors_only) backend_error_projection(argv[1], input);
    else identity_and_payment(argv[1], input);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
