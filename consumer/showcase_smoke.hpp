#pragma once
#include <cstdlib>
#include <algorithm>
// Restricted showcase acceptance uses the shipped business pack and its real
// bridge protocol. The general SDK fixture exercises a different application.
inline int showcase_smoke(const std::string& pack, Host host, const std::string& report) {
  const auto storage = std::filesystem::temp_directory_path() /
      ("weapp-showcase-consumer-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(storage);
  try {
    require(c_consumer_smoke() == 0, "installed C interface failed");
    Config config; config.storage_root = storage.string(); config.storage_key = "first";
    Status status; auto first = Session::create(config, status); ok(status);
    config.storage_key = "second"; auto second = Session::create(config, status); ok(status);
    std::string first_id, second_id; std::vector<std::string> errors;
    const auto listen = [&](std::string& id, const Event& event) {
      if (event.type == EventType::Error) { errors.push_back("native runtime error event"); return; }
      if (event.type != EventType::Message) return;
      const auto& message = std::get<Value::Object>(event.fields.at("message").data);
      const auto type = message.find("type"); if (type == message.end()) return;
      if (std::get<std::string>(type->second.data) != "showcase.state.ready") return;
      require(std::get<double>(message.at("protocolVersion").data) == 1, "wrong showcase protocol");
      id = std::get<std::string>(message.at("sessionId").data);
    };
    first->on_event([&](const Event& event) { listen(first_id, event); });
    second->on_event([&](const Event& event) { listen(second_id, event); });
    auto left = View::create(first, host.parent, {0,0,360,420}, status); ok(status);
    auto right = View::create(second, host.parent, {380,0,360,420}, status); ok(status);
    auto wait = [&](const std::string& phase, auto predicate) {
      consumer::wait_until(phase, std::chrono::seconds(15), predicate,
        [&] { host.spin(.01); }, [] { return std::chrono::steady_clock::now(); },
        [&] { return "first=" + first_id + ", second=" + second_id + ", errors=" + std::to_string(errors.size()); });
    };
    wait("both native showcase views to receive their first layout", [&] {
      return host.laid_out(*left) && host.laid_out(*right);
    });
    ok(first->load(pack)); ok(second->load(pack));
    wait("two real showcase previews and independent bridge sessions", [&] {
      return left->input_enabled() && right->input_enabled() && !first_id.empty() && !second_id.empty();
    });
    require(first_id != second_id, "guest bridge sessions were shared");
    Frame initial; ok(first->snapshot(initial));
    require(initial && initial.width() > 0 && initial.height() > 0 && initial.byte_size() > 0, "showcase frame is empty");
    const auto before_restore = first->state().revision;
    ok(first->send_message(Value::Object{{"type","showcase.state.restore"},{"protocolVersion",1},
      {"sessionId",first_id},{"state",Value::Object{{"schemaVersion",1},{"foodNote","native installed consumer"}}}}));
    wait("actual host-to-guest restore renders the business pack", [&] { return first->state().revision > before_restore; });
    const auto second_revision = second->state().revision;
    const auto before_click = first->state().revision;
    host.click(*left,135,385);
    wait("native pointer opens the business shop", [&] { return first->state().revision > before_click; });
    require(second->state().revision == second_revision, "native pointer leaked to another session");
    Frame shop; ok(first->snapshot(shop));
    require(shop.byte_size() == initial.byte_size() &&
      !std::equal(shop.pixels(), shop.pixels()+shop.byte_size(), initial.pixels()), "native navigation did not change rendered pixels");
    const auto generation = first->state().generation;
    if (const char* foreign = std::getenv("WEAPP_SHOWCASE_FOREIGN_PACK")) {
      const auto denied = first->load(foreign);
      require(!denied && denied.code == "ERR_UNTRUSTED_APPLICATION", "foreign pack policy was not enforced");
      require(first->state().generation == generation, "denied pack replaced the active business app");
    } else throw std::runtime_error("explicit foreign pack is required for restricted acceptance");
    Capabilities capabilities; ok(first->capabilities(capabilities));
    require(capabilities.quickjs && capabilities.skia_cpu, "showcase did not execute actual QuickJS and Skia");
    ok(left->set_bounds({0,0,280,300})); host.spin(.2);
    Frame resized; ok(first->snapshot(resized));
    require(resized.width() < shop.width() && resized.height() < shop.height(), "physical resize did not reach renderer");
    ok(first->flush_storage()); ok(first->close()); ok(second->close());
    wait("both showcase sessions close", [&] { return first->state().status == SessionState::Closed && second->state().status == SessionState::Closed; });
    left.reset(); right.reset(); first.reset(); second.reset();
    require(initial.pixels() && initial.byte_size(), "immutable frame lifetime failed");
    require(errors.empty(), "showcase raised an unexpected native error event");
    std::filesystem::remove_all(storage);
    const std::string result = "PASS restricted showcase installed C/C++; exact business pack; real QuickJS/Skia; two native views and bridge sessions; host restore; native shop pointer and isolated state; foreign pack denied with rollback; resize; close; retained frame";
    std::cout << result << std::endl; if (!report.empty()) { std::ofstream out(report); out << result << "\n"; }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL restricted showcase: " << error.what() << std::endl;
    std::filesystem::remove_all(storage); return 1;
  }
}
