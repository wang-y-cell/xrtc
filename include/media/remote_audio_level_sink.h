#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>

#include "api/media_stream_interface.h"

#include <media/voice_activity_detector.h>

namespace xrtc {

/// 挂在远端音频轨上的 sink：把解码后的 PCM 折算成音量/说话状态并上报。
///
/// 线程约定：
/// - OnData 由 WebRTC 音频线程调用（约每 10ms 一帧）；
/// - TickSilence / set_params / NotifyDetached 由持有方（CallSession，api 线程）
///   调用；
/// - 共享状态由 mutex 保护；回调在解锁之后触发，避免把使用方代码锁在里面。
class RemoteAudioLevelSink final : public webrtc::AudioTrackSinkInterface {
public:
    using LevelCallback = std::function<void(uint64_t feed_id, int level)>;
    using TalkingCallback = std::function<void(uint64_t feed_id, bool talking)>;

    RemoteAudioLevelSink(uint64_t feed_id, VoiceActivityDetector::Params params,
                         LevelCallback on_level, TalkingCallback on_talking);
    ~RemoteAudioLevelSink() override;

    RemoteAudioLevelSink(const RemoteAudioLevelSink&) = delete;
    RemoteAudioLevelSink& operator=(const RemoteAudioLevelSink&) = delete;

    // webrtc::AudioTrackSinkInterface
    void OnData(const void* audio_data, int bits_per_sample, int sample_rate,
                size_t number_of_channels, size_t number_of_frames) override;

    /// 兜底推进：对端静音/断流后不再来帧时，由看门狗定时调用
    void TickSilence(int64_t now_ms);

    void set_params(const VoiceActivityDetector::Params& params);

    uint64_t feed_id() const { return feed_id_; }

    /// 解绑时调用：若正处于“说话”状态，补一次 talking=false
    void NotifyDetached();

private:
    void Emit(const VoiceActivityDetector::Update& update);

    uint64_t feed_id_ = 0;
    mutable std::mutex mutex_;
    VoiceActivityDetector detector_;
    LevelCallback on_level_;
    TalkingCallback on_talking_;
};

}  // namespace xrtc
