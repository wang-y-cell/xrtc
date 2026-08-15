#pragma once

#include <memory>

#include "api/audio/audio_device.h"
#include "api/scoped_refptr.h"
#include <modules/audio_device/include/audio_device.h>
#include <modules/video_capture/video_capture.h>
#include <xrtc/ixrtc_engine.h>

namespace xrtc {

class CallSession;

class XRtcEngine : public IXRtcEngine {
public:
    XRtcEngine();
    ~XRtcEngine() override;

    std::vector<XRTCDeviceInfo> get_video_device_info() override;
    std::vector<XRTCDeviceInfo> get_audio_device_info() override;

    IXRtcMediaSource* create_video_source(
        const ixrtc_video_config& video_config) override;
    void destroy_video_source(IXRtcMediaSource* video_source) override;

    void join(const XRTCJoinConfig& config) override;
    void leave() override;
    void mute_audio(bool mute) override;
    void mute_video(bool mute) override;

private:
    std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> video_device;
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> audio_device;
    std::unique_ptr<CallSession> call_session_;
};

}  // namespace xrtc
