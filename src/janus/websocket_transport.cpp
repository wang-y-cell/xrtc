#include <janus/websocket_transport.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <spdlog/spdlog.h>

namespace xrtc {
namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

}  // namespace

struct WebsocketTransport::Impl {
    MessageCallback on_message;
    VoidCallback on_connected;
    VoidCallback on_disconnected;
    ErrorCallback on_error;

    std::string address;
    std::string path = "/";
    std::string host_header;
    int port = 80;
    bool use_ssl = false;
    std::string protocol = "janus-protocol";

    // 每次 open 新建，避免 restart 后残留 stop/cancel 回调立刻结束 run()
    std::unique_ptr<net::io_context> ioc;
    std::unique_ptr<websocket::stream<beast::tcp_stream>> ws;
    beast::flat_buffer read_buf;
    std::deque<std::string> write_queue;
    bool writing = false;

    std::thread thread;
    std::atomic<bool> connected{false};
    std::atomic<bool> closing{false};
    std::atomic<bool> error_notified{false};
    std::mutex cb_mutex;

    void notify_error(const std::string& err) {
        bool expected = false;
        if (!error_notified.compare_exchange_strong(expected, true)) {
            return;
        }
        spdlog::error("[ws] ERROR: {}", err);
        ErrorCallback cb;
        {
            std::lock_guard<std::mutex> lock(cb_mutex);
            cb = on_error;
        }
        if (cb) {
            cb(err);
        }
    }

    bool parse_url(const std::string& url) {
        use_ssl = false;
        port = 80;
        path = "/";
        address.clear();
        host_header.clear();

        std::string rest = url;
        if (rest.rfind("wss://", 0) == 0) {
            use_ssl = true;
            port = 443;
            rest = rest.substr(6);
        } else if (rest.rfind("ws://", 0) == 0) {
            rest = rest.substr(5);
        } else {
            return false;
        }

        const auto slash = rest.find('/');
        std::string hostport =
            slash == std::string::npos ? rest : rest.substr(0, slash);
        path = slash == std::string::npos ? "/" : rest.substr(slash);

        const auto colon = hostport.rfind(':');
        if (colon != std::string::npos) {
            address = hostport.substr(0, colon);
            port = std::stoi(hostport.substr(colon + 1));
        } else {
            address = hostport;
        }
        if (address.empty()) {
            return false;
        }

        if ((port == 80 && !use_ssl) || (port == 443 && use_ssl)) {
            host_header = address;
        } else {
            host_header = address + ":" + std::to_string(port);
        }
        return true;
    }

    void fail(beast::error_code ec, const char* what) {
        if (closing.load()) {
            return;
        }
        // operation_aborted 常见于主动 close/cancel，不当作成连接失败
        if (ec == net::error::operation_aborted) {
            return;
        }
        notify_error(std::string(what) + ": " + ec.message());
        stop_ioc();
    }

    void stop_ioc() {
        if (ioc) {
            ioc->stop();
        }
    }

    void notify_disconnected_if_needed() {
        const bool was_connected = connected.exchange(false);
        if (!was_connected) {
            return;
        }
        VoidCallback cb;
        {
            std::lock_guard<std::mutex> lock(cb_mutex);
            cb = on_disconnected;
        }
        if (cb) {
            cb();
        }
    }

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

    void on_connect(beast::error_code ec) {
        if (closing.load()) {
            return;
        }
        if (ec) {
            return fail(ec, "connect");
        }
        spdlog::info("[ws] TCP connected {}:{}", address, port);

        beast::get_lowest_layer(*ws).expires_never();
        // 关闭 idle timeout，避免 Janus 静默期被 Beast 主动掐断
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
        spdlog::info("[ws] WebSocket ESTABLISHED proto={}", protocol);
        connected = true;

        VoidCallback cb;
        {
            std::lock_guard<std::mutex> lock(cb_mutex);
            cb = on_connected;
        }
        if (cb) {
            cb();
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
        spdlog::info("[ws] RX {} bytes: {}", payload.size(),
                     payload.substr(0, 300));

        MessageCallback cb;
        {
            std::lock_guard<std::mutex> lock(cb_mutex);
            cb = on_message;
        }
        if (cb) {
            cb(payload);
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
        spdlog::info("[ws] TX {} bytes: {}", write_queue.front().size(),
                     write_queue.front().substr(0, 300));
        write_queue.pop_front();
        if (!write_queue.empty()) {
            do_write();
        } else {
            writing = false;
        }
    }

    void service_loop() {
        spdlog::info("[ws] service_loop enter");
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

        // 尚未连上就退出（例如残留 stop / 被拒后二次 open 失败）必须上报，
        // 否则 UI 会一直停在 joining...
        if (!closing.load() && !connected.load() && !error_notified.load()) {
            notify_error("websocket stopped before connected");
        }

        notify_disconnected_if_needed();
        ws.reset();
        write_queue.clear();
        writing = false;
        spdlog::info("[ws] service_loop exit");
    }
};

WebsocketTransport::WebsocketTransport() : impl_(std::make_unique<Impl>()) {}

WebsocketTransport::~WebsocketTransport() {
    close();
}

void WebsocketTransport::set_callbacks(MessageCallback on_message,
                                       VoidCallback on_connected,
                                       VoidCallback on_disconnected,
                                       ErrorCallback on_error) {
    std::lock_guard<std::mutex> lock(impl_->cb_mutex);
    impl_->on_message = std::move(on_message);
    impl_->on_connected = std::move(on_connected);
    impl_->on_disconnected = std::move(on_disconnected);
    impl_->on_error = std::move(on_error);
}

bool WebsocketTransport::open(const std::string& url) {
    spdlog::info("[ws] open url={}", url);
    close();

    if (!impl_->parse_url(url)) {
        spdlog::info("[ws] invalid url");
        return false;
    }
    if (impl_->use_ssl) {
        impl_->notify_error("wss not supported (Boost.Beast without OpenSSL)");
        return false;
    }
    spdlog::info("[ws] parsed host={} port={} path={}", impl_->address,
                 impl_->port, impl_->path);

    impl_->closing = false;
    impl_->error_notified = false;
    impl_->connected = false;
    impl_->writing = false;
    impl_->write_queue.clear();
    impl_->read_buf.clear();
    impl_->ws.reset();
    // 关键全新 io_context，杜绝上次 stop 残留回调
    impl_->ioc = std::make_unique<net::io_context>();

    impl_->thread = std::thread([this]() { impl_->service_loop(); });
    return true;
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
    // stop() 线程安全；不要 post 残留回调到下次 open 的 context
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
