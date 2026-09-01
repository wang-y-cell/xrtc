#include <media/audio_capture.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

#include "api/audio/audio_device.h"
#include "api/peer_connection_interface.h"
#include "rtc_base/thread.h"
#include <engine/xrtc_global.h>
#include <spdlog/spdlog.h>
#include <xrtc/ixrtc_engine.h>

namespace xrtc {
namespace {

///获得当前时间戳，单位为毫秒
int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

///把一帧 16-bit PCM 转成 0～100 的音量值，给进度条用
int LevelFromPcm16(const int16_t* samples, size_t count) {
    //空数据返回0
    if (!samples || count == 0) {
        return 0;
    }
    double sum_sq = 0.0;
    //峰值
    int peak = 0;
    //遍历这一帧的每个采样点，算出峰值和平方和
    for (size_t i = 0; i < count; ++i) {
        const int v = samples[i];
        const int a = v >= 0 ? v : -v;
        if (a > peak) {
            peak = a;
        }
        //平方和
        sum_sq += static_cast<double>(v) * static_cast<double>(v);
    }
    //均方根
    const double rms = std::sqrt(sum_sq / static_cast<double>(count));
    //混合音量,多看平均响度，也兼顾瞬间大声
    const double mixed = 0.7 * rms + 0.3 * static_cast<double>(peak);
    //转换为0～100的音量值
    // 32768 是 16-bit 的峰值，1.8 是放大系数，让音量看起来更明显
    //clamp 是限制在0～100之间
    return std::clamp(static_cast<int>(mixed / 32768.0 * 100.0 * 1.8), 0, 100);
}

webrtc::Thread* AdmWorker() {
    return XRtcGlobal::instance().worker_thread();
}

webrtc::Thread* ApiThread() {
    return XRtcGlobal::instance().api_thread();
}

}  // namespace

std::unique_ptr<AudioCapture> AudioCapture::Create(
    webrtc::scoped_refptr<XrtcAudioDeviceModule> adm,
    const std::string& device_id) {
    if (!adm) {
        spdlog::error("[audio-cap] Create failed: adm null");
        return nullptr;
    }
    auto capture =
        std::unique_ptr<AudioCapture>(new AudioCapture(std::move(adm), device_id));
    if (!device_id.empty() && !capture->apply_device(device_id)) {
        spdlog::error("[audio-cap] apply_device failed: {}", device_id);
        return nullptr;
    }
    return capture;
}

AudioCapture::AudioCapture(webrtc::scoped_refptr<XrtcAudioDeviceModule> adm,
                           std::string device_id)
    : adm_(std::move(adm)), device_id_(std::move(device_id)) {}

AudioCapture::~AudioCapture() {
    stop();
}

std::vector<XRTCDeviceInfo> AudioCapture::get_audio_device_info(
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> platform_adm) {
    std::vector<XRTCDeviceInfo> out;
    if (!platform_adm) {
        return out;
    }
    if (platform_adm->Init() != 0) {
        spdlog::error("[audio-cap] platform ADM Init failed while listing");
    }
    const int16_t total = platform_adm->RecordingDevices();
    if (total <= 0) {
        return out;
    }
    const int count = (total > 32) ? 0 : static_cast<int>(total);
    char id[webrtc::kAdmMaxGuidSize];
    char name[webrtc::kAdmMaxDeviceNameSize];
    for (int i = 0; i < count; ++i) {
        id[0] = '\0';
        name[0] = '\0';
        if (platform_adm->RecordingDeviceName(static_cast<uint16_t>(i), name,
                                              id) != 0) {
            continue;
        }
        XRTCDeviceInfo info;
        info.device_name = name;
        info.device_id = id;
        out.push_back(std::move(info));
    }
    return out;
}

std::vector<XRTCDeviceInfo> AudioCapture::get_playout_device_info(
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> platform_adm) {
    std::vector<XRTCDeviceInfo> out;
    if (!platform_adm) {
        return out;
    }
    if (platform_adm->Init() != 0) {
        spdlog::error("[audio-cap] platform ADM Init failed while listing playout");
    }
    const int16_t total = platform_adm->PlayoutDevices();
    if (total <= 0) {
        return out;
    }
    const int count = (total > 32) ? 0 : static_cast<int>(total);
    char id[webrtc::kAdmMaxGuidSize];
    char name[webrtc::kAdmMaxDeviceNameSize];
    for (int i = 0; i < count; ++i) {
        id[0] = '\0';
        name[0] = '\0';
        if (platform_adm->PlayoutDeviceName(static_cast<uint16_t>(i), name,
                                            id) != 0) {
            continue;
        }
        XRTCDeviceInfo info;
        info.device_name = name;
        info.device_id = id;
        out.push_back(std::move(info));
    }
    return out;
}

bool AudioCapture::set_playout_device(
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm,
    const std::string& device_id) {
    if (!adm) {
        return false;
    }
    auto* worker = AdmWorker();
    auto run = [adm, device_id]() -> bool {
        if (adm->Init() != 0) {
            spdlog::error("[audio-cap] ADM Init failed in set_playout_device");
            return false;
        }
        const int16_t total = adm->PlayoutDevices();
        if (total <= 0) {
            spdlog::error("[audio-cap] no playout devices");
            return false;
        }
        const int count = (total > 32) ? 0 : static_cast<int>(total);
        if (count <= 0) {
            return false;
        }

        int index = 0;
        if (!device_id.empty()) {
            index = -1;
            char id[webrtc::kAdmMaxGuidSize];
            char name[webrtc::kAdmMaxDeviceNameSize];
            for (int i = 0; i < count; ++i) {
                id[0] = '\0';
                name[0] = '\0';
                if (adm->PlayoutDeviceName(static_cast<uint16_t>(i), name, id) !=
                    0) {
                    continue;
                }
                if (device_id == id || device_id == name) {
                    index = i;
                    break;
                }
            }
            if (index < 0) {
                spdlog::error("[audio-cap] playout device not found: {}",
                              device_id);
                return false;
            }
        }

        const bool was_playing = adm->Playing();
        if (was_playing) {
            adm->StopPlayout();
        }
        if (adm->SetPlayoutDevice(static_cast<uint16_t>(index)) != 0) {
            spdlog::error("[audio-cap] SetPlayoutDevice({}) failed", index);
            return false;
        }
        if (adm->InitSpeaker() != 0) {
            spdlog::warn("[audio-cap] InitSpeaker failed after SetPlayoutDevice");
        }
        if (was_playing) {
            if (adm->InitPlayout() != 0) {
                spdlog::error("[audio-cap] InitPlayout failed after device switch");
                return false;
            }
            if (adm->StartPlayout() != 0) {
                spdlog::error("[audio-cap] StartPlayout failed after device switch");
                return false;
            }
        }
        spdlog::info("[audio-cap] playout device index={} id={}", index,
                     device_id.empty() ? "(default/first)" : device_id);
        return true;
    };

    if (worker && webrtc::Thread::Current() != worker) {
        return worker->BlockingCall(run);
    }
    return run();
}

bool AudioCapture::ensure_init() {
    if (!adm_) {
        return false;
    }
    if (adm_->Init() != 0) {
        spdlog::error("[audio-cap] ADM Init failed");
        return false;
    }
    return true;
}

int AudioCapture::resolve_index(const std::string& device_id) const {
    if (!adm_) {
        return -1;
    }
    const int16_t total = adm_->RecordingDevices();
    if (total <= 0) {
        return -1;
    }
    const int count = (total > 32) ? 0 : static_cast<int>(total);
    if (count <= 0) {
        return -1;
    }
    if (device_id.empty()) {
        return 0;
    }
    char id[webrtc::kAdmMaxGuidSize];
    char name[webrtc::kAdmMaxDeviceNameSize];
    for (int i = 0; i < count; ++i) {
        id[0] = '\0';
        name[0] = '\0';
        if (adm_->RecordingDeviceName(static_cast<uint16_t>(i), name, id) != 0) {
            continue;
        }
        if (device_id == id || device_id == name) {
            return i;
        }
    }
    spdlog::warn("[audio-cap] device_id not found: {}", device_id);
    return -1;
}

bool AudioCapture::apply_device(const std::string& device_id) {
    auto* worker = AdmWorker();
    if (worker && webrtc::Thread::Current() != worker) {
        return worker->BlockingCall(
            [this, device_id]() { return apply_device(device_id); });
    }
    if (!ensure_init()) {
        return false;
    }
    const int index = resolve_index(device_id);
    if (index < 0) {
        return false;
    }
    if (adm_->SetRecordingDevice(static_cast<uint16_t>(index)) != 0) {
        spdlog::error("[audio-cap] SetRecordingDevice({}) failed", index);
        return false;
    }
    device_index_ = index;
    if (!device_id.empty()) {
        device_id_ = device_id;
    } else {
        char id[webrtc::kAdmMaxGuidSize];
        char name[webrtc::kAdmMaxDeviceNameSize];
        id[0] = '\0';
        name[0] = '\0';
        if (adm_->RecordingDeviceName(static_cast<uint16_t>(index), name, id) ==
            0) {
            device_id_ = id;
        }
    }
    spdlog::info("[audio-cap] selected mic index={} id={}", device_index_,
                 device_id_);
    return true;
}

bool AudioCapture::open_on_worker() {
    if (!adm_) {
        return false;
    }
    if (device_index_ < 0 && !apply_device(device_id_)) {
        return false;
    }
    if (adm_->InitMicrophone() != 0) {
        spdlog::error("[audio-cap] InitMicrophone failed");
        return false;
    }
    // 旁路须在录音前挂好；进房时 VoiceEngine Register 与此兼容
    if (adm_->EnsurePlatformAudioCallback() != 0) {
        spdlog::error("[audio-cap] EnsurePlatformAudioCallback failed");
        return false;
    }
    adm_->SetCaptureListener(this);
    level_tapping_ = true;
    opened_ = true;
    spdlog::info("[audio-cap] opened (tap only) device={}", device_id_);
    return true;
}

bool AudioCapture::start_recording_on_worker() {
    if (!adm_) {
        return false;
    }
    if (!opened_ && !open_on_worker()) {
        return false;
    }
    adm_->SetCaptureEnabled(true);
    if (adm_->Recording()) {
        started_ = true;
        return true;
    }
    if (adm_->InitRecording() != 0) {
        spdlog::error("[audio-cap] InitRecording failed");
        return false;
    }
    if (adm_->StartRecording() != 0) {
        spdlog::error("[audio-cap] StartRecording failed");
        return false;
    }
    started_ = true;
    spdlog::info("[audio-cap] started (WebRTC ADM) device={}", device_id_);
    return true;
}

bool AudioCapture::stop_on_worker() {
    if (!adm_) {
        opened_ = false;
        started_ = false;
        level_tapping_ = false;
        return true;
    }
    adm_->SetCaptureListener(nullptr);
    level_tapping_ = false;
    adm_->SetCaptureEnabled(false);
    if (adm_->Recording()) {
        adm_->StopRecording();
    }
    started_ = false;
    opened_ = false;
    spdlog::info("[audio-cap] stopped");
    return true;
}

bool AudioCapture::StopHardwareRecording() {
    // ADM 启停须在 worker；勿再绕 api，避免与 session 同线程死锁
    auto* worker = AdmWorker();
    auto run = [this]() -> bool {
        if (!adm_) {
            started_ = false;
            return true;
        }
        adm_->SetCaptureEnabled(false);
        if (adm_->Recording()) {
            adm_->StopRecording();
        }
        started_ = false;
        spdlog::info("[audio-cap] hardware recording stopped (keep open)");
        return true;
    };
    if (worker && webrtc::Thread::Current() != worker) {
        return worker->BlockingCall(run);
    }
    return run();
}

bool AudioCapture::open() {
    auto* api = ApiThread();
    if (api && webrtc::Thread::Current() != api) {
        return api->BlockingCall([this]() { return open(); });
    }
    if (opened_) {
        return true;
    }
    if (!adm_) {
        return false;
    }
    if (!XRtcGlobal::instance().GetOrCreatePeerConnectionFactory()) {
        spdlog::error("[audio-cap] PeerConnectionFactory unavailable");
        return false;
    }
    auto* worker = AdmWorker();
    if (worker) {
        return worker->BlockingCall([this]() { return open_on_worker(); });
    }
    return open_on_worker();
}

bool AudioCapture::start() {
    auto* api = ApiThread();
    if (api && webrtc::Thread::Current() != api) {
        return api->BlockingCall([this]() { return start(); });
    }
    //如果当前的麦克风在跑了,就返回
    if (started_ && adm_ && adm_->Recording()) {
        return true;
    }
    if (!adm_) {
        return false;
    }
    if (!XRtcGlobal::instance().GetOrCreatePeerConnectionFactory()) {
        spdlog::error("[audio-cap] PeerConnectionFactory unavailable");
        return false;
    }
    auto* worker = AdmWorker();
    if (worker) {
        return worker->BlockingCall(
            [this]() { return start_recording_on_worker(); });
    }
    return start_recording_on_worker();
}

bool AudioCapture::stop() {
    auto* api = ApiThread();
    if (api && webrtc::Thread::Current() != api) {
        return api->BlockingCall([this]() { return stop(); });
    }
    auto* worker = AdmWorker();
    if (worker && webrtc::Thread::Current() != worker) {
        return worker->BlockingCall([this]() { return stop_on_worker(); });
    }
    return stop_on_worker();
}

bool AudioCapture::device_switch(const std::string& device_id) {
    auto* api = ApiThread();
    if (api && webrtc::Thread::Current() != api) {
        return api->BlockingCall(
            [this, device_id]() { return device_switch(device_id); });
    }
    const bool was_recording = started_;
    const bool was_open = opened_;
    if (was_recording || was_open) {
        stop();
    }
    if (!apply_device(device_id)) {
        return false;
    }
    if (was_recording) {
        return start();
    }
    if (was_open) {
        return open();
    }
    return true;
}

void AudioCapture::OnAdmCaptureData(const void* audio_samples,
                                    size_t num_samples,
                                    size_t bytes_per_sample,
                                    size_t num_channels,
                                    uint32_t /*sample_rate_hz*/) {
    if (!audio_samples || num_samples == 0) {
        return;
    }
    if (num_channels == 0) {
        num_channels = 1;
    }

    size_t total = 0;
    //这一帧里一共有多少个 int16 采样点（total），好拿去算音量
    if (bytes_per_sample == sizeof(int16_t) ||
        bytes_per_sample == sizeof(int16_t) * num_channels) {
        total = num_samples * num_channels;
    } else {
        total = num_samples;
    }
    if (total == 0) {
        return;
    }

    //每 50ms 计算一次音量,避免频繁计算
    const int64_t now = NowMs();
    if (now - last_level_ms_ < 50) {
        return;
    }
    last_level_ms_ = now;

    const auto* src = static_cast<const int16_t*>(audio_samples);
    const int level = LevelFromPcm16(src, total);
    if (auto* obs = XRtcGlobal::instance().observer()) {
        obs->on_audio_level(this, level);
    }
}

}  // namespace xrtc
