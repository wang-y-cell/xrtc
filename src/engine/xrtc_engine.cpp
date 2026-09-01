#include <engine/xrtc_engine.h>

#include "api/audio/create_audio_device_module.h"
#include "api/environment/environment_factory.h"
#include "rtc_base/thread.h"
#include <engine/xrtc_global.h>
#include <media/audio_capture.h>
#include <media/vcm_capture.h>
#include <media/video_capability_selector.h>
#include <media/xrtc_audio_device_module.h>
#include <session/call_session.h>
#include <spdlog/spdlog.h>

namespace xrtc {

namespace {

ixrtc_video_config MergeVideoConfig(const ixrtc_video_config& config,
                                    const XRTCVideoCaptureRequest& request) {
    ixrtc_video_config merged = config;
    const bool use_engine_request =
        merged.width <= 0 && merged.height <= 0 && merged.fps <= 0;
    if (merged.width <= 0) {
        merged.width = request.format.width;
    }
    if (merged.height <= 0) {
        merged.height = request.format.height;
    }
    if (merged.fps <= 0) {
        merged.fps = request.format.fps;
    }
    if (use_engine_request) {
        merged.select_strategy = request.strategy;
    }
    return merged;
}

XRTCJoinConfig MergeJoinConfig(const XRTCJoinConfig& config,
                               const XRTCVideoCaptureRequest& request) {
    XRTCJoinConfig merged = config;
    const bool use_engine_request =
        merged.width <= 0 && merged.height <= 0 && merged.fps <= 0;
    if (merged.width <= 0) {
        merged.width = request.format.width;
    }
    if (merged.height <= 0) {
        merged.height = request.format.height;
    }
    if (merged.fps <= 0) {
        merged.fps = request.format.fps;
    }
    if (use_engine_request) {
        merged.select_strategy = request.strategy;
    }
    return merged;
}

}  // namespace

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

XRtcEngine::XRtcEngine() {
    /*
     * 延迟 ADM Init：在 Windows 上过早 Init 麦克风可能干扰同进程
     * libwebsockets 的 select() 连接检测。真正推流前再 Init。
     * XrtcAudioDeviceModule 包装平台 ADM，旁路拷贝采集 PCM 供预览音量。
     */
    auto platform = webrtc::CreateAudioDeviceModule(
        webrtc::CreateEnvironment(),
        webrtc::AudioDeviceModule::kPlatformDefaultAudio);
    audio_device_ = XrtcAudioDeviceModule::Create(std::move(platform));
    if (audio_device_) {
        XRtcGlobal::instance().InitAudioDeviceModule(audio_device_);
    }
}

XRtcEngine::~XRtcEngine() {
    call_session_.reset();
}

std::vector<XRTCDeviceInfo> XRtcEngine::get_video_device_info() {
    return XRtcGlobal::instance().api_thread()->BlockingCall(
        []() -> std::vector<XRTCDeviceInfo> {
            return VcmCapture::get_video_device_info();
        });
}

std::vector<XRTCDeviceInfo> XRtcEngine::get_audio_device_info() {
    return XRtcGlobal::instance().api_thread()->BlockingCall(
        [this]() -> std::vector<XRTCDeviceInfo> {
            return AudioCapture::get_audio_device_info(
                audio_device_ ? audio_device_->platform() : nullptr);
        });
}

std::vector<XRTCVideoFormat> XRtcEngine::get_video_capabilities(
    const std::string& device_id) {
    return XRtcGlobal::instance().api_thread()->BlockingCall(
        [device_id]() -> std::vector<XRTCVideoFormat> {
            std::vector<XRTCVideoFormat> out;
            if (device_id.empty()) {
                return out;
            }
            const auto caps =
                VideoCapabilitySelector::ListCapabilities(device_id);
            out.reserve(caps.size());
            for (const auto& f : caps) {
                out.push_back({f.width, f.height, f.fps});
            }
            return out;
        });
}

XRTCVideoFormat XRtcEngine::select_video_format(
    const std::string& device_id,
    const XRTCVideoFormat& requested,
    XRTCVideoSelectStrategy strategy) {
    return XRtcGlobal::instance().api_thread()->BlockingCall(
        [device_id, requested, strategy]() -> XRTCVideoFormat {
            if (device_id.empty()) {
                return requested;
            }
            const auto matched = VideoCapabilitySelector::Resolve(
                device_id, requested.width, requested.height, requested.fps,
                strategy);
            return {matched.width, matched.height, matched.maxFPS};
        });
}

void XRtcEngine::set_video_capture_request(
    const XRTCVideoCaptureRequest& request) {
    XRtcGlobal::instance().api_thread()->BlockingCall([this, request]() {
        video_capture_request_ = request;
    });
}

XRTCVideoCaptureRequest XRtcEngine::get_video_capture_request() const {
    return XRtcGlobal::instance().api_thread()->BlockingCall(
        [this]() -> XRTCVideoCaptureRequest { return video_capture_request_; });
}

IXRtcMediaSource* XRtcEngine::create_video_source(
    const ixrtc_video_config& video_config
) {
    return XRtcGlobal::instance().api_thread()->BlockingCall(
        [this, video_config]() -> IXRtcMediaSource* {
            const ixrtc_video_config merged =
                MergeVideoConfig(video_config, video_capture_request_);
            auto capture = VcmCapture::Create(
                merged.width, merged.height, merged.fps, merged.device_id,
                merged.select_strategy);
            return capture.release();
        });
}

void XRtcEngine::destroy_video_source(IXRtcMediaSource* video_source) {
    XRtcGlobal::instance().api_thread()->PostTask([video_source]() {
        delete video_source;
    });
}

IXRtcMediaSource* XRtcEngine::create_audio_source(
    const ixrtc_audio_config& audio_config) {
    return XRtcGlobal::instance().api_thread()->BlockingCall(
        [this, audio_config]() -> IXRtcMediaSource* {
            auto capture =
                AudioCapture::Create(audio_device_, audio_config.device_id);
            if (!capture) {
                return nullptr;
            }
            return capture.release();
        });
}

void XRtcEngine::destroy_audio_source(IXRtcMediaSource* audio_source) {
    XRtcGlobal::instance().api_thread()->PostTask([audio_source]() {
        delete audio_source;
    });
}

void XRtcEngine::join(const XRTCJoinConfig& config) {
    const XRTCVideoCaptureRequest request = get_video_capture_request();
    XRTCJoinConfig cfg = MergeJoinConfig(config, request);
    //如果摄像头id为空,使用摄像头列表的第一个摄像头
    if (cfg.video_device_id.empty()) {
        auto devices = get_video_device_info();
        if (!devices.empty()) {
            cfg.video_device_id = devices.front().device_id;
        }
    }
    //如果麦克风id为空,使用麦克风列表的第一个设备
    if (cfg.audio_device_id.empty()) {
        auto devices = get_audio_device_info();
        if (!devices.empty()) {
            cfg.audio_device_id = devices.front().device_id;
        }
    }
    //如果名称为空,使用默认名称
    if (cfg.display_name.empty()) {
        cfg.display_name = "xrtc-user";
    }

    spdlog::info("[engine] join post Start url={} room={} cam={} mic={}",
                 cfg.janus_ws_url, cfg.room_id, cfg.video_device_id,
                 cfg.audio_device_id);
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
    //如果当前线程是api线程,则直接调用stop函数
    if (webrtc::Thread::Current() == XRtcGlobal::instance().api_thread()) {
        stop();
    } else {
        //如果当前线程不是api线程,则将stop函数放入api线程中执行,如果在api线程阻塞调用
        //就会出现死锁
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

void XRtcEngine::start_local_video() {
    if (!call_session_ || !call_session_->active()) {
        spdlog::warn("[engine] start_local_video: not in call");
        return;
    }
    if (!call_session_->StartLocalVideo()) {
        spdlog::error("[engine] start_local_video: capture failed");
        return;
    }
    call_session_->MuteVideo(false);
}

void XRtcEngine::stop_local_video() {
    if (!call_session_) {
        return;
    }
    // 先停采集并推黑帧（轨仍 enable），再 mute，远端才能收到黑帧
    call_session_->StopLocalVideo();
    call_session_->MuteVideo(true);
}

void XRtcEngine::start_local_audio() {
    if (!call_session_ || !call_session_->active()) {
        spdlog::warn("[engine] start_local_audio: not in call");
        return;
    }
    if (!call_session_->StartLocalAudio()) {
        spdlog::error("[engine] start_local_audio: capture failed");
        return;
    }
    call_session_->MuteAudio(false);
}

void XRtcEngine::stop_local_audio() {
    if (!call_session_) {
        return;
    }
    call_session_->MuteAudio(true);
    call_session_->StopLocalAudio();
}

}  // namespace xrtc
