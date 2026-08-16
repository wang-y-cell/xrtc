#pragma once

#include <functional>
#include <memory>
#include <string>

namespace xrtc {

/// Boost.Beast WebSocket 客户端（无 Qt）
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
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xrtc
