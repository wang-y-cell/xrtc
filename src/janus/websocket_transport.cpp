#include <janus/websocket_transport.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <xrtc/xrtc_log.h>

namespace xrtc {
namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

}  // namespace

struct WebsocketTransport::Impl {
    //指回外面的 WebsocketTransport，用来 emit 信号
    WebsocketTransport* owner = nullptr;

    //解析后的 ws://host:port/path，以及 janus-protocol
    std::string address; //websocket地址(域名或者IP地址)
    std::string path = "/"; //websocket路径
    std::string host_header; //websocket主机头(http请求头中的Host字段,http1.1之后必带)
    int port = 80; //websocket端口
    bool use_ssl = false; //websocket是否使用SSL加密
    std::string protocol = "janus-protocol";

    //boost::asio::io_context，用来管理网络事件
    std::unique_ptr<net::io_context> ioc;
    //boost::beast::websocket::stream，用来管理 WebSocket 连接
    std::unique_ptr<websocket::stream<beast::tcp_stream>> ws;
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
        use_ssl = false;
        port = 80;
        path = "/";
        address.clear();
        host_header.clear();

        std::string rest = url;
        if (rest.rfind("wss://", 0) == 0) { //如果url以wss://开头
            use_ssl = true; //使用ssl
            port = 443; //端口443
            rest = rest.substr(6); //去掉wss://
        } else if (rest.rfind("ws://", 0) == 0) { //如果url以ws://开头
            rest = rest.substr(5); //去掉ws://
        } else {
            return false; //如果url不是wss://或ws://开头，返回false
        }

        //hostport是第一个/之前的字符串,path是/之后的所有字符串
        const auto slash = rest.find('/'); //找到/的位置
        std::string hostport =
            slash == std::string::npos ? rest : rest.substr(0, slash);
        path = slash == std::string::npos ? "/" : rest.substr(slash);

        const auto colon = hostport.rfind(':'); //找到:的位置
        if (colon != std::string::npos) { //如果找到:
            address = hostport.substr(0, colon); //hostport的前一部分是地址
            port = std::stoi(hostport.substr(colon + 1)); //hostport的后一部分是端口
        } else {
            address = hostport; //如果没找到:，则地址就是hostport
        }
        if (address.empty()) { //如果地址为空，返回false
            return false;
        }

        //如果端口是80且没有使用ssl，或者端口是443且使用ssl，则host_header就是地址
        if ((port == 80 && !use_ssl) || (port == 443 && use_ssl)) {
            host_header = address; //80和443是默认端口,host中可以省略端口
        } else { //如果端口不是80且没有使用ssl，或者端口不是443且使用ssl，则host_header就是地址:端口
            //如果不是默认端口而是使用其他端口，则host_header就是地址:端口
            host_header = address + ":" + std::to_string(port);
        }
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
      @brief 处理dns解析成功后或者解析失败后调用的回调
      负责异步开启tcp连接
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
        //设置连接超时时间,接下来的网络操作（
        // 如读取、写入或连接）指定一个相对时间，
        // 如果操作在该时间内未完成，流将自动关闭并抛出超时错误
        beast::get_lowest_layer(*ws).expires_after(std::chrono::seconds(10));
        //异步连接
        beast::get_lowest_layer(*ws).async_connect( //获得底层的tcp流，并异步连接dns解析的结果
            results,
            [this](beast::error_code ec2,
                   tcp::resolver::results_type::endpoint_type) {
                on_connect(ec2); //收到连接结果后调用on_connect
            });
    }

    /**
      @brief 处理tcp连接到服务端之后调用的回调
      如果连接成功就处理websocket握手请求
      @param ec 错误码
    */
    void on_connect(beast::error_code ec) {
        if (closing.load()) { //如果正在关闭，则返回,不执行这个函数
            return;
        }
        if (ec) { //如果连接有错误,返回错误
            return fail(ec, "connect");
        }
        spdlog::info("[ws] TCP connected {}:{}", address, port);

        //获得websocket底层流(tcp流) 
        // 该流上的后续异步操作（如连接、读取、写入）
        // 将不再受之前设置的定时器控制，它们会一直挂起，
        // 直到操作成功完成、发生网络错误，或者被手动取消
        //TCP 连接建立后，后续的超时控制权必须移交给上层的 WebSocket 流。
        // 如果不关闭底层超时，TCP 层的计时器可能会与 WebSocket 层的计时器发生冲突
        beast::get_lowest_layer(*ws).expires_never();
        websocket::stream_base::timeout opt{};
        //设置websocket握手阶段的超时时间
        opt.handshake_timeout = std::chrono::seconds(30); 
        //禁用空闲超时。这意味着如果连接建立后长时间没有数据收发，WebSocket 流也不会因为“空闲”而主动断开连接
        opt.idle_timeout = websocket::stream_base::none();
        //设置websocket选项
        ws->set_option(opt);
        //使用 decorator 装饰器模式，在发送 WebSocket 握手请求前，拦截并修改底层的 HTTP 请求
        //在这里，它向 HTTP 请求头中注入了 Sec-WebSocket-Protocol 字段，值为你类成员变量 protocol（即 "janus-protocol"）
        //这是与 Janus Gateway 等特定服务器通信的必要条件。服务器会根据这个头部字段来确认客户端使用的是哪种子协议
        ws->set_option(websocket::stream_base::decorator(
            [this](websocket::request_type& req) {
                req.set(http::field::sec_websocket_protocol, protocol);
            }));

        //异步发送websocket握手请求，当收到握手响应时调用on_handshake
        ws->async_handshake(host_header, path,
                            [this](beast::error_code ec2) { on_handshake(ec2); });
    }

    /**
      @brief 处理websocket握手成功或者失败后调用的回调
      @param ec 错误码
    */
    void on_handshake(beast::error_code ec) {
        if (closing.load()) { //如果正在关闭，则返回,不执行这个函数
            return;
        }
        if (ec) { //如果握手有错误,返回错误
            return fail(ec, "handshake");
        }
        spdlog::info("[ws] WebSocket ESTABLISHED proto={}", protocol);
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
        ws->async_read(read_buf, [this](beast::error_code ec, std::size_t) {
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
        //移动读取指针到缓冲区的末尾，表示已经处理了这些数据
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
        //如果准备发送消息的队列中没有内容，或者websocket流为空，或者连接状态为false，则返回
        if (write_queue.empty() || !ws || !connected.load()) {
            writing = false; //设置写状态为false
            return;
        }
        writing = true; //设置写状态为true
        ws->text(true); //式设置后续发送消息的帧类型（Opcode）为文本
        //当缓冲区中的所有字节都成功拷贝到操作系统的内核发送缓冲区,调用里面的回调函数
        ws->async_write(net::buffer(write_queue.front()),
                        [this](beast::error_code ec, std::size_t) {
                            on_write(ec);
                        });
    }

    ///发送系统缓冲区的数据内容
    void on_write(beast::error_code ec) {
        if (ec) { //如果发送有错误,返回错误
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
        spdlog::info("[ws] service_loop enter");
        try {
            auto resolver = std::make_shared<tcp::resolver>(*ioc);
            ws = std::make_unique<websocket::stream<beast::tcp_stream>>(*ioc);

            resolver->async_resolve( //异步解析dns地址,当收到解析结果时调用on_resolve
                address, std::to_string(port),
                [this, resolver](beast::error_code ec,
                                 tcp::resolver::results_type results) {
                    on_resolve(ec, std::move(results));
                });

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
        //重置websocket流
        ws.reset();
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

XRtcStatus WebsocketTransport::open(const std::string& url) {
    spdlog::info("[ws] open url={}", url);
    close();
    //解析url地址,失败返回错误
    if (!impl_->parse_url(url)) {
        spdlog::info("[ws] invalid url");
        return xrtc_err(XRtcError::kInvalidParam);
    }
    //如果使用的ssl,则返回错误(因为目前不支持ssl)
    if (impl_->use_ssl) {
        impl_->notify_error("wss not supported (Boost.Beast without OpenSSL)");
        return xrtc_err(XRtcError::kSignalingFailed);
    }
    spdlog::info("[ws] parsed host={} port={} path={}", impl_->address,
                     impl_->port, impl_->path);

    //重置websocket连接状态
    impl_->closing = false;
    impl_->error_notified = false;
    impl_->connected = false;
    impl_->writing = false;
    impl_->write_queue.clear();
    impl_->read_buf.clear();
    impl_->ws.reset();
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
        impl_->ws.reset();
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

    impl_->ws.reset();
    impl_->ioc.reset();
    impl_->write_queue.clear();
    impl_->connected = false;
}

}  // namespace xrtc
