#include <gtest/gtest.h>

#include <janus/janus_event_utils.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

TEST(JanusEventUtils, IgnoresUnpublishedOk) {
    const json data = {{"unpublished", "ok"}};
    EXPECT_FALSE(xrtc::ParsePublisherLeftFeed(data).has_value());
}

TEST(JanusEventUtils, ParsesNumericUnpublished) {
    const json data = {{"unpublished", 42}};
    auto feed = xrtc::ParsePublisherLeftFeed(data);
    ASSERT_TRUE(feed.has_value());
    EXPECT_EQ(*feed, 42u);
}

TEST(JanusEventUtils, ParsesStringFeedId) {
    const json data = {{"leaving", "1001"}};
    auto feed = xrtc::ParsePublisherLeftFeed(data);
    ASSERT_TRUE(feed.has_value());
    EXPECT_EQ(*feed, 1001u);
}

TEST(JanusEventUtils, IgnoresZeroAndGarbage) {
    EXPECT_FALSE(xrtc::ParsePublisherLeftFeed({{"unpublished", 0}}).has_value());
    EXPECT_FALSE(
        xrtc::ParsePublisherLeftFeed({{"leaving", "not-a-number"}}).has_value());
    EXPECT_FALSE(xrtc::ParsePublisherLeftFeed(json::object()).has_value());
}

TEST(JanusEventUtils, PrefersUnpublishedOverLeavingWhenBothPresent) {
    const json data = {{"unpublished", 7}, {"leaving", 8}};
    auto feed = xrtc::ParsePublisherLeftFeed(data);
    ASSERT_TRUE(feed.has_value());
    EXPECT_EQ(*feed, 7u);
}

TEST(JanusEventUtils, EmptyOkVariantsIgnored) {
    EXPECT_FALSE(xrtc::ParsePublisherLeftFeed({{"unpublished", ""}}).has_value());
    EXPECT_FALSE(xrtc::ParsePublisherLeftFeed({{"leaving", "ok"}}).has_value());
}
