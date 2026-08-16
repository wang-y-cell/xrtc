#include <media/video_track_source.h>

#include "api/make_ref_counted.h"

namespace xrtc {

webrtc::scoped_refptr<XrtcVideoTrackSource> XrtcVideoTrackSource::Create() {
    return webrtc::make_ref_counted<XrtcVideoTrackSource>();
}

XrtcVideoTrackSource::XrtcVideoTrackSource()
    : webrtc::VideoTrackSource(/*remote=*/false) {}

void XrtcVideoTrackSource::PushFrame(const webrtc::VideoFrame& frame) {
    //把这一帧视频交给 VideoBroadcaster，由它分发给所有订阅者
    broadcaster_.OnFrame(frame);
}

void XrtcVideoTrackSource::SetLive(bool live) {
    SetState(live ? SourceState::kLive : SourceState::kEnded);
}

webrtc::VideoSourceInterface<webrtc::VideoFrame>* XrtcVideoTrackSource::source() {
    return &broadcaster_;
}

}  // namespace xrtc
