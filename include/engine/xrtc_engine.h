#pragma once

#include <cstdint>
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
    Rest<> set_playout_device(const std::string& device_id) override;
    Rest<> set_audio_device(const std::string& device_id) override;
    Rest<> set_video_device(const std::string& device_id) override;
    /// 枚举显示器，见 IXRtcEngine::get_screen_info
    std::vector<XRTCScreenInfo> get_screen_info() override;

    std::vector<XRTCVideoFormat> get_video_capabilities(
        const std::string& device_id) override;

    XRTCVideoFormat select_video_format(
        const std::string& device_id,
        const XRTCVideoFormat& requested,
        XRTCVideoSelectStrategy strategy) override;

    void set_video_capture_request(
        const XRTCVideoCaptureRequest& request) override;
    XRTCVideoCaptureRequest get_video_capture_request() const override;

    void set_screen_share_config(
        const XRTCScreenShareConfig& config) override;
    XRTCScreenShareConfig get_screen_share_config() const override;

    IXRtcMediaSource* create_video_source(
        const ixrtc_video_config& video_config) override;
    void destroy_video_source(IXRtcMediaSource* video_source) override;
    /// 创建屏幕预览源。screen_id 为空用主屏，释放用 destroy_video_source
    IXRtcMediaSource* create_screen_source(const std::string& screen_id) override;

    IXRtcMediaSource* create_audio_source(
        const ixrtc_audio_config& audio_config) override;
    void destroy_audio_source(IXRtcMediaSource* audio_source) override;

    void join(const XRTCJoinConfig& config) override;
    void leave() override;
    void mute_audio(bool mute) override;
    void mute_video(bool mute) override;

    Rest<> start_local_video() override;
    Rest<> stop_local_video() override;
    /// 会议内共享屏幕并取消视频静音。会停掉正在进行的摄像头采集
    Rest<> start_local_screen(const std::string& screen_id) override;
    /// 停止屏幕采集。若当前推的是屏幕，同时静音视频轨
    Rest<> stop_local_screen() override;
    Rest<> start_local_audio() override;
    Rest<> stop_local_audio() override;

private:
    XRTCVideoCaptureRequest video_capture_request_;
    /// 屏幕共享画质（跨会话保存，join 时同步给 CallSession）
    XRTCScreenShareConfig screen_share_config_;
    webrtc::scoped_refptr<XrtcAudioDeviceModule> audio_device_;
    std::unique_ptr<CallSession> call_session_;
    /// leave/destroy 递增，作废已排队的 join PostTask
    uint64_t join_generation_ = 0;
};

}  // namespace xrtc
