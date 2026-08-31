#pragma once

#include <xrtc/ixrtc_media_source.h>
#include <xrtc/xrtc_defines.h>
#include <media/video_track_source.h>

#include <memory>
#include <string>
#include <vector>

#include <modules/video_capture/video_capture.h>
#include <rtc_base/thread.h>

namespace xrtc {

/// 任何需要获取视频帧的模块，都需要继承webrtc::VideoSinkInterface并实现这个接口
///实现了该接口的类，必须重写 OnFrame(const webrtc::VideoFrame& frame) 方法。当上游的视频轨道（VideoTrack）有新的视频帧可用时，WebRTC 引擎会自动调用这个方法来分发数据
///模板参数 webrtc::VideoFrame 起到了指定数据类型的作用
class VcmCapture : public IXRtcMediaSource,
                   public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    /** 
     * @brief 创建视频采集模块
     * @param width 视频宽度
     * @param height 视频高度
     * @param fps 视频帧率
     * @param device_id 设备id
     * @param strategy 能力选择策略
     * @return 视频采集模块
    */
    static std::unique_ptr<VcmCapture> Create(
        size_t width,
        size_t height,
        int fps,
        const std::string& device_id,
        XRTCVideoSelectStrategy strategy =
            XRTCVideoSelectStrategy::kPreferRequested);

    /// 枚举本机摄像头
    static std::vector<XRTCDeviceInfo> get_video_device_info();

    ~VcmCapture() override;

    VcmCapture(const VcmCapture&) = delete;
    VcmCapture& operator=(const VcmCapture&) = delete;

    bool start() override;
    bool stop() override;
    bool device_switch(const std::string& device_id) override;

    /// 当前实际采集格式（可能经设备能力匹配后与请求不同）
    XRTCVideoFormat capture_format() const;

    /// 修改期望采集格式；采集中会按策略重选能力并重启
    bool set_capture_request(const XRTCVideoFormat& requested);

    /**
     * @brief 视频采集数据回调
     * @param frame 视频帧
    */
    void OnFrame(const webrtc::VideoFrame& frame) override;

    /**
     * @brief 设置视频源
     * @param track_source 视频源
    */
    void set_track_source(
        webrtc::scoped_refptr<XrtcVideoTrackSource> track_source);

    /// 按策略重选能力并重启采集（会话中途改分辨率用）
    bool restart(size_t width, size_t height, int fps);

    size_t width() const { return _width; }
    size_t height() const { return _height; }
    int fps() const { return _fps; }
    const std::string& device_id() const { return _device_id; }

private:
    VcmCapture(size_t width, size_t height, int fps,
               const std::string& device_id,
               XRTCVideoSelectStrategy strategy) noexcept;
    bool init(size_t width, size_t height, int fps,
              const std::string& device_id);
    void apply_capability(const webrtc::VideoCaptureCapability& capability);
    void destroy();
    /// 仅释放 VCM，保留 track_source_（换设备用）
    void release_vcm();

    size_t _width;
    size_t _height;
    int _fps;
    std::string _device_id;
    XRTCVideoSelectStrategy _select_strategy;
    webrtc::Thread* _current_thread;
    webrtc::scoped_refptr<webrtc::VideoCaptureModule> _vcm;
    webrtc::VideoCaptureCapability _capability;
    webrtc::scoped_refptr<XrtcVideoTrackSource> track_source_;
};

}  // namespace xrtc
