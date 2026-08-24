#pragma once

#include <xrtc/ixrtc_media_source.h>
#include <media/video_track_source.h>

#include <memory>
#include <string>

#include <modules/video_capture/video_capture.h>
#include <rtc_base/thread.h>

namespace xrtc {

class VcmCapture : public IXRtcMediaSource,
                   public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    /** 
     * @brief 创建视频采集模块
     * @param width 视频宽度
     * @param height 视频高度
     * @param fps 视频帧率
     * @param device_id 设备id
     * @return 视频采集模块
    */
    static VcmCapture* create(size_t width, size_t height, int fps,
                              const std::string& device_id);

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

private:
    VcmCapture(size_t width, size_t height, int fps,
               const std::string& device_id) noexcept;
    bool init(size_t width, size_t height, int fps,
              const std::string& device_id);
    void destroy();

    //视频宽度,构造函数初始化 
    size_t _width;
    //视频高度,构造函数初始化 
    size_t _height;
    //视频帧率,构造函数初始化 
    int _fps;
    //设备id,构造函数初始化 
    std::string _device_id;
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
