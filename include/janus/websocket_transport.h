#pragma once

#include <memory>
#include <string>

#include "concurrency/signal_and_slots.h"
#include <xrtc/xrtc_defines.h>
#include <internal/xrtc_result.h>

namespace xrtc {

/// WebSocket(TLS) 连接参数。仅对 wss:// 生效。
struct WebsocketTlsOptions {
    /// 是否校验服务端证书链与主机名；生产环境必须为 true。
    /// 置 false 只用于自签名证书的自建 Janus 联调。
    bool verify = true;
    /// CA 证书链文件（PEM）。为空时依次尝试环境变量
    /// XRTC_CA_FILE / SSL_CERT_FILE / CURL_CA_BUNDLE，最后用 OpenSSL 默认路径。
    std::string ca_file;
};

/// WebSocket 客户端（Boost.Beast）：ws:// 明文，wss:// TLS。
/// 只负责连接与收发文本帧，Janus 协议由 JanusClient 负责。
/// TLS 支持在编译期由 XRTC_HAVE_WSS 决定；未编入时 wss:// 会返回明确错误。
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
    Rest<> open(const std::string& url,
                const WebsocketTlsOptions& tls = WebsocketTlsOptions());
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
