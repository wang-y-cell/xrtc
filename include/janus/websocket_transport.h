#pragma once

#include <memory>
#include <string>

#include "concurrency/signal_and_slots/signal_and_slots.h"
#include <xrtc/xrtc_result.h>

namespace xrtc {

/// Boost.Beast WebSocket 客户端（无 Qt）；事件用 utils::signal
class WebsocketTransport : public utils::object {
public:
    WebsocketTransport();
    ~WebsocketTransport() override;

    WebsocketTransport(const WebsocketTransport&) = delete;
    WebsocketTransport& operator=(const WebsocketTransport&) = delete;

    utils::signal<std::string> message{this};
    utils::signal<> connected{this};
    utils::signal<> disconnected{this};
    utils::signal<std::string> error{this};

    XRtcStatus open(const std::string& url);
    void send_text(const std::string& text);
    void close();

private:
    struct Impl;
    /** @brief 在构造函数中创建, 管理实际的 WebSocket 连接*/
    std::unique_ptr<Impl> impl_;
};

}  // namespace xrtc
