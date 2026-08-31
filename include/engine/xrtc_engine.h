#pragma once

#include <memory>
#include <vector>

#include "api/scoped_refptr.h"
#include "modules/video_capture/video_capture.h"
#include <xrtc/ixrtc_engine.h>

namespace webrtc {
class AudioDeviceModule;
}  // namespace webrtc

namespace xrtc {

class CallSession;

class XRtcEngine : public IXRtcEngine {
public:
    XRtcEngine();
    ~XRtcEngine() override;

    std::vector<XRTCDeviceInfo> get_video_device_info() override;
    std::vector<XRTCDeviceInfo> get_audio_device_info() override;

    std::vector<XRTCVideoFormat> get_video_capabilities(
        const std::string& device_id) override;
    XRTCVideoFormat select_video_format(
        const std::string& device_id,
        const XRTCVideoFormat& requested,
        XRTCVideoSelectStrategy strategy) override;

    void set_video_capture_request(
        const XRTCVideoCaptureRequest& request) override;
    XRTCVideoCaptureRequest get_video_capture_request() const override;

    IXRtcMediaSource* create_video_source(
        const ixrtc_video_config& video_config) override;
    void destroy_video_source(IXRtcMediaSource* video_source) override;

    void join(const XRTCJoinConfig& config) override;
    void leave() override;
    void mute_audio(bool mute) override;
    void mute_video(bool mute) override;

private:
    XRTCVideoCaptureRequest video_capture_request_;
    std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> video_device;
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> audio_device;
    std::unique_ptr<CallSession> call_session_;
};

}  // namespace xrtc
