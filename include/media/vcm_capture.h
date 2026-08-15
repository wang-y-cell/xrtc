#pragma once

#include <xrtc/ixrtc_media_source.h>
#include <media/video_track_source.h>

#include <memory>
#include <string>

#include <modules/video_capture/video_capture.h>
#include <rtc_base/thread.h>

namespace xrtc {

class VcmCapture : public IXRtcMediaSource,
                   public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    static VcmCapture* create(size_t width, size_t height, int fps,
                              const std::string& device_id);

    ~VcmCapture() override;

    bool start() override;
    bool stop() override;
    void OnFrame(const webrtc::VideoFrame& frame) override;

    void set_track_source(
        webrtc::scoped_refptr<XrtcVideoTrackSource> track_source);

private:
    VcmCapture(size_t width, size_t height, int fps,
               const std::string& device_id) noexcept;
    bool init(size_t width, size_t height, int fps,
              const std::string& device_id);
    void destroy();

    size_t _width;
    size_t _height;
    int _fps;
    std::string _device_id;
    webrtc::Thread* _current_thread;
    webrtc::scoped_refptr<webrtc::VideoCaptureModule> _vcm;
    webrtc::VideoCaptureCapability _capability;
    webrtc::scoped_refptr<XrtcVideoTrackSource> track_source_;
};

}  // namespace xrtc
