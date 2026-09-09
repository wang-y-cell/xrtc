#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "api/media_stream_interface.h"
#include "api/scoped_refptr.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include <xrtc/xrtc_defines.h>

namespace xrtc {

/// 远端 VideoTrack 的 sink：取 I420（可降分辨率），回调业务层
class RemoteVideoSink : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    using FrameCallback =
        std::function<void(uint64_t feed_id, const XRTCVideoFrame& frame)>;

    RemoteVideoSink(uint64_t feed_id, FrameCallback cb);
    void OnFrame(const webrtc::VideoFrame& frame) override;

private:
    ///@brief 远端视频流的ID
    uint64_t feed_id_ = 0;
    ///@brief 将 I420 帧交给上层（OpenGL 等）
    FrameCallback callback_;
};

/// 绑定远端 VideoTrack 与 Sink；析构时必须 RemoveSink，否则会堆损坏
struct RemoteVideoAttachment {
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> track;
    std::unique_ptr<RemoteVideoSink> sink;

    RemoteVideoAttachment() = default;
    ~RemoteVideoAttachment() { Detach(); }

    RemoteVideoAttachment(const RemoteVideoAttachment&) = delete;
    RemoteVideoAttachment& operator=(const RemoteVideoAttachment&) = delete;

    RemoteVideoAttachment(RemoteVideoAttachment&& other) noexcept
        : track(std::move(other.track)), sink(std::move(other.sink)) {}

    RemoteVideoAttachment& operator=(RemoteVideoAttachment&& other) noexcept {
        if (this != &other) {
            Detach();
            track = std::move(other.track);
            sink = std::move(other.sink);
        }
        return *this;
    }

    void Detach() {
        if (track && sink) {
            track->RemoveSink(sink.get());
        }
        sink.reset();
        track = nullptr;
    }
};

}  // namespace xrtc
