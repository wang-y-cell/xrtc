#pragma once

#include <memory>
#include <string>

#include "concurrency/signal_and_slots/signal_and_slots.h"
#include <xrtc/xrtc_defines.h>
#include <internal/xrtc_result.h>

namespace xrtc {

/// Boost.Beast WebSocket 客户端（无 Qt）；事件用 utils::signal
class WebsocketTransport : public utils::object {
public:
    WebsocketTransport();
    ~WebsocketTransport() override;

    WebsocketTransport(const WebsocketTransport&) = delete;
    WebsocketTransport& operator=(const WebsocketTransport&) = delete;

    //这些信号连接的都是janus_client中的槽函数
    utils::signal<std::string> message{this}; //接收到网络数据之后调用槽函数,将数据传递给janus_client
    utils::signal<> connected{this}; //websocket握手成功之后调用槽函数,将握手成功传递给janus_client
    utils::signal<> disconnected{this}; //连接断开之后调用槽函数,将数据传递给janus_client
    utils::signal<std::string> error{this}; //接收到错误之后调用槽函数,将错误传递给janus_client

    //解析这个url之后连接这个服务器,并开启网络线程,线程运行事件循环,
    // 等待网络事件的发生,websocket握手完成,可以接收数据和发送数据(send_text)
    XRtcStatus open(const std::string& url);
    //发送消息到服务端
    void send_text(const std::string& text);
    //关闭连接
    void close();

private:
    struct Impl;
    /** @brief 在构造函数中创建, 管理实际的 WebSocket 连接*/
    std::unique_ptr<Impl> impl_;
};

}  // namespace xrtc
