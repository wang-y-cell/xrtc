#include <gtest/gtest.h>

#include <session/session_lifecycle.h>

using Event = xrtc::SessionLifecycle::Event;

TEST(SessionLifecycle, StartThenUserStopCleansAndNotifiesLeave) {
    xrtc::SessionLifecycle life;
    auto start = life.On(Event::kStartOk);
    EXPECT_FALSE(start.ignore);
    EXPECT_TRUE(life.active);
    life.MarkJoinNotified();

    auto stop = life.On(Event::kUserStop);
    EXPECT_TRUE(stop.set_inactive);
    EXPECT_TRUE(stop.disconnect);
    EXPECT_TRUE(stop.cleanup_media);
    EXPECT_TRUE(stop.notify_leave);
    EXPECT_FALSE(stop.notify_join_fail);
    EXPECT_EQ(stop.leave_or_join_error, xrtc::XRtcError::kNOERROR);
    EXPECT_FALSE(life.active);
    life.FinishTeardown();
}

TEST(SessionLifecycle, FailJoinBeforeNotifiedCleansAndNotifiesJoinFail) {
    xrtc::SessionLifecycle life;
    life.On(Event::kStartOk);
    auto fail = life.On(Event::kFailJoin, "no camera");
    EXPECT_TRUE(fail.cleanup_media);
    EXPECT_TRUE(fail.disconnect);
    EXPECT_TRUE(fail.notify_join_fail);
    EXPECT_FALSE(fail.notify_leave);
    EXPECT_FALSE(life.active);
    life.FinishTeardown();
}

TEST(SessionLifecycle, SignalingErrorAfterJoinNotifiesLeaveAndCleans) {
    xrtc::SessionLifecycle life;
    life.On(Event::kStartOk);
    life.MarkJoinNotified();
    auto err = life.On(Event::kSignalingError, "hangup");
    EXPECT_TRUE(err.cleanup_media);
    EXPECT_TRUE(err.disconnect);
    EXPECT_TRUE(err.notify_leave);
    EXPECT_FALSE(err.notify_join_fail);
    EXPECT_EQ(err.leave_or_join_error, xrtc::XRtcError::kSignalingFailed);
    life.FinishTeardown();
}

TEST(SessionLifecycle, QuietDestroyWhileActiveTreatsAsUnexpectedLeave) {
    xrtc::SessionLifecycle life;
    life.On(Event::kStartOk);
    life.MarkJoinNotified();
    auto destroyed = life.On(Event::kQuietDestroy, "ws closed");
    EXPECT_FALSE(destroyed.ignore);
    EXPECT_TRUE(destroyed.cleanup_media);
    EXPECT_FALSE(destroyed.disconnect);  // 连接已断
    EXPECT_TRUE(destroyed.notify_leave);
    EXPECT_EQ(destroyed.leave_or_join_error, xrtc::XRtcError::kSignalingFailed);
    life.FinishTeardown();
}

TEST(SessionLifecycle, QuietDestroyDuringTeardownIsIgnored) {
    xrtc::SessionLifecycle life;
    life.On(Event::kStartOk);
    life.MarkJoinNotified();
    auto stop = life.On(Event::kUserStop);
    EXPECT_TRUE(stop.notify_leave);
    // Stop 触发 Disconnect → destroyed 重入
    auto destroyed = life.On(Event::kQuietDestroy);
    EXPECT_TRUE(destroyed.ignore);
    EXPECT_FALSE(destroyed.notify_leave);
    life.FinishTeardown();
}

TEST(SessionLifecycle, QuietDestroyWhenInactiveOnlyCleansMedia) {
    xrtc::SessionLifecycle life;
    life.On(Event::kStartOk);
    life.On(Event::kUserStop);
    life.FinishTeardown();
    auto destroyed = life.On(Event::kQuietDestroy);
    EXPECT_TRUE(destroyed.ignore);
    EXPECT_TRUE(destroyed.cleanup_media);
    EXPECT_FALSE(destroyed.notify_leave);
    EXPECT_FALSE(destroyed.notify_join_fail);
}

TEST(SessionLifecycle, IceFailedAfterJoinLeavesAndCleans) {
    xrtc::SessionLifecycle life;
    life.On(Event::kStartOk);
    life.MarkJoinNotified();
    auto ice = life.On(Event::kIceFailed, "ice failed");
    EXPECT_TRUE(ice.cleanup_media);
    EXPECT_TRUE(ice.disconnect);
    EXPECT_TRUE(ice.notify_leave);
    EXPECT_EQ(ice.leave_or_join_error, xrtc::XRtcError::kPeerConnectionFailed);
    life.FinishTeardown();
}

TEST(SessionLifecycle, IceFailedBeforeJoinBecomesFailJoin) {
    xrtc::SessionLifecycle life;
    life.On(Event::kStartOk);
    auto ice = life.On(Event::kIceFailed, "pc init race");
    EXPECT_TRUE(ice.cleanup_media);
    EXPECT_TRUE(ice.notify_join_fail);
    EXPECT_FALSE(ice.notify_leave);
    life.FinishTeardown();
}

TEST(SessionLifecycle, DoubleStartRejected) {
    xrtc::SessionLifecycle life;
    life.On(Event::kStartOk);
    auto again = life.On(Event::kStartOk);
    EXPECT_TRUE(again.ignore);
    EXPECT_TRUE(again.notify_join_fail);
    EXPECT_EQ(again.leave_or_join_error, xrtc::XRtcError::kAlreadyInCall);
    EXPECT_TRUE(life.active);
}

TEST(SessionLifecycle, GenerationBumpsOnEachTerminalEvent) {
    xrtc::SessionLifecycle life;
    life.On(Event::kStartOk);
    const auto g1 = life.generation;
    life.MarkJoinNotified();
    life.On(Event::kUserStop);
    EXPECT_GT(life.generation, g1);
    life.FinishTeardown();

    life.On(Event::kStartOk);
    const auto g2 = life.generation;
    life.On(Event::kFailJoin, "x");
    EXPECT_GT(life.generation, g2);
    life.FinishTeardown();
}
