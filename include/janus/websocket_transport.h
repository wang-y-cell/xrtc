#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

struct lws_context;
struct lws;

namespace xrtc {

/// libwebsockets 客户端传输（独立服务线程，无 Qt）
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

    MessageCallback on_message_;
    VoidCallback on_connected_;
    VoidCallback on_disconnected_;
    ErrorCallback on_error_;

    std::mutex mutex_;
    std::queue<std::string> send_queue_;
    lws* wsi_ = nullptr;
    std::atomic<bool> connected_{false};

private:
    void service_loop();
    bool parse_url(const std::string& url);
    void request_writable();

    std::thread thread_;
    lws_context* context_ = nullptr;

    std::string address_;
    std::string path_;
    int port_ = 80;
    bool use_ssl_ = false;
    std::string protocol_ = "janus-protocol";

    std::atomic<bool> running_{false};
    std::atomic<bool> destroy_flag_{false};
};

}  // namespace xrtc
