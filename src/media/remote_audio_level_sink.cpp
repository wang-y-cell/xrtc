#include <media/remote_audio_level_sink.h>

#include <utility>

#include "rtc_base/time_utils.h"

namespace xrtc {

RemoteAudioLevelSink::RemoteAudioLevelSink(
    uint64_t feed_id, VoiceActivityDetector::Params params,
    LevelCallback on_level, TalkingCallback on_talking)
    : feed_id_(feed_id),
      detector_(params),
      on_level_(std::move(on_level)),
      on_talking_(std::move(on_talking)) {}

RemoteAudioLevelSink::~RemoteAudioLevelSink() = default;

void RemoteAudioLevelSink::OnData(const void* audio_data, int bits_per_sample,
                                  int sample_rate, size_t number_of_channels,
                                  size_t number_of_frames) {
    VoiceActivityDetector::Update update;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        update = detector_.OnPcm16(audio_data, bits_per_sample, sample_rate,
                                   number_of_channels, number_of_frames,
                                   webrtc::TimeMillis());
    }
    Emit(update);
}

void RemoteAudioLevelSink::TickSilence(int64_t now_ms) {
    VoiceActivityDetector::Update update;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        update = detector_.OnSilence(now_ms);
    }
    Emit(update);
}

void RemoteAudioLevelSink::set_params(
    const VoiceActivityDetector::Params& params) {
    std::lock_guard<std::mutex> lock(mutex_);
    detector_.set_params(params);
}

void RemoteAudioLevelSink::NotifyDetached() {
    bool was_talking = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        was_talking = detector_.talking();
        detector_.Reset();
    }
    if (was_talking && on_talking_) {
        on_talking_(feed_id_, false);
    }
}

void RemoteAudioLevelSink::Emit(const VoiceActivityDetector::Update& update) {
    // 先说话状态再音量：UI 通常先关心“谁开始说话”
    if (update.emit_talking && on_talking_) {
        on_talking_(feed_id_, update.talking);
    }
    if (update.emit_level && on_level_) {
        on_level_(feed_id_, update.level);
    }
}

}  // namespace xrtc
