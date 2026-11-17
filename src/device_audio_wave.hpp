#pragma once
#include "device_audio_common.hpp"
#include <cstring>
namespace weapp::desktop::detail {
// Accept only a self-contained RIFF/WAVE PCM or IEEE-float stream. In
// particular, playlists, URLs, codec-private and external-reference chunks
// never reach the operating system's media resolver.
inline void validate_audio_wave(const std::vector<uint8_t>& bytes) {
  constexpr size_t limit = 32 * 1024 * 1024;
  auto reject = [] { throw AudioFailure("audio requires a bounded self-contained PCM/IEEE-float RIFF/WAVE source", "unsupported"); };
  if (bytes.size() < 44 || bytes.size() > limit) reject();
  auto tag = [&](size_t offset, const char* value) { return std::memcmp(bytes.data() + offset, value, 4) == 0; };
  auto u16 = [&](size_t offset) { return uint32_t(bytes[offset]) | uint32_t(bytes[offset + 1]) << 8; };
  auto u32 = [&](size_t offset) { return u16(offset) | u16(offset + 2) << 16; };
  if (!tag(0, "RIFF") || !tag(8, "WAVE") || uint64_t(u32(4)) + 8 != bytes.size()) reject();
  bool format = false, data = false; uint32_t alignment = 0; size_t offset = 12;
  while (offset < bytes.size()) {
    if (bytes.size() - offset < 8) reject();
    const auto length = size_t(u32(offset + 4));
    if (length > bytes.size() - offset - 8) reject();
    const auto start = offset + 8;
    if (tag(offset, "fmt ")) {
      if (format || data || length != 16) reject();
      const auto codec = u16(start), channels = u16(start + 2), rate = u32(start + 4), byte_rate = u32(start + 8), bits = u16(start + 14);
      alignment = u16(start + 12);
      if ((codec != 1 && codec != 3) || channels < 1 || channels > 8 || rate < 8000 || rate > 192000 ||
          (codec == 1 && bits != 8 && bits != 16 && bits != 24 && bits != 32) || (codec == 3 && bits != 32 && bits != 64) ||
          alignment != channels * bits / 8 || byte_rate != rate * alignment) reject();
      format = true;
    } else if (tag(offset, "data")) {
      if (!format || data || !length || length % alignment != 0) reject(); data = true;
    } else if (!tag(offset, "JUNK") && !tag(offset, "PAD ")) reject();
    if ((length & 1) && start + length == bytes.size()) reject();
    offset = start + length + (length & 1);
  }
  if (!format || !data || offset != bytes.size()) reject();
}
} // namespace weapp::desktop::detail
