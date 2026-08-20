#include <media/vcm_capture.h>

#include <modules/video_capture/video_capture_factory.h>
#include <rtc_base/logging.h>

#include "api/video/i420_buffer.h"
#include <engine/xrtc_global.h>
#include <xrtc/ixrtc_engine.h>
#include "libyuv/convert_argb.h"
#include <xrtc/xrtc_log.h>

namespace xrtc {

VcmCapture::VcmCapture(size_t width, size_t height, int fps,
                       const std::string& device_id) noexcept
    : _width(width),
      _height(height),
      _fps(fps),
      _device_id(device_id),
      _current_thread(webrtc::Thread::Current()) {}

VcmCapture::~VcmCapture() {
    destroy();
}

VcmCapture* VcmCapture::create(size_t width, size_t height, int fps,
                               const std::string& device_id) {
    VcmCapture* capture = new VcmCapture(width, height, fps, device_id);
    if (capture && capture->init(width, height, fps, device_id)) {
        return capture;
    }
    delete capture;
    return nullptr;
}

void VcmCapture::set_track_source(
    webrtc::scoped_refptr<XrtcVideoTrackSource> track_source
) {
    track_source_ = std::move(track_source);
    if (track_source_) {
        track_source_->SetLive(true);
    }
}

bool VcmCapture::start() {
    _current_thread->PostTask([this]() {
        XRtcError error = XRtcError::kNOERROR;
        do {
            if (!_vcm) {
                spdlog::error("视频采集模块未初始化");
                error = XRtcError::kVideoSourceNotInit;
                break;
            }
            if (_vcm->StartCapture(_capability) != 0) {
                spdlog::warn("开始采集失败, device_id: {}", _vcm->CurrentDeviceName());
                error = XRtcError::kVideoSourceStartFailed;
                break;
            }
            spdlog::debug("开始采集成功, device_id: {}", _vcm->CurrentDeviceName());
        } while (false);

        XRtcEngineObserver* observer = XRtcGlobal::instance().observer();
        if (observer) {
            observer->video_source_start_event(this, error);
        }
    });
    return true;
}

bool VcmCapture::stop() {
    _current_thread->PostTask([this]() {
        XRtcError error = XRtcError::kNOERROR;
        do {
            if (!_vcm) {
                spdlog::error("视频采集模块未初始化");
                error = XRtcError::kVideoSourceNotInit;
                break;
            }
            _vcm->StopCapture();
        } while (false);

        if (track_source_) {
            track_source_->SetLive(false);
        }

        XRtcEngineObserver* observer = XRtcGlobal::instance().observer();
        if (observer) {
            observer->video_source_stop_event(this, error);
        }
    });
    return true;
}

/**
 * @brief 初始化视频采集模块
 * @param width 视频宽度
 * @param height 视频高度
 * @param fps 视频帧率
 * @param device_id 设备id
 * @return 是否成功
*/
bool VcmCapture::init(size_t width, size_t height, int fps,
                      const std::string& device_id) {
    //根据设备id创建视频采集模块
    _vcm = webrtc::VideoCaptureFactory::Create(device_id.c_str());
    if (!_vcm) {
        spdlog::error("创建视频采集模块失败, device_id: {}", device_id);
        return false;
    }

    //注册视频采集数据回调,当有视频数据时，会回调OnFrame方法
    _vcm->RegisterCaptureDataCallback(this);

    //获取视频采集设备信息
    std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> device_info(
        webrtc::VideoCaptureFactory::CreateDeviceInfo());
    //获取视频采集设备信息,并设置视频采集能力
    device_info->GetCapability(_vcm->CurrentDeviceName(), 0, _capability);
    _capability.width = static_cast<int32_t>(width);
    _capability.height = static_cast<int32_t>(height);
    _capability.maxFPS = fps;
    _capability.videoType = webrtc::VideoType::kI420;

    return true;
}

void VcmCapture::OnFrame(const webrtc::VideoFrame& frame) {
    if (track_source_) {
        track_source_->PushFrame(frame);
    }

    XRtcEngineObserver* observer = XRtcGlobal::instance().observer();
    if (!observer) {
        return;
    }

    webrtc::scoped_refptr<webrtc::I420BufferInterface> buffer(
        frame.video_frame_buffer()->ToI420());
    if (!buffer) {
        return;
    }

    if (frame.rotation() != webrtc::kVideoRotation_0) {
        buffer = webrtc::I420Buffer::Rotate(*buffer, frame.rotation());
    }

    const int width = buffer->width();
    const int height = buffer->height();
    if (width <= 0 || height <= 0) {
        return;
    }

    auto argb = std::make_shared<std::vector<uint8_t>>(
        static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    libyuv::I420ToARGB(buffer->DataY(), buffer->StrideY(), buffer->DataU(),
                       buffer->StrideU(), buffer->DataV(), buffer->StrideV(),
                       argb->data(), width * 4, width, height);

    XRTCVideoFrame video_frame;
    video_frame.width = width;
    video_frame.height = height;
    video_frame.argb = std::move(argb);
    observer->on_video_frame(this, video_frame);
}

void VcmCapture::destroy() {
    if (_vcm) {
        _vcm->StopCapture();
        _vcm->DeRegisterCaptureDataCallback();
        _vcm = nullptr;
    }
    track_source_ = nullptr;
}

}  // namespace xrtc
