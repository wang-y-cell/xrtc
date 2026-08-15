#include <janus/websocket_transport.h>

#include <cstring>
#include <vector>

#include <libwebsockets.h>
#include <spdlog/spdlog.h>

namespace xrtc {
namespace {

WebsocketTransport* FromContext(struct lws_context* ctx) {
    return static_cast<WebsocketTransport*>(lws_context_user(ctx));
}

}  // namespace

int LwsCallback(struct lws* wsi, enum lws_callback_reasons reason, void* /*user*/,
                void* in, size_t len) {
    auto* self = FromContext(lws_get_context(wsi));
    if (!self) {
        return 0;
    }

    switch (reason) {
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
            const char* msg = in ? static_cast<const char*>(in) : "connection error";
            spdlog::error("lws connection error: {}", msg);
            self->connected_ = false;
            self->wsi_ = nullptr;
            if (self->on_error_) {
                self->on_error_(msg);
            }
            break;
        }
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            spdlog::info("lws client established");
            self->connected_ = true;
            self->wsi_ = wsi;
            if (self->on_connected_) {
                self->on_connected_();
            }
            lws_callback_on_writable(wsi);
            break;
        case LWS_CALLBACK_CLIENT_RECEIVE: {
            if (self->on_message_ && in && len > 0) {
                self->on_message_(
                    std::string(static_cast<const char*>(in), len));
            }
            break;
        }
        case LWS_CALLBACK_CLIENT_WRITEABLE: {
            std::string msg;
            {
                std::lock_guard<std::mutex> lock(self->mutex_);
                if (self->send_queue_.empty()) {
                    break;
                }
                msg = std::move(self->send_queue_.front());
                self->send_queue_.pop();
            }
            std::vector<unsigned char> buf(LWS_PRE + msg.size());
            std::memcpy(buf.data() + LWS_PRE, msg.data(), msg.size());
            const int n = lws_write(wsi, buf.data() + LWS_PRE,
                                    msg.size(), LWS_WRITE_TEXT);
            if (n < 0) {
                spdlog::error("lws_write failed");
                return -1;
            }
            {
                std::lock_guard<std::mutex> lock(self->mutex_);
                if (!self->send_queue_.empty()) {
                    lws_callback_on_writable(wsi);
                }
            }
            break;
        }
        case LWS_CALLBACK_CLIENT_CLOSED:
            spdlog::info("lws client closed");
            self->connected_ = false;
            self->wsi_ = nullptr;
            if (self->on_disconnected_) {
                self->on_disconnected_();
            }
            break;
        case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
            if (self->wsi_ && self->connected_) {
                std::lock_guard<std::mutex> lock(self->mutex_);
                if (!self->send_queue_.empty()) {
                    lws_callback_on_writable(self->wsi_);
                }
            }
            break;
        default:
            break;
    }
    return 0;
}

static const struct lws_protocols kProtocols[] = {
    {"janus-protocol", LwsCallback, 0, 4096, 0, nullptr, 0},
    LWS_PROTOCOL_LIST_TERM,
};

WebsocketTransport::WebsocketTransport() = default;

WebsocketTransport::~WebsocketTransport() {
    close();
}

void WebsocketTransport::set_callbacks(MessageCallback on_message,
                                       VoidCallback on_connected,
                                       VoidCallback on_disconnected,
                                       ErrorCallback on_error) {
    on_message_ = std::move(on_message);
    on_connected_ = std::move(on_connected);
    on_disconnected_ = std::move(on_disconnected);
    on_error_ = std::move(on_error);
}

bool WebsocketTransport::parse_url(const std::string& url) {
    use_ssl_ = false;
    port_ = 80;
    path_ = "/";
    address_.clear();

    std::string rest = url;
    if (rest.rfind("wss://", 0) == 0) {
        use_ssl_ = true;
        port_ = 443;
        rest = rest.substr(6);
    } else if (rest.rfind("ws://", 0) == 0) {
        rest = rest.substr(5);
    } else {
        return false;
    }

    const auto slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    path_ = slash == std::string::npos ? "/" : rest.substr(slash);

    const auto colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        address_ = hostport.substr(0, colon);
        port_ = std::stoi(hostport.substr(colon + 1));
    } else {
        address_ = hostport;
    }
    return !address_.empty();
}

bool WebsocketTransport::open(const std::string& url) {
    close();
    if (!parse_url(url)) {
        spdlog::error("invalid websocket url: {}", url);
        return false;
    }
    if (use_ssl_) {
        spdlog::warn("wss requested but SSL may be disabled in build; continuing");
    }

    destroy_flag_ = false;
    running_ = true;
    thread_ = std::thread([this]() { service_loop(); });
    return true;
}

void WebsocketTransport::service_loop() {
    struct lws_context_creation_info info {};
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = kProtocols;
    info.user = this;
    info.options = 0;

    context_ = lws_create_context(&info);
    if (!context_) {
        spdlog::error("lws_create_context failed");
        running_ = false;
        if (on_error_) {
            on_error_("lws_create_context failed");
        }
        return;
    }

    struct lws_client_connect_info ccinfo {};
    ccinfo.context = context_;
    ccinfo.address = address_.c_str();
    ccinfo.port = port_;
    ccinfo.path = path_.c_str();
    ccinfo.host = address_.c_str();
    ccinfo.origin = address_.c_str();
    ccinfo.protocol = protocol_.c_str();
    ccinfo.ssl_connection = use_ssl_ ? LCCSCF_USE_SSL : 0;
    ccinfo.pwsi = &wsi_;

    if (!lws_client_connect_via_info(&ccinfo)) {
        spdlog::error("lws_client_connect_via_info failed");
        if (on_error_) {
            on_error_("connect failed");
        }
    }

    while (!destroy_flag_) {
        lws_service(context_, 50);
    }

    lws_context_destroy(context_);
    context_ = nullptr;
    wsi_ = nullptr;
    connected_ = false;
    running_ = false;
}

void WebsocketTransport::send_text(const std::string& text) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        send_queue_.push(text);
    }
    request_writable();
}

void WebsocketTransport::request_writable() {
    if (context_) {
        lws_cancel_service(context_);
        if (wsi_ && connected_) {
            lws_callback_on_writable(wsi_);
        }
    }
}

void WebsocketTransport::close() {
    destroy_flag_ = true;
    if (context_) {
        lws_cancel_service(context_);
    }
    if (thread_.joinable()) {
        //不可以join自己线程，否则会死锁
        if (thread_.get_id() == std::this_thread::get_id()) {
            thread_.detach();
        } else {
            thread_.join();
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::queue<std::string> empty;
        send_queue_.swap(empty);
    }
}

}  // namespace xrtc
