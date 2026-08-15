#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace xrtc {

/// 纯 Winsock WebSocket 客户端（不依赖 libwebsockets，无 Qt）
class WebsocketTransport {
public:
    using MessageCallback = std::function<void(const std::string&)>;
    using VoidCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const std::string&)>;

    WebsocketTransport();
    ~WebsocketTransport();

    WebsocketTransport(const WebsocketTransport&) = delete;
    WebsocketTransport& operator=(const WebsocketTransport&) = delete;

    void set_callbacks(MessageCallback on_message, VoidCallback on_connected,
                       VoidCallback on_disconnected, ErrorCallback on_error);

    bool open(const std::string& url);
    void send_text(const std::string& text);
    void close();

private:
    void service_loop();
    bool parse_url(const std::string& url);
    bool tcp_connect();
    bool ws_handshake();
    bool send_frame(const std::string& text);
    bool recv_some();
    void drain_frames();
    void notify_error(const std::string& err);
    void sock_close();

    MessageCallback on_message_;
    VoidCallback on_connected_;
    VoidCallback on_disconnected_;
    ErrorCallback on_error_;

    std::mutex mutex_;
    std::queue<std::string> send_queue_;

    std::thread thread_;
    std::uintptr_t sock_ = ~static_cast<std::uintptr_t>(0);  // INVALID_SOCKET

    std::string address_;
    std::string path_;
    int port_ = 80;
    bool use_ssl_ = false;
    std::string protocol_ = "janus-protocol";

    std::vector<char> rx_buf_;

    std::atomic<bool> connected_{false};
    std::atomic<bool> destroy_flag_{false};
    std::atomic<bool> error_notified_{false};
    std::atomic<bool> running_{false};
};

}  // namespace xrtc
