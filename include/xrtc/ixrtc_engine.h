#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <internal/xrtc_result.h>
#include <xrtc/xrtc_defines.h>
#include <xrtc/ixrtc_media_source.h>

namespace xrtc {

class XRtcEngineObserver {
public:
    virtual ~XRtcEngineObserver() = default;

    ///开启视频资源之后，观察者界面需要展示的逻辑
    virtual void video_source_start_event(IXRtcMediaSource* video_source,
                                          XRtcError error) {} //开启视频资源之后界面的动作,

    ///关闭视频资源之后，观察者界面需要展示的逻辑
    virtual void video_source_stop_event(IXRtcMediaSource* video_source,
                                         XRtcError error) {}

    // 本地/远端视频帧（I420 + 时间戳/旋转，供 OpenGL 或客户端自行同步）
    virtual void on_video_frame(IXRtcMediaSource* video_source,
                                const XRTCVideoFrame& frame) {}

    virtual void on_join_result(XRtcError error, const std::string& message) {}

    virtual void on_leave(XRtcError error) {}

    virtual void on_connection_state(XRTCConnectionState state) {}

    virtual void on_remote_user_joined(const XRTCRemoteUser& user) {}

    virtual void on_remote_user_left(const XRTCRemoteUser& user) {}

    // 远端视频帧（含 timestamp_us / ntp / rtp / rotation）；尽快返回并切 UI 线程
    virtual void on_remote_video_frame(uint64_t feed_id,
                                       const XRTCVideoFrame& frame) {}

    /// 本地麦克风音量 0~100（自采 AudioCapture）；尽快返回并切 UI 线程
    virtual void on_audio_level(IXRtcMediaSource* audio_source, int level) {
        (void)audio_source;
        (void)level;
    }

    /// 远端音量 0~100（按 feed 区分，约每 level_interval_ms 一次，含静音时的 0）。
    /// 用于音量条；只关心"谁在说话"时用下面的 on_remote_talking 更省事。
    virtual void on_remote_audio_level(uint64_t feed_id, int level) {
        (void)feed_id;
        (void)level;
    }

    /// 远端是否在说话：由远端音量 + 迟滞(threshold/release_ms)判定，
    /// 只在状态切换时回调（不说话 -> 说话、说完 -> 静音）。
    /// 远端静音/断流时由看门狗定时降级，因此一定会收到收尾的 false。
    virtual void on_remote_talking(uint64_t feed_id, bool talking) {
        (void)feed_id;
        (void)talking;
    }
};

class IXRtcEngine {
public:
    virtual ~IXRtcEngine() = default;
    virtual std::vector<XRTCDeviceInfo> get_video_device_info() = 0;
    virtual std::vector<XRTCDeviceInfo> get_audio_device_info() = 0;
    /// 枚举扬声器 / 播放设备（与麦克风列表分开）
    virtual std::vector<XRTCDeviceInfo> get_playout_device_info() = 0;
    /// 选择远端音频播放设备；空 id 表示默认。可在进房前或会议中调用
    virtual Rest<> set_playout_device(const std::string& device_id) = 0;
    /// 选择麦克风；会中有采集器则立即切换，否则记住供下次 StartLocalAudio / join
    virtual Rest<> set_audio_device(const std::string& device_id) = 0;
    /// 选择摄像头；会中有采集器则立即切换，否则记住供下次 StartLocalVideo / join
    virtual Rest<> set_video_device(const std::string& device_id) = 0;

    /// 枚举显示器。screen_id 传给 start_local_screen / create_screen_source
    virtual std::vector<XRTCScreenInfo> get_screen_info() = 0;

    /// 枚举摄像头支持的采集格式（可能含重复分辨率、不同 pixel format）
    virtual std::vector<XRTCVideoFormat> get_video_capabilities(
        const std::string& device_id) = 0;

    /// 按策略在设备能力中选出最终可用格式（含 WebRTC best-match）
    /// 推荐在 create_video_source / join 前调用，用于 UI 展示实际分辨率
    virtual XRTCVideoFormat select_video_format(
        const std::string& device_id,
        const XRTCVideoFormat& requested,
        XRTCVideoSelectStrategy strategy =
            XRTCVideoSelectStrategy::kPreferRequested) = 0;

    /// 设置/读取全局默认采集请求（create_video_source / join 中宽高帧率为 0 时使用）
    virtual void set_video_capture_request(
        const XRTCVideoCaptureRequest& request) = 0;
    virtual XRTCVideoCaptureRequest get_video_capture_request() const = 0;

    /// 设置/读取屏幕共享画质（分辨率上限、帧率、码率、远端渲染上限）。
    /// 采集类参数在下次 start_local_screen 生效；远端渲染上限对新订阅立即生效。
    virtual void set_screen_share_config(
        const XRTCScreenShareConfig& config) = 0;
    virtual XRTCScreenShareConfig get_screen_share_config() const = 0;

    /// 设置/读取远端说话检测参数（阈值 / 防抖 / 音量回调间隔）。
    /// 会中修改对已订阅的远端立即生效；进房前设置则本次会话沿用。
    virtual void set_voice_activity_config(
        const XRTCVoiceActivityConfig& config) = 0;
    virtual XRTCVoiceActivityConfig get_voice_activity_config() const = 0;

    virtual IXRtcMediaSource* create_video_source(
        const ixrtc_video_config& video_config) = 0;
    virtual void destroy_video_source(IXRtcMediaSource* video_source) = 0;

    /// 创建屏幕采集源（预览）。screen_id 为空则用主屏。释放仍用 destroy_video_source
    virtual IXRtcMediaSource* create_screen_source(
        const std::string& screen_id) = 0;

    /// 创建本地音频采集源（WebRTC ADM + 旁路音量）；勿与 join 同时开同一 ADM
    virtual IXRtcMediaSource* create_audio_source(
        const ixrtc_audio_config& audio_config) = 0;
    virtual void destroy_audio_source(IXRtcMediaSource* audio_source) = 0;

    /// 加入 Janus VideoRoom（准备轨与采集器，默认不开采不推流）；异步，结果走 on_join_result
    virtual void join(const XRTCJoinConfig& config) = 0;
    /// 离开房间并释放通话资源
    virtual void leave() = 0;

    /// 仅控制是否推流（轨 enable）；不影响硬件采集
    virtual void mute_audio(bool mute) = 0;
    virtual void mute_video(bool mute) = 0;

    /// 会议内：开摄像头采集并开始推流；若正在共享屏幕则先停屏幕
    virtual Rest<> start_local_video() = 0;
    /// 会议内：停推流，并停摄像头与屏幕采集
    virtual Rest<> stop_local_video() = 0;
    /// 会议内：共享屏幕并推流。screen_id 为空用主屏；会停掉摄像头采集
    virtual Rest<> start_local_screen(const std::string& screen_id) = 0;
    /// 会议内：停止屏幕采集。若当前推的是屏幕，同时停止视频推流
    virtual Rest<> stop_local_screen() = 0;
    /// 会议内：开麦采集并开始推流；失败见 Rest<>
    virtual Rest<> start_local_audio() = 0;
    /// 会议内：停推流并停硬件录音
    virtual Rest<> stop_local_audio() = 0;
};

IXRtcEngine* create_xrtc_engine(XRtcEngineObserver* observer);
void destroy_xrtc_engine(IXRtcEngine* engine);

}  // namespace xrtc
