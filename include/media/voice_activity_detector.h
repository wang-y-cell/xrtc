#pragma once

#include <cstddef>
#include <cstdint>

namespace xrtc {

/// 远端说话检测：PCM 帧 → 0~100 音量 + 是否在说话（带释放迟滞）。
///
/// 纯逻辑，不碰线程与 WebRTC，可单测。上层负责：
/// - 每收到一帧远端音频就调 OnPcm16；
/// - 定时（如 200ms）调 OnSilence，处理“对端静音 / 断流后不再来帧”；
/// - 按 Update 里的 emit_* 决定是否回调观察者。
///
/// 判定规则：
/// - 瞬时音量 >= talking_threshold 立即算“在说话”；
/// - 低于阈值后要连续 release_ms 才判为“说完”，避免字与字之间闪断；
/// - 音量按时间衰减（0.6/ms），meter 不会因为一帧大声就卡住；
/// - on_remote_audio_level 每 level_interval_ms 最多回调一次。
class VoiceActivityDetector {
public:
    struct Params {
        /// 0~100：瞬时音量 >= 该值算“在说话”
        int talking_threshold = 15;
        /// 低于阈值后持续多少毫秒才判为“说完”
        int release_ms = 600;
        /// 音量回调的最小间隔（毫秒），<=0 表示每帧都报
        int level_interval_ms = 100;
    };

    /// 单次输入的输出
    struct Update {
        bool emit_level = false;
        int level = 0;
        bool emit_talking = false;
        bool talking = false;
    };

    VoiceActivityDetector() = default;
    explicit VoiceActivityDetector(const Params& params);

    /// 参数会做范围收敛（阈值 1~100，其余 >= 0）
    void set_params(const Params& params);
    const Params& params() const { return params_; }

    /// 一帧 16-bit PCM（多声道交错）。bits_per_sample != 16 时忽略。
    Update OnPcm16(const void* data, int bits_per_sample, int sample_rate_hz,
                   size_t channels, size_t frames, int64_t now_ms);

    /// 没有新帧时的兜底推进（对端静音/断流）
    Update OnSilence(int64_t now_ms);

    /// 清空状态（解绑远端音频时用）
    void Reset();

    int level() const { return level_; }
    bool talking() const { return talking_; }

private:
    Update Evaluate(int instant_level, int64_t now_ms);

    Params params_;
    int level_ = 0;
    bool talking_ = false;
    int64_t last_above_ms_ = 0;
    int64_t last_level_ms_ = 0;
    int64_t last_update_ms_ = 0;
    bool has_update_ = false;
    bool has_level_ = false;
};

}  // namespace xrtc
