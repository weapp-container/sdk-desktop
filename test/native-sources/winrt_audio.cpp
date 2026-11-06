#include "device_audio_winrt.hpp"
weapp::desktop::detail::AudioFactory checked_audio_factory() {
  return weapp::desktop::detail::win_audio_factory();
}
