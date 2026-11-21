#pragma once

#include <memory>
#include <string>

#include "concurrency/signal_and_slots.h"
#include <internal/xrtc_result.h>
#include <janus/websocket_config.h>

namespace xrtc {

/// WebSocket 客户端（Boost.Beast）：ws:// 明文，wss:// TLS。
/// 只负责连接与收发文本帧，Janus 协议由 JanusClient 负责。
///
/// 与上层解耦的点：
/// - 连接参数全部来自 WebsocketTransportConfig，传输层不读环境变量、不认识 Janus；
/// - 明文/TLS 的差异收敛在内部通道接口里，对外只有 open/send_text/close；
/// - TLS 支持在编译期由 XRTC_HAVE_WSS 决定，未编入时 wss:// 返回明确错误。
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

    /// 建立连接并开启网络线程。
    /// 返回失败表示参数层面的错误（URL 非法、CA 加载失败、wss 未编入）；
    /// 连接/握手/读写的异步失败通过 error 信号上报，只上报一次。
    Rest<> open(const WebsocketTransportConfig& config);
    //发送消息到服务端,真正发送消息的函数,但是由jansu_client调用,用来发送正真的janus信令信息
    void send_text(const std::string& text);
    //关闭连接
    void close();

private:
    struct Impl;
    /** @brief 在构造函数中创建, 管理实际的 WebSocket 连接*/
    std::unique_ptr<Impl> impl_;
};

}  // namespace xrtc
