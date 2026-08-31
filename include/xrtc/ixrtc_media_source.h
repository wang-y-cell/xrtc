#pragma once 

#include <string>

namespace xrtc {

/// 音视频采集公共接口（启停 / 切换设备）
class IXRtcMediaSource {
public:
    virtual ~IXRtcMediaSource() = default;

    /// 开始采集
    virtual bool start() = 0;
    /// 停止采集
    virtual bool stop() = 0;
    /// 切换采集设备（device_id 为空时由实现决定默认设备）
    virtual bool device_switch(const std::string& device_id) = 0;
};

} //namespace xrtc
