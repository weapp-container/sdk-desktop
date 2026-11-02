#include <gtk/gtk.h>

#include <algorithm>
#include <mutex>

#include "host_services_internal.hpp"
#if WEAPP_DEVICE_CURL
#include <curl/curl.h>
#endif

namespace weapp::desktop::detail {
bool host_http_available() {
#if WEAPP_DEVICE_CURL
  return true;
#else
  return false;
#endif
}
HostHttpResponse host_http_post(const std::string& endpoint,
                                const std::string& path,
                                const std::string& bearer,
                                const std::string& body,
                                HostCancellation cancel,
                                std::chrono::milliseconds timeout) {
#if WEAPP_DEVICE_CURL
  static std::once_flag once;
  static CURLcode initialized = CURLE_FAILED_INIT;
  std::call_once(once,
                 [] { initialized = curl_global_init(CURL_GLOBAL_DEFAULT); });
  if (initialized != CURLE_OK)
    return {
        Status::error("ERR_NETWORK", "libcurl initialization failed"), 0, {}};
  struct Request {
    HostHttpResponse response;
    HostCancellation cancellation;
    bool oversized = false;
  } request{{}, cancel, false};
  auto* curl = curl_easy_init();
  if (!curl)
    return {
        Status::error("ERR_NETWORK", "Cannot allocate HTTPS request"), 0, {}};
  const auto url = endpoint + path,
             authorization = "Authorization: Bearer " + bearer;
  curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, authorization.c_str());
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, curl_off_t(body.size()));
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_NETRC, long(CURL_NETRC_IGNORED));
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, long(timeout.count()));
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                   long(std::min<int64_t>(timeout.count(), 10000)));
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &request);
  curl_easy_setopt(
      curl, CURLOPT_XFERINFOFUNCTION,
      +[](void* data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
        return static_cast<Request*>(data)->cancellation.cancelled() ? 1 : 0;
      });
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &request);
  curl_easy_setopt(
      curl, CURLOPT_WRITEFUNCTION,
      +[](char* bytes, size_t size, size_t count, void* data) -> size_t {
        auto* request = static_cast<Request*>(data);
        if (size && count > 262144 / size) {
          request->oversized = true;
          return 0;
        }
        auto length = size * count;
        if (request->response.body.size() + length > 262144) {
          request->oversized = true;
          return 0;
        }
        request->response.body.append(bytes, length);
        return length;
      });
  const auto result = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  request.response.http_status = int(status);
  if (cancel.cancelled())
    request.response.status =
        Status::error("ERR_CANCELLED", "Host request cancelled");
  else if (request.oversized)
    request.response.status =
        Status::error("ERR_RESOURCE_LIMIT", "Backend response exceeds 256 KiB");
  else if (result != CURLE_OK)
    request.response.status = Status::error(
        result == CURLE_OPERATION_TIMEDOUT ? "ERR_TIMEOUT" : "ERR_NETWORK",
        curl_easy_strerror(result));
  return request.response;
#else
  (void)endpoint;
  (void)path;
  (void)bearer;
  (void)body;
  (void)cancel;
  (void)timeout;
  return {
      Status::error("ERR_UNSUPPORTED",
                    "This Linux build requires libcurl 7.85+ for host HTTPS"),
      0,
      {}};
#endif
}
namespace {
struct GtkConsent {
  GtkWidget* dialog = nullptr;
  std::function<void(HostResult<bool>)> complete;
  HostCancellation cancel;
  bool done = false;
  void finish(int response) {
    if (done) return;
    done = true;
    auto* owned = dialog;
    dialog = nullptr;
    if (owned) gtk_widget_destroy(owned);
    if (cancel.cancelled() ||
        (response != GTK_RESPONSE_ACCEPT && response != GTK_RESPONSE_REJECT))
      complete(
          {Status::error("ERR_CANCELLED", "Consent cancelled"), false, {}});
    else
      complete({{}, response == GTK_RESPONSE_ACCEPT, {}});
  }
};
class LinuxHostConsent final : public HostConsentPresenter {
 public:
  explicit LinuxHostConsent(GtkWidget* parent) : parent_(parent) {
    g_object_add_weak_pointer(G_OBJECT(parent_),
                              reinterpret_cast<gpointer*>(&parent_));
  }
  ~LinuxHostConsent() override {
    if (parent_)
      g_object_remove_weak_pointer(G_OBJECT(parent_),
                                   reinterpret_cast<gpointer*>(&parent_));
  }
  void present(const HostConsentRequest& request, HostCancellation cancellation,
               std::function<void(HostResult<bool>)> complete) override {
    if (owner_ != std::this_thread::get_id()) {
      complete({Status::error("ERR_WRONG_THREAD",
                              "Consent requires its GTK owner thread"),
                false,
                {}});
      return;
    }
    auto* window = parent_ ? gtk_widget_get_toplevel(parent_) : nullptr;
    if (!window || !GTK_IS_WINDOW(window) || !gtk_widget_get_visible(window) ||
        cancellation.cancelled()) {
      complete({Status::error("ERR_INVALID_REQUEST",
                              "Visible host GtkWindow required for consent"),
                false,
                {}});
      return;
    }
    const bool phone = request.policy.kind == HostConsentKind::Phone;
    const auto text = request.context.application.app_id + "\n\n" +
                      (phone ? "允许此小程序获取宿主账号已验证的手机号？"
                             : "允许此小程序获取宿主账号的昵称和头像？") +
                      "\n\n用途：" + request.policy.purpose + "\n权限版本：" +
                      request.policy.permission_version;
    auto state = std::make_shared<GtkConsent>();
    state->cancel = cancellation;
    state->complete = std::move(complete);
    state->dialog = gtk_message_dialog_new(
        GTK_WINDOW(window), GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s",
        phone ? "授权手机号" : "授权昵称和头像");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(state->dialog),
                                             "%s", text.c_str());
    gtk_dialog_add_buttons(GTK_DIALOG(state->dialog), "拒绝",
                           GTK_RESPONSE_REJECT, "允许", GTK_RESPONSE_ACCEPT,
                           nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(state->dialog),
                                    GTK_RESPONSE_REJECT);
    auto* retained = new std::shared_ptr<GtkConsent>(state);
    g_signal_connect_data(
        state->dialog, "response",
        G_CALLBACK(+[](GtkDialog*, gint response, gpointer data) {
          auto state = *static_cast<std::shared_ptr<GtkConsent>*>(data);
          state->finish(response);
        }),
        retained,
        +[](gpointer data, GClosure*) {
          delete static_cast<std::shared_ptr<GtkConsent>*>(data);
        },
        GConnectFlags(0));
    g_signal_connect_data(
        state->dialog, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer data) {
          auto state = *static_cast<std::shared_ptr<GtkConsent>*>(data);
          state->dialog = nullptr;
          state->finish(GTK_RESPONSE_CANCEL);
        }),
        new std::shared_ptr<GtkConsent>(state),
        +[](gpointer data, GClosure*) {
          delete static_cast<std::shared_ptr<GtkConsent>*>(data);
        },
        GConnectFlags(0));
    std::weak_ptr<GtkConsent> weak = state;
    cancellation.on_cancel([weak] {
      if (auto state = weak.lock())
        g_main_context_invoke(
            nullptr,
            +[](gpointer data) -> gboolean {
              std::unique_ptr<std::shared_ptr<GtkConsent>> retained(
                  static_cast<std::shared_ptr<GtkConsent>*>(data));
              auto state = *retained;
              if (state->dialog)
                gtk_dialog_response(GTK_DIALOG(state->dialog),
                                    GTK_RESPONSE_CANCEL);
              return G_SOURCE_REMOVE;
            },
            new std::shared_ptr<GtkConsent>(std::move(state)));
    });
    if (state->dialog) gtk_widget_show_all(state->dialog);
  }

 private:
  GtkWidget* parent_;
  const std::thread::id owner_ = std::this_thread::get_id();
};
}  // namespace
std::shared_ptr<HostConsentPresenter> host_consent_presenter(void* parent,
                                                             Status& status) {
  if (!parent || !GTK_IS_WIDGET(parent)) {
    status = Status::error("ERR_INVALID_REQUEST",
                           "Configure a native host GtkWidget");
    return {};
  }
  status = {};
  return std::make_shared<LinuxHostConsent>(GTK_WIDGET(parent));
}
}  // namespace weapp::desktop::detail
