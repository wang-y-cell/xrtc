#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <vector>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <concurrency/signal_and_slots.h>
#include <janus/janus_client.h>
#include <janus/websocket_transport.h>
#include <xrtc/xrtc_defines.h>

namespace {

using namespace std::chrono_literals;

std::string Env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

/// 等待条件成立（带超时），避免测试挂死
template <typename Pred>
bool WaitFor(Pred pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return pred();
}

/// 接收 websocket 传输层信号
struct TransportProbe : utils::object {
    std::atomic<bool> connected{false};
    std::atomic<bool> got_message{false};
    std::atomic<bool> got_error{false};
    std::mutex mutex;
    std::string message;
    std::string error;

    utils::slots_t<> on_connected() {
        connected.store(true);
        return {};
    }
    utils::slots_t<> on_message(const std::string& text) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            message = text;
        }
        got_message.store(true);
        return {};
    }
    utils::slots_t<> on_error(const std::string& text) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            error = text;
        }
        got_error.store(true);
        return {};
    }
};

/// 接收 JanusClient 的超时信号
struct TimeoutProbe : utils::object {
    std::atomic<bool> timed_out{false};
    std::mutex mutex;
    uint64_t feed_id = 0;
    std::string what;

    utils::slots_t<> on_timeout(uint64_t id, const std::string& detail) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            feed_id = id;
            what = detail;
        }
        timed_out.store(true);
        return {};
    }
};

xrtc::WebsocketTlsOptions TlsFromEnv() {
    xrtc::WebsocketTlsOptions tls;
    tls.ca_file = Env("XRTC_CA_FILE");
    const std::string verify = Env("XRTC_TLS_VERIFY");
    if (!verify.empty()) {
        tls.verify = verify != "0";
    }
    return tls;
}

}  // namespace

// 需要本地 wss 服务端：设置 XRTC_WSS_SMOKE_URL 后运行。
// 例：wss://localhost:8989/janus + XRTC_CA_FILE=build/test-certs/cert.pem
TEST(WebsocketTls, EchoRoundTrip) {
    const std::string url = Env("XRTC_WSS_SMOKE_URL");
    if (url.empty()) {
        std::cout << "[skip] set XRTC_WSS_SMOKE_URL=wss://host:port/janus\n";
        return;
    }

    utils::worker_thread loop;
    loop.start();
    TransportProbe probe;
    probe.move_to_thread(&loop);
    xrtc::WebsocketTransport ws;

    std::vector<utils::scoped_connection> conns;
    conns.emplace_back(
        utils::connect(ws.connected, &probe, &TransportProbe::on_connected));
    conns.emplace_back(
        utils::connect(ws.message, &probe, &TransportProbe::on_message));
    conns.emplace_back(
        utils::connect(ws.error, &probe, &TransportProbe::on_error));

    const auto st = [&] {
        // 覆盖「域名（SAN dNSName）+ 自签 CA」这条路径：曾经的实现用
        // X509_VERIFY_PARAM_set1_host，在本 SDK 的 BoringSSL 上返回 0，
        // 导致所有 wss://域名 都连不上；现由 X509_check_host 完成。
        xrtc::WebsocketTransportConfig config;
        config.url = url;
        config.tls = TlsFromEnv();
        return ws.open(config);
    }();
    ASSERT_TRUE(st) << xrtc::XRtcErrorToString(st.error());

    ASSERT_TRUE(WaitFor([&] { return probe.connected.load(); }, 10s))
        << "connect failed: "
        << [&] {
               std::lock_guard<std::mutex> lock(probe.mutex);
               return probe.error;
           }();

    ws.send_text("{\"janus\":\"create\",\"transaction\":\"smoke-1\"}");
    ASSERT_TRUE(WaitFor([&] { return probe.got_message.load(); }, 10s))
        << "no echo within 10s";
    {
        std::lock_guard<std::mutex> lock(probe.mutex);
        EXPECT_NE(probe.message.find("janus"), std::string::npos)
            << "unexpected echo: " << probe.message;
    }
    ws.close();
}

// 不需要服务端：CA 文件加载失败必须在 open() 阶段同步报错，
// 而不是等到握手时才给出笼统的 handshake 失败。
TEST(WebsocketTls, MissingCaFileFailsSynchronously) {
    xrtc::WebsocketTransport ws;
    xrtc::WebsocketTransportConfig config;
    config.url = "wss://localhost:8989/janus";
    config.tls.ca_file = "Z:/xrtc-definitely-missing/ca.pem";

    const auto st = ws.open(config);
    EXPECT_FALSE(st);
    EXPECT_EQ(st.error(), xrtc::XRtcError::kSignalingFailed);
}

// 不需要服务端：allow_plaintext_ws=false 时 ws:// 必须在连接前被拒绝。
// 用不可能连通的端口（9）证明拒绝发生在网络动作之前。
TEST(JanusSignaling, RejectsPlaintextWhenTlsRequired) {
    utils::worker_thread loop;
    loop.start();
    xrtc::JanusClient janus;
    janus.move_to_thread(&loop);

    xrtc::XRTCJoinConfig config;
    config.janus_ws_url = "ws://127.0.0.1:9/janus";
    config.allow_plaintext_ws = false;
    config.signal_timeout_ms = 500;

    const auto st = janus.Connect(config);
    EXPECT_FALSE(st);
    EXPECT_EQ(st.error(), xrtc::XRtcError::kInvalidParam);
    janus.Disconnect();
}

// 需要“接受连接但从不回包”的 ws 服务端：设置 XRTC_WS_SILENT_URL 后运行。
// 用于验证 #34 的请求超时：create 发出后应在 signal_timeout_ms 内触发
// request_timeout(feed=0, "create-session")。
TEST(JanusSignaling, CreateRequestTimesOut) {
    const std::string url = Env("XRTC_WS_SILENT_URL");
    if (url.empty()) {
        std::cout << "[skip] set XRTC_WS_SILENT_URL=ws://host:port/janus\n";
        return;
    }

    utils::worker_thread loop;
    loop.start();
    TimeoutProbe probe;
    probe.move_to_thread(&loop);

    xrtc::JanusClient janus;
    janus.move_to_thread(&loop);
    auto conn = utils::connect(janus.request_timeout, &probe,
                               &TimeoutProbe::on_timeout);

    xrtc::XRTCJoinConfig config;
    config.janus_ws_url = url;
    config.room_id = 1234;
    config.display_name = "timeout-test";
    config.signal_timeout_ms = 500;
    config.ca_file = Env("XRTC_CA_FILE");
    const std::string verify = Env("XRTC_TLS_VERIFY");
    config.tls_verify = verify.empty() || verify != "0";
    ASSERT_TRUE(janus.Connect(config));

    ASSERT_TRUE(WaitFor([&] { return probe.timed_out.load(); }, 5s))
        << "no request_timeout within 5s";
    {
        std::lock_guard<std::mutex> lock(probe.mutex);
        EXPECT_EQ(probe.feed_id, 0u);
        EXPECT_EQ(probe.what, "create-session");
    }
    janus.Disconnect();
}
