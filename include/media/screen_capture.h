#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "api/scoped_refptr.h"
#include <media/video_track_source.h>
#include <xrtc/ixrtc_media_source.h>
#include <xrtc/xrtc_defines.h>

namespace xrtc {

class ScreenGrabber;

/// 屏幕采集。通过 ScreenGrabber 取桌面帧，再转成 I420 推流和本地预览。
/// 与摄像头共用一条视频轨时，由 CallSession 保证二者不会同时 start。
class ScreenCapture : public IXRtcMediaSource {
public:
    /// screen_id 为空则选主屏。没有显示器时返回 nullptr
    static std::unique_ptr<ScreenCapture> Create(const std::string& screen_id);

    /// 枚举显示器（主屏排在最前），不打开采集会话
    static std::vector<XRTCScreenInfo> get_screen_info();

    ~ScreenCapture() override;

    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    /// 打开平台屏幕采集并在采集线程里取帧；已在采集则直接成功
    Rest<> start() override;
    /// 停采集线程并释放平台采集。未在采集时不做回调
    Rest<> stop() override;
    /// 换显示器。正在采集时会停掉再按新 id 重新打开
    Rest<> device_switch(const std::string& device_id) override;

    /// 采集帧推入这条轨道（会议推流）。可空，仅预览时不设
    void set_track_source(
        webrtc::scoped_refptr<XrtcVideoTrackSource> track_source);
    /// 是否向 Observer 送本地预览。不影响推流
    void set_local_preview_enabled(bool enabled);

    /// 实际输出宽高与帧率（长边不超过 1920，约 15fps）
    XRTCVideoFormat capture_format() const;
    /// 解析后的显示器设备名，例如 \\.\DISPLAY1
    const std::string& screen_id() const { return screen_id_; }
    /// 采集线程是否已经成功打开
    bool started() const { return running_; }

private:
    explicit ScreenCapture(std::string screen_id);

    /// 调用方已持有 life_mu_。建采集线程并等 open_grabber 结果
    Rest<> start_locked();
    /// 调用方已持有 life_mu_。通知线程退出、join，并在曾采集时 SetLive(false)
    void stop_locked();
    /// 按 screen_id_ 打开平台抓屏，并记下输出宽高
    bool open_grabber();
    /// 释放平台抓屏占用的系统资源
    void close_grabber();
    /// 取桌面帧（光标已由 WebRTC 合成）、转 I420，然后推流并回调预览
    void capture_loop();

    std::string screen_id_;
    int fps_ = 15;

    mutable std::mutex life_mu_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    bool running_ = false;

    mutable std::mutex cb_mu_;
    webrtc::scoped_refptr<XrtcVideoTrackSource> track_source_;
    bool preview_enabled_ = true;
    int out_width_ = 0;
    int out_height_ = 0;

    std::unique_ptr<ScreenGrabber> grabber_;
};

}  // namespace xrtc
