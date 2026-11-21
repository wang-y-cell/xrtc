#include <janus/websocket_config.h>

#include <cstdlib>
#include <utility>

#include <janus/websocket_url.h>

namespace xrtc {
namespace {

/// 允许的 CA 环境变量，按优先级排列
constexpr const char* kCaEnvNames[] = {"XRTC_CA_FILE", "SSL_CERT_FILE",
                                       "CURL_CA_BUNDLE"};

const char* GetEnv(const char* name) {
    return std::getenv(name);
}

}  // namespace

WebsocketTlsOptions ResolveWebsocketTlsOptions(bool verify,
                                               std::string ca_file) {
    return ResolveWebsocketTlsOptions(verify, std::move(ca_file), &GetEnv);
}

WebsocketTlsOptions ResolveWebsocketTlsOptions(bool verify, std::string ca_file,
                                               TlsEnvLookup lookup) {
    WebsocketTlsOptions out;
    out.verify = verify;
    out.ca_file = std::move(ca_file);
    if (!out.ca_file.empty() || !lookup) {
        return out;
    }
    for (const char* name : kCaEnvNames) {
        const char* value = lookup(name);
        if (value && value[0] != '\0') {
            out.ca_file = value;
            break;
        }
    }
    return out;
}

WebsocketUrlPolicy CheckWebsocketUrlPolicy(const std::string& url,
                                           bool allow_plaintext) {
    WebsocketUrlParts parts;
    if (!ParseWebsocketUrl(url, &parts)) {
        return WebsocketUrlPolicy::kInvalidUrl;
    }
    if (!parts.use_ssl && !allow_plaintext) {
        return WebsocketUrlPolicy::kPlaintextRejected;
    }
    return WebsocketUrlPolicy::kOk;
}

}  // namespace xrtc
