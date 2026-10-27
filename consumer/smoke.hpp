#pragma once
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <weapp/desktop.hpp>
using namespace weapp::desktop;
extern "C" int c_consumer_smoke(void);
inline void require(bool yes, const std::string& message) {
  if (!yes) throw std::runtime_error(message);
}
inline void ok(const Status& s) {
  if (!s) throw std::runtime_error(s.code + ": " + s.message);
}
struct Host {
  void* parent;
  std::function<void(double)> spin;
  std::function<void(View&, double, double)> click;
  std::function<void(View&, double, double, double)> drag;
  std::function<void(View&)> key;
  std::function<void(bool)> window_visible;
};
inline int smoke(const std::string& pack, Host host,
                 const std::string& report) {
  const auto unique = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  auto storage = std::filesystem::temp_directory_path() /
                 ("weapp-desktop-consumer-" + unique);
  std::filesystem::create_directories(storage);
  try {
    require(c_consumer_smoke() == 0, "installed C interface consumer failed");
    Config config;
    config.storage_root = storage.string();
    config.storage_key = "first";
    Status status;
    auto first = Session::create(config, status);
    ok(status);
    config.storage_key = "second";
    auto second = Session::create(config, status);
    ok(status);
    std::vector<EventType> first_events, second_events;
    std::map<std::string, Value::Object> messages;
    std::vector<std::string> errors;
    auto required_message = [&](const std::string& kind) -> const Value::Object& {
      auto found = messages.find(kind);
      if (found == messages.end()) {
        std::string seen;
        for (const auto& entry : messages) seen += " " + entry.first;
        throw std::runtime_error("missing consumer message " + kind + "; received:" + seen +
                                 "; SDK errors: " + std::to_string(errors.size()));
      }
      return found->second;
    };
    first->on_event([&](const Event& e) {
      first_events.push_back(e.type);
      if (e.type == EventType::Message) {
        const auto& message = std::get<Value::Object>(e.fields.at("message").data);
        messages[std::get<std::string>(message.at("kind").data)] = message;
      }
      if (e.type == EventType::Error) errors.push_back("first error event");
    });
    second->on_event([&](const Event& e) {
      second_events.push_back(e.type);
      if (e.type == EventType::Error) errors.push_back("second error event");
    });
    auto left = View::create(first, host.parent, {0, 0, 360, 420}, status);
    ok(status);
    auto right = View::create(second, host.parent, {380, 0, 360, 420}, status);
    ok(status);
    host.spin(.15);
    auto duplicate = View::create(first, host.parent, {0, 0, 50, 50}, status);
    require(!duplicate && status.code == "ERR_SURFACE_ATTACHED",
            "second presenter was not rejected");
    ok(first->load(pack));
    ok(second->load(pack));
    host.spin(.2);
    Frame a, b;
    ok(first->snapshot(a));
    ok(second->snapshot(b));
    require(a && a.width() > 0 && a.height() > 0 &&
                a.byte_size() == size_t(a.stride()) * a.height(),
            "first frame is invalid");
    const auto second_revision = second->state().revision;
    const auto before = first->state().revision;
    ok(first->send_message(Value::Object{{"op", "inspect"}}));
    host.spin(.05);
    const auto geometry = required_message("geometry");
    const auto tap = std::get<Value::Object>(geometry.at("tap").data);
    host.click(*left, std::get<double>(tap.at("x").data), std::get<double>(tap.at("y").data));
    host.spin(.15);
    require(first->state().revision > before &&
                std::get<double>(required_message("tap").at("count").data) == 1 &&
                messages.count("timer") == 1,
            "native pointer input did not reach JavaScript and its timer");
    require(second->state().revision == second_revision,
            "input leaked into the second session");
    const auto scroll = std::get<Value::Object>(geometry.at("scroll").data);
    const double x = std::get<double>(scroll.at("left").data) + std::get<double>(scroll.at("width").data) / 2;
    const double top = std::get<double>(scroll.at("top").data), height = std::get<double>(scroll.at("height").data);
    host.drag(*left, x, top + height * .8, top + height * .2);
    host.spin(.05);
    require(std::get<double>(required_message("scroll").at("scrollTop").data) > 0,
            "native drag input did not scroll the shared fixture");
    ok(first->navigate("pages/detail/index", {{"source", "native-consumer"}}));
    host.spin(.05);
    require(first->state().route == "pages/detail/index", "navigation did not push the detail page");
    ok(first->back());
    host.spin(.05);
    require(first->state().route == "pages/home/index", "back did not restore the home page");
    host.key(*left);
    ok(first->pump());
    auto generation = first->state().generation;
    auto preserved = first->state().revision;
    auto invalid = first->load(pack + "/missing");
    require(!invalid, "invalid candidate unexpectedly loaded");
    require(first->state().generation == generation,
            "failed load replaced the working runtime");
#ifdef __APPLE__
    require(first->state().revision > preserved,
            "reattaching Metal did not accept a fresh presentation revision");
#else
    require(first->state().revision == preserved,
            "failed load changed the raster frame revision");
#endif
    ok(first->pump());
    Capabilities capabilities;
    ok(first->capabilities(capabilities));
    require(capabilities.quickjs && capabilities.skia_cpu,
            "consumer did not use real QuickJS/Skia");
#ifdef __APPLE__
    require(capabilities.metal && capabilities.gpu_presented,
            "macOS consumer did not attach real Metal");
#else
    require(!capabilities.gpu_presented,
            "raster consumer claimed a GPU surface");
#endif
    const double ratio = double(a.width()) / 360.0;
    ok(left->set_bounds({0, 0, 280, 300}));
    host.spin(.15);
    Frame resized;
    ok(first->snapshot(resized));
    require(resized.width() == uint32_t(std::lround(280 * ratio)) &&
                resized.height() == uint32_t(std::lround(300 * ratio)),
            "DPR resize did not reach snapshot dimensions");
    ok(left->set_visible(false));
    host.spin(.05);
    require(!first->pointer({PointerAction::Down, 0, 100, 100}),
            "hidden session accepted input");
    ok(first->flush_storage());
    host.spin(.1);  // service timer continues while hidden
    ok(left->set_visible(true));
    host.spin(.1);
    host.window_visible(false);
    host.spin(.05);
    require(!first->state().foreground,
            "parent-window hide did not suspend the session");
    host.window_visible(true);
    host.spin(.1);
    require(first->state().foreground,
            "parent-window show did not resume the session");
    auto wrong = Status{};
    std::thread worker([&] { wrong = first->back(); });
    worker.join();
    require(wrong.code == "ERR_WRONG_THREAD",
            "wrong-thread command was not rejected");
    ok(first->close());
    ok(second->close());
    for (int n = 0; n < 300 && (first->state().status != SessionState::Closed ||
                                second->state().status != SessionState::Closed);
         ++n)
      host.spin(.01);
    require(first->state().status == SessionState::Closed &&
                second->state().status == SessionState::Closed,
            "async close did not finish while the host loop pumped");
    left.reset();
    right.reset();
    first.reset();
    second.reset();
    require(a.pixels() != nullptr && a.byte_size() > 0,
            "immutable frame did not survive session destruction");
    config.storage_key = "first";
    config.metrics = {360, 420, 1, 1024, 768, 0};
    auto reopened = Session::create(config, status);
    ok(status);
    bool restored = false;
    reopened->on_event([&](const Event& event) {
      if (event.type != EventType::Message) return;
      const auto& message = std::get<Value::Object>(event.fields.at("message").data);
      if (std::get<std::string>(message.at("kind").data) == "initial")
        restored = std::get<double>(message.at("stored").data) == 1;
    });
    ok(reopened->load(pack));
    ok(reopened->pump());
    require(restored, "persisted click state did not survive close/reopen");
    ok(reopened->close());
    for (int n = 0; n < 300 && reopened->state().status != SessionState::Closed;
         ++n)
      host.spin(.01);
    require(reopened->state().status == SessionState::Closed,
            "storage lock was not released for reopen");
    reopened.reset();
    require(errors.empty(), "native event loop reported unexpected SDK errors");
    std::filesystem::remove_all(storage);
    const std::string message =
        "PASS installed C/C++ consumers; two native views; real QuickJS/Skia; "
        "isolated native pointer input; JavaScript timer; native drag scroll; navigation/back; keyboard; failed load rollback; "
        "physical DPR resize; hidden service pump; parent-window visibility "
        "restore; owner thread; async close; "
        "frame lifetime; persistent lock reopen";
    std::cout << message << std::endl;
    if (!report.empty()) {
      std::ofstream out(report);
      out << message << "\n";
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << std::endl;
    std::filesystem::remove_all(storage);
    return 1;
  }
}
