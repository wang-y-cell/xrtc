#pragma once

#include "api/media_stream_interface.h"
#include "api/video/video_frame.h"
#include "api/video/video_source_interface.h"
#include "api/video/video_broadcaster.h"
#include "pc/video_track_source.h"

namespace xrtc {

/// 将 VCM 采集帧推入 PeerConnection 的本地视频源
///XrtcVideoTrackSource 是项目里的本地摄像头视频源适配器，作用是把 VcmCapture 采集到的帧，接入 WebRTC 的推流链路
///用 VideoBroadcaster 把采集帧分发给 WebRTC 内部订阅者
class XrtcVideoTrackSource : public webrtc::VideoTrackSource {
public:
    static webrtc::scoped_refptr<XrtcVideoTrackSource> Create();

    ///@brief 将视频推入broadcaster_中
    ///摄像头每采一帧，VcmCapture::OnFrame 调用它，把帧交给 broadcaster_
    void PushFrame(const webrtc::VideoFrame& frame);
    ///@brief 设置视频源是否为实时状态
    ///设置源状态为 kLive / kEnded，告诉 WebRTC 是否还在产生视频
    ///@param live 是否为实时状态
    void SetLive(bool live);

protected:
    XrtcVideoTrackSource();
    ~XrtcVideoTrackSource() override = default;

    ///@brief 获取视频源接口_broadcaster_地址
    webrtc::VideoSourceInterface<webrtc::VideoFrame>* source() override;

private:
    /** 
    * @brief 视频广播器,用于将视频帧推入PeerConnection的本地视频源
    * 直接初始化
     */
    webrtc::VideoBroadcaster broadcaster_;
};

}  // namespace xrtc
