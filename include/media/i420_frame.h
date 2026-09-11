#pragma once

#include <memory>

#include "api/scoped_refptr.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include <xrtc/xrtc_defines.h>

namespace xrtc {

inline int RotationToDegrees(webrtc::VideoRotation rotation) {
    switch (rotation) {
        case webrtc::kVideoRotation_90:
            return 90;
        case webrtc::kVideoRotation_180:
            return 180;
        case webrtc::kVideoRotation_270:
            return 270;
        case webrtc::kVideoRotation_0:
        default:
            return 0;
    }
}

inline void CopyVideoTiming(XRTCVideoFrame* out,
                            const webrtc::VideoFrame& src) {
    if (!out) {
        return;
    }
    out->timestamp_us = src.timestamp_us();
    out->ntp_time_ms = src.ntp_time_ms();
    out->rtp_timestamp = src.rtp_timestamp();
    out->render_time_ms = src.render_time_ms();
    out->rotation_degrees = RotationToDegrees(src.rotation());
}

/// 从 WebRTC I420 构造对外帧：零拷贝平面指针，storage 延长 buffer 寿命。
[[nodiscard]] inline XRTCVideoFrame MakeXRTCVideoFrame(
    webrtc::scoped_refptr<webrtc::I420BufferInterface> buffer) {
    XRTCVideoFrame frame;
    if (!buffer) {
        return frame;
    }
    frame.width = buffer->width();
    frame.height = buffer->height();
    frame.stride_y = buffer->StrideY();
    frame.stride_u = buffer->StrideU();
    frame.stride_v = buffer->StrideV();
    frame.data_y = buffer->DataY();
    frame.data_u = buffer->DataU();
    frame.data_v = buffer->DataV();
    webrtc::I420BufferInterface* raw = buffer.get();
    frame.storage = std::shared_ptr<void>(
        raw, [buffer = std::move(buffer)](void*) mutable { buffer = nullptr; });
    return frame;
}

/// 同上，并带上 webrtc::VideoFrame 的时间戳/旋转，供客户端音画同步。
[[nodiscard]] inline XRTCVideoFrame MakeXRTCVideoFrame(
    webrtc::scoped_refptr<webrtc::I420BufferInterface> buffer,
    const webrtc::VideoFrame& timing) {
    XRTCVideoFrame frame = MakeXRTCVideoFrame(std::move(buffer));
    if (frame.valid()) {
        CopyVideoTiming(&frame, timing);
    }
    return frame;
}

}  // namespace xrtc
