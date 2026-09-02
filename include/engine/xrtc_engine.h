#pragma once

#include <memory>
#include <vector>

#include "api/scoped_refptr.h"
#include <xrtc/ixrtc_engine.h>

namespace xrtc {

class CallSession;
class XrtcAudioDeviceModule;

class XRtcEngine : public IXRtcEngine {
public:
    XRtcEngine();
    ~XRtcEngine() override;

    std::vector<XRTCDeviceInfo> get_video_device_info() override;
    std::vector<XRTCDeviceInfo> get_audio_device_info() override;
    std::vector<XRTCDeviceInfo> get_playout_device_info() override;
    bool set_playout_device(const std::string& device_id) override;

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

    IXRtcMediaSource* create_audio_source(
        const ixrtc_audio_config& audio_config) override;
    void destroy_audio_source(IXRtcMediaSource* audio_source) override;

    void join(const XRTCJoinConfig& config) override;
    void leave() override;
    void mute_audio(bool mute) override;
    void mute_video(bool mute) override;

    bool start_local_video() override;
    void stop_local_video() override;
    bool start_local_audio() override;
    void stop_local_audio() override;

private:
    XRTCVideoCaptureRequest video_capture_request_;
    webrtc::scoped_refptr<XrtcAudioDeviceModule> audio_device_;
    std::unique_ptr<CallSession> call_session_;
};

}  // namespace xrtc
