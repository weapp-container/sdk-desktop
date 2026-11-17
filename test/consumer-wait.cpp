// Deterministic host-loop scheduling test. No runtime, window or GPU is used.
#include "../consumer/wait.hpp"
#include <cassert>
#include <iostream>

using weapp::desktop::consumer::wait_until;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

int main() {
  Clock::time_point time{};
  int spins = 0, descriptions = 0;
  auto now = [&] { return time; };
  auto describe = [&] {
    ++descriptions;
    return std::string("generation=7 revision=9 inputEnabled=false; SDK errors: [ERR_RENDER]");
  };
  wait_until("already presented", 3s, [] { return true; },
      [&] { ++spins; }, now, describe);
  assert(spins == 0 && descriptions == 0);

  // A valid async presentation may complete later than the old 200 ms sleep.
  wait_until("presentation", 3s, [&] { return spins == 5; },
      [&] { ++spins; time += 50ms; }, now, describe);
  assert(spins == 5 && time.time_since_epoch() == 250ms && descriptions == 0);

  // Hosts with asynchronous allocation must observe both views, including a
  // second view allocated on a later UI turn, before starting either load.
  spins = 0;
  bool left_allocated = false, right_allocated = false;
  wait_until("both native layouts", 3s,
      [&] { return left_allocated && right_allocated; },
      [&] {
        ++spins;
        time += 50ms;
        if (spins == 2) left_allocated = true;
        if (spins == 7) right_allocated = true;
      }, now, describe);
  assert(spins == 7 && left_allocated && right_allocated && descriptions == 0);

  spins = 0;
  bool timed_out = false;
  try {
    wait_until("geometry response", 3s, [] { return false; },
        [&] { ++spins; time += 1s; }, now, describe);
  } catch (const std::runtime_error& error) {
    timed_out = true;
    const std::string text = error.what();
    assert(text.find("timed out waiting for geometry response") != std::string::npos);
    assert(text.find("generation=7 revision=9 inputEnabled=false") != std::string::npos);
    assert(text.find("ERR_RENDER") != std::string::npos);
  }
  assert(timed_out && spins == 3 && descriptions == 1);
  std::cout << "PASS consumer host-loop wait: immediate, delayed, bounded timeout diagnostics\n";
}
