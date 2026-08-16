#pragma once

#include <memory>
#include <string>

#include "component/signal_and_slots/signal_and_slots.h"
#include <xrtc/xrtc_result.h>

namespace xrtc {

/// Boost.Beast WebSocket 客户端（无 Qt）；事件用 utils::signal
class WebsocketTransport {
public:
    WebsocketTransport();
    ~WebsocketTransport();

    WebsocketTransport(const WebsocketTransport&) = delete;
    WebsocketTransport& operator=(const WebsocketTransport&) = delete;

    utils::signal<std::string> message;
    utils::signal<> connected;
    utils::signal<> disconnected;
    utils::signal<std::string> error;

    XRtcStatus open(const std::string& url);
    void send_text(const std::string& text);
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xrtc
