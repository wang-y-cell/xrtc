#include <media/remote_video_sink.h>
#include <media/argb_frame_pool.h>

#include <algorithm>

#include "api/video/i420_buffer.h"
#include "libyuv/convert_argb.h"
#include "rtc_base/logging.h"

namespace xrtc {

RemoteVideoSink::RemoteVideoSink(uint64_t feed_id, FrameCallback cb)
    : feed_id_(feed_id), callback_(std::move(cb)) {}

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

    // 远端小窗预览：最长边压到 960，降低多路 ARGB 成本
    constexpr int kMaxPreviewLongEdge = 960;
    const int long_edge = std::max(width, height);
    if (long_edge > kMaxPreviewLongEdge) {
        int pw = width;
        int ph = height;
        if (width >= height) {
            pw = kMaxPreviewLongEdge;
            ph = std::max(2, height * kMaxPreviewLongEdge / width);
        } else {
            ph = kMaxPreviewLongEdge;
            pw = std::max(2, width * kMaxPreviewLongEdge / height);
        }
        pw &= ~1;
        ph &= ~1;
        auto scaled = webrtc::I420Buffer::Create(pw, ph);
        scaled->ScaleFrom(*buffer);
        buffer = scaled;
        width = pw;
        height = ph;
    }

    auto argb = AcquireArgbBuffer(width, height);
    if (!argb) {
        return;
    }
    libyuv::I420ToARGB(buffer->DataY(), buffer->StrideY(), buffer->DataU(),
                       buffer->StrideU(), buffer->DataV(), buffer->StrideV(),
                       argb.get(), width * 4, width, height);

    XRTCVideoFrame video_frame;
    video_frame.width = width;
    video_frame.height = height;
    video_frame.argb = std::move(argb);
    callback_(feed_id_, video_frame);
}

}  // namespace xrtc
