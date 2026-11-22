#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "rtc_base/time_utils.h"

#include <media/audio_level.h>
#include <media/remote_audio_level_sink.h>
#include <media/voice_activity_detector.h>

namespace {

/// 方波：RMS 与峰值都等于 amplitude，便于断言音量刻度
std::vector<int16_t> MakeSquare(int amplitude, size_t samples) {
    std::vector<int16_t> pcm(samples);
    for (size_t i = 0; i < samples; ++i) {
        pcm[i] = static_cast<int16_t>(i % 2 == 0 ? amplitude : -amplitude);
    }
    return pcm;
}

/// 10ms @ 48kHz 单声道 = 480 个采样点
constexpr size_t kFrameSamples = 480;

xrtc::VoiceActivityDetector::Update Feed(
    xrtc::VoiceActivityDetector* detector, const std::vector<int16_t>& pcm,
    int64_t now_ms) {
    return detector->OnPcm16(pcm.data(), 16, 48000, 1, pcm.size(), now_ms);
}

}  // namespace

// 本地麦克风与远端检测共用同一刻度：静音 0、方波按幅度线性、满量程夹到 100
TEST(AudioLevel, MapsPcm16ToZeroToHundred) {
    const auto silence = std::vector<int16_t>(kFrameSamples, 0);
    EXPECT_EQ(xrtc::AudioLevelFromPcm16(silence.data(), silence.size()), 0);

    const auto quiet = MakeSquare(500, kFrameSamples);
    const int quiet_level = xrtc::AudioLevelFromPcm16(quiet.data(), quiet.size());
    EXPECT_GE(quiet_level, 1);
    EXPECT_LE(quiet_level, 5);

    const auto half = MakeSquare(16000, kFrameSamples);
    const int half_level = xrtc::AudioLevelFromPcm16(half.data(), half.size());
    EXPECT_GE(half_level, 80);
    EXPECT_LE(half_level, 95);

    const auto full = MakeSquare(32767, kFrameSamples);
    EXPECT_EQ(xrtc::AudioLevelFromPcm16(full.data(), full.size()), 100);

    EXPECT_EQ(xrtc::AudioLevelFromPcm16(nullptr, 10), 0);
    EXPECT_EQ(xrtc::AudioLevelFromPcm16(silence.data(), 0), 0);
}

// 大声帧：立刻进入 talking，且只回调一次状态切换
TEST(VoiceActivityDetector, LoudFramesStartTalkingOnce) {
    xrtc::VoiceActivityDetector detector;
    const auto loud = MakeSquare(10000, kFrameSamples);

    const auto first = Feed(&detector, loud, 0);
    EXPECT_TRUE(first.emit_level);
    EXPECT_TRUE(first.emit_talking);
    EXPECT_TRUE(first.talking);
    EXPECT_GE(first.level, 50);

    const auto second = Feed(&detector, loud, 10);
    EXPECT_FALSE(second.emit_talking);  // 状态没变，不重复通知
    EXPECT_TRUE(second.talking);
    EXPECT_FALSE(second.emit_level);  // 间隔不足 100ms，音量节流
}

// 低于阈值后要等 release_ms 才判为说完（字间不闪断）
TEST(VoiceActivityDetector, ReleaseDelayKeepsTalkingThenFalls) {
    xrtc::VoiceActivityDetector detector;
    const auto loud = MakeSquare(10000, kFrameSamples);
    ASSERT_TRUE(Feed(&detector, loud, 0).talking);

    const auto early = detector.OnSilence(300);
    EXPECT_TRUE(early.talking);
    EXPECT_FALSE(early.emit_talking);

    const auto late = detector.OnSilence(700);
    EXPECT_FALSE(late.talking);
    EXPECT_TRUE(late.emit_talking);
}

// release_ms=0：静音立刻结束
TEST(VoiceActivityDetector, ZeroReleaseStopsImmediately) {
    xrtc::VoiceActivityDetector::Params params;
    params.release_ms = 0;
    xrtc::VoiceActivityDetector detector(params);

    const auto loud = MakeSquare(10000, kFrameSamples);
    ASSERT_TRUE(Feed(&detector, loud, 0).talking);

    const auto silent = detector.OnSilence(10);
    EXPECT_FALSE(silent.talking);
    EXPECT_TRUE(silent.emit_talking);
}

// 阈值以下的声音不触发 talking，但音量照常上报
TEST(VoiceActivityDetector, QuietAudioDoesNotTalk) {
    xrtc::VoiceActivityDetector detector;
    const auto quiet = MakeSquare(300, kFrameSamples);

    const auto update = Feed(&detector, quiet, 0);
    EXPECT_TRUE(update.emit_level);
    EXPECT_LT(update.level, 15);
    EXPECT_FALSE(update.talking);
    EXPECT_FALSE(update.emit_talking);
}

// 音量随静音衰减到 0，且按 level_interval_ms 节流
TEST(VoiceActivityDetector, LevelDecaysAndIsThrottled) {
    xrtc::VoiceActivityDetector detector;
    const auto loud = MakeSquare(20000, kFrameSamples);
    ASSERT_GT(Feed(&detector, loud, 0).level, 50);

    EXPECT_FALSE(detector.OnSilence(50).emit_level);  // 未到 100ms
    const auto decayed = detector.OnSilence(200);
    EXPECT_TRUE(decayed.emit_level);
    EXPECT_LT(decayed.level, 50);
    EXPECT_EQ(detector.OnSilence(4000).level, 0);
}

// 参数收敛：阈值 0→1，负数毫秒→0，超大间隔→2000
TEST(VoiceActivityDetector, ClampsParams) {
    xrtc::VoiceActivityDetector::Params params;
    params.talking_threshold = 0;
    params.release_ms = -100;
    params.level_interval_ms = 9999;
    xrtc::VoiceActivityDetector detector(params);

    EXPECT_EQ(detector.params().talking_threshold, 1);
    EXPECT_EQ(detector.params().release_ms, 0);
    EXPECT_EQ(detector.params().level_interval_ms, 2000);
}

// 非 16-bit / 空数据不产生任何更新
TEST(VoiceActivityDetector, IgnoresUnsupportedInput) {
    xrtc::VoiceActivityDetector detector;
    const auto pcm = MakeSquare(10000, kFrameSamples);
    const auto bad_bits =
        detector.OnPcm16(pcm.data(), 32, 48000, 1, pcm.size(), 0);
    EXPECT_FALSE(bad_bits.emit_level);
    EXPECT_FALSE(bad_bits.emit_talking);
    EXPECT_EQ(detector.level(), 0);
}

// sink 端到端：OnData 触发回调；解绑时补 talking=false
TEST(RemoteAudioLevelSink, EmitsLevelAndTalkingThenDetach) {
    xrtc::VoiceActivityDetector::Params params;
    int last_level = -1;
    bool talking = false;
    int talking_events = 0;

    xrtc::RemoteAudioLevelSink sink(
        42, params,
        [&](uint64_t feed_id, int level) {
            EXPECT_EQ(feed_id, 42u);
            last_level = level;
        },
        [&](uint64_t feed_id, bool value) {
            EXPECT_EQ(feed_id, 42u);
            talking = value;
            ++talking_events;
        });

    const auto loud = MakeSquare(12000, kFrameSamples);
    sink.OnData(loud.data(), 16, 48000, 1, loud.size());
    EXPECT_GE(last_level, 50);
    EXPECT_TRUE(talking);
    EXPECT_EQ(talking_events, 1);

    // 断流：看门狗用“未来时间”推进，release 到期后应收到 false
    sink.TickSilence(webrtc::TimeMillis() + 2000);
    EXPECT_FALSE(talking);
    EXPECT_EQ(talking_events, 2);

    // 还在说话时被解绑 → 补一次 false，不留高亮
    sink.OnData(loud.data(), 16, 48000, 1, loud.size());
    ASSERT_TRUE(talking);
    sink.NotifyDetached();
    EXPECT_FALSE(talking);
    EXPECT_EQ(talking_events, 4);
}
