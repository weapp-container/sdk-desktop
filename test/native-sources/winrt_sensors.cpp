#include "device_sensor_winrt.hpp"
// Force all three projected event registration/removal specializations to be
// instantiated against the installed official C++/WinRT headers.
weapp::desktop::detail::SensorFactory checked_sensor_factory() {
  return weapp::desktop::detail::win_sensor_factory();
}
