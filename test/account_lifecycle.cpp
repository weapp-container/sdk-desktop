#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#include "internal.hpp"
#include "weapp/desktop.h"
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
Json state_value(bool app = false) {
  return {{"hostLoggedIn", true},
          {"appLoggedIn", app},
          {"profileAuthorized", false},
          {"phoneAuthorized", false}};
}
Json application(const std::string& root, const std::string& domain,
                 const std::string& app) {
  return {{"hostAppId", domain},
          {"tenantId", "tenant"},
          {"publisherId", "publisher"},
          {"appId", app},
          {"environment", "trial"},
          {"version", "1.0.0"},
          {"packRoot", root}};
}
Json services(const Json& app) {
  return {
      {"application", app},
      {"account", {{"principalId", "alice"}, {"epoch", "1"}}},
      {"apis", {"wx.login", "weappContainer.identity.getState"}},
      {"syncValues", {{"weappContainer.identity.getState", state_value()}}}};
}
std::shared_future<AccountCleanupResult> ready(AccountCleanupResult result) {
  std::promise<AccountCleanupResult> promise;
  auto future = promise.get_future().share();
  promise.set_value(std::move(result));
  return future;
}
struct Fixture {
  std::shared_ptr<Session> session;
  std::vector<Json> messages, errors, calls;
  Fixture(const std::string& pack, const Json& config,
          AccountCleanupHandler handler = {}) {
    Config settings;
    settings.metrics = {2, 1, 1, 2, 1, 0};
    settings.host_services = object(config);
    Status status;
    session = Session::create(settings, status);
    ok(status);
    require(bool(session), "session not created");
    if (handler) ok(session->set_account_cleanup_handler(std::move(handler)));
    session->on_event([this](const Event& event) {
      const auto fields = encode(event.fields);
      if (event.type == EventType::Message)
        messages.push_back(fields.at("message"));
      if (event.type == EventType::Error) errors.push_back(fields);
      if (event.type == EventType::HostCall) calls.push_back(fields);
    });
    ok(session->load(pack));
    pump();
    require(messages.size() == 1 && messages[0]["kind"] == "start",
            "actual QuickJS fixture did not start");
  }
  ~Fixture() {
    session->on_event({});
    auto status = session->close();
    for (int i = 0; i < 1000 && session->state().status != SessionState::Closed;
         ++i) {
      session->pump();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  void pump() { ok(session->pump()); }
  Json command(const std::string& operation,
               const Json& payload = Json::object()) {
    Value result;
    ok(session->host_services_command(operation, object(payload), &result));
    return encode(result);
  }
  Json change(const std::string& principal, const std::string& epoch,
              const std::string& app) {
    return command(
        "switchAccount",
        {{"account", {{"principalId", principal}, {"epoch", epoch}}},
         {"launches",
          {{app, {{"options", {{"query", {{"fresh", principal}}}}}}}}}});
  }
  Json receipt() { return command("queryAccountSwitch"); }
  void started(const std::string& principal) {
    for (int i = 0; i < 1000; ++i) {
      pump();
      if (messages.size() > 1 && messages.back().value("kind", "") == "start" &&
          messages.back()["query"].value("fresh", "") == principal)
        return;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("replacement did not start: " + receipt().dump() +
                             " errors=" + Json(errors).dump());
  }
};
void revoking_starts_real_cleanup_and_rejects_late_login(
    const std::string& pack, const std::string& root) {
  Fixture fixture(pack, services(application(root, "desktop-revoking", "one")));
  ok(fixture.session->send_message(decode(Json{{"op", "login"}})));
  fixture.pump();
  require(fixture.calls.size() == 1,
          "wx.login did not cross public SDK to host event");
  const auto call = fixture.calls[0];
  require(call["context"]["account"]["epoch"] == "1",
          "trusted context lost epoch");
  fixture.change("bob", "2", "one");
  fixture.started("bob");
  require(fixture.messages.size() == 2,
          "unexpected guest messages after switch");
  const auto stale = fixture.session->complete_host_call(
      call.at("callId"), decode(Json{{"code", "obsolete-code"}}));
  // Settling an already retired call is an idempotent no-op; its callback
  // must never enter the replacement realm.
  ok(stale);
  fixture.pump();
  require(fixture.messages.size() == 2, "late login entered replacement JS");
}
void asynchronous_snapshots_precede_actual_js(const std::string& pack,
                                              const std::string& root) {
  std::promise<AccountCleanupResult> terminal;
  auto future = terminal.get_future().share();
  int calls = 0;
  Fixture fixture(pack, services(application(root, "desktop-snapshot", "one")),
                  [&](const AccountCleanupContext& context) {
                    ++calls;
                    require(encode(context.account)["epoch"] == "2",
                            "hook received stale account");
                    require(encode(context.application)["packRoot"] == root,
                            "hook lost trusted package binding");
                    return future;
                  });
  fixture.change("bob", "2", "one");
  fixture.pump();
  require(calls == 1, "cleanup hook was not started once");
  for (int i = 0; i < 10; ++i) fixture.pump();
  require(calls == 1 && fixture.messages.size() == 1,
          "pending cleanup restarted or released JS");
  require(!fixture.session->send_message(decode(Json{{"op", "login"}})),
          "business input escaped cleanup gate");
  terminal.set_value(
      {{},
       object(Json{{"weappContainer.identity.getState", state_value(true)}})});
  fixture.started("bob");
  require(fixture.messages.back()["identity"]["appLoggedIn"] == true,
          "replacement JS missed prepared identity snapshot");
  require(calls == 1, "completed hook repeated");
}
void omitted_identity_snapshots_never_replay_the_previous_account(
    const std::string& pack, const std::string& root) {
  auto configured =
      services(application(root, "desktop-omitted-snapshot", "one"));
  configured["syncValues"]["weappContainer.identity.getState"] = {
      {"hostLoggedIn", true},
      {"appLoggedIn", true},
      {"profileAuthorized", true},
      {"phoneAuthorized", true}};
  Fixture fixture(pack, configured,
                  [](const AccountCleanupContext&) { return ready({{}, {}}); });
  require(fixture.messages[0]["identity"]["appLoggedIn"] == true,
          "initial prepared identity not observed by actual JS");
  fixture.change("bob", "2", "one");
  fixture.started("bob");
  auto identity = fixture.messages.back()["identity"];
  require(identity["hostLoggedIn"] == true &&
              identity["appLoggedIn"] == false &&
              identity["profileAuthorized"] == false &&
              identity["phoneAuthorized"] == false,
          "omitted replacement identity replayed old grants");
  // On the raster test runtime the replacement is already presented. A load
  // exercises configuration replay through a second actual JS heap.
  require(fixture.receipt().value("phase", "") == "complete",
          "replay regression requires raster presentation; GPU acceptance uses "
          "a real View");
  ok(fixture.session->load(pack));
  fixture.pump();
  require(fixture.messages.size() == 3 &&
              fixture.messages.back()["identity"] == identity,
          "reload restored previous-account identity grants");
}
void shared_epoch_and_superseded_tasks_keep_latest_owner(
    const std::string& pack, const std::string& root) {
  std::promise<AccountCleanupResult> old_terminal, new_terminal;
  auto old_future = old_terminal.get_future().share(),
       new_future = new_terminal.get_future().share();
  std::vector<std::string> epochs;
  Fixture one(pack, services(application(root, "desktop-shared", "one")));
  Fixture two(pack, services(application(root, "desktop-shared", "two")),
              [&](const AccountCleanupContext& context) {
                auto epoch =
                    encode(context.account).at("epoch").get<std::string>();
                epochs.push_back(epoch);
                return epoch == "2" ? old_future : new_future;
              });
  one.change("bob", "2", "one");
  require(!two.session->send_message(decode(Json{{"op", "login"}})),
          "sibling accepted old identity before its owner pumped");
  two.pump();
  require(epochs == std::vector<std::string>{"2"},
          "first cleanup did not start");
  one.change("carol", "3", "two");
  two.pump();
  require(epochs.size() == 1,
          "superseding cleanup overtook an unfinished prior task");
  old_terminal.set_value(
      {{},
       object(Json{{"weappContainer.identity.getState", state_value(true)}})});
  two.pump();
  require(epochs == std::vector<std::string>({"2", "3"}),
          "latest cleanup did not follow completed prior task");
  require(two.messages.size() == 1, "superseded JS owner started");
  new_terminal.set_value(
      {{},
       object(Json{{"weappContainer.identity.getState", state_value(false)}})});
  two.started("carol");
  require(two.messages.back()["identity"]["appLoggedIn"] == false,
          "stale identity snapshot reached current owner");
}
void failed_hook_requires_explicit_retry(const std::string& pack,
                                         const std::string& root) {
  int attempts = 0;
  Fixture fixture(
      pack, services(application(root, "desktop-retry", "one")),
      [&](const AccountCleanupContext&) {
        ++attempts;
        if (attempts == 1)
          return ready(
              {Status::error("ERR_BACKEND", "identity preflight failed"), {}});
        return ready({{},
                      object(Json{{"weappContainer.identity.getState",
                                   state_value(true)}})});
      });
  fixture.change("bob", "2", "one");
  for (int i = 0; i < 8; ++i) fixture.pump();
  require(attempts == 1 && fixture.messages.size() == 1,
          "failed cleanup retried or revived old owner");
  require(!fixture.errors.empty(), "cleanup failure was hidden");
  ok(fixture.session->retry_account_cleanup());
  fixture.started("bob");
  require(attempts == 2 &&
              fixture.messages.back()["identity"]["appLoggedIn"] == true,
          "retry did not prepare new owner");
}
void close_drains_pending_task_without_starting_replacement(
    const std::string& pack, const std::string& root) {
  std::promise<AccountCleanupResult> terminal;
  auto future = terminal.get_future().share();
  Fixture fixture(pack, services(application(root, "desktop-close", "one")),
                  [&](const AccountCleanupContext&) { return future; });
  fixture.change("bob", "2", "one");
  fixture.pump();
  const auto closing = fixture.session->close();
  require(!closing && closing.code == "ERR_RESOURCE_CLEANUP_PENDING",
          "close released pending cleanup task");
  terminal.set_value(
      {{},
       object(Json{{"weappContainer.identity.getState", state_value(true)}})});
  for (int i = 0;
       i < 1000 && fixture.session->state().status != SessionState::Closed;
       ++i) {
    fixture.pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(fixture.session->state().status == SessionState::Closed,
          "close did not progress after cleanup terminal");
  require(fixture.messages.size() == 1, "close started a replacement account");
}
void cleanup_failure_during_close_requires_retry(const std::string& pack,
                                                 const std::string& root) {
  std::promise<AccountCleanupResult> terminal;
  auto future = terminal.get_future().share();
  int attempts = 0;
  Fixture fixture(pack,
                  services(application(root, "desktop-close-failure", "one")),
                  [&](const AccountCleanupContext&) {
                    return ++attempts == 1 ? future : ready({{}, {}});
                  });
  fixture.change("bob", "2", "one");
  fixture.pump();
  require(fixture.session->close().code == "ERR_RESOURCE_CLEANUP_PENDING",
          "pending close lost its cleanup barrier");
  terminal.set_value(
      {Status::error("ERR_RESOURCE_RELEASE", "native host cleanup failed"),
       {}});
  const auto progress = fixture.session->pump();
  require(!progress && progress.code == "ERR_RESOURCE_RELEASE",
          "close ignored a failed resource cleanup");
  require(fixture.session->state().status != SessionState::Closed &&
              fixture.messages.size() == 1,
          "failed close released the old resource boundary");
  ok(fixture.session->retry_account_cleanup());
  for (int i = 0;
       i < 1000 && fixture.session->state().status != SessionState::Closed;
       ++i) {
    fixture.pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(attempts == 2 &&
              fixture.session->state().status == SessionState::Closed &&
              fixture.messages.size() == 1,
          "close retry restarted guest or failed to close");
}
void deferred_tasks_never_block_the_ui_thread(const std::string& pack,
                                              const std::string& root) {
  bool executed = false;
  int attempts = 0;
  Fixture fixture(pack, services(application(root, "desktop-deferred", "one")),
                  [&](const AccountCleanupContext&) {
                    if (++attempts == 1)
                      return std::async(std::launch::deferred,
                                        [&] {
                                          executed = true;
                                          return AccountCleanupResult{};
                                        })
                          .share();
                    return ready({{}, {}});
                  });
  fixture.change("bob", "2", "one");
  for (int i = 0; i < 4; ++i) fixture.pump();
  require(!executed && attempts == 1 && fixture.messages.size() == 1 &&
              !fixture.errors.empty(),
          "deferred work executed or silently stalled on UI thread");
  ok(fixture.session->retry_account_cleanup());
  fixture.started("bob");
  require(!executed, "retry executed an abandoned deferred task");
}
using CValue = std::unique_ptr<weapp_desktop_value,
                               decltype(&weapp_desktop_value_destroy)>;
CValue c_value(const Json& input) {
  CValue result(nullptr, weapp_desktop_value_destroy);
  if (input.is_null())
    result.reset(weapp_desktop_value_null());
  else if (input.is_boolean())
    result.reset(weapp_desktop_value_bool(input.get<bool>()));
  else if (input.is_number())
    result.reset(weapp_desktop_value_number(input.get<double>()));
  else if (input.is_string())
    result.reset(weapp_desktop_value_string(input.get<std::string>().c_str()));
  else if (input.is_array()) {
    result.reset(weapp_desktop_value_array());
    for (const auto& item : input) {
      auto child = c_value(item);
      require(weapp_desktop_value_append(result.get(), child.get()) == 0,
              "C array construction failed");
    }
  } else {
    result.reset(weapp_desktop_value_object());
    for (auto item = input.begin(); item != input.end(); ++item) {
      auto child = c_value(item.value());
      require(weapp_desktop_value_set(result.get(), item.key().c_str(),
                                      child.get()) == 0,
              "C object construction failed");
    }
  }
  require(bool(result), "C value allocation failed");
  return result;
}
void c_host_transport_prepares_identity_on_owner_thread(
    const std::string& pack, const std::string& root) {
  struct Context {
    int starts = 0, switches = 0;
    bool app_logged_in = false;
    weapp_desktop_account_cleanup* terminal = nullptr;
    std::string epoch;
  };
  Context context;
  weapp_desktop_config settings{};
  settings.metrics = {2, 1, 1, 2, 1, 0};
  weapp_desktop_session* session = nullptr;
  weapp_desktop_error error{};
  require(weapp_desktop_session_create(&settings, &session, &error) == 0,
          error.message);
  auto config =
      c_value(services(application(root, "desktop-c-transport", "one")));
  require(weapp_desktop_session_configure_host_services(session, config.get(),
                                                        &error) == 0,
          error.message);
  require(weapp_desktop_session_register_api_handler(session, "wx.login",
                                                     &error) == 0,
          error.message);
  auto event = [](void* ptr, weapp_desktop_event_type type,
                  const weapp_desktop_value* fields) {
    auto& value = *static_cast<Context*>(ptr);
    if (type == WEAPP_DESKTOP_EVENT_ACCOUNT_SWITCH) ++value.switches;
    if (type == WEAPP_DESKTOP_EVENT_MESSAGE) {
      const auto* message = weapp_desktop_value_get(fields, "message");
      const char* kind = weapp_desktop_value_get_string(
          weapp_desktop_value_get(message, "kind"));
      if (kind && std::string(kind) == "start") {
        ++value.starts;
        const auto* identity = weapp_desktop_value_get(message, "identity");
        value.app_logged_in = weapp_desktop_value_get_bool(
            weapp_desktop_value_get(identity, "appLoggedIn"));
      }
    }
  };
  require(weapp_desktop_session_on_event(session, event, &context, &error) == 0,
          error.message);
  auto cleanup = [](void* ptr, const char*, const weapp_desktop_value*,
                    const weapp_desktop_value* account,
                    weapp_desktop_account_cleanup* terminal) {
    auto& value = *static_cast<Context*>(ptr);
    value.terminal = terminal;
    value.epoch = weapp_desktop_value_get_string(
        weapp_desktop_value_get(account, "epoch"));
  };
  require(weapp_desktop_session_set_account_cleanup_handler(
              session, cleanup, &context, &error) == 0,
          error.message);
  require(weapp_desktop_session_load(session, pack.c_str(), nullptr, nullptr,
                                     &error) == 0,
          error.message);
  require(weapp_desktop_session_pump(session, &error) == 0, error.message);
  require(context.starts == 1, "C public SDK fixture did not enter actual JS");
  auto change =
      c_value(Json{{"account", {{"principalId", "bob"}, {"epoch", "2"}}}});
  weapp_desktop_value* receipt = nullptr;
  require(weapp_desktop_session_host_services_command(
              session, "switchAccount", change.get(), &receipt, &error) == 0,
          error.message);
  weapp_desktop_value_destroy(receipt);
  for (int i = 0; i < 20 && !context.terminal; ++i)
    require(weapp_desktop_session_pump(session, &error) == 0, error.message);
  require(context.terminal && context.epoch == "2" && context.switches > 0,
          "C cleanup/event lost trusted account operation");
  weapp_desktop_state state{};
  require(weapp_desktop_session_get_state(session, &state, &error) == 0,
          error.message);
  require(state.status == WEAPP_DESKTOP_IDENTITY_SWITCHING,
          "C identity-switching state mapping is incorrect");
  auto snapshots =
      c_value(Json{{"weappContainer.identity.getState", state_value(true)}});
  int completion = -1;
  weapp_desktop_error thread_error{};
  std::thread worker([&] {
    completion = weapp_desktop_account_cleanup_complete(
        &context.terminal, snapshots.get(), nullptr, nullptr, &thread_error);
  });
  worker.join();
  require(completion == 0 && !context.terminal, thread_error.message);
  require(weapp_desktop_account_cleanup_complete(&context.terminal, nullptr,
                                                 nullptr, nullptr, &error) != 0,
          "C cleanup completion accepted a second settlement");
  for (int i = 0; i < 1000 && context.starts < 2; ++i) {
    require(weapp_desktop_session_pump(session, &error) == 0, error.message);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(context.starts == 2 && context.app_logged_in,
          "C transport did not return prepared identity through real JS");
  require(weapp_desktop_session_close(session, &error) == 0, error.message);
  for (int i = 0; i < 1000; ++i) {
    require(weapp_desktop_session_pump(session, &error) == 0, error.message);
    require(weapp_desktop_session_get_state(session, &state, &error) == 0,
            error.message);
    if (state.status == WEAPP_DESKTOP_CLOSED) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(state.status == WEAPP_DESKTOP_CLOSED,
          "C session close did not reach legacy Closed state");
  require(weapp_desktop_session_destroy(&session, &error) == 0, error.message);
}
void original_c_abi_numbers_and_closed_event_remain_stable() {
  static_assert(WEAPP_DESKTOP_EMPTY == 0 && WEAPP_DESKTOP_READY == 1 &&
                WEAPP_DESKTOP_CLOSING == 2 && WEAPP_DESKTOP_CLOSED == 3);
  static_assert(WEAPP_DESKTOP_EVENT_ERROR == 6 &&
                WEAPP_DESKTOP_EVENT_STORAGE_FLUSHED == 7 &&
                WEAPP_DESKTOP_EVENT_CLOSED == 8 &&
                WEAPP_DESKTOP_EVENT_UNKNOWN == 9);
  weapp_desktop_session* session = nullptr;
  weapp_desktop_error error{};
  require(weapp_desktop_session_create(nullptr, &session, &error) == 0,
          error.message);
  bool closed = false;
  auto callback = [](void* context, weapp_desktop_event_type type,
                     const weapp_desktop_value*) {
    if (type == WEAPP_DESKTOP_EVENT_CLOSED) *static_cast<bool*>(context) = true;
  };
  require(
      weapp_desktop_session_on_event(session, callback, &closed, &error) == 0,
      error.message);
  require(weapp_desktop_session_close(session, &error) == 0, error.message);
  require(weapp_desktop_session_pump(session, &error) == 0, error.message);
  weapp_desktop_state state{};
  require(weapp_desktop_session_get_state(session, &state, &error) == 0,
          error.message);
  require(closed && state.status == WEAPP_DESKTOP_CLOSED,
          "legacy C Closed event/state changed numeric identity");
  require(weapp_desktop_session_destroy(&session, &error) == 0, error.message);
}
}  // namespace
int main(int argc, char** argv) {
  try {
    require(argc == 2, "usage: account_lifecycle PACK");
    std::ifstream file(std::filesystem::path(argv[1]) / "manifest.json");
    Json manifest;
    file >> manifest;
    const auto root = manifest.at("integrity").at("root").get<std::string>();
    revoking_starts_real_cleanup_and_rejects_late_login(argv[1], root);
    asynchronous_snapshots_precede_actual_js(argv[1], root);
    omitted_identity_snapshots_never_replay_the_previous_account(argv[1], root);
    shared_epoch_and_superseded_tasks_keep_latest_owner(argv[1], root);
    failed_hook_requires_explicit_retry(argv[1], root);
    close_drains_pending_task_without_starting_replacement(argv[1], root);
    cleanup_failure_during_close_requires_retry(argv[1], root);
    deferred_tasks_never_block_the_ui_thread(argv[1], root);
    c_host_transport_prepares_identity_on_owner_thread(argv[1], root);
    original_c_abi_numbers_and_closed_event_remain_stable();
    std::cout << "Desktop account lifecycle: 10 groups passed (public C++/C "
                 "SDK, native FFI and real QuickJS; authored fixture, no "
                 "device/merchant acceptance)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
