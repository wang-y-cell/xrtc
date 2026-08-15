#include <janus/websocket_transport.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>
#include <sstream>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <spdlog/spdlog.h>

#pragma comment(lib, "ws2_32.lib")

namespace xrtc {
namespace {

// ---- minimal SHA-1 (public domain style) ----
struct Sha1 {
    uint32_t h[5]{0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint64_t total = 0;
    uint8_t buf[64]{};
    size_t buf_len = 0;

    static uint32_t rol(uint32_t v, int n) {
        return (v << n) | (v >> (32 - n));
    }

    void process_block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
                   (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    void update(const void* data, size_t len) {
        const auto* p = static_cast<const uint8_t*>(data);
        total += len;
        while (len > 0) {
            const size_t n = (std::min)(len, 64 - buf_len);
            std::memcpy(buf + buf_len, p, n);
            buf_len += n;
            p += n;
            len -= n;
            if (buf_len == 64) {
                process_block(buf);
                buf_len = 0;
            }
        }
    }

    void final(uint8_t out[20]) {
        const uint64_t bit_len = total * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        pad = 0;
        while (buf_len != 56) {
            update(&pad, 1);
        }
        uint8_t lenb[8];
        for (int i = 0; i < 8; ++i) {
            lenb[7 - i] = static_cast<uint8_t>((bit_len >> (8 * i)) & 0xff);
        }
        update(lenb, 8);
        for (int i = 0; i < 5; ++i) {
            out[i * 4] = static_cast<uint8_t>((h[i] >> 24) & 0xff);
            out[i * 4 + 1] = static_cast<uint8_t>((h[i] >> 16) & 0xff);
            out[i * 4 + 2] = static_cast<uint8_t>((h[i] >> 8) & 0xff);
            out[i * 4 + 3] = static_cast<uint8_t>(h[i] & 0xff);
        }
    }
};

std::string Base64Encode(const uint8_t* data, size_t len) {
    static const char* kTbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t n = (uint32_t(data[i]) << 16) |
                           ((i + 1 < len ? uint32_t(data[i + 1]) : 0) << 8) |
                           (i + 2 < len ? uint32_t(data[i + 2]) : 0);
        out.push_back(kTbl[(n >> 18) & 63]);
        out.push_back(kTbl[(n >> 12) & 63]);
        out.push_back(i + 1 < len ? kTbl[(n >> 6) & 63] : '=');
        out.push_back(i + 2 < len ? kTbl[n & 63] : '=');
    }
    return out;
}

std::string MakeWsKey() {
    uint8_t rnd[16];
    std::random_device rd;
    for (int i = 0; i < 16; ++i) {
        rnd[i] = static_cast<uint8_t>(rd());
    }
    return Base64Encode(rnd, 16);
}

std::string MakeAccept(const std::string& key) {
    const std::string src =
        key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    Sha1 sha;
    sha.update(src.data(), src.size());
    uint8_t dig[20];
    sha.final(dig);
    return Base64Encode(dig, 20);
}

bool SendAll(SOCKET s, const char* data, int len) {
    int sent = 0;
    while (sent < len) {
        const int n = ::send(s, data + sent, len - sent, 0);
        if (n > 0) {
            sent += n;
            continue;
        }
        const int err = WSAGetLastError();
        if (n < 0 && err == WSAEWOULDBLOCK) {
            fd_set wset;
            FD_ZERO(&wset);
            FD_SET(s, &wset);
            timeval tv {};
            tv.tv_sec = 5;
            tv.tv_usec = 0;
            if (::select(0, nullptr, &wset, nullptr, &tv) <= 0) {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

}  // namespace

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
    std::string hostport =
        slash == std::string::npos ? rest : rest.substr(0, slash);
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

void WebsocketTransport::notify_error(const std::string& err) {
    bool expected = false;
    if (!error_notified_.compare_exchange_strong(expected, true)) {
        return;
    }
    spdlog::error("[ws] ERROR: {}", err);
    if (on_error_) {
        on_error_(err);
    }
}

void WebsocketTransport::sock_close() {
    if (sock_ != static_cast<std::uintptr_t>(INVALID_SOCKET)) {
        const SOCKET s = static_cast<SOCKET>(sock_);
        ::shutdown(s, SD_BOTH);
        ::closesocket(s);
        sock_ = static_cast<std::uintptr_t>(INVALID_SOCKET);
    }
    connected_ = false;
}

bool WebsocketTransport::tcp_connect() {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        notify_error("socket() failed");
        return false;
    }

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port_));
    if (inet_pton(AF_INET, address_.c_str(), &addr.sin_addr) != 1) {
        addrinfo hints {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(address_.c_str(), nullptr, &hints, &res) != 0 || !res) {
            ::closesocket(s);
            notify_error("resolve host failed: " + address_);
            return false;
        }
        auto* in = reinterpret_cast<sockaddr_in*>(res->ai_addr);
        addr.sin_addr = in->sin_addr;
        freeaddrinfo(res);
    }

    u_long nonblock = 1;
    ioctlsocket(s, FIONBIO, &nonblock);

    const int cr = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (cr != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
        const int err = WSAGetLastError();
        ::closesocket(s);
        notify_error("connect() failed err=" + std::to_string(err));
        return false;
    }

    if (cr != 0) {
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(s, &wset);
        timeval tv {};
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        const int sel = ::select(0, nullptr, &wset, nullptr, &tv);
        int soerr = 0;
        int sl = sizeof(soerr);
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &sl);
        if (sel <= 0 || soerr != 0) {
            ::closesocket(s);
            notify_error("tcp connect timeout/soerr=" + std::to_string(soerr));
            return false;
        }
    }

    sock_ = static_cast<std::uintptr_t>(s);
    spdlog::info("[ws] TCP connected " + address_ + ":" + std::to_string(port_));
    return true;
}

bool WebsocketTransport::ws_handshake() {
    const SOCKET s = static_cast<SOCKET>(sock_);
    const std::string key = MakeWsKey();
    const std::string expect = MakeAccept(key);

    std::ostringstream req;
    req << "GET " << path_ << " HTTP/1.1\r\n"
        << "Host: " << address_ << ":" << port_ << "\r\n"
        << "Upgrade: websocket\r\n"
        << "Connection: Upgrade\r\n"
        << "Sec-WebSocket-Key: " << key << "\r\n"
        << "Sec-WebSocket-Version: 13\r\n"
        << "Sec-WebSocket-Protocol: " << protocol_ << "\r\n"
        << "\r\n";
    const std::string req_s = req.str();
    if (!SendAll(s, req_s.data(), static_cast<int>(req_s.size()))) {
        notify_error("send handshake failed");
        return false;
    }

    std::string resp;
    char tmp[1024];
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (resp.find("\r\n\r\n") == std::string::npos) {
        if (std::chrono::steady_clock::now() > deadline) {
            notify_error("handshake recv timeout");
            return false;
        }
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(s, &rset);
        timeval tv {};
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        if (::select(0, &rset, nullptr, nullptr, &tv) <= 0) {
            continue;
        }
        const int n = ::recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            notify_error("recv handshake failed");
            return false;
        }
        resp.append(tmp, tmp + n);
        if (resp.size() > 8192) {
            notify_error("handshake response too large");
            return false;
        }
    }

    if (resp.find("101") == std::string::npos) {
        notify_error("handshake not 101: " + resp.substr(0, 120));
        return false;
    }
    (void)expect;

    const auto pos = resp.find("\r\n\r\n");
    if (pos != std::string::npos && pos + 4 < resp.size()) {
        rx_buf_.insert(rx_buf_.end(),
                       resp.begin() + static_cast<std::ptrdiff_t>(pos + 4),
                       resp.end());
    }

    spdlog::info("[ws] WebSocket ESTABLISHED proto=" + protocol_);
    return true;
}

bool WebsocketTransport::send_frame(const std::string& text) {
    const SOCKET s = static_cast<SOCKET>(sock_);
    const size_t len = text.size();
    std::vector<uint8_t> frame;
    frame.reserve(14 + len);

    frame.push_back(0x81);  // FIN + text
    uint8_t mask_bit = 0x80;
    if (len < 126) {
        frame.push_back(static_cast<uint8_t>(mask_bit | len));
    } else if (len <= 0xFFFF) {
        frame.push_back(static_cast<uint8_t>(mask_bit | 126));
        frame.push_back(static_cast<uint8_t>((len >> 8) & 0xff));
        frame.push_back(static_cast<uint8_t>(len & 0xff));
    } else {
        frame.push_back(static_cast<uint8_t>(mask_bit | 127));
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<uint8_t>((len >> (8 * i)) & 0xff));
        }
    }

    uint8_t mask[4];
    std::random_device rd;
    for (int i = 0; i < 4; ++i) {
        mask[i] = static_cast<uint8_t>(rd());
        frame.push_back(mask[i]);
    }
    for (size_t i = 0; i < len; ++i) {
        frame.push_back(static_cast<uint8_t>(text[i]) ^ mask[i % 4]);
    }

    return SendAll(s, reinterpret_cast<const char*>(frame.data()),
                   static_cast<int>(frame.size()));
}

bool WebsocketTransport::recv_some() {
    const SOCKET s = static_cast<SOCKET>(sock_);
    if (s == INVALID_SOCKET) {
        return false;
    }
    char tmp[4096];

    fd_set rset;
    FD_ZERO(&rset);
    FD_SET(s, &rset);
    timeval tv {};
    tv.tv_sec = 0;
    tv.tv_usec = 100000;  // 100ms
    const int sel = ::select(0, &rset, nullptr, nullptr, &tv);
    if (sel < 0) {
        return false;
    }
    if (sel == 0) {
        return true;
    }

    const int n = ::recv(s, tmp, sizeof(tmp), 0);
    if (n == 0) {
        return false;
    }
    if (n < 0) {
        const int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            return true;
        }
        return false;
    }
    rx_buf_.insert(rx_buf_.end(), tmp, tmp + n);
    return true;
}

void WebsocketTransport::drain_frames() {
    while (rx_buf_.size() >= 2) {
        const auto* data = reinterpret_cast<const uint8_t*>(rx_buf_.data());
        const size_t n = rx_buf_.size();
        const uint8_t b0 = data[0];
        const uint8_t b1 = data[1];
        const bool fin = (b0 & 0x80) != 0;
        const uint8_t opcode = b0 & 0x0f;
        const bool masked = (b1 & 0x80) != 0;
        uint64_t payload_len = b1 & 0x7f;
        size_t header_len = 2;

        if (payload_len == 126) {
            if (n < 4) {
                return;
            }
            payload_len = (uint64_t(data[2]) << 8) | uint64_t(data[3]);
            header_len = 4;
        } else if (payload_len == 127) {
            if (n < 10) {
                return;
            }
            payload_len = 0;
            for (int i = 0; i < 8; ++i) {
                payload_len = (payload_len << 8) | data[2 + i];
            }
            header_len = 10;
        }

        if (masked) {
            header_len += 4;
        }
        if (n < header_len + payload_len) {
            return;
        }

        std::string payload;
        payload.resize(static_cast<size_t>(payload_len));
        const uint8_t* p = data + header_len;
        if (masked) {
            const uint8_t* mask = data + header_len - 4;
            for (uint64_t i = 0; i < payload_len; ++i) {
                payload[static_cast<size_t>(i)] =
                    static_cast<char>(p[i] ^ mask[i % 4]);
            }
        } else {
            std::memcpy(payload.data(), p, static_cast<size_t>(payload_len));
        }

        rx_buf_.erase(rx_buf_.begin(),
                      rx_buf_.begin() +
                          static_cast<std::ptrdiff_t>(header_len + payload_len));

        if (opcode == 0x8) {  // close
            destroy_flag_ = true;
            return;
        }
        if (opcode == 0x9) {  // ping -> pong
            // ?????????pong
            continue;
        }
        if ((opcode == 0x1 || opcode == 0x0) && fin) {
            spdlog::info("[ws] RX " + std::to_string(payload.size()) +
                 " bytes: " + payload.substr(0, 300));
            if (on_message_) {
                on_message_(std::move(payload));
            }
        }
    }
}

void WebsocketTransport::service_loop() {
    spdlog::info("[ws] service_loop enter");

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        notify_error("WSAStartup failed");
        running_ = false;
        return;
    }

    if (!tcp_connect()) {
        WSACleanup();
        running_ = false;
        return;
    }
    if (!ws_handshake()) {
        sock_close();
        WSACleanup();
        running_ = false;
        return;
    }

    connected_ = true;
    if (on_connected_) {
        on_connected_();
    }
    drain_frames();

    while (!destroy_flag_) {
        // ?????
        std::string msg;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!send_queue_.empty()) {
                msg = std::move(send_queue_.front());
                send_queue_.pop();
            }
        }
        if (!msg.empty()) {
            spdlog::info("[ws] TX " + std::to_string(msg.size()) +
                 " bytes: " + msg.substr(0, 300));
            if (!send_frame(msg)) {
                notify_error("send_frame failed");
                break;
            }
        }

        if (!recv_some()) {
            if (!destroy_flag_) {
                notify_error("connection closed");
            }
            break;
        }
        drain_frames();
    }

    const bool was_connected = connected_.load();
    sock_close();
    if (was_connected && on_disconnected_) {
        on_disconnected_();
    }
    WSACleanup();
    running_ = false;
    spdlog::info("[ws] service_loop exit");
}

bool WebsocketTransport::open(const std::string& url) {
    spdlog::info("[ws] open url={}", url);
    close();
    if (!parse_url(url)) {
        spdlog::info("[ws] invalid url");
        return false;
    }
    if (use_ssl_) {
        notify_error("wss not supported in winsock transport");
        return false;
    }
    spdlog::info("[ws] parsed host={} port={} path={}", address_, port_, path_);

    destroy_flag_ = false;
    error_notified_ = false;
    connected_ = false;
    rx_buf_.clear();
    running_ = true;
    thread_ = std::thread([this]() { service_loop(); });
    return true;
}

void WebsocketTransport::send_text(const std::string& text) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        send_queue_.push(text);
    }
}

void WebsocketTransport::close() {
    destroy_flag_ = true;
    // ??shutdown????select/recv?????????closesocket????????
    if (sock_ != static_cast<std::uintptr_t>(INVALID_SOCKET)) {
        ::shutdown(static_cast<SOCKET>(sock_), SD_BOTH);
    }
    if (thread_.joinable()) {
        if (thread_.get_id() == std::this_thread::get_id()) {
            thread_.detach();
        } else {
            thread_.join();
        }
    }
    sock_close();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::queue<std::string> empty;
        send_queue_.swap(empty);
    }
}

}  // namespace xrtc
