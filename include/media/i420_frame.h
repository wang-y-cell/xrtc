#pragma once

#include <memory>

#include "api/scoped_refptr.h"
#include "api/video/i420_buffer.h"
#include <xrtc/xrtc_defines.h>

namespace xrtc {

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

}  // namespace xrtc
