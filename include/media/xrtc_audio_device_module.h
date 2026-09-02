#pragma once

#include <atomic>
#include <memory>
#include <mutex>

#include "api/audio/audio_device.h"
#include "api/audio/audio_device_defines.h"
#include "api/scoped_refptr.h"
#include "modules/audio_device/include/audio_device_default.h"

namespace xrtc {

class AudioCapture;

struct AudioCaptureTapState {
    std::mutex mutex;
    AudioCapture* listener = nullptr;
    /// stop 时先置 false，再清空 listener，供录音线程无锁外安全判断
    std::shared_ptr<std::atomic<bool>> listener_alive;
};

/// 包装平台 ADM：录音/播放透传；以 AudioTransport 旁路拷贝 PCM
class XrtcAudioDeviceModule
    : public webrtc::webrtc_impl::AudioDeviceModuleDefault<
          webrtc::AudioDeviceModule>,
      public webrtc::AudioTransport {
public:
    static webrtc::scoped_refptr<XrtcAudioDeviceModule> Create(
        webrtc::scoped_refptr<webrtc::AudioDeviceModule> platform);

    void SetCaptureListener(
        AudioCapture* listener,
        std::shared_ptr<std::atomic<bool>> listener_alive = nullptr);

    /// 用户是否允许硬件录音。false 时 WebRTC 的 StartRecording 会被吞掉（返回成功但不启麦），
    /// 进房默认关麦；预览/手动开麦前设为 true。须在 worker 线程调用。
    void SetCaptureEnabled(bool enabled);
    bool capture_enabled() const { return capture_enabled_.load(); }

    /// 保证平台 ADM 回调到本类（可在 StartRecording 前调用；须在 worker 线程）
    int32_t EnsurePlatformAudioCallback();

    int32_t RegisterAudioCallback(webrtc::AudioTransport* audioCallback) override;
    int32_t Init() override;
    int32_t Terminate() override;
    bool Initialized() const override;

    int16_t PlayoutDevices() override;
    int16_t RecordingDevices() override;
    int32_t PlayoutDeviceName(uint16_t index,
                              char name[webrtc::kAdmMaxDeviceNameSize],
                              char guid[webrtc::kAdmMaxGuidSize]) override;
    int32_t RecordingDeviceName(uint16_t index,
                                char name[webrtc::kAdmMaxDeviceNameSize],
                                char guid[webrtc::kAdmMaxGuidSize]) override;

    int32_t SetPlayoutDevice(uint16_t index) override;
    int32_t SetPlayoutDevice(
        webrtc::AudioDeviceModule::WindowsDeviceType device) override;
    /// @brief 初始化麦克风设备
    /// @param 设备下标
    int32_t SetRecordingDevice(uint16_t index) override;
    int32_t SetRecordingDevice(
        webrtc::AudioDeviceModule::WindowsDeviceType device) override;

    int32_t PlayoutIsAvailable(bool* available) override;
    int32_t InitPlayout() override;
    bool PlayoutIsInitialized() const override;
    int32_t RecordingIsAvailable(bool* available) override;
    ///准备好录音所需的资源（缓冲区、采样格式、和设备的录制会话等），但还没有开始往外送 PCM
    int32_t InitRecording() override;
    bool RecordingIsInitialized() const override;

    int32_t StartPlayout() override;
    int32_t StopPlayout() override;
    bool Playing() const override;
    ///开始采集
    int32_t StartRecording() override;
    int32_t StopRecording() override;
    ///查询底层平台 ADM 当前是否正在录音
    bool Recording() const override;

    int32_t InitSpeaker() override;
    bool SpeakerIsInitialized() const override;
    int32_t InitMicrophone() override;
    bool MicrophoneIsInitialized() const override;

    int32_t StereoPlayoutIsAvailable(bool* available) const override;
    int32_t SetStereoPlayout(bool enable) override;
    int32_t StereoPlayout(bool* enabled) const override;
    int32_t StereoRecordingIsAvailable(bool* available) const override;
    int32_t SetStereoRecording(bool enable) override;
    int32_t StereoRecording(bool* enabled) const override;

    int32_t PlayoutDelay(uint16_t* delayMS) const override;

    webrtc::scoped_refptr<webrtc::AudioDeviceModule> platform() const {
        return platform_;
    }

    /// @brief 开始采集的回调函数
    /// @param audioSamples 音频数据
    /// @param nSamples 音频数据采样数
    /// @param nBytesPerSample 音频数据每个采样字节数
    /// @param nChannels 音频数据通道数
    /// @param samplesPerSec 音频数据采样率
    /// @param totalDelayMS 音频数据总延迟时间
    /// @param clockDrift 音频数据时钟漂移
    /// @param currentMicLevel 当前麦克风级别
    /// @param keyPressed 是否按下按键
    int32_t RecordedDataIsAvailable(const void* audioSamples,
                                    size_t nSamples,
                                    size_t nBytesPerSample,
                                    size_t nChannels,
                                    uint32_t samplesPerSec,
                                    uint32_t totalDelayMS,
                                    int32_t clockDrift,
                                    uint32_t currentMicLevel,
                                    bool keyPressed,
                                    uint32_t& newMicLevel) override;

    int32_t NeedMorePlayData(size_t nSamples,
                             size_t nBytesPerSample,
                             size_t nChannels,
                             uint32_t samplesPerSec,
                             void* audioSamples,
                             size_t& nSamplesOut,
                             int64_t* elapsed_time_ms,
                             int64_t* ntp_time_ms) override;

    void PullRenderData(int bits_per_sample,
                        int sample_rate,
                        size_t number_of_channels,
                        size_t number_of_frames,
                        void* audio_data,
                        int64_t* elapsed_time_ms,
                        int64_t* ntp_time_ms) override;

protected:
    explicit XrtcAudioDeviceModule(
        webrtc::scoped_refptr<webrtc::AudioDeviceModule> platform);
    ~XrtcAudioDeviceModule() override = default;

private:
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> platform_;
    std::shared_ptr<AudioCaptureTapState> tap_;
    mutable std::mutex transport_mutex_;
    webrtc::AudioTransport* webrtc_transport_ = nullptr;
    ///当前回调函数是否设置的标志位
    bool platform_callback_installed_ = false;
    /// 默认关：避免 AddTrack/CreateOffer 触发 AudioState 自动开麦
    std::atomic<bool> capture_enabled_{false};
    /// WebRTC 已请求 StartRecording，但当时 capture_enabled_ 为 false
    std::atomic<bool> pending_start_{false};
};

}  // namespace xrtc
