#pragma once

#include "api/media_stream_interface.h"
#include "api/video/video_frame.h"
#include "api/video/video_source_interface.h"
#include "api/video/video_broadcaster.h"
#include "pc/video_track_source.h"

namespace xrtc {

/// 将 VCM 采集帧推入 PeerConnection 的本地视频源
class XrtcVideoTrackSource : public webrtc::VideoTrackSource {
public:
    static webrtc::scoped_refptr<XrtcVideoTrackSource> Create();

    ///@brief 将视频推入broadcaster_中
    void PushFrame(const webrtc::VideoFrame& frame);
    ///@brief 设置视频源是否为实时状态
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
