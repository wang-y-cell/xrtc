#pragma once

#include <string>

namespace xrtc {

/// WebSocket 传输层的 TLS 参数（仅 wss:// 生效）。
struct WebsocketTlsOptions {
    /// 是否校验证书链与目标身份；生产环境必须为 true。
    /// 置 false 只用于自签名证书的自建 Janus 联调。
    bool verify = true;
    /// CA 证书链文件（PEM）。为空表示用 OpenSSL 默认证书路径。
    /// 环境变量回退（XRTC_CA_FILE 等）由 ResolveWebsocketTlsOptions 完成，
    /// 传输层本身不读环境变量，便于测试与替换配置来源。
    std::string ca_file;
};

/// WebSocket 传输层的连接参数。与具体信令协议无关：
/// Janus 只负责把 subprotocol 填成 "janus-protocol"。
struct WebsocketTransportConfig {
    /// ws:// 或 wss:// 地址
    std::string url;
    /// Sec-WebSocket-Protocol 请求头；空串表示不发送该头
    std::string subprotocol = "janus-protocol";
    /// 仅 wss:// 生效
    WebsocketTlsOptions tls;
};

/// 读环境变量的最小接口，测试可注入假实现。
using TlsEnvLookup = const char* (*)(const char* name);

/// 解析 TLS 参数。优先级：
/// 显式 ca_file > XRTC_CA_FILE > SSL_CERT_FILE > CURL_CA_BUNDLE > 系统默认路径。
WebsocketTlsOptions ResolveWebsocketTlsOptions(bool verify, std::string ca_file);

/// 同上，但环境变量由调用方提供（便于单测，不触碰进程环境）。
WebsocketTlsOptions ResolveWebsocketTlsOptions(bool verify, std::string ca_file,
                                               TlsEnvLookup lookup);

/// 「URL + 是否允许明文」的策略校验结果
enum class WebsocketUrlPolicy {
    kOk,                ///< 通过
    kInvalidUrl,        ///< 不是 ws:// / wss://，或端口非法
    kPlaintextRejected  ///< 是 ws://，但调用方要求必须加密
};

/// 在连接前校验 URL 与明文策略；wss:// 永远允许。
WebsocketUrlPolicy CheckWebsocketUrlPolicy(const std::string& url,
                                           bool allow_plaintext);

}  // namespace xrtc
