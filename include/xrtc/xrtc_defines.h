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

///视频配置,包括视频宽度、视频高度、视频帧率、设备ID
struct ixrtc_video_config {
    int width = 640;          /// 视频宽度
    int height = 480;         /// 视频高度
    int fps = 30;             /// 视频帧率
    std::string device_id;    /// 设备ID
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
    // (这个room_id可以是服务端预先配置好的,也可以创建新的,但是目前的代码还没有实现)
    uint64_t room_id = 1234;    /// 已存在的 VideoRoom id
    std::string display_name;   /// 显示名,这个显示名是用户自己填写的,用于显示在房间里
    std::string pin;            /// 房间 pin（可选）
    std::string video_device_id; /// 为空则用第一个摄像头
    std::string audio_device_id; /// 预留；当前用默认录音设备
    int width = 640;
    int height = 480;
    int fps = 30;
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
