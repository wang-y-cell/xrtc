#include <janus/websocket_transport.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#ifdef XRTC_HAVE_WSS
#include <boost/asio/ssl.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#endif

#include <spdlog/spdlog.h>
#include <janus/websocket_url.h>

namespace xrtc {
namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

using PlainWs = websocket::stream<beast::tcp_stream>;

#ifdef XRTC_HAVE_WSS
namespace ssl = net::ssl;
using TlsWs = websocket::stream<ssl::stream<beast::tcp_stream>>;
#endif

/// ws / wss 共用的握手参数：握手 30s 超时、不做空闲超时、可选子协议。
/// 空闲断线由 JanusClient 的 keepalive 负责，传输层不主动踢连接。
template <typename Stream>
void ApplyHandshakeOptions(Stream& ws, const std::string& subprotocol) {
    websocket::stream_base::timeout opt{};
    opt.handshake_timeout = std::chrono::seconds(30);
    opt.idle_timeout = websocket::stream_base::none();
    ws.set_option(opt);
    if (!subprotocol.empty()) {
        // 注入 Sec-WebSocket-Protocol：Janus 用它确认客户端走的是 janus-protocol
        ws.set_option(websocket::stream_base::decorator(
            [subprotocol](websocket::request_type& req) {
                req.set(http::field::sec_websocket_protocol, subprotocol);
            }));
    }
}

/// 一条 WebSocket 连接通道。
///
/// 这里是「接口解耦」的关键：明文与 TLS 的差异全部收敛在 IWebsocketChannel 的
/// 实现里，状态机（WebsocketTransport::Impl）只面向该接口，不需要 #ifdef，
/// 也不需要同时维护 ws_plain / ws_tls 两个成员和 with_ws() 模板分发。
class IWebsocketChannel {
public:
    /// stage 用于生成可读错误，取值 "tls-handshake" / "handshake"
    using OpenHandler = std::function<void(beast::error_code, const char* stage)>;
    using IoHandler = std::function<void(beast::error_code, std::size_t)>;

    virtual ~IWebsocketChannel() = default;

    /// 底层 TCP 流：TCP 连接与连接超时都作用在它上面
    virtual beast::tcp_stream& tcp_layer() = 0;

    /// 握手前的一次性准备（子协议、SNI）；失败时 error 带可读原因
    virtual bool prepare(std::string* error) = 0;

    /// 安全层握手（仅 wss）+ WebSocket 升级，完成后回调
    virtual void async_open(const std::string& host_header,
                            const std::string& path, OpenHandler on_done) = 0;

    virtual void async_read(beast::flat_buffer& buf, IoHandler on_done) = 0;
    virtual void async_write(const std::string& payload, IoHandler on_done) = 0;
};

/// 明文 ws:// 通道
class PlainChannel final : public IWebsocketChannel {
public:
    PlainChannel(net::io_context& ioc, std::string subprotocol)
        : ws_(ioc), subprotocol_(std::move(subprotocol)) {}

    beast::tcp_stream& tcp_layer() override {
        return beast::get_lowest_layer(ws_);
    }

    bool prepare(std::string*) override {
        ApplyHandshakeOptions(ws_, subprotocol_);
        return true;
    }

    void async_open(const std::string& host_header, const std::string& path,
                    OpenHandler on_done) override {
        ws_.async_handshake(host_header, path,
                            [on_done = std::move(on_done)](beast::error_code ec) {
                                if (on_done) {
                                    on_done(ec, "handshake");
                                }
                            });
    }

    void async_read(beast::flat_buffer& buf, IoHandler on_done) override {
        ws_.async_read(buf, [on_done = std::move(on_done)](beast::error_code ec,
                                                          std::size_t n) {
            if (on_done) {
                on_done(ec, n);
            }
        });
    }

    void async_write(const std::string& payload, IoHandler on_done) override {
        ws_.text(true);
        ws_.async_write(net::buffer(payload),
                        [on_done = std::move(on_done)](beast::error_code ec,
                                                       std::size_t n) {
                            if (on_done) {
                                on_done(ec, n);
                            }
                        });
    }

private:
    PlainWs ws_;
    std::string subprotocol_;
};

#ifdef XRTC_HAVE_WSS
/// wss:// 通道：TLS 握手 → WebSocket 升级
class TlsChannel final : public IWebsocketChannel {
public:
    TlsChannel(net::io_context& ioc, ssl::context& ctx, std::string host,
               bool host_is_ip, std::string subprotocol)
        : ws_(ioc, ctx),
          host_(std::move(host)),
          host_is_ip_(host_is_ip),
          subprotocol_(std::move(subprotocol)) {}

    beast::tcp_stream& tcp_layer() override {
        return beast::get_lowest_layer(ws_);
    }

    bool prepare(std::string* error) override {
        ApplyHandshakeOptions(ws_, subprotocol_);
        // RFC 6066：SNI 只放域名，IP 字面量不发 SNI（虚拟主机仍按 Host 头选证书）
        if (!host_is_ip_ &&
            !SSL_set_tlsext_host_name(ws_.next_layer().native_handle(),
                                      host_.c_str())) {
            if (error) {
                *error = "tls: failed to set SNI host name";
            }
            return false;
        }
        return true;
    }

    void async_open(const std::string& host_header, const std::string& path,
                    OpenHandler on_done) override {
        ws_.next_layer().async_handshake(
            ssl::stream_base::client,
            [this, host_header, path, on_done = std::move(on_done)](
                beast::error_code ec) mutable {
                if (ec) {
                    if (on_done) {
                        on_done(ec, "tls-handshake");
                    }
                    return;
                }
                spdlog::info(
                    "[ws] TLS handshake ok host={} sni={} version={}", host_,
                    !host_is_ip_,
                    SSL_get_version(ws_.next_layer().native_handle()));
                ws_.async_handshake(
                    host_header, path,
                    [on_done = std::move(on_done)](beast::error_code ec2) {
                        if (on_done) {
                            on_done(ec2, "handshake");
                        }
                    });
            });
    }

    void async_read(beast::flat_buffer& buf, IoHandler on_done) override {
        ws_.async_read(buf, [on_done = std::move(on_done)](beast::error_code ec,
                                                          std::size_t n) {
            if (on_done) {
                on_done(ec, n);
            }
        });
    }

    void async_write(const std::string& payload, IoHandler on_done) override {
        ws_.text(true);
        ws_.async_write(net::buffer(payload),
                        [on_done = std::move(on_done)](beast::error_code ec,
                                                       std::size_t n) {
                            if (on_done) {
                                on_done(ec, n);
                            }
                        });
    }

private:
    TlsWs ws_;
    std::string host_;
    bool host_is_ip_ = false;
    std::string subprotocol_;
};
#endif  // XRTC_HAVE_WSS

}  // namespace

struct WebsocketTransport::Impl {
    //指回外面的 WebsocketTransport，用来 emit 信号
    WebsocketTransport* owner = nullptr;

    //解析后的 ws://host:port/path，以及子协议
    std::string address; //websocket地址(域名或者IP地址)
    std::string path = "/"; //websocket路径
    std::string host_header; //websocket主机头(http请求头中的Host字段,http1.1之后必带)
    int port = 80; //websocket端口
    bool use_ssl = false; //websocket是否使用SSL加密
    bool host_is_ip = false; //address 是否为 IP 字面量（决定 SAN 校验方式）
    std::string subprotocol = "janus-protocol"; //Sec-WebSocket-Protocol
    WebsocketTlsOptions tls_options;

    //boost::asio::io_context，用来管理网络事件
    std::unique_ptr<net::io_context> ioc;
    //当前的连接通道（明文或 TLS），由 create_channel() 决定
    std::unique_ptr<IWebsocketChannel> channel;
#ifdef XRTC_HAVE_WSS
    std::unique_ptr<ssl::context> ssl_ctx;
#endif
    //boost::beast::flat_buffer，用来缓存接收到的数据
    beast::flat_buffer read_buf;
    //std::deque<std::string>，用来缓存发送的数据
    std::deque<std::string> write_queue;
    //bool，用来标记是否正在写数据
    bool writing = false;

    //std::thread，用来运行网络事件循环
    std::thread thread;
    //std::atomic<bool>，用来标记是否已连接,这里的连接指的是websocket握手成功后的连接
    std::atomic<bool> connected{false};
    //std::atomic<bool>，用来标记是否正在关闭
    std::atomic<bool> closing{false};
    //std::atomic<bool>，用来标记是否已通知错误
    std::atomic<bool> error_notified{false};

    void notify_error(const std::string& err) {
        //下面这段的意思是错误只通知一次
        bool expected = false;
        if (!error_notified.compare_exchange_strong(expected, true)) {
            return;
        }
        spdlog::error("[ws] ERROR: {}", err);
        if (owner) {
            owner->error.emit(err);
        }
    }

    /**
      @brief 解析 URL，提取 host、port、path,host_header等信息
      @param url 要解析的 URL 字符串
      @return 如果解析成功，返回 true；否则返回 false
    */
    bool parse_url(const std::string& url) {
        WebsocketUrlParts parts;
        if (!ParseWebsocketUrl(url, &parts)) {
            return false;
        }
        use_ssl = parts.use_ssl;
        port = parts.port;
        path = parts.path;
        address = parts.address;
        host_header.clear();

        //如果端口是80且没有使用ssl，或者端口是443且使用ssl，则host_header就是地址
        if ((port == 80 && !use_ssl) || (port == 443 && use_ssl)) {
            host_header = address; //80和443是默认端口,host中可以省略端口
        } else { //如果不是默认端口，则host_header就是地址:端口
            host_header = address + ":" + std::to_string(port);
        }

        // IP 字面量走 SAN iPAddress 校验，域名走 SAN dNSName
        boost::system::error_code ip_ec;
        net::ip::make_address(address, ip_ec);
        host_is_ip = !ip_ec;
        return true;
    }

    /**
      @brief 处理连接失败
      @param ec 错误码
      @param what 错误信息
    */
    void fail(beast::error_code ec, const char* what) {
        if (closing.load()) { //如果正在关闭，则返回
            return;
        }
        //如果错误码为操作被中止，则返回
        if (ec == net::error::operation_aborted) {
            return;
        }
        //通知错误
        notify_error(std::string(what) + ": " + ec.message());
        stop_ioc();
    }

    /** @brief 停止io_context */
    void stop_ioc() {
        if (ioc) {
            ioc->stop();
        }
    }

    /** @brief 通知断开连接 */
    void notify_disconnected_if_needed() {
        //读取connected原来的值,并设置为false
        const bool was_connected = connected.exchange(false);
        if (!was_connected || !owner) { //如果连接状态为false，或者owner为空，则返回
            return;
        }
        owner->disconnected.emit(); //通知断开连接
    }

    /**
      @brief 建立 TLS 上下文（仅 wss）。校验策略与 CA 在这里确定，
             连接阶段的验证由 verify_peer() 在握手过程中执行。
    */
    bool setup_tls_context() {
#ifdef XRTC_HAVE_WSS
        ssl_ctx = std::make_unique<ssl::context>(ssl::context::tls_client);
        ssl_ctx->set_options(ssl::context::default_workarounds |
                             ssl::context::no_sslv2 | ssl::context::no_sslv3 |
                             ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1);
        if (!tls_options.verify) {
            spdlog::warn(
                "[ws] TLS 证书校验已关闭（仅限联调自签名服务器，生产必须开启）");
            ssl_ctx->set_verify_mode(ssl::verify_none);
            return true;
        }
        ssl_ctx->set_verify_mode(ssl::verify_peer);
        ssl_ctx->set_verify_callback(
            [this](bool preverified, ssl::verify_context& ctx) {
                return verify_peer(preverified, ctx);
            });

        boost::system::error_code ec;
        if (!tls_options.ca_file.empty()) {
            ssl_ctx->load_verify_file(tls_options.ca_file, ec);
            if (ec) {
                notify_error("tls: load CA file failed: " + tls_options.ca_file +
                             ": " + ec.message());
                return false;
            }
            spdlog::info("[ws] TLS CA file: {}", tls_options.ca_file);
            return true;
        }
        // 没有显式 CA：用 OpenSSL 默认路径（受 SSL_CERT_DIR / OPENSSL_CONF 控制）
        ssl_ctx->set_default_verify_paths(ec);
        if (ec) {
            spdlog::warn(
                "[ws] TLS 未找到系统 CA 路径({})；如服务端为自签名证书，请设置 "
                "XRTC_CA_FILE",
                ec.message());
        }
        return true;
#else
        return false;
#endif
    }

#ifdef XRTC_HAVE_WSS
    /**
      @brief 证书校验回调。
      链校验交给 OpenSSL（preverified）；目标身份由自己做，原因：
      - 本 SDK 的 BoringSSL 里 X509_VERIFY_PARAM_set1_host 会失败，
        用它会导致所有 wss://域名 连不上；
      - IP 字面量必须走 iPAddress SAN，用 host_name_verification 会漏。
      这里统一用 X509_check_host / X509_check_ip_asc，语义明确、可单测描述。
    */
    bool verify_peer(bool preverified, ssl::verify_context& ctx) {
        X509_STORE_CTX* store = ctx.native_handle();
        if (!store) {
            return false;
        }
        if (!preverified) {
            const int err = X509_STORE_CTX_get_error(store);
            spdlog::error("[ws] TLS 证书链校验失败 depth={} err={}({})",
                          X509_STORE_CTX_get_error_depth(store), err,
                          X509_verify_cert_error_string(err));
            return false;
        }
        if (X509_STORE_CTX_get_error_depth(store) != 0) {
            return true; // 中间证书只做链校验，身份只查叶子
        }
        X509* leaf = X509_STORE_CTX_get_current_cert(store);
        if (!leaf) {
            spdlog::error("[ws] TLS 校验失败：拿不到对端证书");
            return false;
        }
        const int matched =
            host_is_ip
                ? X509_check_ip_asc(leaf, address.c_str(), 0)
                : X509_check_host(leaf, address.c_str(), address.size(), 0,
                                  nullptr);
        if (matched == 1) {
            return true;
        }
        spdlog::error(
            "[ws] TLS 目标身份不匹配 host={} is_ip={} ret={}"
            "（证书需包含对应 SAN；自签证书请用 XRTC_CA_FILE 指定 CA）",
            address, host_is_ip, matched);
        return false;
    }
#endif

    /// 按 URL scheme 选择通道实现；#ifdef 只出现在这里
    std::unique_ptr<IWebsocketChannel> create_channel() {
        if (!ioc) {
            return nullptr;
        }
        if (!use_ssl) {
            return std::make_unique<PlainChannel>(*ioc, subprotocol);
        }
#ifdef XRTC_HAVE_WSS
        if (ssl_ctx) {
            return std::make_unique<TlsChannel>(*ioc, *ssl_ctx, address,
                                               host_is_ip, subprotocol);
        }
#endif
        return nullptr;
    }

    /**
      @brief 处理dns解析成功后或者解析失败后调用的回调,解析成功之后开始tcp三次握手
      @param ec 错误码
      @param results 解析结果
    */
    void on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
        if (closing.load()) { //如果正在关闭，则返回
            return;
        }
        if (ec) { //如果错误码不为空，说明解析出现错误了
            return fail(ec, "resolve"); //调用fail函数通知错误
        }
        if (!channel) {
            return notify_error("connect: no channel");
        }
        beast::tcp_stream* lowest = &channel->tcp_layer();
        //设置连接超时时间：接下来的网络操作在该时间内未完成会自动超时
        lowest->expires_after(std::chrono::seconds(10));
        //异步连接
        lowest->async_connect(
            results,
            [this](beast::error_code ec2,
                   tcp::resolver::results_type::endpoint_type) {
                on_connect(ec2); //收到连接结果后调用on_connect
            });
    }

    /**
      @brief 处理tcp连接到服务端之后调用的回调,连接成功之后开始握手
      @param ec 错误码
    */
    void on_connect(beast::error_code ec) {
        if (closing.load()) { //如果正在关闭，则返回,不执行这个函数
            return;
        }
        if (ec) { //如果连接有错误,返回错误
            return fail(ec, "connect");
        }
        spdlog::info("[ws] TCP connected {}:{} tls={}", address, port, use_ssl);

        if (!channel) {
            return notify_error("connect: no channel");
        }
        //TCP 连接建立后，后续的超时控制权移交给上层的 WebSocket 流
        channel->tcp_layer().expires_never();
        channel->async_open(host_header, path,
                            [this](beast::error_code ec2, const char* stage) {
                                on_channel_open(ec2, stage);
                            });
    }

    /// 通道握手（TLS + WebSocket）完成/失败
    void on_channel_open(beast::error_code ec, const char* stage) {
        if (closing.load()) { //如果正在关闭，则返回
            return;
        }
        if (ec) {
            if (std::strcmp(stage, "tls-handshake") == 0) {
                // 证书问题最常见，把可操作的提示直接写进错误
                return fail(ec,
                            tls_options.verify
                                ? "tls-handshake (证书校验失败？可设置 "
                                  "XRTC_CA_FILE 指向 CA，或用 XRTC_TLS_VERIFY=0 "
                                  "仅限联调)"
                                : "tls-handshake");
            }
            return fail(ec, stage); //握手有错误,返回错误
        }
        spdlog::info("[ws] WebSocket ESTABLISHED proto={} tls={}", subprotocol,
                     use_ssl);
        connected = true; //设置连接状态为true

        if (owner) {
            owner->connected.emit();
        }
        if (!write_queue.empty()) { //如果准备发送消息的队列中有内容,则发送
            do_write();
        }
        //等待接收消息
        do_read();
    }

    /// 读取网络中接收到的数据,将读到的数据放在read_buf中
    void do_read() {
        if (!channel) {
            return;
        }
        channel->async_read(
            read_buf, [this](beast::error_code ec, std::size_t) {
                on_read(ec);
            });
    }

    /// 处理读取到的数据
    void on_read(beast::error_code ec) {
        if (ec) {
            if (closing.load() || ec == websocket::error::closed ||
                ec == net::error::operation_aborted) {
                stop_ioc();
                return;
            }
            return fail(ec, "read");
        }

        //将从网络中接收到的数据转换成字符串
        const std::string payload = beast::buffers_to_string(read_buf.data());
        //移动读取指针到缓冲区的末尾，表示已经处理了数据
        read_buf.consume(read_buf.size());
        spdlog::info("[ws] RX {} bytes: {}", payload.size(),
                         payload.substr(0, 300));

        if (owner) {
            owner->message.emit(payload);
        }
        do_read();
    }

    /// 将消息添加到发送队列中
    void enqueue_write(std::string text) {
        if (!ioc) {
            return;
        }
        //将lambda表达式放入ioc的队列中,当ioc的队列中有内容时,调用里面的回调函数
        net::post(*ioc, [this, msg = std::move(text)]() mutable {
            //将要发送的消息添加到发送队列中
            write_queue.push_back(std::move(msg));
            //如果当前程序不是正在写数据,并且连接状态为true,则发送消息
            if (!writing && connected.load()) {
                do_write();
            }
        });
    }

    /// 发送消息到服务端,将发送队列中的消息发送到服务端中,
    // 如果队列消息没有结束就会一直调用这个函数
    void do_write() {
        //如果准备发送的队列中没有内容，或者连接状态为false，则返回
        if (write_queue.empty() || !connected.load() || !channel) {
            writing = false; //设置写状态为false
            return;
        }
        writing = true; //设置写状态为true
        channel->async_write(
            write_queue.front(),
            [this](beast::error_code ec, std::size_t) { on_write(ec); });
    }

    ///发送系统缓冲区的数据内容
    void on_write(beast::error_code ec) {
        if (ec) { //如果发送有错误，返回错误
            writing = false; //设置写状态为false
            //如果正在关闭，或者操作被中止，则返回
            if (closing.load() || ec == net::error::operation_aborted) {
                return;
            }
            return fail(ec, "write");
        }
        spdlog::info("[ws] TX {} bytes: {}", write_queue.front().size(),
                         write_queue.front().substr(0, 300));
        //移除队列头部的消息
        write_queue.pop_front();
        //如果队列中还有内容，则继续发送
        if (!write_queue.empty()) {
            do_write();
        } else {
            //如果队列中没有内容，则设置写状态为false
            writing = false; //设置写状态为false
        }
    }

    ///网络线程运行的事件循环
    void service_loop() {
        spdlog::info("[ws] service_loop enter (tls={})", use_ssl);
        try {
            auto resolver = std::make_shared<tcp::resolver>(*ioc);
            channel = create_channel();
            std::string prepare_error;
            if (!channel) {
                notify_error("connect: no channel (wss 未编入 ?)");
            } else if (!channel->prepare(&prepare_error)) {
                notify_error(prepare_error.empty() ? "connect: prepare failed"
                                                   : prepare_error);
            } else {
                //异步解析dns地址,当收到解析结果时调用on_resolve
                resolver->async_resolve(
                    address, std::to_string(port),
                    [this, resolver](beast::error_code ec,
                                     tcp::resolver::results_type results) {
                        on_resolve(ec, std::move(results));
                    });
            }

            ioc->run(); //开启时间循环
        } catch (const std::exception& ex) {
            if (!closing.load()) {
                notify_error(std::string("exception: ") + ex.what());
            }
        }

        //如果不是正在关闭，或者连接状态为false，或者错误已通知，则通知错误
        if (!closing.load() && !connected.load() && !error_notified.load()) {
            notify_error("websocket stopped before connected");
        }

        //通知连接断开
        notify_disconnected_if_needed();
        //释放连接通道与 TLS 上下文
        channel.reset();
#ifdef XRTC_HAVE_WSS
        ssl_ctx.reset();
#endif
        //清空发送队列
        write_queue.clear();
        writing = false; //设置写状态为false
        spdlog::info("[ws] service_loop exit");
    }
};

WebsocketTransport::WebsocketTransport()
    : impl_(std::make_unique<Impl>()) {
    impl_->owner = this;
}

WebsocketTransport::~WebsocketTransport() {
    invalidate();
    close();
}

Rest<> WebsocketTransport::open(const WebsocketTransportConfig& config) {
    spdlog::info("[ws] open url={} verify={} subprotocol={}", config.url,
                 config.tls.verify, config.subprotocol);
    close();
    //解析url地址,失败返回错误
    if (!impl_->parse_url(config.url)) {
        spdlog::info("[ws] invalid url");
        return xrtc_err(XRtcError::kInvalidParam);
    }
    impl_->tls_options = config.tls;
    impl_->subprotocol = config.subprotocol;
#ifndef XRTC_HAVE_WSS
    //如果使用了 ssl 但本构建未启用 WSS，返回明确错误（而不是笼统的握手失败）
    if (impl_->use_ssl) {
        impl_->notify_error(
            "wss not supported: this build has no TLS "
            "(reconfigure with -DXRTC_ENABLE_WSS=ON)");
        return xrtc_err(XRtcError::kSignalingFailed);
    }
#else
    if (impl_->use_ssl && !impl_->setup_tls_context()) {
        return xrtc_err(XRtcError::kSignalingFailed);
    }
#endif
    spdlog::info("[ws] parsed host={} port={} path={}", impl_->address,
                     impl_->port, impl_->path);

    //重置websocket连接状态
    impl_->closing = false;
    impl_->error_notified = false;
    impl_->connected = false;
    impl_->writing = false;
    impl_->write_queue.clear();
    impl_->read_buf.clear();
    impl_->channel.reset();
    impl_->ioc = std::make_unique<net::io_context>();

    //创建一个网络线程,用来处理网络事件
    impl_->thread = std::thread([this]() { impl_->service_loop(); });
    return xrtc_ok();
}

void WebsocketTransport::send_text(const std::string& text) {
    //如果网络线程不可用，或者ioc不可用，则返回
    if (!impl_->thread.joinable() || !impl_->ioc) {
        return;
    }
    //将消息添加到发送队列中
    impl_->enqueue_write(text);
}

void WebsocketTransport::close() {
    if (!impl_->thread.joinable()) {
        impl_->ioc.reset();
        impl_->channel.reset();
#ifdef XRTC_HAVE_WSS
        impl_->ssl_ctx.reset();
#endif
        return;
    }

    impl_->closing = true;
    if (impl_->ioc) {
        impl_->ioc->stop();
    }

    if (impl_->thread.get_id() == std::this_thread::get_id()) {
        impl_->thread.detach();
    } else {
        impl_->thread.join();
    }

    impl_->channel.reset();
#ifdef XRTC_HAVE_WSS
    impl_->ssl_ctx.reset();
#endif
    impl_->ioc.reset();
    impl_->write_queue.clear();
    impl_->connected = false;
}

}  // namespace xrtc
