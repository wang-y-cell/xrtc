#pragma once

#include <xrtc/ixrtc_media_source.h>
#include <xrtc/xrtc_defines.h>
#include <media/video_track_source.h>

#include <memory>
#include <string>

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
    static VcmCapture* create(
        size_t width,
        size_t height,
        int fps,
        const std::string& device_id,
        XRTCVideoSelectStrategy strategy =
            XRTCVideoSelectStrategy::kPreferRequested);

    ~VcmCapture() override;

    /**
     * @brief 开始视频采集,会打开设置id的设备(摄像头),并将每一帧传入onframe中
     * @return 是否成功
    */
    bool start() override;
    /**
     * @brief 停止视频采集
     * @return 是否成功
    */
    bool stop() override;

    XRTCVideoFormat capture_format() const override;
    bool set_capture_request(const XRTCVideoFormat& requested) override;

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

    /// 按策略重选能力并重启采集（会话中途改分辨率用；不影响公开 API）
    bool restart(size_t width, size_t height, int fps);

    size_t width() const { return _width; }
    size_t height() const { return _height; }
    int fps() const { return _fps; }

private:
    VcmCapture(size_t width, size_t height, int fps,
               const std::string& device_id,
               XRTCVideoSelectStrategy strategy) noexcept;
    bool init(size_t width, size_t height, int fps,
              const std::string& device_id);
    void apply_capability(const webrtc::VideoCaptureCapability& capability);
    void destroy();

    //视频宽度,构造函数初始化 
    size_t _width;
    //视频高度,构造函数初始化 
    size_t _height;
    //视频帧率,构造函数初始化 
    int _fps;
    //设备id,构造函数初始化 
    std::string _device_id;
    XRTCVideoSelectStrategy _select_strategy;
    //当前线程,构造函数初始化 
    webrtc::Thread* _current_thread;
    /**
     * @brief 在init函数中创建,根据设备id创建视频采集模块,画面返回在OnFrame方法中,
     * 当调用start函数时,会开始采集视频,并回调OnFrame方法
    */
    webrtc::scoped_refptr<webrtc::VideoCaptureModule> _vcm;
    //视频采集能力,构造函数初始化, 在init函数中获取视频采集设备信息,并设置视频采集能力
    webrtc::VideoCaptureCapability _capability;
    /**
     * @brief 视频源,在set_track_source函数中创建
     * 负责处理OnFrame中的视频帧,并推入PeerConnection的本地视频源
    */
    webrtc::scoped_refptr<XrtcVideoTrackSource> track_source_;
};

}  // namespace xrtc
