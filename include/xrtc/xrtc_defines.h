#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace xrtc {

///视频设备信息,包括设备名称和设备ID
struct XRTCDeviceInfo {
    std::string device_name;  /// 设备名称
    std::string device_id;    /// 设备ID
};

/// 视频采集格式（宽高帧率）
struct XRTCVideoFormat {
    int width = 0;
    int height = 0;
    int fps = 0;
};

/// 在设备能力中选择采集格式的策略
enum class XRTCVideoSelectStrategy {
    kPreferRequested = 0,  /// 贴近请求的宽高帧率
    kPreferStandard = 1,   /// 优先 1280x720@30，其次 640x480
};

/// 视频采集请求（期望格式 + 选择策略）
struct XRTCVideoCaptureRequest {
    XRTCVideoFormat format{640, 480, 30};
    XRTCVideoSelectStrategy strategy =
        XRTCVideoSelectStrategy::kPreferRequested;
};

///视频配置,包括视频宽度、视频高度、视频帧率、设备ID
struct ixrtc_video_config {
    int width = 0;            /// 0 表示使用 engine 当前采集请求
    int height = 0;
    int fps = 0;
    std::string device_id;    /// 设备ID
    /// kPreferRequested 且未单独指定时，沿用 engine 当前策略
    XRTCVideoSelectStrategy select_strategy =
        XRTCVideoSelectStrategy::kPreferRequested;
};

/// 音频采集配置
struct ixrtc_audio_config {
    std::string device_id;  /// 为空则用默认麦克风
};

///ICE服务器信息,包括ICE服务器地址、ICE服务器用户名、ICE服务器密码
struct XRTCIceServer {
    std::string uri;       /// 例如 stun:stun.l.google.com:19302
    std::string username;  /// TURN 用户名（可选）
    std::string password;  /// TURN 密码（可选）
};

/// 加入 Janus VideoRoom 的配置
///加入房间的配置,包括Janus WebSocket URL、房间ID、显示名、房间PIN、视频设备ID、音频设备ID、视频宽度、视频高度、视频帧率、ICE服务器信息
struct XRTCJoinConfig {
    std::string janus_ws_url;   /// 例如 ws://host:8188/ 或 wss://host/janus
    uint64_t room_id = 1234;    /// VideoRoom id（可预存，也可配合 create_room_if_missing 动态创建）
    std::string display_name;   /// 显示名,这个显示名是用户自己填写的,用于显示在房间里
    std::string pin;            /// 房间 pin（可选）
    /// 若为 true：attach 后先 create，房间已存在（427）则仍继续 join
    bool create_room_if_missing = false;
    std::string room_description;  /// create 时的房间描述（可选）
    uint32_t max_publishers = 6;   /// create 时最大发布者数
    std::string admin_key;         /// Janus VideoRoom admin_key（服务端要求时填写）
    std::string video_device_id; /// 为空则用第一个摄像头
    std::string audio_device_id; /// 为空则用第一个麦克风（WebRTC ADM 经 AudioCapture）
    int width = 0;   /// 0 表示使用 engine 当前采集请求
    int height = 0;
    int fps = 0;
    /// kPreferRequested 且未单独指定时，沿用 engine 当前策略
    XRTCVideoSelectStrategy select_strategy =
        XRTCVideoSelectStrategy::kPreferRequested;
    std::vector<XRTCIceServer> ice_servers;  /// 为空则使用默认 STUN
};

///连接状态,包括新连接、连接中、连接成功、连接失败、连接关闭
enum class XRTCConnectionState {
    kNew = 0,
    kConnecting,
    kConnected,
    kDisconnected,
    kFailed,
    kClosed,
};

///远端用户信息,包括远端用户ID、远端用户显示名
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
