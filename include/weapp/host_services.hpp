#pragma once
#include <atomic>
#include <chrono>

#include "weapp/desktop.hpp"

namespace weapp::desktop {
// Native installation/account data. Epoch is decimal u64, never a JSON double.
struct HostApplication {
  std::string host_app_id, tenant_id, publisher_id, app_id, environment,
      version, pack_root;
};
struct HostAccount {
  std::string principal_id, epoch;
};
struct HostApiContext {
  HostApplication application;
  HostAccount account;
  std::string session_id, call_id, api, deadline_ns, cancel_token;
  // Exact accepted wire context, including incarnation/revision fields. Native
  // preflight can omit runtime-only correlation; guest calls cannot.
  Value::Object wire;
  Value::Object value() const;
  static Status parse(const Value::Object&, HostApiContext&);
};
class HostCancellation final {
 public:
  HostCancellation();
  bool cancelled() const;
  void cancel() const;
  // Native resource interruption only. Never enter Session from this callback.
  void on_cancel(std::function<void()>) const;

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
template <class T>
struct HostResult {
  Status status;
  T value{};
  std::string backend_code;
  explicit operator bool() const { return bool(status); }
};
struct HostProfile {
  std::string nick_name, avatar_url;
};
struct HostIdentityState {
  bool host_logged_in = false, app_logged_in = false,
       profile_authorized = false, phone_authorized = false;
  Value::Object fields;
};
struct HostLoginCode {
  std::string code;
  double expires_in = 0;
};
enum class HostConsentKind { Profile, Phone };
struct HostAuthorizationPolicy {
  HostConsentKind kind = HostConsentKind::Profile;
  std::string purpose, permission_version;
};
struct HostPaymentOrder {
  std::string order_id, channel, product, status, kind;
  Value::Object launch;
};
struct HostBackendOptions {
  std::string endpoint;
  bool allow_loopback_http = false;
  // Invoked on the calling thread. The host keeps and refreshes credentials.
  std::function<std::string(const HostApiContext&)> host_token;
};
// A bounded native executor owns HTTPS requests. Futures contain no host
// bearer. Redirects/cookies are disabled; responses are limited to 256 KiB.
class HostServicesClient final {
 public:
  static std::shared_ptr<HostServicesClient> create(HostBackendOptions,
                                                    Status&);
  ~HostServicesClient();
  std::shared_future<HostResult<HostIdentityState>> identity_state(
      const HostApiContext&, HostCancellation = {});
  std::shared_future<HostResult<HostLoginCode>> login(const HostApiContext&,
                                                      HostCancellation = {});
  std::shared_future<HostResult<Value::Object>> check_session(
      const HostApiContext&, HostCancellation = {});
  std::shared_future<HostResult<HostProfile>> profile(const HostApiContext&,
                                                      HostCancellation = {});
  std::shared_future<HostResult<HostAuthorizationPolicy>> authorization_policy(
      const HostApiContext&, HostConsentKind, HostCancellation = {});
  std::shared_future<HostResult<Value::Object>> consent(
      const HostApiContext&, const HostAuthorizationPolicy&, bool granted,
      HostCancellation = {});
  std::shared_future<HostResult<HostLoginCode>> phone_code(
      const HostApiContext&, HostCancellation = {});
  std::shared_future<HostResult<Value::Object>> logout_app(
      const HostApiContext&, HostCancellation = {});
  std::shared_future<HostResult<HostPaymentOrder>> payment_launch(
      const HostApiContext&, const std::string& order_id,
      HostCancellation = {});
  std::shared_future<HostResult<Value::Object>> prepare_configuration(
      Value::Object);
  std::shared_future<AccountCleanupResult> prepare_account(
      const AccountCleanupContext&, HostCancellation = {});
  // Cancels queued/active requests. Never logs out the host or cancels a trade.
  void close();

 private:
  struct Impl;
  explicit HostServicesClient(std::shared_ptr<Impl>);
  std::shared_ptr<Impl> impl_;
};
struct HostConsentRequest {
  HostApiContext context;
  HostAuthorizationPolicy policy;
};
class HostConsentPresenter {
 public:
  virtual ~HostConsentPresenter() = default;
  // Called on Session's UI thread. Completion may occur on any thread.
  virtual void present(const HostConsentRequest&, HostCancellation,
                       std::function<void(HostResult<bool>)>) = 0;
  // parent: a visible NSView*, HWND or GtkWidget*. No independent window.
  static std::shared_ptr<HostConsentPresenter> native(void* parent, Status&);
};
class HostPaymentChannel {
 public:
  virtual ~HostPaymentChannel() = default;
  virtual bool supports(const std::string& channel, const std::string& product,
                        const std::string& kind) const = 0;
  // A successful launch is only UI completion; the backend must confirm paid.
  // Completion is terminal: release owned UI/tasks before calling it, including
  // cancellation. A QR cashier may offer an explicit "finished" action; that
  // action is never itself evidence of payment.
  // Desktop channels must explicitly support official QR/H5 products. APP or
  // JSAPI signatures cannot be mapped to these products by the SDK.
  virtual void launch(const HostPaymentOrder&, const HostApiContext&,
                      HostCancellation, std::function<void(Status)>) = 0;
};
struct HostServicesOptions {
  std::shared_ptr<HostServicesClient> backend;
  std::shared_ptr<HostConsentPresenter> consent;
  std::vector<std::shared_ptr<HostPaymentChannel>> payments;
  // Local reference backend only, never an automatic fallback for real pay.
  bool allow_reference_payments = false;
  std::chrono::milliseconds payment_confirmation_timeout{15000};
};
// Identity + payment adapter independent of the application's on_event
// listener. OS/network callbacks enqueue; pump alone settles calls and sends JS
// events.
class HostServices final : public std::enable_shared_from_this<HostServices> {
 public:
  static std::shared_ptr<HostServices> create(HostServicesOptions, Status&);
  ~HostServices();
  Status install(const std::shared_ptr<Session>&);
  Status current_context(HostApiContext&);
  std::shared_future<HostResult<HostIdentityState>> refresh(
      const HostApiContext&);
  std::shared_future<HostResult<Value::Object>> revoke(const HostApiContext&,
                                                       HostConsentKind);

 private:
  struct Impl;
  explicit HostServices(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> impl_;
  bool dispatch(const Event&);
  Status pump();
  Status retire();
  std::shared_future<AccountCleanupResult> prepare_account(
      const AccountCleanupContext&);
  friend struct detail::Core;
  friend class Session;
};
struct HostAccountSwitch {
  std::string operation_id, epoch, phase, session_id;
  Value::Object fields;
};
// Delegates shared-account ownership to Rust. It never creates a parallel epoch
// registry. All methods run on the session owner thread; controller stays
// stable.
class HostAccountManager final {
 public:
  explicit HostAccountManager(std::shared_ptr<Session> session)
      : session_(std::move(session)) {}
  Status switch_account(const HostAccount&,
                        const std::map<std::string, LoadOptions>& launches,
                        HostAccountSwitch&);
  Status query(HostAccountSwitch&, const std::string& operation_id = {});
  Status cancel(const std::string& operation_id);
  Status retry(const LoadOptions&);
  Status delete_local_data(const std::string& principal_id,
                           uint64_t& deleted_scopes);

 private:
  std::shared_ptr<Session> session_;
};
}  // namespace weapp::desktop
