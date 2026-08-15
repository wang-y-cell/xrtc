#pragma once 

namespace xrtc {


class IXRtcMediaSource {
public:
    virtual ~IXRtcMediaSource() = default;
    virtual bool start() = 0; // 开始采集媒体数据
    virtual bool stop() = 0; // 停止采集媒体数据
private:

};


} //namespace xrtc