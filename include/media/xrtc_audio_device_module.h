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

/// 录音旁路监听状态：平台 ADM → 本模块 →（可选）AudioCapture 电平/预览。
/// 与录音线程共享；换 listener 时先把 listener_alive 置 false，避免 UAF。
struct AudioCaptureTapState {
    std::mutex mutex;
    AudioCapture* listener = nullptr;
    /// stop / 换 listener 时先置 false，再清空 listener，供录音线程无锁外安全判断
    std::shared_ptr<std::atomic<bool>> listener_alive;
};

/// 包装平台 AudioDeviceModule（Windows WASAPI 等），插入 PeerConnectionFactory。
///
/// 职责：
/// 1. 绝大部分设备/启停 API **透传**给 platform_；
/// 2. 实现 AudioTransport：平台回调先到本类，再转给 WebRTC VoiceEngine，
///    同时可旁路一份 PCM 给 AudioCapture（电平、本地预览）；
/// 3. 用 capture_enabled_ 挡住「进房自动开麦」：WebRTC StartRecording 可先记
///    pending，等用户开麦后再真正启动硬件。
///
/// 线程：与平台 ADM 一致，多数方法须在 PeerConnectionFactory 的 worker_thread 上调用。
class XrtcAudioDeviceModule
    : public webrtc::webrtc_impl::AudioDeviceModuleDefault<
          webrtc::AudioDeviceModule>,
      public webrtc::AudioTransport {
public:
    /// 用已创建的平台 ADM 构造包装层；platform 为空则返回 nullptr
    static webrtc::scoped_refptr<XrtcAudioDeviceModule> Create(
        webrtc::scoped_refptr<webrtc::AudioDeviceModule> platform);

    /// 注册/清除 PCM 旁路监听（AudioCapture）。
    /// listener_alive 与 AudioCapture 同寿；清空时先置 false 再摘指针。
    void SetCaptureListener(
        AudioCapture* listener,
        std::shared_ptr<std::atomic<bool>> listener_alive = nullptr);

    /// 用户是否允许硬件录音。
    /// false：吞掉 WebRTC 的 StartRecording（返回成功但不启麦），进房默认关麦；
    /// true：若此前有 pending_start_，会补一次真正的 StartRecording。
    /// 须在 worker 线程调用。
    void SetCaptureEnabled(bool enabled);
    bool capture_enabled() const { return capture_enabled_.load(); }

    /// 让平台 ADM 的 AudioTransport 指向本对象（录音前可调；须在 worker 线程）。
    /// 已安装则跳过；录音中重复 Register 可能失败。
    int32_t EnsurePlatformAudioCallback();

    /// WebRTC 注册其 AudioTransport；本类保存指针并把平台回调设为 this，再转发。
    int32_t RegisterAudioCallback(webrtc::AudioTransport* audioCallback) override;

    int32_t Init() override;
    int32_t Terminate() override;
    bool Initialized() const override;

    // ---- 设备枚举 / 选择（透传 platform_）----
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
    /// 按索引选择麦克风（透传）
    int32_t SetRecordingDevice(uint16_t index) override;
    int32_t SetRecordingDevice(
        webrtc::AudioDeviceModule::WindowsDeviceType device) override;

    // ---- 播放 / 录音初始化与启停 ----
    int32_t PlayoutIsAvailable(bool* available) override;
    int32_t InitPlayout() override;
    bool PlayoutIsInitialized() const override;
    int32_t RecordingIsAvailable(bool* available) override;
    /// 准备录音资源（缓冲、格式、会话），尚未向外送 PCM
    int32_t InitRecording() override;
    bool RecordingIsInitialized() const override;

    int32_t StartPlayout() override;
    int32_t StopPlayout() override;
    bool Playing() const override;
    /// 开始硬件录音；若 capture_enabled_==false 则记 pending 并返回成功（不启麦）
    int32_t StartRecording() override;
    int32_t StopRecording() override;
    /// 底层平台 ADM 当前是否正在录音
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

    /// 底层未包装的平台 ADM（枚举设备等可直接用）
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> platform() const {
        return platform_;
    }

    // ---- AudioTransport：平台 → 本类 → WebRTC（及旁路 AudioCapture）----

    /// 麦克风 PCM 到达：旁路给 listener，再转发给 webrtc_transport_
    /// @param audioSamples 交错 PCM
    /// @param nSamples 每通道采样数
    /// @param nBytesPerSample 每采样字节数（如 2=int16）
    /// @param nChannels 声道数
    /// @param samplesPerSec 采样率
    /// @param totalDelayMS 采集侧估计延迟
    /// @param clockDrift 时钟漂移
    /// @param currentMicLevel 当前麦克风电平（平台提供）
    /// @param keyPressed 是否检测到按键（AEC 等用）
    /// @param newMicLevel 输出：建议的新麦克风电平
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

    /// 扬声器需要填充播放缓冲：转发给 WebRTC 拉取远端混音 PCM
    int32_t NeedMorePlayData(size_t nSamples,
                             size_t nBytesPerSample,
                             size_t nChannels,
                             uint32_t samplesPerSec,
                             void* audioSamples,
                             size_t& nSamplesOut,
                             int64_t* elapsed_time_ms,
                             int64_t* ntp_time_ms) override;

    /// 部分平台拉取渲染数据的备用接口，同样转发给 webrtc_transport_
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
    /// WebRTC VoiceEngine 注册的 AudioTransport（录音/播放转发目标）
    webrtc::AudioTransport* webrtc_transport_ = nullptr;
    /// 平台 RegisterAudioCallback(this) 是否已成功
    bool platform_callback_installed_ = false;
    /// 默认关：避免 AddTrack/CreateOffer 触发 AudioState 自动开麦
    std::atomic<bool> capture_enabled_{false};
    /// WebRTC 已请求 StartRecording，但当时 capture_enabled_ 为 false，开麦后补启
    std::atomic<bool> pending_start_{false};
};

}  // namespace xrtc
