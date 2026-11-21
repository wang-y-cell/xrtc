#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include <janus/websocket_config.h>

namespace {

/// 假环境：只提供 XRTC_CA_FILE
const char* EnvXrtcOnly(const char* name) {
    return std::strcmp(name, "XRTC_CA_FILE") == 0 ? "C:/ca/xrtc.pem" : nullptr;
}

/// 假环境：只提供 SSL_CERT_FILE（应优先于 CURL_CA_BUNDLE）
const char* EnvSslOnly(const char* name) {
    return std::strcmp(name, "SSL_CERT_FILE") == 0 ? "C:/ca/ssl.pem" : nullptr;
}

/// 假环境：只有 CURL_CA_BUNDLE
const char* EnvCurlOnly(const char* name) {
    return std::strcmp(name, "CURL_CA_BUNDLE") == 0 ? "C:/ca/curl.pem"
                                                    : nullptr;
}

/// 假环境：变量存在但为空串，应视为未设置
const char* EnvEmpty(const char*) {
    return "";
}

/// 假环境：什么都没有
const char* EnvNone(const char*) {
    return nullptr;
}

}  // namespace

// 显式 ca_file 必须压过所有环境变量（调用方显式指定优先）
TEST(WebsocketTlsResolve, ExplicitCaFileWinsOverEnv) {
    const auto opt =
        xrtc::ResolveWebsocketTlsOptions(true, "C:/ca/explicit.pem", &EnvXrtcOnly);
    EXPECT_EQ(opt.ca_file, "C:/ca/explicit.pem");
    EXPECT_TRUE(opt.verify);
}

// 环境变量优先级：XRTC_CA_FILE > SSL_CERT_FILE > CURL_CA_BUNDLE
TEST(WebsocketTlsResolve, EnvPriorityOrder) {
    EXPECT_EQ(xrtc::ResolveWebsocketTlsOptions(true, "", &EnvXrtcOnly).ca_file,
              "C:/ca/xrtc.pem");
    EXPECT_EQ(xrtc::ResolveWebsocketTlsOptions(true, "", &EnvSslOnly).ca_file,
              "C:/ca/ssl.pem");
    EXPECT_EQ(xrtc::ResolveWebsocketTlsOptions(true, "", &EnvCurlOnly).ca_file,
              "C:/ca/curl.pem");
}

// verify 与 ca_file 互不影响：关闭校验时仍保留 CA 信息供日志/排查
TEST(WebsocketTlsResolve, KeepsVerifyFlagIndependent) {
    const auto opt = xrtc::ResolveWebsocketTlsOptions(false, "C:/ca/x.pem", &EnvNone);
    EXPECT_FALSE(opt.verify);
    EXPECT_EQ(opt.ca_file, "C:/ca/x.pem");
}

// 空串环境变量视为未设置；完全没有时 ca_file 留空（走 OpenSSL 默认路径）
TEST(WebsocketTlsResolve, EmptyEnvTreatedAsUnset) {
    EXPECT_TRUE(
        xrtc::ResolveWebsocketTlsOptions(true, "", &EnvEmpty).ca_file.empty());
    EXPECT_TRUE(
        xrtc::ResolveWebsocketTlsOptions(true, "", &EnvNone).ca_file.empty());
}

// lookup 为空指针时不能崩（测试/嵌入场景可能不提供环境）
TEST(WebsocketTlsResolve, NullLookupIsSafe) {
    EXPECT_TRUE(
        xrtc::ResolveWebsocketTlsOptions(true, "", nullptr).ca_file.empty());
}

// wss 永远合法；ws 是否合法由 allow_plaintext 决定
TEST(WebsocketUrlPolicy, AcceptsWssPlainWhenAllowed) {
    EXPECT_EQ(xrtc::CheckWebsocketUrlPolicy("wss://janus.example.com/janus",
                                            false),
              xrtc::WebsocketUrlPolicy::kOk);
    EXPECT_EQ(xrtc::CheckWebsocketUrlPolicy("ws://192.168.1.10:8188/janus",
                                            true),
              xrtc::WebsocketUrlPolicy::kOk);
}

TEST(WebsocketUrlPolicy, RejectsPlaintextWhenTlsRequired) {
    EXPECT_EQ(xrtc::CheckWebsocketUrlPolicy("ws://janus.example.com/janus",
                                            false),
              xrtc::WebsocketUrlPolicy::kPlaintextRejected);
}

TEST(WebsocketUrlPolicy, RejectsUnknownSchemeAndBadPort) {
    EXPECT_EQ(xrtc::CheckWebsocketUrlPolicy("http://janus.example.com", true),
              xrtc::WebsocketUrlPolicy::kInvalidUrl);
    EXPECT_EQ(xrtc::CheckWebsocketUrlPolicy("", true),
              xrtc::WebsocketUrlPolicy::kInvalidUrl);
    EXPECT_EQ(xrtc::CheckWebsocketUrlPolicy("wss://host:70000/janus", true),
              xrtc::WebsocketUrlPolicy::kInvalidUrl);
}

// 传输层配置的默认值即 Janus 的约定，避免上层每次手填
TEST(WebsocketTransportConfig, DefaultsMatchJanusContract) {
    const xrtc::WebsocketTransportConfig cfg;
    EXPECT_EQ(cfg.subprotocol, std::string("janus-protocol"));
    EXPECT_TRUE(cfg.tls.verify);
    EXPECT_TRUE(cfg.tls.ca_file.empty());
    EXPECT_TRUE(cfg.url.empty());
}
