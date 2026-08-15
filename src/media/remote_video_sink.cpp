#include <media/remote_video_sink.h>

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

    const int width = buffer->width();
    const int height = buffer->height();
    if (width <= 0 || height <= 0) {
        return;
    }

    auto argb = std::make_shared<std::vector<uint8_t>>(
        static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    libyuv::I420ToARGB(buffer->DataY(), buffer->StrideY(), buffer->DataU(),
                       buffer->StrideU(), buffer->DataV(), buffer->StrideV(),
                       argb->data(), width * 4, width, height);

    XRTCVideoFrame video_frame;
    video_frame.width = width;
    video_frame.height = height;
    video_frame.argb = std::move(argb);
    callback_(feed_id_, video_frame);
}

}  // namespace xrtc
