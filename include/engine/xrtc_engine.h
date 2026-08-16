#pragma once

#include <memory>

#include "api/audio/audio_device.h"
#include "api/scoped_refptr.h"
#include <modules/audio_device/include/audio_device.h>
#include <modules/video_capture/video_capture.h>
#include <xrtc/ixrtc_engine.h>
#include <xrtc/xrtc_log.h>

namespace xrtc {


class CallSession;

class XRtcEngine : public IXRtcEngine {
public:
    XRtcEngine();
    ~XRtcEngine() override;

    ///获取视频设备信息
    std::vector<XRTCDeviceInfo> get_video_device_info() override;
    ///获取音频设备信息
    std::vector<XRTCDeviceInfo> get_audio_device_info() override;

    IXRtcMediaSource* create_video_source(
        const ixrtc_video_config& video_config) override;
    void destroy_video_source(IXRtcMediaSource* video_source) override;

    void join(const XRTCJoinConfig& config) override;
    void leave() override;
    void mute_audio(bool mute) override;
    void mute_video(bool mute) override;

private:
    ///视频设备信息
    std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> video_device;
    ///音频设备信息
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> audio_device;
    ///通话会话
    std::unique_ptr<CallSession> call_session_;
};

}  // namespace xrtc
