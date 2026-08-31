#include <media/vcm_capture.h>

#include <modules/video_capture/video_capture_factory.h>
#include <rtc_base/logging.h>

#include "api/video/i420_buffer.h"
#include <engine/xrtc_global.h>
#include <media/video_capability_selector.h>
#include <xrtc/ixrtc_engine.h>
#include "libyuv/convert_argb.h"
#include <spdlog/spdlog.h>

namespace xrtc {

VcmCapture::VcmCapture(size_t width, size_t height, int fps,
                       const std::string& device_id,
                       XRTCVideoSelectStrategy strategy) noexcept
    : _width(width),
      _height(height),
      _fps(fps),
      _device_id(device_id),
      _select_strategy(strategy),
      _current_thread(webrtc::Thread::Current()) {}

VcmCapture::~VcmCapture() {
    destroy();
}

std::unique_ptr<VcmCapture> VcmCapture::Create(
    size_t width,
    size_t height,
    int fps,
    const std::string& device_id,
    XRTCVideoSelectStrategy strategy) {
    std::unique_ptr<VcmCapture> capture(
        new VcmCapture(width, height, fps, device_id, strategy));
    if (capture && capture->init(width, height, fps, device_id)) {
        return capture;
    }
    return nullptr;
}

std::vector<XRTCDeviceInfo> VcmCapture::get_video_device_info() {
    std::vector<XRTCDeviceInfo> device_info;
    std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> info(
        webrtc::VideoCaptureFactory::CreateDeviceInfo());
    if (!info) {
        spdlog::warn("[vcm] CreateDeviceInfo failed");
        return device_info;
    }
    const uint32_t total = info->NumberOfDevices();
    if (total == 0) {
        spdlog::warn("没有找到摄像头设备");
        return device_info;
    }
    char id[1024];
    char name[256];
    for (uint32_t i = 0; i < total; ++i) {
        id[0] = '\0';
        name[0] = '\0';
        if (info->GetDeviceName(i, name, sizeof(name), id, sizeof(id)) == 0) {
            XRTCDeviceInfo item;
            item.device_name = name;
            item.device_id = id;
            device_info.push_back(std::move(item));
        }
    }
    return device_info;
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
    auto do_start = [this]() {
        XRtcError error = XRtcError::kNOERROR;
        do {
            if (!_vcm) {
                spdlog::error("视频采集模块未初始化");
                error = XRtcError::kVideoSourceNotInit;
                break;
            }
            if (_vcm->StartCapture(_capability) != 0) {
                spdlog::warn("开始采集失败, device_id: {}",
                             _vcm->CurrentDeviceName());
                error = XRtcError::kVideoSourceStartFailed;
                break;
            }
            spdlog::debug("开始采集成功, device_id: {}",
                          _vcm->CurrentDeviceName());
        } while (false);

        XRtcEngineObserver* observer = XRtcGlobal::instance().observer();
        if (observer) {
            observer->video_source_start_event(this, error);
        }
    };

  // 已在采集线程则同步执行，避免调用方紧接着 delete 时任务尚未运行。
    if (_current_thread->IsCurrent()) {
        do_start();
    } else {
        _current_thread->BlockingCall(do_start);
    }
    return true;
}

bool VcmCapture::stop() {
    auto do_stop = [this]() {
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
    };

    // 已在采集线程则同步执行，避免调用方紧接着 delete 时任务尚未运行。
    if (_current_thread->IsCurrent()) {
        do_stop();
    } else {
        _current_thread->BlockingCall(do_stop);
    }
    return true;
}

void VcmCapture::apply_capability(
    const webrtc::VideoCaptureCapability& capability) {
    _capability = capability;
    _width = static_cast<size_t>(_capability.width);
    _height = static_cast<size_t>(_capability.height);
    _fps = _capability.maxFPS;
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
    _vcm = webrtc::VideoCaptureFactory::Create(device_id.c_str());
    if (!_vcm) {
        spdlog::error("创建视频采集模块失败, device_id: {}", device_id);
        return false;
    }

    _vcm->RegisterCaptureDataCallback(this);

    // 用策略在设备真实能力中选出最终格式，避免硬写不支持的分辨率导致 StartCapture 失败
    const char* uid = _vcm->CurrentDeviceName();
    const std::string resolve_id =
        (uid && uid[0] != '\0') ? std::string(uid) : device_id;
    apply_capability(VideoCapabilitySelector::Resolve(
        resolve_id, static_cast<int>(width), static_cast<int>(height), fps,
        _select_strategy));
    return true;
}

bool VcmCapture::restart(size_t width, size_t height, int fps) {
    auto do_restart = [this, width, height, fps]() -> bool {
        if (!_vcm) {
            spdlog::error("视频采集模块未初始化, 无法 restart");
            return false;
        }

        const bool was_started = _vcm->CaptureStarted();
        if (was_started) {
            _vcm->StopCapture();
        }

        const char* uid = _vcm->CurrentDeviceName();
        const std::string resolve_id =
            (uid && uid[0] != '\0') ? std::string(uid) : _device_id;
        apply_capability(VideoCapabilitySelector::Resolve(
            resolve_id, static_cast<int>(width), static_cast<int>(height), fps,
            _select_strategy));

        if (!was_started) {
            return true;
        }
        if (_vcm->StartCapture(_capability) != 0) {
            spdlog::warn("restart 后 StartCapture 失败, device_id: {}",
                         resolve_id);
            return false;
        }
        return true;
    };

    if (_current_thread->IsCurrent()) {
        return do_restart();
    }
    return _current_thread->BlockingCall(do_restart);
}

bool VcmCapture::device_switch(const std::string& device_id) {
    if (device_id.empty()) {
        spdlog::warn("[vcm] device_switch ignored: empty device_id");
        return false;
    }
    auto do_switch = [this, device_id]() -> bool {
        if (device_id == _device_id && _vcm) {
            return true;
        }
        const bool was_started = _vcm && _vcm->CaptureStarted();
        release_vcm();
        if (!init(_width, _height, _fps, device_id)) {
            spdlog::error("[vcm] device_switch init failed: {}", device_id);
            return false;
        }
        _device_id = device_id;
        if (!was_started) {
            return true;
        }
        if (_vcm->StartCapture(_capability) != 0) {
            spdlog::warn("[vcm] device_switch StartCapture failed: {}",
                         device_id);
            return false;
        }
        spdlog::info("[vcm] switched camera to {}", device_id);
        return true;
    };

    if (_current_thread->IsCurrent()) {
        return do_switch();
    }
    return _current_thread->BlockingCall(do_switch);
}

XRTCVideoFormat VcmCapture::capture_format() const {
    return {static_cast<int>(_width), static_cast<int>(_height), _fps};
}

bool VcmCapture::set_capture_request(const XRTCVideoFormat& requested) {
    if (requested.width <= 0 || requested.height <= 0 || requested.fps <= 0) {
        spdlog::warn("set_capture_request ignored: invalid {}x{}@{}",
                     requested.width, requested.height, requested.fps);
        return false;
    }
    return restart(static_cast<size_t>(requested.width),
                   static_cast<size_t>(requested.height), requested.fps);
}

void VcmCapture::OnFrame(const webrtc::VideoFrame& frame) {
    //如果有track_source,将视频帧推入他
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

void VcmCapture::release_vcm() {
    if (_vcm) {
        _vcm->StopCapture();
        _vcm->DeRegisterCaptureDataCallback();
        _vcm = nullptr;
    }
}

void VcmCapture::destroy() {
    auto do_destroy = [this]() {
        release_vcm();
        track_source_ = nullptr;
    };

    if (_current_thread->IsCurrent()) {
        do_destroy();
    } else {
        _current_thread->BlockingCall(do_destroy);
    }
}

}  // namespace xrtc
