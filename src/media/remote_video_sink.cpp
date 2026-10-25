#include <media/remote_video_sink.h>
#include <media/i420_frame.h>

#include <algorithm>

#include "api/video/i420_buffer.h"

namespace xrtc {

RemoteVideoSink::RemoteVideoSink(uint64_t feed_id, int max_long_edge,
                                 FrameCallback cb)
    : feed_id_(feed_id),
      max_long_edge_(max_long_edge <= 0 ? 0 : std::max(240, max_long_edge)),
      callback_(std::move(cb)) {}

void RemoteVideoSink::OnFrame(const webrtc::VideoFrame& frame) {
    if (!callback_) {
        return;
    }

    webrtc::scoped_refptr<webrtc::I420BufferInterface> buffer(
        frame.video_frame_buffer()->ToI420());
    if (!buffer) {
        return;
    }

    if (frame.rotation() != webrtc::kVideoRotation_0) {
        buffer = webrtc::I420Buffer::Rotate(*buffer, frame.rotation());
    }

    int width = buffer->width();
    int height = buffer->height();
    if (width <= 0 || height <= 0) {
        return;
    }

    // 默认不缩放：远端按对端原始分辨率渲染（1080p/1440p 都保留）。
    // 只有显式配置了 remote_max_long_edge 才降分辨率（多路时省 CPU）。
    if (max_long_edge_ > 0) {
        const int long_edge = std::max(width, height);
        if (long_edge > max_long_edge_) {
            int pw = width;
            int ph = height;
            if (width >= height) {
                pw = max_long_edge_;
                ph = std::max(2, height * max_long_edge_ / width);
            } else {
                ph = max_long_edge_;
                pw = std::max(2, width * max_long_edge_ / height);
            }
            pw &= ~1;
            ph &= ~1;
            auto scaled = webrtc::I420Buffer::Create(pw, ph);
            scaled->ScaleFrom(*buffer);
            buffer = scaled;
        }
    }

    XRTCVideoFrame out = MakeXRTCVideoFrame(std::move(buffer), frame);
    // 像素已按 rotation 转正，避免客户端再旋转
    out.rotation_degrees = 0;
    callback_(feed_id_, out);
}

}  // namespace xrtc
