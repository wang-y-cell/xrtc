#include <gtest/gtest.h>

#include <janus/websocket_url.h>

TEST(WebsocketUrl, ParsesWsWithPortAndPath) {
    xrtc::WebsocketUrlParts parts;
    ASSERT_TRUE(xrtc::ParseWebsocketUrl("ws://8.153.155.18:8188/janus", &parts));
    EXPECT_FALSE(parts.use_ssl);
    EXPECT_EQ(parts.address, "8.153.155.18");
    EXPECT_EQ(parts.port, 8188);
    EXPECT_EQ(parts.path, "/janus");
}

TEST(WebsocketUrl, ParsesWssDefaultPort) {
    xrtc::WebsocketUrlParts parts;
    ASSERT_TRUE(xrtc::ParseWebsocketUrl("wss://example.com/ws", &parts));
    EXPECT_TRUE(parts.use_ssl);
    EXPECT_EQ(parts.address, "example.com");
    EXPECT_EQ(parts.port, 443);
    EXPECT_EQ(parts.path, "/ws");
}

TEST(WebsocketUrl, RejectsInvalidPort) {
    xrtc::WebsocketUrlParts parts;
    EXPECT_FALSE(xrtc::ParseWebsocketUrl("ws://host:abc/", &parts));
    EXPECT_FALSE(xrtc::ParseWebsocketUrl("ws://host:99999/", &parts));
    EXPECT_FALSE(xrtc::ParseWebsocketUrl("http://host/", &parts));
}
