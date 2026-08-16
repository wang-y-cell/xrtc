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

    /**
     * @brief 创建视频源
     * @param video_config 视频配置
     * @return 视频源
    */
    IXRtcMediaSource* create_video_source(
        const ixrtc_video_config& video_config) override;
    /**
     * @brief 销毁视频源
     * @param video_source 视频源
    */
    void destroy_video_source(IXRtcMediaSource* video_source) override;

    /**
     * @brief 加入通话
     * @param config 通话配置
    */
    void join(const XRTCJoinConfig& config) override;
    /**
     * @brief 离开通话
    */
    void leave() override;
    /**
     * @brief 静音音频
     * @param mute 是否静音
    */
    void mute_audio(bool mute) override;
    /**
     * @brief 静音视频
     * @param mute 是否静音
    */
    void mute_video(bool mute) override;

private:
    /**
     * @brief 视频设备信息,用于获取视频设备信息
     * 在构造函数中创建,用于获取视频设备信息
    */
    std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> video_device;
    /**
     * @brief 音频设备信息,用于获取音频设备信息
     * 在构造函数中创建,用于获取音频设备信息
    */
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> audio_device;
    /**
     * @brief 通话会话
     * 在join函数中创建,用于通话会话
    */
    std::unique_ptr<CallSession> call_session_;
};

}  // namespace xrtc
