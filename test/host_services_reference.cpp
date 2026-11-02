#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

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
    complete({{}, grant, {}});
  }
};
struct ReferenceChannel final : HostPaymentChannel {
  Json input;
  bool confirm = true;
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
              std::function<void(Status)> complete) override {
    ++launches;
    const auto confirm_now = confirm;
    workers.push_back(
        std::async(std::launch::async, [input = input, id = order.order_id,
                                        cancellation, complete, confirm_now] {
          if (cancellation.cancelled()) {
            complete(
                Status::error("ERR_CANCELLED", "Reference launch cancelled"));
            return;
          }
          try {
            if (confirm_now)
              control(input, "/fixture/confirm", {{"orderId", id}});
            complete({});
          } catch (const std::exception& e) {
            complete(Status::error("ERR_REFERENCE_TEST", e.what()));
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
    require(argc == 3,
            "Expected authored pack and private native fixture input");
    std::ifstream stream(argv[2]);
    Json input;
    stream >> input;
    transport_guards(input);
    identity_and_payment(argv[1], input);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
