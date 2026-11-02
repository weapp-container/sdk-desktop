#include "device_apis_common.hpp"
#include <iostream>

using namespace weapp::desktop::detail;
namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
struct Fixture {
  std::filesystem::path root = std::filesystem::temp_directory_path() /
      ("weapp-publication-test-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  std::shared_ptr<DeviceQueue> queue = std::make_shared<DeviceQueue>();
  std::atomic<bool> cancelled{false};
  Fixture() { std::filesystem::create_directories(root); }
  ~Fixture() { std::filesystem::remove_all(root); }
  bool publish(const std::string& name) {
    DeviceSideEffect publication(queue, &cancelled);
    if (!publication) return false;
    managed_write(root, root / name, {'d', 'a', 't', 'a'});
    require(queue_complete(queue, name, Json{{"filePath", name}}), "completion was rejected");
    return true;
  }
};
void cancellation_wins_before_pending_publication() {
  Fixture fixture;
  std::promise<void> ready, release;
  auto released = release.get_future();
  auto worker = std::async(std::launch::async, [&] {
    ready.set_value();
    released.wait();  // A completed OS request has not entered publication yet.
    return fixture.publish("cancelled.bin");
  });
  ready.get_future().wait();
  cancel_device_work(fixture.queue, fixture.cancelled);
  release.set_value();
  require(!worker.get(), "cancelled worker published a file");
  require(!std::filesystem::exists(fixture.root / "cancelled.bin"), "cancelled file exists");
  require(fixture.queue->items.empty(), "cancelled worker queued a completion");
}
void cancellation_waits_for_accepted_write_and_completion() {
  Fixture fixture;
  std::promise<void> entered, release, cancelling;
  auto released = release.get_future();
  auto worker = std::async(std::launch::async, [&] {
    DeviceSideEffect publication(fixture.queue, &fixture.cancelled);
    require(bool(publication), "publication did not acquire its owner");
    entered.set_value();
    released.wait();
    managed_write(fixture.root, fixture.root / "accepted.bin", {'o', 'k'});
    require(queue_complete(fixture.queue, "accepted", Json{{"ok", true}}), "accepted result was rejected");
  });
  entered.get_future().wait();
  auto cancellation = std::async(std::launch::async, [&] {
    cancelling.set_value();
    cancel_device_work(fixture.queue, fixture.cancelled);
    require(std::filesystem::exists(fixture.root / "accepted.bin"), "cancel returned before accepted write settled");
    std::lock_guard<std::mutex> guard(fixture.queue->mutex);
    require(fixture.queue->items.size() == 1, "cancel returned before accepted completion settled");
  });
  cancelling.get_future().wait();
  const auto pending = cancellation.wait_for(std::chrono::milliseconds(50));
  release.set_value();
  worker.get();
  cancellation.get();
  require(pending == std::future_status::timeout, "cancel escaped an active publication");
  require(!fixture.publish("too-late.bin"), "a later publication escaped cancellation");
  require(!std::filesystem::exists(fixture.root / "too-late.bin"), "post-cancel file exists");
}
void retired_owner_rejects_publication() {
  Fixture fixture;
  {
    std::lock_guard<std::mutex> barrier(fixture.queue->side_effects);
    fixture.queue->closed = true;
  }
  require(!fixture.publish("retired.bin"), "retired owner accepted publication");
  require(fixture.queue->items.empty(), "retired owner queued a completion");
}
}  // namespace
int main() {
  try {
    cancellation_wins_before_pending_publication();
    cancellation_waits_for_accepted_write_and_completion();
    retired_owner_rejects_publication();
    std::cout << "Device publication barrier: 3 cases passed (real managed files; fake OS completion timing)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
