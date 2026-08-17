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
    std::string address;
    std::string path = "/";
    std::string host_header;
    int port = 80;
    bool use_ssl = false;
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
    //std::atomic<bool>，用来标记是否已连接
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
        utils::log::error("[ws] ERROR: {}", err);
        if (owner) {
            owner->error.emit(err);
        }
    }

    /**
      @brief 解析 URL，提取 host、port、path 等信息
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
            host_header = address;
        } else { //如果端口不是80且没有使用ssl，或者端口不是443且使用ssl，则host_header就是地址:端口
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
        if (closing.load()) {
            return;
        }
        if (ec == net::error::operation_aborted) {
            return;
        }
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
        const bool was_connected = connected.exchange(false); //如果连接状态为true，则设置为false
        if (!was_connected || !owner) { //如果连接状态为false，或者owner为空，则返回
            return;
        }
        owner->disconnected.emit(); //通知断开连接
    }

    /**
      @brief 处理解析失败
      @param ec 错误码
      @param results 解析结果
    */
    void on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
        if (closing.load()) {
            return;
        }
        if (ec) {
            return fail(ec, "resolve");
        }
        beast::get_lowest_layer(*ws).expires_after(std::chrono::seconds(10));
        beast::get_lowest_layer(*ws).async_connect(
            results,
            [this](beast::error_code ec2,
                   tcp::resolver::results_type::endpoint_type) {
                on_connect(ec2);
            });
    }

    /**
      @brief 处理连接成功
      @param ec 错误码
    */
    void on_connect(beast::error_code ec) {
        if (closing.load()) {
            return;
        }
        if (ec) {
            return fail(ec, "connect");
        }
        utils::log::info("[ws] TCP connected {}:{}", address, port);

        beast::get_lowest_layer(*ws).expires_never();
        websocket::stream_base::timeout opt{};
        opt.handshake_timeout = std::chrono::seconds(30);
        opt.idle_timeout = websocket::stream_base::none();
        ws->set_option(opt);
        ws->set_option(websocket::stream_base::decorator(
            [this](websocket::request_type& req) {
                req.set(http::field::sec_websocket_protocol, protocol);
            }));

        ws->async_handshake(host_header, path,
                            [this](beast::error_code ec2) { on_handshake(ec2); });
    }

    void on_handshake(beast::error_code ec) {
        if (closing.load()) {
            return;
        }
        if (ec) {
            return fail(ec, "handshake");
        }
        utils::log::info("[ws] WebSocket ESTABLISHED proto={}", protocol);
        connected = true;

        if (owner) {
            owner->connected.emit();
        }
        if (!write_queue.empty()) {
            do_write();
        }
        do_read();
    }

    void do_read() {
        ws->async_read(read_buf, [this](beast::error_code ec, std::size_t) {
            on_read(ec);
        });
    }

    void on_read(beast::error_code ec) {
        if (ec) {
            if (closing.load() || ec == websocket::error::closed ||
                ec == net::error::operation_aborted) {
                stop_ioc();
                return;
            }
            return fail(ec, "read");
        }

        const std::string payload = beast::buffers_to_string(read_buf.data());
        read_buf.consume(read_buf.size());
        utils::log::info("[ws] RX {} bytes: {}", payload.size(),
                         payload.substr(0, 300));

        if (owner) {
            owner->message.emit(payload);
        }
        do_read();
    }

    void enqueue_write(std::string text) {
        if (!ioc) {
            return;
        }
        net::post(*ioc, [this, msg = std::move(text)]() mutable {
            write_queue.push_back(std::move(msg));
            if (!writing && connected.load()) {
                do_write();
            }
        });
    }

    void do_write() {
        if (write_queue.empty() || !ws || !connected.load()) {
            writing = false;
            return;
        }
        writing = true;
        ws->text(true);
        ws->async_write(net::buffer(write_queue.front()),
                        [this](beast::error_code ec, std::size_t) {
                            on_write(ec);
                        });
    }

    void on_write(beast::error_code ec) {
        if (ec) {
            writing = false;
            if (closing.load() || ec == net::error::operation_aborted) {
                return;
            }
            return fail(ec, "write");
        }
        utils::log::info("[ws] TX {} bytes: {}", write_queue.front().size(),
                         write_queue.front().substr(0, 300));
        write_queue.pop_front();
        if (!write_queue.empty()) {
            do_write();
        } else {
            writing = false;
        }
    }

    void service_loop() {
        utils::log::info("[ws] service_loop enter");
        try {
            auto resolver = std::make_shared<tcp::resolver>(*ioc);
            ws = std::make_unique<websocket::stream<beast::tcp_stream>>(*ioc);

            resolver->async_resolve(
                address, std::to_string(port),
                [this, resolver](beast::error_code ec,
                                 tcp::resolver::results_type results) {
                    on_resolve(ec, std::move(results));
                });

            ioc->run();
        } catch (const std::exception& ex) {
            if (!closing.load()) {
                notify_error(std::string("exception: ") + ex.what());
            }
        }

        if (!closing.load() && !connected.load() && !error_notified.load()) {
            notify_error("websocket stopped before connected");
        }

        notify_disconnected_if_needed();
        ws.reset();
        write_queue.clear();
        writing = false;
        utils::log::info("[ws] service_loop exit");
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
    utils::log::info("[ws] open url={}", url);
    close();

    if (!impl_->parse_url(url)) {
        utils::log::info("[ws] invalid url");
        return xrtc_err(XRtcError::kInvalidParam);
    }
    if (impl_->use_ssl) {
        impl_->notify_error("wss not supported (Boost.Beast without OpenSSL)");
        return xrtc_err(XRtcError::kSignalingFailed);
    }
    utils::log::info("[ws] parsed host={} port={} path={}", impl_->address,
                     impl_->port, impl_->path);

    impl_->closing = false;
    impl_->error_notified = false;
    impl_->connected = false;
    impl_->writing = false;
    impl_->write_queue.clear();
    impl_->read_buf.clear();
    impl_->ws.reset();
    impl_->ioc = std::make_unique<net::io_context>();

    impl_->thread = std::thread([this]() { impl_->service_loop(); });
    return xrtc_ok();
}

void WebsocketTransport::send_text(const std::string& text) {
    if (!impl_->thread.joinable() || !impl_->ioc) {
        return;
    }
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
