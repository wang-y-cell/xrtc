#include <media/xrtc_audio_device_module.h>

#include "api/make_ref_counted.h"
#include <media/audio_capture.h>
#include <spdlog/spdlog.h>

namespace xrtc {

webrtc::scoped_refptr<XrtcAudioDeviceModule> XrtcAudioDeviceModule::Create(
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> platform) {
    if (!platform) {
        return nullptr;
    }
    return webrtc::make_ref_counted<XrtcAudioDeviceModule>(std::move(platform));
}

XrtcAudioDeviceModule::XrtcAudioDeviceModule(
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> platform)
    : platform_(std::move(platform)),
      tap_(std::make_shared<AudioCaptureTapState>()) {}

void XrtcAudioDeviceModule::SetCaptureListener(
    AudioCapture* listener,
    std::shared_ptr<std::atomic<bool>> listener_alive) {
    std::lock_guard<std::mutex> lock(tap_->mutex);
    if (tap_->listener_alive) {
        tap_->listener_alive->store(false, std::memory_order_release);
    }
    tap_->listener = listener;
    tap_->listener_alive = std::move(listener_alive);
    if (tap_->listener && tap_->listener_alive) {
        tap_->listener_alive->store(true, std::memory_order_release);
    }
}

void XrtcAudioDeviceModule::SetCaptureEnabled(bool enabled) {
    capture_enabled_.store(enabled);
    if (!enabled) {
        if (platform_ && platform_->Recording()) {
            platform_->StopRecording();
        }
        return;
    }
    // 开麦：补上此前被吞掉的 WebRTC StartRecording
    if (pending_start_.load() && platform_ && !platform_->Recording()) {
        EnsurePlatformAudioCallback();
        const int32_t r = platform_->StartRecording();
        if (r == 0) {
            pending_start_.store(false);
            spdlog::info("[xrtc-adm] pending StartRecording applied");
        } else {
            spdlog::error("[xrtc-adm] pending StartRecording failed: {}", r);
        }
    }
}

int32_t XrtcAudioDeviceModule::EnsurePlatformAudioCallback() {
    // 已安装则跳过；录音中再次 Register 会失败（AudioDeviceBuffer 限制）
    if (platform_callback_installed_) {
        return 0;
    }
    const int32_t r = platform_->RegisterAudioCallback(this);
    if (r == 0) {
        platform_callback_installed_ = true;
    } else {
        spdlog::error("[xrtc-adm] EnsurePlatformAudioCallback failed: {}", r);
    }
    return r;
}

int32_t XrtcAudioDeviceModule::RegisterAudioCallback(
    webrtc::AudioTransport* audioCallback) {
    {
        std::lock_guard<std::mutex> lock(transport_mutex_);
        webrtc_transport_ = audioCallback;
    }

    if (!audioCallback) {
        // VoiceEngine Terminate：若仍有预览 listener，保持本类回调
        AudioCapture* listener = nullptr;
        {
            std::lock_guard<std::mutex> lock(tap_->mutex);
            listener = tap_->listener;
        }
        if (!listener) {
            platform_callback_installed_ = false;
            return platform_->RegisterAudioCallback(nullptr);
        }
        return EnsurePlatformAudioCallback();
    }

    // 始终让平台回调本类，再转发到 WebRTC AudioTransport
    const int32_t r = platform_->RegisterAudioCallback(this);
    if (r == 0) {
        platform_callback_installed_ = true;
    } else if (platform_callback_installed_) {
        // 预览已在录音时，平台拒绝改回调；本类已挂上，仍可转发
        return 0;
    }
    return r;
}

int32_t XrtcAudioDeviceModule::Init() {
    return platform_->Init();
}

int32_t XrtcAudioDeviceModule::Terminate() {
    return platform_->Terminate();
}

bool XrtcAudioDeviceModule::Initialized() const {
    return platform_->Initialized();
}

int16_t XrtcAudioDeviceModule::PlayoutDevices() {
    return platform_->PlayoutDevices();
}

int16_t XrtcAudioDeviceModule::RecordingDevices() {
    return platform_->RecordingDevices();
}

int32_t XrtcAudioDeviceModule::PlayoutDeviceName(
    uint16_t index,
    char name[webrtc::kAdmMaxDeviceNameSize],
    char guid[webrtc::kAdmMaxGuidSize]) {
    return platform_->PlayoutDeviceName(index, name, guid);
}

int32_t XrtcAudioDeviceModule::RecordingDeviceName(
    uint16_t index,
    char name[webrtc::kAdmMaxDeviceNameSize],
    char guid[webrtc::kAdmMaxGuidSize]) {
    return platform_->RecordingDeviceName(index, name, guid);
}

int32_t XrtcAudioDeviceModule::SetPlayoutDevice(uint16_t index) {
    return platform_->SetPlayoutDevice(index);
}

int32_t XrtcAudioDeviceModule::SetPlayoutDevice(
    webrtc::AudioDeviceModule::WindowsDeviceType device) {
    return platform_->SetPlayoutDevice(device);
}

int32_t XrtcAudioDeviceModule::SetRecordingDevice(uint16_t index) {
    return platform_->SetRecordingDevice(index);
}

int32_t XrtcAudioDeviceModule::SetRecordingDevice(
    webrtc::AudioDeviceModule::WindowsDeviceType device) {
    return platform_->SetRecordingDevice(device);
}

int32_t XrtcAudioDeviceModule::PlayoutIsAvailable(bool* available) {
    return platform_->PlayoutIsAvailable(available);
}

int32_t XrtcAudioDeviceModule::InitPlayout() {
    return platform_->InitPlayout();
}

bool XrtcAudioDeviceModule::PlayoutIsInitialized() const {
    return platform_->PlayoutIsInitialized();
}

int32_t XrtcAudioDeviceModule::RecordingIsAvailable(bool* available) {
    return platform_->RecordingIsAvailable(available);
}

int32_t XrtcAudioDeviceModule::InitRecording() {
    return platform_->InitRecording();
}

bool XrtcAudioDeviceModule::RecordingIsInitialized() const {
    return platform_->RecordingIsInitialized();
}

int32_t XrtcAudioDeviceModule::StartPlayout() {
    return platform_->StartPlayout();
}

int32_t XrtcAudioDeviceModule::StopPlayout() {
    return platform_->StopPlayout();
}

bool XrtcAudioDeviceModule::Playing() const {
    return platform_->Playing();
}

int32_t XrtcAudioDeviceModule::StartRecording() {
    //跳过webrtc开启轨道的时候自动采集
    if (!capture_enabled_.load()) {
        pending_start_.store(true);
        spdlog::info(
            "[xrtc-adm] StartRecording suppressed (capture disabled)");
        return 0;
    }
    pending_start_.store(false);
    // 预览路径：CreatePeerConnectionFactory 不一定立刻 Init VoiceEngine，
    // 不注册 AudioTransport 时 StartRecording 会亮麦但丢弃 PCM。
    EnsurePlatformAudioCallback();
    const int32_t r = platform_->StartRecording();
    if (r != 0) {
        spdlog::error("[xrtc-adm] StartRecording failed: {}", r);
    }
    return r;
}

int32_t XrtcAudioDeviceModule::StopRecording() {
    pending_start_.store(false);
    return platform_->StopRecording();
}

bool XrtcAudioDeviceModule::Recording() const {
    return platform_->Recording();
}

int32_t XrtcAudioDeviceModule::InitSpeaker() {
    return platform_->InitSpeaker();
}

bool XrtcAudioDeviceModule::SpeakerIsInitialized() const {
    return platform_->SpeakerIsInitialized();
}

int32_t XrtcAudioDeviceModule::InitMicrophone() {
    return platform_->InitMicrophone();
}

bool XrtcAudioDeviceModule::MicrophoneIsInitialized() const {
    return platform_->MicrophoneIsInitialized();
}

int32_t XrtcAudioDeviceModule::StereoPlayoutIsAvailable(bool* available) const {
    return platform_->StereoPlayoutIsAvailable(available);
}

int32_t XrtcAudioDeviceModule::SetStereoPlayout(bool enable) {
    return platform_->SetStereoPlayout(enable);
}

int32_t XrtcAudioDeviceModule::StereoPlayout(bool* enabled) const {
    return platform_->StereoPlayout(enabled);
}

int32_t XrtcAudioDeviceModule::StereoRecordingIsAvailable(
    bool* available) const {
    return platform_->StereoRecordingIsAvailable(available);
}

int32_t XrtcAudioDeviceModule::SetStereoRecording(bool enable) {
    return platform_->SetStereoRecording(enable);
}

int32_t XrtcAudioDeviceModule::StereoRecording(bool* enabled) const {
    return platform_->StereoRecording(enabled);
}

int32_t XrtcAudioDeviceModule::PlayoutDelay(uint16_t* delayMS) const {
    return platform_->PlayoutDelay(delayMS);
}

int32_t XrtcAudioDeviceModule::RecordedDataIsAvailable(
    const void* audioSamples,
    size_t nSamples,
    size_t nBytesPerSample,
    size_t nChannels,
    uint32_t samplesPerSec,
    uint32_t totalDelayMS,
    int32_t clockDrift,
    uint32_t currentMicLevel,
    bool keyPressed,
    uint32_t& newMicLevel) {
    AudioCapture* listener = nullptr;
    std::shared_ptr<std::atomic<bool>> alive;
    {
        std::lock_guard<std::mutex> lock(tap_->mutex);
        listener = tap_->listener;
        alive = tap_->listener_alive;
    }
    // 锁外回调，避免观察者同步 stop 时与 SetCaptureListener 死锁；
    // alive 在 stop 时先于清空 listener 置 false，避免 UAF。
    if (listener && alive && alive->load(std::memory_order_acquire)) {
        listener->OnAdmCaptureData(audioSamples, nSamples, nBytesPerSample,
                                   nChannels, samplesPerSec);
    }

    webrtc::AudioTransport* transport = nullptr;
    {
        std::lock_guard<std::mutex> lock(transport_mutex_);
        transport = webrtc_transport_;
    }
    if (!transport) {
        return 0;
    }
    return transport->RecordedDataIsAvailable(
        audioSamples, nSamples, nBytesPerSample, nChannels, samplesPerSec,
        totalDelayMS, clockDrift, currentMicLevel, keyPressed, newMicLevel);
}

int32_t XrtcAudioDeviceModule::NeedMorePlayData(size_t nSamples,
                                                size_t nBytesPerSample,
                                                size_t nChannels,
                                                uint32_t samplesPerSec,
                                                void* audioSamples,
                                                size_t& nSamplesOut,
                                                int64_t* elapsed_time_ms,
                                                int64_t* ntp_time_ms) {
    webrtc::AudioTransport* transport = nullptr;
    {
        std::lock_guard<std::mutex> lock(transport_mutex_);
        transport = webrtc_transport_;
    }
    if (!transport) {
        nSamplesOut = 0;
        return 0;
    }
    return transport->NeedMorePlayData(nSamples, nBytesPerSample, nChannels,
                                       samplesPerSec, audioSamples, nSamplesOut,
                                       elapsed_time_ms, ntp_time_ms);
}

void XrtcAudioDeviceModule::PullRenderData(int bits_per_sample,
                                           int sample_rate,
                                           size_t number_of_channels,
                                           size_t number_of_frames,
                                           void* audio_data,
                                           int64_t* elapsed_time_ms,
                                           int64_t* ntp_time_ms) {
    webrtc::AudioTransport* transport = nullptr;
    {
        std::lock_guard<std::mutex> lock(transport_mutex_);
        transport = webrtc_transport_;
    }
    if (!transport) {
        return;
    }
    transport->PullRenderData(bits_per_sample, sample_rate, number_of_channels,
                              number_of_frames, audio_data, elapsed_time_ms,
                              ntp_time_ms);
}

}  // namespace xrtc
