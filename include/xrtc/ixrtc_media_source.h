#pragma once 

#include <xrtc/xrtc_defines.h>

namespace xrtc {


class IXRtcMediaSource {
public:
    virtual ~IXRtcMediaSource() = default;
    virtual bool start() = 0; // 开始采集媒体数据
    virtual bool stop() = 0; // 停止采集媒体数据

    /// 当前实际采集格式（可能经设备能力匹配后与请求不同）
    virtual XRTCVideoFormat capture_format() const = 0;

    /// 修改期望采集格式；采集中会按策略重选能力并重启
    virtual bool set_capture_request(const XRTCVideoFormat& requested) = 0;
};


} //namespace xrtc