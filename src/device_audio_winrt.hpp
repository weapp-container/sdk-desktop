#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <roapi.h>
#include <robuffer.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Storage.Streams.h>
#include "device_audio_wave.hpp"

namespace weapp::desktop::detail {
namespace win_audio {
namespace playback = winrt::Windows::Media::Playback;
namespace streams = winrt::Windows::Storage::Streams;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IClosable;
struct Apartment {
  Apartment() { winrt::check_hresult(RoInitialize(RO_INIT_MULTITHREADED)); }
  ~Apartment() { RoUninitialize(); }
};
inline AudioFailure native_failure(const winrt::hresult_error& error) {
  return AudioFailure("Windows audio: " + winrt::to_string(error.message()), error.code() == E_ACCESSDENIED ? "permission_denied" : "invalid_request", int32_t(error.code()));
}
template<class Action> void release_step(Action action) {
  try { action(); } catch (const winrt::hresult_error& error) { if (error.code() != RO_E_CLOSED) throw; }
}
struct Notice { std::string event; Json extra = Json::object(); };
struct Signal {
  std::atomic<bool> alive{true}, opened{false}, failed{false};
  std::atomic<uint64_t> seeks{0};
  std::mutex mutex;
  std::deque<Notice> notices;
  Json error;
  void push(const std::weak_ptr<NativeAudio>& weak, uint64_t generation, Notice notice) {
    auto state = weak.lock(); if (!alive || !state || !state->current(generation)) return;
    std::lock_guard<std::mutex> guard(mutex);
    if (notices.size() >= 64) { state->fault("Windows audio native notice queue overflow"); return; }
    notices.push_back(std::move(notice)); state->changed.notify_all();
  }
};
class Backend final : public AudioBackend {
  // MTA outlives every native object and checked release operation.
  std::unique_ptr<Apartment> apartment_;
  std::filesystem::path root_;
  playback::MediaPlayer player_{nullptr};
  playback::MediaPlaybackSession session_{nullptr};
  winrt::Windows::Media::Core::MediaSource source_{nullptr};
  streams::InMemoryRandomAccessStream stream_{nullptr};
  streams::Buffer buffer_{nullptr};
  winrt::Windows::Foundation::IAsyncOperationWithProgress<uint32_t, uint32_t> write_{nullptr};
  std::shared_ptr<Signal> signal_;
  uint64_t generation_ = 0;
  Json source_error_;
  bool source_ready_ = false, error_sent_ = false, stopped_ = false;
  winrt::event_token opened_{}, ended_{}, failed_{}, state_{}, seeking_{}, waiting_{};
  bool has_opened_ = false, has_ended_ = false, has_failed_ = false, has_state_ = false, has_seeking_ = false, has_waiting_ = false;
  playback::MediaPlaybackState last_state_ = playback::MediaPlaybackState::None;
  std::chrono::steady_clock::time_point sampled_{};

  Json properties() {
    if (!session_ || !source_ready_) return {{"paused", true}, {"currentTime", 0.0}, {"duration", 0.0}, {"buffered", 0.0}};
    auto seconds = [](auto time) { const auto value = double(time.count()) / 10000000.0; if (!std::isfinite(value) || value < 0) throw AudioFailure("Windows returned invalid audio time"); return value; };
    const auto duration = seconds(session_.NaturalDuration());
    auto progress = session_.DownloadProgress();
    if (!std::isfinite(progress) || progress < 0 || progress > 1) throw AudioFailure("Windows returned invalid audio download progress");
    const auto current = session_.PlaybackState();
    return {{"paused", current != playback::MediaPlaybackState::Playing && current != playback::MediaPlaybackState::Buffering},
      {"currentTime", seconds(session_.Position())}, {"duration", duration}, {"buffered", progress * duration}};
  }
  bool release_decoder() {
    if (signal_) signal_->alive = false;
    // Projected event removal is noexcept and hides HRESULT. Use the official
    // SDK ABI and acknowledge each exact registration separately.
#define WEAPP_AUDIO_REMOVE(flag, object, interface_type, name, token) \
    if (flag) { release_step([&] { auto projection = object.as<interface_type>(); auto abi = static_cast<winrt::impl::abi_t<interface_type>*>(winrt::get_abi(projection)); winrt::check_hresult(abi->remove_##name(token)); }); flag = false; }
    WEAPP_AUDIO_REMOVE(has_opened_, player_, playback::IMediaPlayer, MediaOpened, opened_)
    WEAPP_AUDIO_REMOVE(has_ended_, player_, playback::IMediaPlayer, MediaEnded, ended_)
    WEAPP_AUDIO_REMOVE(has_failed_, player_, playback::IMediaPlayer, MediaFailed, failed_)
    WEAPP_AUDIO_REMOVE(has_state_, session_, playback::IMediaPlaybackSession, PlaybackStateChanged, state_)
    WEAPP_AUDIO_REMOVE(has_seeking_, session_, playback::IMediaPlaybackSession, SeekCompleted, seeking_)
    WEAPP_AUDIO_REMOVE(has_waiting_, session_, playback::IMediaPlaybackSession, BufferingStarted, waiting_)
#undef WEAPP_AUDIO_REMOVE
    if (write_) {
      if (write_.Status() == AsyncStatus::Started) { write_.Cancel(); return false; }
      release_step([&] { write_.Close(); }); write_ = nullptr; buffer_ = nullptr;
    }
    if (player_) { release_step([&] { player_.as<IClosable>().Close(); }); player_ = nullptr; session_ = nullptr; }
    if (source_) { release_step([&] { source_.as<IClosable>().Close(); }); source_ = nullptr; }
    if (stream_) { release_step([&] { stream_.Close(); }); stream_ = nullptr; }
    buffer_ = nullptr; signal_.reset(); source_ready_ = false; return true;
  }
  void drain_decoder() {
    while (true) {
      try { if (release_decoder()) return; } catch (const winrt::hresult_error&) { /* Preserve exact handles and retry. */ }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  void check(const std::shared_ptr<NativeAudio>& state, const std::shared_ptr<AudioCommand>& command) {
    if (command->cancelled || !state->current(command->generation)) throw AudioFailure("audio command was cancelled or its source retired", "cancelled");
    if (!source_error_.is_null() || (signal_ && signal_->failed)) {
      Json detail = source_error_; if (detail.is_null()) { std::lock_guard<std::mutex> guard(signal_->mutex); detail = signal_->error; }
      if (!error_sent_) { state->sample(generation_, Json::object(), "Error", detail); error_sent_ = true; }
      throw AudioFailure(detail.value("errMsg", "Windows media decode failed"), "invalid_request", detail.value("errCode", int64_t(0)), detail);
    }
  }
  template<class Predicate> void await(const std::shared_ptr<NativeAudio>& state, const std::shared_ptr<AudioCommand>& command, Predicate done) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (true) {
      check(state, command); if (done()) return; poll(state); check(state, command);
      if (std::chrono::steady_clock::now() >= deadline) throw AudioFailure("Windows audio terminal callback timed out", "timeout");
      std::unique_lock<std::mutex> lock(state->mutex); state->changed.wait_for(lock, std::chrono::milliseconds(10));
    }
  }
  void write_source(const std::shared_ptr<NativeAudio>& state, const std::shared_ptr<AudioCommand>& command, const std::vector<uint8_t>& bytes) {
    stream_ = streams::InMemoryRandomAccessStream(); buffer_ = streams::Buffer(uint32_t(bytes.size())); buffer_.Length(uint32_t(bytes.size()));
    uint8_t* target = nullptr; winrt::check_hresult(buffer_.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&target));
    std::memcpy(target, bytes.data(), bytes.size()); write_ = stream_.WriteAsync(buffer_);
    bool cancelled = false; auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (write_.Status() == AsyncStatus::Started) {
      if (!cancelled && (command->cancelled || !state->current(command->generation) || std::chrono::steady_clock::now() >= deadline)) { write_.Cancel(); cancelled = true; }
      // A cancellation request is not native completion. Keep the operation,
      // memory and MTA until its actual terminal state and checked Close.
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::exception_ptr error; uint32_t count = 0;
    try { count = write_.GetResults(); } catch (...) { error = std::current_exception(); }
    write_.Close(); write_ = nullptr; buffer_ = nullptr;
    if (error) std::rethrow_exception(error);
    check(state, command); if (cancelled) throw AudioFailure("audio source write timed out", "timeout");
    if (count != bytes.size()) throw AudioFailure("Windows audio stream write was incomplete"); stream_.Seek(0);
  }
  void attach(const std::shared_ptr<NativeAudio>& owner) {
    auto weak = std::weak_ptr<NativeAudio>(owner); auto signal = signal_; auto generation = generation_;
    opened_ = player_.MediaOpened([weak, signal, generation](const auto&, const auto&) { if (!signal->alive) return; signal->opened = true; if (auto state = weak.lock()) state->changed.notify_all(); }); has_opened_ = true;
    ended_ = player_.MediaEnded([weak, signal, generation](const auto&, const auto&) { signal->push(weak, generation, {"Ended"}); }); has_ended_ = true;
    failed_ = player_.MediaFailed([weak, signal, generation](const auto&, const playback::MediaPlayerFailedEventArgs& args) {
      if (!signal->alive) return;
      try {
        Json error = {{"errMsg", "Windows media: " + winrt::to_string(args.ErrorMessage())}, {"errCode", int32_t(args.ExtendedErrorCode())}, {"nativeMediaError", int32_t(args.Error())}, {"nativeErrorMessage", winrt::to_string(args.ErrorMessage())}};
        { std::lock_guard<std::mutex> guard(signal->mutex); signal->error = error; } signal->failed = true; signal->push(weak, generation, {"Error", error});
      } catch (...) { if (auto state = weak.lock()) state->fault("Windows audio failure callback could not be read"); }
    }); has_failed_ = true;
    state_ = session_.PlaybackStateChanged([weak, signal, generation](const auto&, const auto&) { signal->push(weak, generation, {"State"}); }); has_state_ = true;
    seeking_ = session_.SeekCompleted([weak, signal, generation](const auto&, const auto&) { if (!signal->alive) return; ++signal->seeks; if (auto state = weak.lock()) state->changed.notify_all(); }); has_seeking_ = true;
    waiting_ = session_.BufferingStarted([weak, signal, generation](const auto&, const auto&) { signal->push(weak, generation, {"Waiting"}); }); has_waiting_ = true;
  }
  void seek(const std::shared_ptr<NativeAudio>& state, const std::shared_ptr<AudioCommand>& command, double seconds, bool notify) {
    if (!source_ready_ || !session_.CanSeek()) throw AudioFailure("Windows audio source is not seekable", "unsupported");
    if (!std::isfinite(seconds) || seconds < 0 || static_cast<long double>(seconds) * 10000000.L >= 9223372036854775808.L) throw AudioFailure("invalid audio seek position");
    const auto target = int64_t(static_cast<long double>(seconds) * 10000000.L); const auto before = session_.Position().count();
    if (before == target) return;
    const auto expected = signal_->seeks.load() + 1;
    session_.Position(winrt::Windows::Foundation::TimeSpan(target));
    if (notify) state->sample(generation_, properties(), "Seeking");
    await(state, command, [&] { return signal_->seeks >= expected; });
    if (notify) state->sample(generation_, properties(), "Seeked");
  }
 public:
  explicit Backend(std::filesystem::path root) : root_(std::move(root)) {}
  void open(const std::shared_ptr<NativeAudio>&) override {
    try { apartment_ = std::make_unique<Apartment>(); player_ = playback::MediaPlayer(); player_.AutoPlay(false); player_.CommandManager().IsEnabled(false); }
    catch (const winrt::hresult_error& error) { throw native_failure(error); }
  }
  void poll(const std::shared_ptr<NativeAudio>& state) override {
    try {
      if (!state->current(generation_)) {
        drain_decoder(); generation_ = state->generation; stopped_ = false; error_sent_ = false; source_error_ = Json();
        if (!state->stopping()) state->sample(generation_, properties()); return;
      }
      if (!signal_) return;
      std::deque<Notice> notices; { std::lock_guard<std::mutex> guard(signal_->mutex); notices.swap(signal_->notices); }
      for (auto& notice : notices) {
        if (!state->current(generation_)) return;
        if (notice.event == "State") {
          auto value = session_.PlaybackState();
          if (source_ready_ && value != last_state_) {
            if (value == playback::MediaPlaybackState::Playing) state->sample(generation_, properties(), "Play");
            else if (value == playback::MediaPlaybackState::Paused && !stopped_) state->sample(generation_, properties(), "Pause");
            last_state_ = value;
          }
        } else if (notice.event == "Error") { if (!error_sent_) { state->sample(generation_, Json::object(), "Error", notice.extra); error_sent_ = true; } }
        else state->sample(generation_, properties(), notice.event, notice.extra);
      }
      if (signal_->failed) { { std::lock_guard<std::mutex> guard(signal_->mutex); source_error_ = signal_->error; } drain_decoder(); state->sample(generation_, properties()); return; }
      if (source_ready_ && session_.PlaybackState() == playback::MediaPlaybackState::Playing && std::chrono::steady_clock::now() - sampled_ >= std::chrono::milliseconds(250)) {
        sampled_ = std::chrono::steady_clock::now(); state->sample(generation_, properties());
      }
    } catch (const winrt::hresult_error& error) { throw native_failure(error); }
  }
  Json execute(const std::shared_ptr<NativeAudio>& state, const std::shared_ptr<AudioCommand>& command) override {
    try {
      const auto& member = command->member;
      if (member == "src") { if (command->cancelled || !state->current(command->generation)) throw AudioFailure("audio source was retired", "cancelled"); } else check(state, command);
      if (command->payload.value("action", "") == "set") {
        auto value = command->payload.at("value");
        if (member == "src") {
          drain_decoder(); generation_ = command->generation; stopped_ = false; error_sent_ = false; source_error_ = Json(); last_state_ = playback::MediaPlaybackState::None;
          state->sample(generation_, properties()); const auto path = value.get<std::string>(); if (path.empty()) return Json::object();
          auto bytes = command->resource;
          if (path.rfind("wxfile://user/", 0) == 0) { ManagedFileLock lock(root_); bytes = read_managed(managed_path(root_, path), 32 * 1024 * 1024); }
          validate_audio_wave(bytes); check(state, command); write_source(state, command, bytes);
          player_ = playback::MediaPlayer(); player_.AutoPlay(false); player_.CommandManager().IsEnabled(false); session_ = player_.PlaybackSession();
          signal_ = std::make_shared<Signal>(); attach(state); auto settings = state->snapshot();
          player_.Volume(settings.at("volume").get<double>()); player_.IsLoopingEnabled(settings.at("loop").get<bool>()); session_.PlaybackRate(settings.at("playbackRate").get<double>());
          source_ = winrt::Windows::Media::Core::MediaSource::CreateFromStream(stream_, L"audio/wav"); player_.Source(source_);
          await(state, command, [&] { return signal_ && signal_->opened; }); source_ready_ = true;
          seek(state, command, settings.at("startTime").get<double>(), false);
          state->sample(generation_, properties(), "Canplay");
          if (settings.at("autoplay").get<bool>()) { player_.Play(); await(state, command, [&] { return session_.PlaybackState() == playback::MediaPlaybackState::Playing; }); }
        } else if (player_ && source_ready_) {
          if (member == "volume") player_.Volume(value.get<double>());
          else if (member == "loop") player_.IsLoopingEnabled(value.get<bool>());
          else if (member == "playbackRate") session_.PlaybackRate(value.get<double>());
          // autoplay/startTime apply to the next source acquisition.
        }
      } else {
        if (!player_ || !source_ready_) throw AudioFailure("audio has no open source");
        if (member == "play") { stopped_ = false; player_.Play(); await(state, command, [&] { return session_.PlaybackState() == playback::MediaPlaybackState::Playing; }); }
        else if (member == "pause") { player_.Pause(); await(state, command, [&] { return session_.PlaybackState() == playback::MediaPlaybackState::Paused; }); }
        else if (member == "stop") { stopped_ = true; player_.Pause(); await(state, command, [&] { return session_.PlaybackState() == playback::MediaPlaybackState::Paused; }); seek(state, command, 0, false); state->sample(generation_, properties(), "Stop"); }
        else if (member == "seek") {
          const auto args = command->payload.value("args", Json::array());
          auto position = command->payload.value("position", args.is_array() && !args.empty() ? args.at(0) : Json());
          if (!position.is_number()) throw AudioFailure("seek requires one numeric position");
          seek(state, command, position.get<double>(), true);
        }
      }
      poll(state); return Json::object();
    } catch (const winrt::hresult_error& error) {
      auto failure = native_failure(error); drain_decoder(); if (state->current(generation_)) state->sample(generation_, properties()); throw failure;
    } catch (...) { drain_decoder(); if (state->current(generation_)) state->sample(generation_, properties()); throw; }
  }
  bool cleanup(const std::shared_ptr<NativeAudio>&) override { try { return release_decoder(); } catch (const winrt::hresult_error&) { return false; } }
};
} // namespace win_audio
inline AudioFactory win_audio_factory() { return [](const std::filesystem::path& root) { return std::make_unique<win_audio::Backend>(root); }; }
} // namespace weapp::desktop::detail
