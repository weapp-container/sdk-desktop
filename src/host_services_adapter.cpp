#include <algorithm>
#include <mutex>

#include "host_services_internal.hpp"
namespace weapp::desktop {
using detail::decode;
using detail::encode;
using detail::Json;
namespace {
Value::Object object(const Json& j) {
  return std::get<Value::Object>(decode(j).data);
}
const std::set<std::string> identity_apis = {
    "wx.login",
    "wx.checkSession",
    "wx.getUserInfo",
    "wx.getUserProfile",
    "component:button.open-type.getPhoneNumber",
    "component:button.bindgetphonenumber",
    "weappContainer.identity.authorizePhone",
    "weappContainer.identity.logout",
    "weappContainer.identity.getState",
    "weappContainer.identity.onChange",
    "weappContainer.identity.offChange"};
const std::set<std::string> payment_apis = {"wx.requestPayment",
                                            "weappContainer.payment.request"};
Status invalid(const std::string& message) {
  return Status::error("ERR_INVALID_REQUEST", message);
}
Status cancelled() {
  return Status::error("ERR_CANCELLED", "Native host operation was revoked");
}
template <class T>
struct Mailbox {
  std::mutex mutex;
  bool settled = false;
  T value{};
  void put(T next) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!settled) {
      value = std::move(next);
      settled = true;
    }
  }
};
bool same_owner(const HostApiContext& a, const HostApiContext& b) {
  auto x = encode(a.value()), y = encode(b.value());
  return x["application"] == y["application"] && x["account"] == y["account"] &&
         a.session_id == b.session_id;
}
}  // namespace
struct HostServices::Impl {
  HostServicesOptions options;
  std::weak_ptr<Session> session;
  std::thread::id owner = std::this_thread::get_id();
  struct Call {
    HostApiContext context;
    Json payload;
    HostCancellation cancel;
    bool retained = false;
  };
  std::map<std::string, std::shared_ptr<Call>> calls;
  struct Subscription {
    std::shared_ptr<Call> call;
    uint64_t sequence = 0;
  };
  std::map<std::string, Subscription> subscriptions;
  std::vector<std::function<bool()>> pending;
  std::set<std::string> installed;
  std::vector<std::shared_ptr<HostCancellation>> native_work;
  std::function<bool(const HostApiContext&)> matches;
  bool payment_busy = false, consent_busy = false, retiring = false;
  HostCancellation account_preflight;
  Status transport_error;
  explicit Impl(HostServicesOptions value) : options(std::move(value)) {}
  void forget(const std::shared_ptr<HostCancellation>& token) {
    native_work.erase(
        std::remove(native_work.begin(), native_work.end(), token),
        native_work.end());
  }
  Status check() const {
    return owner == std::this_thread::get_id()
               ? Status{}
               : Status::error(
                     "ERR_WRONG_THREAD",
                     "Host services belong to their creating UI thread");
  }
  Status command(const std::string& operation, const Json& body) {
    auto s = session.lock();
    return s ? s->host_services_command(operation, object(body)) : cancelled();
  }
  Status require_snapshot_owner_context() {
    auto s=session.lock();if(!s)return cancelled();Capabilities capabilities;
    auto status=s->capabilities(capabilities);if(!status)return status;
    return capabilities.snapshot_owner_context?Status{}:Status::error("ERR_UNSUPPORTED_CAPABILITY","runtime lacks native snapshot owner context 1.0");
  }
  bool current(const std::shared_ptr<Call>& call) const {
    const auto found = calls.find(call->context.call_id);
    auto s = session.lock();
    return !retiring && !call->cancel.cancelled() && found != calls.end() &&
           found->second == call && s && matches && matches(call->context);
  }
  void stop_call(const std::string& id) {
    auto found = calls.find(id);
    if (found == calls.end()) return;
    found->second->cancel.cancel();
    for (auto i = subscriptions.begin(); i != subscriptions.end();)
      if (i->second.call == found->second)
        i = subscriptions.erase(i);
      else
        ++i;
    calls.erase(found);
  }
  Status complete(const std::shared_ptr<Call>& call, Json value) {
    if (!current(call)) return {};
    value["errMsg"] = call->context.api + ":ok";
    auto status =
        command("completeHostCall", {{"callId", call->context.call_id},
                                     {"context", encode(call->context.value())},
                                     {"result", value}});
    if (!status) {
      transport_error = status;
      fail(call, status);
      return status;
    }
    if (!call->retained) calls.erase(call->context.call_id);
    return status;
  }
  void fail(const std::shared_ptr<Call>& call, Status status,
            const std::string& backend = {}, Value::Object api_details = {}) {
    if (!current(call)) return;
    auto code = status.code == "ERR_PERMISSION_DENIED" ? "permission_denied"
                : status.code == "ERR_UNSUPPORTED"     ? "unsupported"
                : status.code == "ERR_TIMEOUT"         ? "timeout"
                : status.code == "ERR_CANCELLED"       ? "cancelled"
                                                       : "invalid_request";
    Json details = encode(api_details);
    if (!details.contains("errMsg"))
      details["errMsg"] = call->context.api + ":fail " + status.message;
    if (!details.contains("errCode"))
      details["errCode"] = status.code == "ERR_PERMISSION_DENIED" ? 2001 : -1;
    if (!backend.empty()) details["backendCode"] = backend;
    auto failed = command("completeHostCall",
                          {{"callId", call->context.call_id},
                           {"context", encode(call->context.value())},
                           {"error",
                            {{"code", code},
                             {"message", status.message},
                             {"retryable", status.code == "ERR_TIMEOUT"},
                             {"details", details}}}});
    if (!failed) transport_error = failed;
    stop_call(call->context.call_id);
  }
  template <class T, class F>
  void await(std::shared_future<HostResult<T>> future, F next) {
    pending.push_back(
        [future = std::move(future), next = std::move(next)]() mutable {
          auto state = future.wait_for(std::chrono::seconds(0));
          if (state == std::future_status::timeout) return false;
          HostResult<T> result;
          try {
            if (state == std::future_status::deferred)
              result.status =
                  invalid("Deferred host work cannot execute on the UI thread");
            else
              result = future.get();
          } catch (const std::exception& e) {
            result.status = invalid(e.what());
          }
          next(std::move(result));
          return true;
        });
  }
  Status snapshot(const HostApiContext& context,
                  const HostIdentityState& state) {
    auto s = session.lock();
    if (!s || retiring || !matches || !matches(context)) return cancelled();
    auto status = command(
        "updateHostServiceSnapshots",
        {{"context", encode(context.value())},
         {"values",
          {{"weappContainer.identity.getState", encode(state.fields)}}}});
    if (!status) return status;
    for (auto& entry : subscriptions) {
      auto& sub = entry.second;
      if (!current(sub.call) || !same_owner(sub.call->context, context))
        continue;
      (void)command("hostApiEvent",
                    {{"sessionId", sub.call->context.session_id},
                     {"accountEpoch", sub.call->context.account.epoch},
                     {"subscriptionId", entry.first},
                     {"sequence", std::to_string(++sub.sequence)},
                     {"value", encode(state.fields)}});
    }
    return {};
  }
  template <class F>
  void refresh_call(const std::shared_ptr<Call>& call, F next) {
    await(options.backend->identity_state(call->context, call->cancel),
          [this, call, next = std::move(next)](auto result) mutable {
            if (!current(call)) return;
            if (!result) {
              fail(call, result.status, result.backend_code);
              return;
            }
            auto status = snapshot(call->context, result.value);
            if (!status) {
              fail(call, status);
              return;
            }
            next();
          });
  }
  template <class T, class F>
  void disclosure(const std::shared_ptr<Call>& call,
                  std::shared_future<HostResult<T>> future, F transform) {
    await(std::move(future), [this, call, transform = std::move(transform)](
                                 auto result) mutable {
      if (!current(call)) return;
      if (!result) {
        // Even a revoked app session refreshes the visible synchronous state.
        if (result.backend_code == "invalid_app_session")
          refresh_call(call, [this, call, result] {
            fail(call, result.status, result.backend_code);
          });
        else
          fail(call, result.status, result.backend_code);
        return;
      }
      auto value = transform(result.value);
      refresh_call(call, [this, call, value = std::move(value)]() mutable {
        (void)complete(call, std::move(value));
      });
    });
  }
  void profile(const std::shared_ptr<Call>& call) {
    disclosure(call, options.backend->profile(call->context, call->cancel),
               [](const HostProfile& profile) {
                 return Json{{"userInfo",
                              {{"nickName", profile.nick_name},
                               {"avatarUrl", profile.avatar_url}}}};
               });
  }
  void authorize(const std::shared_ptr<Call>& call, bool phone) {
    if (!options.consent) {
      fail(call, Status::error("ERR_UNSUPPORTED",
                               "Configure a visible native consent presenter"));
      return;
    }
    if (consent_busy) {
      fail(call, invalid("Another native consent prompt is active"));
      return;
    }
    consent_busy = true;
    const auto kind = phone ? HostConsentKind::Phone : HostConsentKind::Profile;
    await(options.backend->authorization_policy(call->context, kind,
                                                call->cancel),
          [this, call, phone](auto policy) {
            if (!current(call)) {
              consent_busy = false;
              return;
            }
            if (!policy) {
              consent_busy = false;
              fail(call, policy.status, policy.backend_code);
              return;
            }
            auto box = std::make_shared<Mailbox<HostResult<bool>>>();
            try {
              options.consent->present(
                  {call->context, policy.value}, call->cancel,
                  [box](auto result) { box->put(std::move(result)); });
            } catch (const std::exception& e) {
              box->put({invalid(e.what()), false, {}});
            }
            pending.push_back([this, call, phone, policy = policy.value, box] {
              HostResult<bool> decision;
              {
                std::lock_guard<std::mutex> lock(box->mutex);
                if (!box->settled) return false;
                decision = box->value;
              }
              consent_busy = false;
              if (!current(call)) return true;
              if (!decision) {
                fail(call, decision.status, decision.backend_code);
                return true;
              }
              await(
                  options.backend->consent(call->context, policy,
                                           decision.value, call->cancel),
                  [this, call, phone, granted = decision.value](auto recorded) {
                    if (!current(call)) return;
                    if (!recorded) {
                      fail(call, recorded.status, recorded.backend_code);
                      return;
                    }
                    if (!granted) {
                      refresh_call(call, [this, call] {
                        fail(call, Status::error("ERR_PERMISSION_DENIED",
                                                 "auth deny"));
                      });
                      return;
                    }
                    if (phone)
                      disclosure(call,
                                 options.backend->phone_code(call->context,
                                                             call->cancel),
                                 [](const HostLoginCode& value) {
                                   return Json{{"code", value.code},
                                               {"expiresIn", value.expires_in}};
                                 });
                    else
                      profile(call);
                  });
              return true;
            });
          });
  }
  void confirm_payment(const std::shared_ptr<Call>& call, const std::string& id,
                       std::chrono::steady_clock::time_point deadline) {
    if (!current(call)) {
      payment_busy = false;
      return;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      payment_busy = false;
      fail(call, Status::error("ERR_TIMEOUT",
                               "Awaiting verified server payment confirmation; "
                               "the trade was not cancelled"));
      return;
    }
    await(
        options.backend->payment_launch(call->context, id, call->cancel),
        [this, call, id, deadline](auto result) {
          if (!current(call)) {
            payment_busy = false;
            return;
          }
          if (!result) {
            payment_busy = false;
            fail(call, result.status, result.backend_code);
            return;
          }
          if (result.value.status == "paid") {
            payment_busy = false;
            (void)complete(call, {{"orderId", id}, {"serverConfirmed", true}});
            return;
          }
          if (result.value.status == "closed" ||
              result.value.status == "refunded") {
            payment_busy = false;
            fail(call, invalid("Backend reports a closed or refunded order"));
            return;
          }
          const auto retry =
              std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
          pending.push_back([this, call, id, deadline, retry] {
            if (!current(call)) {
              payment_busy = false;
              return true;
            }
            if (std::chrono::steady_clock::now() < retry) return false;
            confirm_payment(call, id, deadline);
            return true;
          });
        });
  }
  void payment(const std::shared_ptr<Call>& call) {
    if (payment_busy) {
      fail(call, invalid("Another payment UI is active"));
      return;
    }
    for (const auto* key :
         {"appId", "prepayId", "paySign", "package", "orderString", "signType"})
      if (call->payload.contains(key)) {
        fail(call, invalid("Pass a trusted backend orderId; platform payment "
                           "signatures are not interchangeable"));
        return;
      }
    if (!call->payload.contains("orderId") ||
        !call->payload["orderId"].is_string()) {
      fail(call, invalid("Backend orderId required"));
      return;
    }
    const auto id = call->payload["orderId"].get<std::string>();
    payment_busy = true;
    await(
        options.backend->payment_launch(call->context, id, call->cancel),
        [this, call, id](auto result) {
          if (!current(call)) {
            payment_busy = false;
            return;
          }
          if (!result) {
            payment_busy = false;
            fail(call, result.status, result.backend_code);
            return;
          }
          const auto& order = result.value;
          if (order.status == "paid") {
            payment_busy = false;
            (void)complete(call, {{"orderId", id}, {"serverConfirmed", true}});
            return;
          }
          const bool reference = options.allow_reference_payments &&
                                 order.channel == "reference" &&
                                 order.product == "app" &&
                                 order.kind == "reference";
          const bool official =
              (order.channel == "wechat" || order.channel == "alipay") &&
              ((order.product == "native" &&
                order.kind == order.channel + "-qr") ||
               (order.product == "h5" && order.kind == order.channel + "-h5"));
          if (!reference && !official) {
            payment_busy = false;
            fail(call,
                 Status::error("ERR_UNSUPPORTED",
                               "This desktop adapter requires an explicitly "
                               "configured official QR/H5 channel"));
            return;
          }
          std::shared_ptr<HostPaymentChannel> selected;
          try {
            for (const auto& channel : options.payments)
              if (channel &&
                  channel->supports(order.channel, order.product, order.kind)) {
                if (selected) {
                  payment_busy = false;
                  fail(call, invalid("Ambiguous desktop payment channel"));
                  return;
                }
                selected = channel;
              }
          } catch (const std::exception& error) {
            payment_busy = false;
            fail(call, invalid(error.what()));
            return;
          }
          if (!selected) {
            payment_busy = false;
            fail(call,
                 Status::error(
                     "ERR_UNSUPPORTED",
                     "Official desktop payment channel is not configured"));
            return;
          }
          auto box = std::make_shared<Mailbox<HostApiError>>();
          try {
            selected->launch(order, call->context, call->cancel,
                             [box](auto value) { box->put(std::move(value)); });
          } catch (const std::exception& e) {
            box->put({invalid(e.what()), {}});
          }
          auto deadline = std::chrono::steady_clock::now() +
                          options.payment_confirmation_timeout;
          pending.push_back([this, call, id, deadline, box] {
            HostApiError launched;
            bool settled;
            {
              std::lock_guard<std::mutex> lock(box->mutex);
              settled = box->settled;
              launched = box->value;
            }
            if (!settled) {
              if (current(call) &&
                  std::chrono::steady_clock::now() >= deadline) {
                fail(call, Status::error("ERR_TIMEOUT",
                                         "Payment UI launch timed out; the "
                                         "trade was not cancelled"));
              }
              // Cancellation must drain the channel's actual owned UI/task.
              return false;
            }
            if (!current(call)) {
              payment_busy = false;
              return true;
            }
            if (!launched) {
              payment_busy = false;
              fail(call, launched.status, {}, std::move(launched.details));
              return true;
            }
            confirm_payment(call, id, deadline);
            return true;
          });
        });
  }
  void invoke(const std::shared_ptr<Call>& call) {
    if(auto status=require_snapshot_owner_context();!status){fail(call,status);return;}
    const auto& api = call->context.api;
    if (payment_apis.count(api)) {
      payment(call);
      return;
    }
    if (api == "wx.login")
      disclosure(call, options.backend->login(call->context, call->cancel),
                 [](const HostLoginCode& c) {
                   return Json{{"code", c.code}, {"expiresIn", c.expires_in}};
                 });
    else if (api == "wx.checkSession")
      disclosure(call,
                 options.backend->check_session(call->context, call->cancel),
                 [](const auto& value) { return encode(value); });
    else if (api == "wx.getUserInfo")
      profile(call);
    else if (api == "wx.getUserProfile" ||
             api == "weappContainer.identity.authorizePhone")
      authorize(call, api == "weappContainer.identity.authorizePhone");
    else if (api == "weappContainer.identity.logout")
      disclosure(call, options.backend->logout_app(call->context, call->cancel),
                 [](const auto& value) { return encode(value); });
    else if (api == "weappContainer.identity.onChange" ||
             api == "weappContainer.identity.offChange") {
      auto id = call->payload.at("subscriptionId").get<std::string>();
      if (api == "weappContainer.identity.offChange" ||
          call->payload.value("action", "") == "unsubscribe") {
        auto found = subscriptions.find(id);
        if (found != subscriptions.end()) {
          auto old = found->second.call->context.call_id;
          stop_call(old);
        }
      } else {
        if (subscriptions.size() >= 128) {
          fail(call, Status::error("ERR_RESOURCE_LIMIT",
                                   "Identity subscription budget exceeded"));
          return;
        }
        auto previous = subscriptions.find(id);
        if (previous != subscriptions.end())
          stop_call(previous->second.call->context.call_id);
        call->retained = true;
        subscriptions[id] = {call, 0};
      }
      (void)complete(call, Json::object());
    } else
      fail(call,
           Status::error(
               "ERR_UNSUPPORTED",
               api == "weappContainer.identity.getState"
                   ? "Identity state uses the prepared synchronous snapshot"
                   : "Phone authorization requires the runtime-bound native "
                     "button gesture"));
  }
};
HostServices::HostServices(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
HostServices::~HostServices() {
  for (auto& item : impl_->calls) item.second->cancel.cancel();
}
std::shared_ptr<HostServices> HostServices::create(HostServicesOptions options,
                                                   Status& status) {
  if (!options.backend ||
      options.payment_confirmation_timeout < std::chrono::seconds(1) ||
      options.payment_confirmation_timeout > std::chrono::minutes(5)) {
    status =
        invalid("Configure a backend and bounded payment confirmation timeout");
    return {};
  }
  status = {};
  return std::shared_ptr<HostServices>(
      new HostServices(std::make_unique<Impl>(std::move(options))));
}
Status HostServices::install(const std::shared_ptr<Session>& session) {
  if (auto status = impl_->check(); !status) return status;
  if (!session) return invalid("Session required");
  if (auto status = session->core_->check(); !status) return status;
  if (!impl_->session.expired() || session->core_->services)
    return invalid("Host services adapter is already installed");
  if (session->state().status != SessionState::Empty)
    return invalid("Install host services before loading JS");
  auto config = encode(session->core_->config.host_services);
  try {
    (void)config.at("syncValues")
        .at("weappContainer.identity.getState")
        .at("hostLoggedIn")
        .get<bool>();
  } catch (...) {
    return invalid(
        "Fetch backend prepare_configuration before installing identity "
        "services");
  }
  HostApiContext trusted;
  auto status =
      HostApiContext::parse(session->core_->config.host_services, trusted);
  if (!status) return status;
  auto apis = identity_apis;
  if (!impl_->options.consent)
    for (const auto* api :
         {"wx.getUserProfile", "component:button.open-type.getPhoneNumber",
          "component:button.bindgetphonenumber",
          "weappContainer.identity.authorizePhone"})
      apis.erase(api);
  if (!impl_->options.payments.empty())
    apis.insert(payment_apis.begin(), payment_apis.end());
  for (const auto& api : apis) {
    auto s = session->register_api_handler(api);
    if (!s) return s;
  }
  impl_->session = session;
  std::weak_ptr<Session> weak = session;
  impl_->matches = [weak](const HostApiContext& context) {
    auto session = weak.lock();
    if (!session) return false;
    auto j = encode(session->core_->config.host_services),
         c = encode(context.value());
    if (j.at("application") != c.at("application") ||
        j.at("account") != c.at("account"))
      return false;
    Json state;
    auto status = session->core_->call("state", Json::object(), &state);
    if (!status) return false;
    const auto phase = state.value("accountSwitch", Json::object());
    const bool live = state.value("status", "") == "ready" ||
                      (phase.is_object() &&
                       phase.value("phase", "") == "awaitingPresentation");
    return status && live && state.contains("sessionId") &&
           state["sessionId"].is_string() &&
           state["sessionId"] == context.session_id;
  };
  impl_->installed = std::move(apis);
  session->core_->services = shared_from_this();
  return {};
}
Status HostServices::current_context(HostApiContext& context) {
  if (auto status = impl_->check(); !status) return status;
  if (auto status = impl_->require_snapshot_owner_context(); !status) return status;
  auto session = impl_->session.lock();
  if (!session) return cancelled();
  Json state;
  auto status = session->core_->call("state", Json::object(), &state);
  if (!status) return status;
  if (state.value("status", "") != "ready")
    return Status::error("ERR_IDENTITY_SWITCHING",
                         "Active identity owner required");
  auto config = encode(session->core_->config.host_services);
  config["sessionId"] = state.at("sessionId");
  return HostApiContext::parse(object(config), context);
}
bool HostServices::dispatch(const Event& event) {
  auto j = encode(event.fields);
  if (event.type == EventType::AccountSwitch) {
    if (j.value("receipt", Json::object()).value("phase", "") == "revoking") {
      impl_->retiring = true;
      impl_->account_preflight.cancel();
      for (auto& item : impl_->calls) item.second->cancel.cancel();
      impl_->subscriptions.clear();
    } else if (j.value("receipt", Json::object()).value("phase", "") ==
               "complete")
      impl_->retiring = false;
    return false;
  }
  if (event.type == EventType::HostCallCancelled) {
    impl_->stop_call(j.value("callId", ""));
    return false;
  }
  if (event.type != EventType::HostCall) return false;
  const auto api = j.value("method", "");
  if (!impl_->installed.count(api)) return false;
  auto call = std::make_shared<Impl::Call>();
  auto status = HostApiContext::parse(object(j.at("context")), call->context);
  if (!status) return false;
  if (call->context.call_id != j.value("callId", "") ||
      call->context.api != api || call->context.session_id.empty())
    return false;
  if (!impl_->matches || !impl_->matches(call->context)) return true;
  impl_->retiring = false;
  call->payload = j.value("payload", Json::object());
  impl_->calls[call->context.call_id] = call;
  if (impl_->calls.size() > 256) {
    impl_->fail(call, Status::error("ERR_RESOURCE_LIMIT",
                                    "Host service call budget exceeded"));
    return true;
  }
  try {
    impl_->invoke(call);
  } catch (const std::exception& e) {
    impl_->fail(call, invalid(e.what()));
  }
  return true;
}
Status HostServices::pump() {
  if (auto status = impl_->check(); !status) return status;
  std::vector<std::function<bool()>> pending;
  pending.swap(impl_->pending);
  for (auto& poll : pending)
    if (!poll()) impl_->pending.push_back(std::move(poll));
  auto status = impl_->transport_error;
  impl_->transport_error = {};
  return status;
}
Status HostServices::retire() {
  if (auto status = impl_->check(); !status) return status;
  impl_->retiring = true;
  impl_->account_preflight.cancel();
  for (auto& token : impl_->native_work) token->cancel();
  for (auto& item : impl_->calls) item.second->cancel.cancel();
  impl_->subscriptions.clear();
  (void)pump();
  if (!impl_->pending.empty())
    return Status::error("ERR_RESOURCE_CLEANUP_PENDING",
                         "Native identity/payment tasks are draining");
  impl_->calls.clear();
  impl_->payment_busy = false;
  impl_->consent_busy = false;
  return {};
}
std::shared_future<AccountCleanupResult> HostServices::prepare_account(
    const AccountCleanupContext& context) {
  impl_->account_preflight = HostCancellation{};
  return impl_->options.backend->prepare_account(context,
                                                 impl_->account_preflight);
}
std::shared_future<HostResult<HostIdentityState>> HostServices::refresh(
    const HostApiContext& context) {
  auto promise =
      std::make_shared<std::promise<HostResult<HostIdentityState>>>();
  auto future = promise->get_future().share();
  if (auto status = impl_->check(); !status) {
    promise->set_value({status, {}, {}});
    return future;
  }
  if (auto status = impl_->require_snapshot_owner_context(); !status) {
    promise->set_value({status, {}, {}});return future;
  }
  if (impl_->retiring || !impl_->matches || !impl_->matches(context)) {
    promise->set_value({cancelled(), {}, {}});
    return future;
  }
  if (impl_->native_work.size() >= 256) {
    promise->set_value({Status::error("ERR_RESOURCE_LIMIT",
                                      "Native host operation budget exceeded"),
                        {},
                        {}});
    return future;
  }
  auto token = std::make_shared<HostCancellation>();
  impl_->native_work.push_back(token);
  impl_->await(impl_->options.backend->identity_state(context, *token),
               [this, context, promise, token](auto result) {
                 if (result)
                   result.status = impl_->snapshot(context, result.value);
                 impl_->forget(token);
                 promise->set_value(std::move(result));
               });
  return future;
}
std::shared_future<HostResult<Value::Object>> HostServices::revoke(
    const HostApiContext& context, HostConsentKind kind) {
  auto promise = std::make_shared<std::promise<HostResult<Value::Object>>>();
  auto future = promise->get_future().share();
  if (auto status = impl_->check(); !status) {
    promise->set_value({status, {}, {}});
    return future;
  }
  if (auto status = impl_->require_snapshot_owner_context(); !status) {
    promise->set_value({status, {}, {}});return future;
  }
  if (impl_->retiring || !impl_->matches || !impl_->matches(context)) {
    promise->set_value({cancelled(), {}, {}});
    return future;
  }
  if (impl_->native_work.size() >= 256) {
    promise->set_value({Status::error("ERR_RESOURCE_LIMIT",
                                      "Native host operation budget exceeded"),
                        {},
                        {}});
    return future;
  }
  auto token = std::make_shared<HostCancellation>();
  impl_->native_work.push_back(token);
  impl_->await(
      impl_->options.backend->consent(context, {kind, {}, {}}, false, *token),
      [this, context, promise, token](auto result) {
        if (!result) {
          impl_->forget(token);
          promise->set_value(std::move(result));
          return;
        }
        impl_->await(
            impl_->options.backend->identity_state(context, *token),
            [this, context, promise, result, token](auto state) mutable {
              if (state)
                result.status = impl_->snapshot(context, state.value);
              else
                result.status = state.status;
              impl_->forget(token);
              promise->set_value(std::move(result));
            });
      });
  return future;
}
}  // namespace weapp::desktop
