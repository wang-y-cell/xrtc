#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include <api/video/video_frame.h>
#include <api/video/video_sink_interface.h>
#include <engine/xrtc_global.h>
#include <media/screen_capture.h>
#include <media/video_track_source.h>
#include <xrtc/ixrtc_engine.h>
#include <xrtc/xrtc_defines.h>

namespace {

using namespace std::chrono_literals;

struct FrameProbe : xrtc::XRtcEngineObserver {
    std::atomic<int> frames{0};
    std::atomic<int> width{0};
    std::atomic<int> height{0};

    void on_video_frame(xrtc::IXRtcMediaSource*,
                        const xrtc::XRTCVideoFrame& frame) override {
        if (!frame.valid()) {
            return;
        }
        width.store(frame.width);
        height.store(frame.height);
        frames.fetch_add(1);
    }
};

/// 直接挂在推流源上的 sink：验证真正送进编码器的分辨率
struct PushProbe : webrtc::VideoSinkInterface<webrtc::VideoFrame> {
    std::atomic<int> frames{0};
    std::atomic<int> width{0};
    std::atomic<int> height{0};

    void OnFrame(const webrtc::VideoFrame& frame) override {
        const auto buffer = frame.video_frame_buffer();
        if (!buffer) {
            return;
        }
        width.store(buffer->width());
        height.store(buffer->height());
        frames.fetch_add(1);
    }
};

}  // namespace

// 需要真实桌面：设置 XRTC_SCREEN_SMOKE=1 后运行。
// 验证：采集能打开、按 max_long_edge 缩放到 <=1280、能以 30fps 配置出帧。
TEST(ScreenCapture, HonoursMaxLongEdge) {
    if (std::getenv("XRTC_SCREEN_SMOKE") == nullptr) {
        std::cout << "[skip] set XRTC_SCREEN_SMOKE=1 to run (needs a desktop)\n";
        return;
    }
    const auto screens = xrtc::ScreenCapture::get_screen_info();
    if (screens.empty()) {
        std::cout << "[skip] no screen found\n";
        return;
    }

    FrameProbe probe;
    PushProbe push;
    xrtc::XRtcGlobal::instance().set_observer(&probe);

    int edge = 1280;
    if (const char* env_edge = std::getenv("XRTC_SCREEN_TEST_EDGE")) {
        edge = std::atoi(env_edge);
    }
    auto capture = xrtc::ScreenCapture::Create("", edge, 30);
    ASSERT_TRUE(capture);
    auto source = xrtc::XrtcVideoTrackSource::Create();
    capture->set_track_source(source);
    source->AddOrUpdateSink(&push, webrtc::VideoSinkWants{});
    ASSERT_TRUE(capture->start());

    const auto fmt = capture->capture_format();
    EXPECT_EQ(fmt.fps, 30);
    if (edge > 0) {
        EXPECT_LE(std::max(fmt.width, fmt.height), edge);
    } else {
        // edge=0：不缩放，应与屏幕源分辨率一致
        EXPECT_EQ(fmt.width, screens.front().width);
        EXPECT_EQ(fmt.height, screens.front().height);
    }
    EXPECT_EQ(fmt.width % 2, 0);
    EXPECT_EQ(fmt.height % 2, 0);

    const auto begin = std::chrono::steady_clock::now();
    const auto deadline = begin + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(20ms);
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin)
            .count();
    const double fps = push.frames.load() / seconds;
    std::cout << "[screen] smoke: edge=" << edge << " push=" << fmt.width << "x"
              << fmt.height << " frames=" << push.frames.load()
              << " fps=" << fps << "\n";

    EXPECT_GE(probe.frames.load(), 2) << "no preview frames in 2s";
    // 推流帧必须就是 capture_format 的分辨率
    EXPECT_GE(push.frames.load(), 2) << "no pushed frames in 2s";
    EXPECT_EQ(push.width.load(), fmt.width);
    EXPECT_EQ(push.height.load(), fmt.height);
    if (probe.frames.load() > 0) {
        // 预览回调单独压到 640（自看省 CPU），只要比例一致即可
        EXPECT_LE(std::max(probe.width.load(), probe.height.load()), 640);
        EXPECT_EQ(probe.width.load() % 2, 0);
        EXPECT_EQ(probe.height.load() % 2, 0);
        const double preview_ratio =
            static_cast<double>(probe.width.load()) / probe.height.load();
        const double push_ratio =
            static_cast<double>(fmt.width) / fmt.height;
        EXPECT_NEAR(preview_ratio, push_ratio, 0.02);
    }

    capture->stop();
    source->RemoveSink(&push);
    xrtc::XRtcGlobal::instance().set_observer(nullptr);
}
