#include <media/voice_activity_detector.h>

#include <algorithm>
#include <cmath>

#include <media/audio_level.h>

namespace xrtc {
namespace {

/// 音量衰减速度：每毫秒掉 0.6（100 → 0 约 170ms），meter 回落不拖沓
constexpr double kDecayPerMs = 0.6;

int ClampParams(int value, int lo, int hi) {
    return std::clamp(value, lo, hi);
}

}  // namespace

VoiceActivityDetector::VoiceActivityDetector(const Params& params) {
    set_params(params);
}

void VoiceActivityDetector::set_params(const Params& params) {
    params_.talking_threshold = ClampParams(params.talking_threshold, 1, 100);
    params_.release_ms = ClampParams(params.release_ms, 0, 10000);
    params_.level_interval_ms = ClampParams(params.level_interval_ms, 0, 2000);
}

VoiceActivityDetector::Update VoiceActivityDetector::OnPcm16(
    const void* data, int bits_per_sample, int sample_rate_hz, size_t channels,
    size_t frames, int64_t now_ms) {
    if (!data || frames == 0 || bits_per_sample != 16) {
        return {};
    }
    (void)sample_rate_hz;  // 音量与采样率无关，不需要按秒归一化
    if (channels == 0) {
        channels = 1;
    }
    const size_t total = frames * channels;
    if (total == 0) {
        return {};
    }
    const int instant =
        AudioLevelFromPcm16(static_cast<const int16_t*>(data), total);
    return Evaluate(instant, now_ms);
}

VoiceActivityDetector::Update VoiceActivityDetector::OnSilence(int64_t now_ms) {
    return Evaluate(0, now_ms);
}

void VoiceActivityDetector::Reset() {
    level_ = 0;
    talking_ = false;
    last_above_ms_ = 0;
    last_level_ms_ = 0;
    last_update_ms_ = 0;
    has_update_ = false;
    has_level_ = false;
}

VoiceActivityDetector::Update VoiceActivityDetector::Evaluate(int instant_level,
                                                             int64_t now_ms) {
    Update update;

    // 1) 音量：取“瞬时值”和“按时间衰减后的旧值”中较大的一个，attack 快、release 柔和
    const int64_t dt = has_update_ ? std::max<int64_t>(0, now_ms - last_update_ms_)
                                   : 0;
    last_update_ms_ = now_ms;
    has_update_ = true;
    const int decayed =
        level_ - static_cast<int>(std::lround(kDecayPerMs * static_cast<double>(dt)));
    level_ = std::clamp(std::max(instant_level, decayed), 0, 100);

    // 2) 是否在说话：起说立即，说完要等 release_ms
    if (instant_level >= params_.talking_threshold) {
        last_above_ms_ = now_ms;
    }
    const bool want_talking =
        talking_ ? (now_ms - last_above_ms_ < params_.release_ms)
                 : (instant_level >= params_.talking_threshold);
    if (want_talking != talking_) {
        talking_ = want_talking;
        update.emit_talking = true;
    }
    update.talking = talking_;
    update.level = level_;

    // 3) 音量按间隔节流
    if (!has_level_ ||
        now_ms - last_level_ms_ >= params_.level_interval_ms) {
        update.emit_level = true;
        last_level_ms_ = now_ms;
        has_level_ = true;
    }
    return update;
}

}  // namespace xrtc
