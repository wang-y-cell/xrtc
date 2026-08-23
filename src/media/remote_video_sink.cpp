#include <media/remote_video_sink.h>

#include "api/video/i420_buffer.h"
#include "libyuv/convert_argb.h"
#include "rtc_base/logging.h"

namespace xrtc {

RemoteVideoSink::RemoteVideoSink(uint64_t feed_id, FrameCallback cb)
    : feed_id_(feed_id), callback_(std::move(cb)) {}

/// @brief 远端视频帧的接收与转换入口：WebRTC 从 Janus 拉下来的每一帧视频都会进这里，
///转成 UI 能用的 ARGB，再交给上层回调
/// @param frame 远端视频帧,frame可能是yuv420,也可能是其他的视频格式
void RemoteVideoSink::OnFrame(const webrtc::VideoFrame& frame) {
    if (!callback_) {
        return;
    }

    // 无论原来的视频格式是什么, 都将 WebRTC 的 VideoFrame 转换为 I420
    webrtc::scoped_refptr<webrtc::I420BufferInterface> buffer(
        frame.video_frame_buffer()->ToI420());
    if (!buffer) {
        return;
    }

    //若 frame.rotation() 不是 0，用 I420Buffer::Rotate 摆正画面
    //frame中有一段旋转元数据,frame.rotation()就是读取这段数据
    //webrtc::kVideoRotation_0表示不旋转
    //webrtc::kVideoRotation_90表示90度旋转
    //webrtc::kVideoRotation_180表示180度旋转
    //webrtc::kVideoRotation_270表示270度旋转
    //frame.rotation()表示要把画面移动多少度才可以恢复
    if (frame.rotation() != webrtc::kVideoRotation_0) {
        //buffer是像素, frame.rotation()是旋转角度, 将buffer旋转frame.rotation()角度
        buffer = webrtc::I420Buffer::Rotate(*buffer, frame.rotation());
    }

    const int width = buffer->width();
    const int height = buffer->height();
    if (width <= 0 || height <= 0) {
        return;
    }

    auto argb = std::make_shared<std::vector<uint8_t>>(
        static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    //将I420格式转换为ARGB格式
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
