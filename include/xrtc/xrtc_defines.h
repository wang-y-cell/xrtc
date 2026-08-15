#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace xrtc {

struct XRTCDeviceInfo {
    std::string device_name;  /// 设备名称
    std::string device_id;    /// 设备ID
};

struct ixrtc_video_config {
    int width = 640;          /// 视频宽度
    int height = 480;         /// 视频高度
    int fps = 30;             /// 视频帧率
    std::string device_id;    /// 设备ID
};

struct XRTCIceServer {
    std::string uri;       /// 例如 stun:stun.l.google.com:19302
    std::string username;  /// TURN 用户名（可选）
    std::string password;  /// TURN 密码（可选）
};

/// 加入 Janus VideoRoom 的配置
struct XRTCJoinConfig {
    std::string janus_ws_url;   /// 例如 ws://host:8188/ 或 wss://host/janus
    uint64_t room_id = 1234;    /// 已存在的 VideoRoom id
    std::string display_name;   /// 显示名
    std::string pin;            /// 房间 pin（可选）
    std::string video_device_id; /// 为空则用第一个摄像头
    std::string audio_device_id; /// 预留；当前用默认录音设备
    int width = 640;
    int height = 480;
    int fps = 30;
    std::vector<XRTCIceServer> ice_servers;  /// 为空则使用默认 STUN
};

enum class XRTCConnectionState {
    kNew = 0,
    kConnecting,
    kConnected,
    kDisconnected,
    kFailed,
    kClosed,
};

struct XRTCRemoteUser {
    uint64_t feed_id = 0;
    std::string display;
};

// 预览用 ARGB 帧（小端内存布局为 B,G,R,A，对应 QImage::Format_ARGB32）
struct XRTCVideoFrame {
    int width = 0;
    int height = 0;
    std::shared_ptr<std::vector<uint8_t>> argb;
};

}  // namespace xrtc
