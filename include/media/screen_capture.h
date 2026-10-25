#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "api/scoped_refptr.h"
#include "api/video/i420_buffer.h"
#include <media/video_track_source.h>
#include <xrtc/ixrtc_media_source.h>
#include <xrtc/xrtc_defines.h>

namespace xrtc {

class ScreenGrabber;

/// 屏幕采集。通过 ScreenGrabber 取桌面原始帧，
/// 用 libyuv 做 BGRA→I420 与缩放（双线性），再推流和本地预览。
/// 与摄像头共用一条视频轨时，由 CallSession 保证二者不会同时 start。
class ScreenCapture : public IXRtcMediaSource {
public:
    /// screen_id 为空则选主屏。max_long_edge<=0 表示不缩放；
    /// fps 为采集/输出帧率上限。没有显示器时返回 nullptr
    static std::unique_ptr<ScreenCapture> Create(const std::string& screen_id,
                                                 int max_long_edge = 2560,
                                                 int fps = 30);

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

    /// 实际输出宽高与帧率（宽高受 max_long_edge 约束）
    XRTCVideoFormat capture_format() const;
    /// 创建时指定的帧率上限
    int configured_fps() const { return fps_; }
    /// 创建时指定的长边上限；0 表示不缩放
    int configured_max_long_edge() const { return max_long_edge_; }
    /// 解析后的显示器设备名，例如 \\.\DISPLAY1
    const std::string& screen_id() const { return screen_id_; }
    /// 采集线程是否已经成功打开
    bool started() const { return running_; }

private:
    ScreenCapture(std::string screen_id, int max_long_edge, int fps);

    /// 调用方已持有 life_mu_。建采集线程并等 open_grabber 结果
    Rest<> start_locked();
    /// 调用方已持有 life_mu_。通知线程退出、join，并在曾采集时 SetLive(false)
    void stop_locked();
    /// 按 screen_id_ 打开平台抓屏，并记下输出宽高
    bool open_grabber();
    /// 释放平台抓屏占用的系统资源
    void close_grabber();
    /// 取桌面原始帧（光标已由 WebRTC 合成），转 I420/缩放后推流并回调预览
    void capture_loop();
    /// 按当前源分辨率与 max_long_edge_ 重算输出宽高
    void refresh_output_size(int src_w, int src_h);

    std::string screen_id_;
    int fps_ = 30;
    /// 采集长边上限；0 = 不缩放
    int max_long_edge_ = 2560;

    mutable std::mutex life_mu_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    bool running_ = false;

    mutable std::mutex cb_mu_;
    webrtc::scoped_refptr<XrtcVideoTrackSource> track_source_;
    bool preview_enabled_ = true;
    int out_width_ = 0;
    int out_height_ = 0;

    /// 采集线程私有的中间缓冲（源分辨率 I420），避免每帧分配
    webrtc::scoped_refptr<webrtc::I420Buffer> full_buffer_;
    int full_width_ = 0;
    int full_height_ = 0;

    std::unique_ptr<ScreenGrabber> grabber_;
};

}  // namespace xrtc
