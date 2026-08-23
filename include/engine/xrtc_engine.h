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

    ///获取视频设备信息,返回设备信息列表,列表中的每个元素包括设备名称和设备ID
    std::vector<XRTCDeviceInfo> get_video_device_info() override;
    ///获取音频设备信息,返回设备信息列表,列表中的每个元素包括设备名称和设备ID
    std::vector<XRTCDeviceInfo> get_audio_device_info() override;

    /**
     * @brief 创建视频源,根据参数中填写的视频配置,创建视频源
     * 此处的视频源是继承自IXRtcMediaSource的VcmCapture类
     * 获得视频源有视频画面渲染的内容,但是不具备发送视频流的功能
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
     * @brief 加入通话,创建call_session会话对象,这个对象具有发送和接收视频流的功能
     * 创建成功之后直接调用call_session的start函数,开始通话
     * @param config 通话配置
    */
    void join(const XRTCJoinConfig& config) override;
    /**
     * @brief 离开通话,这个函数可以在api线程调用,也可以在其他线程中调用,因为如果在其他线程中调用
     * 他会判断并使用api线程调用
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
