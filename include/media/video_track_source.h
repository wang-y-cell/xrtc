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

    void PushFrame(const webrtc::VideoFrame& frame);
    void SetLive(bool live);

protected:
    XrtcVideoTrackSource();
    ~XrtcVideoTrackSource() override = default;

    webrtc::VideoSourceInterface<webrtc::VideoFrame>* source() override;

private:
    webrtc::VideoBroadcaster broadcaster_;
};

}  // namespace xrtc
