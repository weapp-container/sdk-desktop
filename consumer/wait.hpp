#pragma once
#include <chrono>
#include <stdexcept>
#include <string>

namespace weapp::desktop::consumer {
// Wait only for host-loop progress. This deadline never changes the runtime's
// execution budget, and the caller must issue an operation outside this loop.
template <class Ready, class Spin, class Now, class Describe>
void wait_until(const std::string& phase,
                std::chrono::steady_clock::duration timeout, Ready ready,
                Spin spin, Now now, Describe describe) {
  const auto deadline = now() + timeout;
  while (!ready()) {
    if (now() >= deadline)
      throw std::runtime_error("timed out waiting for " + phase + "; " +
                               describe());
    spin();
  }
}
}  // namespace weapp::desktop::consumer
