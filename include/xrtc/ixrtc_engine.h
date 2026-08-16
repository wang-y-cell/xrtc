#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <xrtc/xrtc_defines.h>
#include <xrtc/ixrtc_media_source.h>

namespace xrtc {

enum class XRtcError {
    kNOERROR = 0,
    kVideoSourceNotInit = -999,
    kVideoSourceStartFailed,
    kAlreadyInCall,
    kNotInCall,
    kInvalidParam,
    kSignalingFailed,
    kPeerConnectionFailed,
    kMediaStartFailed,
};

class XRtcEngineObserver {
public:
    virtual ~XRtcEngineObserver() = default;

    ///开启视频资源之后，界面需要展示的逻辑
    virtual void video_source_start_event(IXRtcMediaSource* video_source,
                                          XRtcError error) {} //开启视频资源之后界面的动作,

    ///关闭视频资源之后，界面需要展示的逻辑
    virtual void video_source_stop_event(IXRtcMediaSource* video_source,
                                         XRtcError error) {}

    // 本地采集线程回调；实现侧应尽快返回，并自行切到 UI 线程渲染
    virtual void on_video_frame(IXRtcMediaSource* video_source,
                                const XRTCVideoFrame& frame) {}

    virtual void on_join_result(XRtcError error, const std::string& message) {}

    virtual void on_leave(XRtcError error) {}

    virtual void on_connection_state(XRTCConnectionState state) {}

    virtual void on_remote_user_joined(const XRTCRemoteUser& user) {}

    virtual void on_remote_user_left(const XRTCRemoteUser& user) {}

    // 远端视频帧；实现侧应尽快返回，并自行切到 UI 线程渲染
    virtual void on_remote_video_frame(uint64_t feed_id,
                                       const XRTCVideoFrame& frame) {}
};

class IXRtcEngine {
public:
    virtual ~IXRtcEngine() = default;
    virtual std::vector<XRTCDeviceInfo> get_video_device_info() = 0;
    virtual std::vector<XRTCDeviceInfo> get_audio_device_info() = 0;
    virtual IXRtcMediaSource* create_video_source(
        const ixrtc_video_config& video_config) = 0;
    virtual void destroy_video_source(IXRtcMediaSource* video_source) = 0;

    /// 加入 Janus VideoRoom（自动采集并推流）；异步，结果走 on_join_result
    virtual void join(const XRTCJoinConfig& config) = 0;
    /// 离开房间并释放通话资源
    virtual void leave() = 0;

    virtual void mute_audio(bool mute) = 0;
    virtual void mute_video(bool mute) = 0;
};

IXRtcEngine* create_xrtc_engine(XRtcEngineObserver* observer);
void destroy_xrtc_engine(IXRtcEngine* engine);

}  // namespace xrtc
