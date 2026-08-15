#pragma once

#include <cstdint>
#include <functional>

#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include <xrtc/xrtc_defines.h>

namespace xrtc {

/// 远端 VideoTrack 的 sink：I420 → ARGB，回调业务层
class RemoteVideoSink : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    using FrameCallback =
        std::function<void(uint64_t feed_id, const XRTCVideoFrame& frame)>;

    RemoteVideoSink(uint64_t feed_id, FrameCallback cb);
    void OnFrame(const webrtc::VideoFrame& frame) override;

private:
    uint64_t feed_id_ = 0;
    FrameCallback callback_;
};

}  // namespace xrtc
