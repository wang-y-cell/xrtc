#include <gtest/gtest.h>

#include <media/device_enum_utils.h>
#include <media/video_capability_selector.h>

TEST(DeviceEnumUtils, ClampsAboveMaxInsteadOfZero) {
    EXPECT_EQ(xrtc::ClampDeviceCount(0), 0);
    EXPECT_EQ(xrtc::ClampDeviceCount(-1), 0);
    EXPECT_EQ(xrtc::ClampDeviceCount(16), 16);
    EXPECT_EQ(xrtc::ClampDeviceCount(32), 32);
    EXPECT_EQ(xrtc::ClampDeviceCount(48), 32);
}

TEST(VideoSelectStrategy, PreferRequestedPicksClosest) {
    const std::vector<xrtc::VideoFormat> caps = {
        {640, 480, 30},
        {1280, 720, 30},
        {1920, 1080, 15},
    };
    xrtc::PreferRequestedStrategy strategy;
    const auto selected = strategy.Select(caps, {1280, 720, 30});
    EXPECT_EQ(selected.width, 1280);
    EXPECT_EQ(selected.height, 720);
    EXPECT_EQ(selected.fps, 30);
}

TEST(VideoSelectStrategy, PreferStandardPrefers720p) {
    const std::vector<xrtc::VideoFormat> caps = {
        {640, 480, 30},
        {1280, 720, 30},
        {320, 240, 30},
    };
    xrtc::PreferStandardStrategy strategy;
    const auto selected = strategy.Select(caps, {320, 240, 15});
    EXPECT_EQ(selected.width, 1280);
    EXPECT_EQ(selected.height, 720);
}

TEST(VideoSelectStrategy, SelectHelperEmptyCapsReturnsRequested) {
    xrtc::PreferRequestedStrategy strategy;
    const xrtc::VideoFormat requested{800, 600, 25};
    const auto selected =
        xrtc::VideoCapabilitySelector::Select({}, requested, strategy);
    EXPECT_EQ(selected.width, requested.width);
    EXPECT_EQ(selected.height, requested.height);
    EXPECT_EQ(selected.fps, requested.fps);
}
