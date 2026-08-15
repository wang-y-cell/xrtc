#include <engine/xrtc_engine.h>

#include "api/audio/create_audio_device_module.h"
#include "api/environment/environment_factory.h"
#include <media/vcm_capture.h>
#include <session/call_session.h>
#include "modules/video_capture/video_capture_factory.h"
#include <engine/xrtc_global.h>

namespace xrtc {

IXRtcEngine* create_xrtc_engine(XRtcEngineObserver* observer) {
    return XRtcGlobal::instance().api_thread()->BlockingCall(
        [observer]() -> IXRtcEngine* {
            XRtcGlobal::instance().set_observer(observer);
            return new XRtcEngine();
        });
}

void destroy_xrtc_engine(IXRtcEngine* engine) {
    if (!engine) {
        return;
    }
    engine->leave();
    XRtcGlobal::instance().api_thread()->BlockingCall([engine]() {
        delete engine;
    });
}

XRtcEngine::XRtcEngine()
    : video_device(webrtc::VideoCaptureFactory::CreateDeviceInfo()),
      audio_device(webrtc::CreateAudioDeviceModule(
          webrtc::CreateEnvironment(),
          webrtc::AudioDeviceModule::kPlatformDefaultAudio)) {
    /*
     * 延迟 ADM Init：在 Windows 上过早 Init 麦克风可能干扰同进程
     * libwebsockets 的 select() 连接检测。真正推流前再 Init。
     */
    if (audio_device) {
        XRtcGlobal::instance().InitAudioDeviceModule(audio_device);
    }
}

XRtcEngine::~XRtcEngine() {
    call_session_.reset();
}

std::vector<XRTCDeviceInfo> XRtcEngine::get_video_device_info() {
    return XRtcGlobal::instance().api_thread()->BlockingCall(
        [this]() -> std::vector<XRTCDeviceInfo> {
            std::vector<XRTCDeviceInfo> device_info;
            uint32_t total = video_device->NumberOfDevices();
            if (total <= 0) {
                return device_info;
            }

            char id[1024];
            char name[256];
            for (size_t i = 0; i < total; i++) {
                id[0] = '\0';
                name[0] = '\0';
                if (video_device->GetDeviceName(i, name, sizeof(name), id,
                                               sizeof(id)) == 0) {
                    XRTCDeviceInfo info;
                    info.device_name = name;
                    info.device_id = id;
                    device_info.push_back(std::move(info));
                }
            }
            return device_info;
        });
}

std::vector<XRTCDeviceInfo> XRtcEngine::get_audio_device_info() {
    return XRtcGlobal::instance().api_thread()->BlockingCall(
        [this]() -> std::vector<XRTCDeviceInfo> {
            std::vector<XRTCDeviceInfo> device_info;
            if (!audio_device) {
                return device_info;
            }
            // 必须先 Init，否则 RecordingDevices() 可能返回垃圾值导致狂打日志
            if (audio_device->Init() != 0) {
                return device_info;
            }
            const int16_t total = audio_device->RecordingDevices();
            if (total <= 0) {
                return device_info;
            }
            // 防御：异常总数直接丢弃
            const int count = (total > 32) ? 0 : static_cast<int>(total);
            char id[128];
            char name[128];
            for (int i = 0; i < count; ++i) {
                id[0] = '\0';
                name[0] = '\0';
                if (audio_device->RecordingDeviceName(i, name, id) == 0) {
                    XRTCDeviceInfo info;
                    info.device_name = name;
                    info.device_id = id;
                    device_info.push_back(std::move(info));
                }
            }
            return device_info;
        });
}

IXRtcMediaSource* XRtcEngine::create_video_source(
    const ixrtc_video_config& video_config) {
    return XRtcGlobal::instance().api_thread()->BlockingCall(
        [video_config]() -> IXRtcMediaSource* {
            return VcmCapture::create(video_config.width, video_config.height,
                                      video_config.fps, video_config.device_id);
        });
}

void XRtcEngine::destroy_video_source(IXRtcMediaSource* video_source) {
    XRtcGlobal::instance().api_thread()->PostTask([video_source]() {
        delete video_source;
    });
}

void XRtcEngine::join(const XRTCJoinConfig& config) {
    XRTCJoinConfig cfg = config;
    //如果摄像头id为空,使用摄像头列表的第一个摄像头
    if (cfg.video_device_id.empty()) {
        auto devices = get_video_device_info();
        if (!devices.empty()) {
            cfg.video_device_id = devices.front().device_id;
        }
    }
    //如果名称为空,使用默认名称
    if (cfg.display_name.empty()) {
        cfg.display_name = "xrtc-user";
    }

    //异步调用Start()
    XRtcGlobal::instance().api_thread()->PostTask([this, cfg]() {
        if (!call_session_) {
            call_session_ = std::make_unique<CallSession>();
        }
        call_session_->Start(cfg);
    });
}

void XRtcEngine::leave() {
    auto stop = [this]() {
        if (call_session_) {
            call_session_->Stop();
        }
    };
    if (webrtc::Thread::Current() == XRtcGlobal::instance().api_thread()) {
        stop();
    } else {
        XRtcGlobal::instance().api_thread()->BlockingCall(stop);
    }
}

void XRtcEngine::mute_audio(bool mute) {
    if (call_session_) {
        call_session_->MuteAudio(mute);
    }
}

void XRtcEngine::mute_video(bool mute) {
    if (call_session_) {
        call_session_->MuteVideo(mute);
    }
}

}  // namespace xrtc
