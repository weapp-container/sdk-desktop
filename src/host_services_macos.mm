#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#include <condition_variable>
#include <mutex>
#include "host_services_internal.hpp"

namespace {
struct HTTPState {
  std::mutex mutex;
  std::condition_variable condition;
  weapp::desktop::detail::HostHttpResponse response;
  bool done = false, oversized = false;
};
NSString* host_string(const std::string& text) {
  return [[NSString alloc] initWithBytes:text.data()
                                  length:text.size()
                                encoding:NSUTF8StringEncoding];
}
}  // namespace
@interface WCDesktopHostHTTP : NSObject <NSURLSessionDataDelegate> {
 @public
  std::shared_ptr<HTTPState> state;
}
@end
@implementation WCDesktopHostHTTP
- (void)URLSession:(NSURLSession*)session
              dataTask:(NSURLSessionDataTask*)task
    didReceiveResponse:(NSURLResponse*)response
     completionHandler:(void (^)(NSURLSessionResponseDisposition))complete {
  (void)session;
  (void)task;
  std::lock_guard<std::mutex> lock(state->mutex);
  if ([response isKindOfClass:[NSHTTPURLResponse class]])
    state->response.http_status = int(((NSHTTPURLResponse*)response).statusCode);
  if (response.expectedContentLength > 262144) {
    state->oversized = true;
    complete(NSURLSessionResponseCancel);
  } else
    complete(NSURLSessionResponseAllow);
}
- (void)URLSession:(NSURLSession*)session
          dataTask:(NSURLSessionDataTask*)task
    didReceiveData:(NSData*)data {
  (void)session;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->response.body.size() + data.length > 262144) {
    state->oversized = true;
    [task cancel];
    return;
  }
  state->response.body.append(static_cast<const char*>(data.bytes), data.length);
}
- (void)URLSession:(NSURLSession*)session
                          task:(NSURLSessionTask*)task
    willPerformHTTPRedirection:(NSHTTPURLResponse*)response
                    newRequest:(NSURLRequest*)request
             completionHandler:(void (^)(NSURLRequest*))complete {
  (void)session;
  (void)task;
  (void)response;
  (void)request;
  complete(nil);
}
- (void)URLSession:(NSURLSession*)session
                   task:(NSURLSessionTask*)task
    didReceiveChallenge:(NSURLAuthenticationChallenge*)challenge
      completionHandler:(void (^)(NSURLSessionAuthChallengeDisposition, NSURLCredential*))complete {
  (void)session;
  (void)task;
  // Preserve the system TLS trust decision without consulting host credentials
  // for HTTP authentication, client certificates or other challenge schemes.
  complete([challenge.protectionSpace.authenticationMethod
               isEqualToString:NSURLAuthenticationMethodServerTrust]
               ? NSURLSessionAuthChallengePerformDefaultHandling
               : NSURLSessionAuthChallengeCancelAuthenticationChallenge,
           nil);
}
- (void)URLSession:(NSURLSession*)session
                    task:(NSURLSessionTask*)task
    didCompleteWithError:(NSError*)error {
  (void)session;
  (void)task;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->oversized)
    state->response.status =
        weapp::desktop::Status::error("ERR_RESOURCE_LIMIT", "Backend response exceeds 256 KiB");
  else if (error)
    state->response.status = weapp::desktop::Status::error(
        error.code == NSURLErrorTimedOut ? "ERR_TIMEOUT" : "ERR_NETWORK",
        error.localizedDescription.UTF8String ?: "Host backend request failed");
  state->done = true;
  state->condition.notify_all();
}
@end

namespace weapp::desktop::detail {
bool host_http_available() { return true; }
HostHttpResponse host_http_post(const std::string& endpoint, const std::string& path,
                                const std::string& bearer, const std::string& body,
                                HostCancellation cancel, std::chrono::milliseconds timeout) {
  @autoreleasepool {
    if (cancel.cancelled())
      return {Status::error("ERR_CANCELLED", "Host request cancelled"), 0, {}};
    auto state = std::make_shared<HTTPState>();
    WCDesktopHostHTTP* delegate = [WCDesktopHostHTTP new];
    delegate->state = state;
    NSURLSessionConfiguration* config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    config.HTTPShouldSetCookies = NO;
    config.HTTPCookieStorage = nil;
    config.URLCache = nil;
    config.timeoutIntervalForRequest = double(timeout.count()) / 1000;
    config.timeoutIntervalForResource = double(timeout.count()) / 1000;
    NSURLSession* session = [NSURLSession sessionWithConfiguration:config
                                                          delegate:delegate
                                                     delegateQueue:nil];
    NSMutableURLRequest* request =
        [NSMutableURLRequest requestWithURL:[NSURL URLWithString:host_string(endpoint + path)]];
    request.HTTPMethod = @"POST";
    [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    [request setValue:host_string("Bearer " + bearer) forHTTPHeaderField:@"Authorization"];
    request.HTTPBody = [NSData dataWithBytes:body.data() length:body.size()];
    NSURLSessionDataTask* task = [session dataTaskWithRequest:request];
    __weak NSURLSessionDataTask* weak_task = task;
    cancel.on_cancel([weak_task] { [weak_task cancel]; });
    [task resume];
    {
      std::unique_lock<std::mutex> lock(state->mutex);
      state->condition.wait(lock, [&] { return state->done; });
    }
    [session finishTasksAndInvalidate];
    if (cancel.cancelled())
      return {Status::error("ERR_CANCELLED", "Host request cancelled"), 0, {}};
    return state->response;
  }
}
class MacHostConsent final : public HostConsentPresenter {
 public:
  explicit MacHostConsent(NSView* parent) : parent_(parent) {}
  void present(const HostConsentRequest& request, HostCancellation cancellation,
               std::function<void(HostResult<bool>)> complete) override {
    if (![NSThread isMainThread]) {
      complete(
          {Status::error("ERR_WRONG_THREAD", "Consent requires AppKit main thread"), false, {}});
      return;
    }
    NSWindow* window = parent_.window;
    if (!window || !window.visible || window.attachedSheet || cancellation.cancelled()) {
      complete(
          {Status::error("ERR_INVALID_REQUEST", "Visible idle host window required for consent"),
           false,
           {}});
      return;
    }
    const bool phone = request.policy.kind == HostConsentKind::Phone;
    NSAlert* alert = [NSAlert new];
    alert.messageText = phone ? @"授权手机号" : @"授权昵称和头像";
    alert.informativeText = host_string(request.context.application.app_id + "\n\n" +
                                        (phone ? "允许此小程序获取宿主账号已验证的手机号？"
                                               : "允许此小程序获取宿主账号的昵称和头像？") +
                                        "\n\n用途：" + request.policy.purpose + "\n权限版本：" +
                                        request.policy.permission_version);
    [alert addButtonWithTitle:@"拒绝"];
    [alert addButtonWithTitle:@"允许"];
    __weak NSAlert* weak_alert = alert;
    __weak NSWindow* weak_window = window;
    cancellation.on_cancel([weak_alert, weak_window] {
      dispatch_async(dispatch_get_main_queue(), ^{
        if (weak_alert.window.sheetParent == weak_window)
          [weak_window endSheet:weak_alert.window returnCode:NSModalResponseCancel];
      });
    });
    [alert beginSheetModalForWindow:window
                  completionHandler:^(NSModalResponse response) {
                    if (cancellation.cancelled() || response == NSModalResponseCancel)
                      complete({Status::error("ERR_CANCELLED", "Consent cancelled"), false, {}});
                    else
                      complete({{}, response == NSAlertSecondButtonReturn, {}});
                  }];
  }

 private:
  __weak NSView* parent_;
};
std::shared_ptr<HostConsentPresenter> host_consent_presenter(void* parent, Status& status) {
  if (![NSThread isMainThread] || !parent) {
    status =
        Status::error("ERR_INVALID_REQUEST", "Configure a host NSView on the AppKit main thread");
    return {};
  }
  status = {};
  return std::make_shared<MacHostConsent>((__bridge NSView*)parent);
}
}  // namespace weapp::desktop::detail
