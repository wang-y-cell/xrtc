#pragma once

#include <cctype>
#include <string>

namespace xrtc {

struct WebsocketUrlParts {
    std::string address;
    std::string path = "/";
    int port = 80;
    bool use_ssl = false;
};

/// 解析 ws:// / wss:// URL；端口非法时返回 false（不抛异常）
inline bool ParseWebsocketUrl(const std::string& url, WebsocketUrlParts* out) {
    if (!out) {
        return false;
    }
    WebsocketUrlParts parts;
    std::string rest = url;
    if (rest.rfind("wss://", 0) == 0) {
        parts.use_ssl = true;
        parts.port = 443;
        rest = rest.substr(6);
    } else if (rest.rfind("ws://", 0) == 0) {
        parts.use_ssl = false;
        parts.port = 80;
        rest = rest.substr(5);
    } else {
        return false;
    }

    const auto slash = rest.find('/');
    std::string hostport =
        slash == std::string::npos ? rest : rest.substr(0, slash);
    parts.path = slash == std::string::npos ? "/" : rest.substr(slash);

    const auto colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        parts.address = hostport.substr(0, colon);
        const std::string port_str = hostport.substr(colon + 1);
        if (port_str.empty()) {
            return false;
        }
        int port = 0;
        for (char c : port_str) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                return false;
            }
            port = port * 10 + (c - '0');
            if (port > 65535) {
                return false;
            }
        }
        if (port <= 0) {
            return false;
        }
        parts.port = port;
    } else {
        parts.address = hostport;
    }

    if (parts.address.empty()) {
        return false;
    }
    *out = std::move(parts);
    return true;
}

}  // namespace xrtc
