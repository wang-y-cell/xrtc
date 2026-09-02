#include <gtest/gtest.h>

#include <janus/subscribe_state.h>

TEST(SubscribeState, BeginSubscribeDedupsPendingAndSubscribed) {
    xrtc::SubscribeState state;
    EXPECT_TRUE(state.TryBeginSubscribe(101));
    EXPECT_TRUE(state.IsPending(101));
    EXPECT_FALSE(state.TryBeginSubscribe(101));  // pending 中不可重复

    state.OnAttachSuccess(101, 9001);
    EXPECT_FALSE(state.IsPending(101));
    EXPECT_TRUE(state.IsSubscribed(101));
    EXPECT_EQ(state.HandleForFeed(101).value_or(0), 9001u);
    EXPECT_FALSE(state.TryBeginSubscribe(101));  // 已订阅不可重复
}

TEST(SubscribeState, AttachFailureClearsPendingAndAllowsRetry) {
    xrtc::SubscribeState state;
    ASSERT_TRUE(state.TryBeginSubscribe(202));
    state.OnAttachFailure(202);
    EXPECT_FALSE(state.IsPending(202));
    EXPECT_FALSE(state.IsSubscribed(202));
    EXPECT_TRUE(state.TryBeginSubscribe(202));  // 失败后可重试
}

TEST(SubscribeState, FeedLeftReturnsHandleAndClears) {
    xrtc::SubscribeState state;
    ASSERT_TRUE(state.TryBeginSubscribe(303));
    state.OnAttachSuccess(303, 7007);
    const auto handle = state.OnFeedLeft(303);
    ASSERT_TRUE(handle.has_value());
    EXPECT_EQ(*handle, 7007u);
    EXPECT_FALSE(state.IsSubscribed(303));
    EXPECT_FALSE(state.HandleForFeed(303).has_value());
    // 再次离开应为空
    EXPECT_FALSE(state.OnFeedLeft(303).has_value());
}

TEST(SubscribeState, FeedLeftWhilePendingOnlyClearsPending) {
    xrtc::SubscribeState state;
    ASSERT_TRUE(state.TryBeginSubscribe(404));
    EXPECT_FALSE(state.OnFeedLeft(404).has_value());
    EXPECT_FALSE(state.IsPending(404));
    EXPECT_TRUE(state.TryBeginSubscribe(404));
}

TEST(SubscribeState, FeedForHandleLookup) {
    xrtc::SubscribeState state;
    ASSERT_TRUE(state.TryBeginSubscribe(1));
    state.OnAttachSuccess(1, 11);
    ASSERT_TRUE(state.TryBeginSubscribe(2));
    state.OnAttachSuccess(2, 22);
    EXPECT_EQ(state.FeedForHandle(11).value_or(0), 1u);
    EXPECT_EQ(state.FeedForHandle(22).value_or(0), 2u);
    EXPECT_FALSE(state.FeedForHandle(999).has_value());
}

TEST(SubscribeState, ClearWipesAll) {
    xrtc::SubscribeState state;
    ASSERT_TRUE(state.TryBeginSubscribe(1));
    state.OnAttachSuccess(1, 11);
    ASSERT_TRUE(state.TryBeginSubscribe(2));
    state.Clear();
    EXPECT_EQ(state.pending_count(), 0u);
    EXPECT_EQ(state.subscribed_count(), 0u);
    EXPECT_TRUE(state.TryBeginSubscribe(1));
}

TEST(SubscribeState, MultipleFeedsIndependent) {
    xrtc::SubscribeState state;
    EXPECT_TRUE(state.TryBeginSubscribe(1));
    EXPECT_TRUE(state.TryBeginSubscribe(2));
    EXPECT_TRUE(state.TryBeginSubscribe(3));
    state.OnAttachFailure(2);
    state.OnAttachSuccess(1, 10);
    EXPECT_TRUE(state.IsSubscribed(1));
    EXPECT_FALSE(state.IsPending(2));
    EXPECT_TRUE(state.IsPending(3));
    EXPECT_EQ(state.pending_count(), 1u);
    EXPECT_EQ(state.subscribed_count(), 1u);
}
