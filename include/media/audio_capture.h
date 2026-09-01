#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "api/scoped_refptr.h"
#include <media/xrtc_audio_device_module.h>
#include <xrtc/ixrtc_media_source.h>
#include <xrtc/xrtc_defines.h>

namespace xrtc {

/// 基于 WebRTC 平台 ADM 的麦克风控制（选麦 / 启停）
/// PCM 经 XrtcAudioDeviceModule 旁路 → on_audio_level
/// ADM 调用必须在 PeerConnectionFactory 的 worker_thread 上执行
class AudioCapture : public IXRtcMediaSource {
public:
    static std::unique_ptr<AudioCapture> Create(
        webrtc::scoped_refptr<XrtcAudioDeviceModule> adm,
        const std::string& device_id);

    static std::vector<XRTCDeviceInfo> get_audio_device_info(
        webrtc::scoped_refptr<webrtc::AudioDeviceModule> platform_adm);

    ~AudioCapture() override;

    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    /// 选麦 + 挂音量旁路, 不采集
    bool open();

    /// open + 若尚未录音则启动录音（预览 / 会议手动开麦）
    bool start() override;
    bool stop() override;
    bool device_switch(const std::string& device_id) override;

    /// 强制停硬件录音（含 WebRTC 已启录的情况）；保持 open/旁路
    bool StopHardwareRecording();

    /// 由 XrtcAudioDeviceModule::RecordedDataIsAvailable 旁路调用
    /// 这个就是从 ADM 获取到的音频数据，然后进行音量计算
    void OnAdmCaptureData(const void* audio_samples,
                          size_t samples_per_channel,
                          size_t bytes_per_sample,
                          size_t num_channels,
                          uint32_t sample_rate_hz);

    const std::string& device_id() const { return device_id_; }
    bool started() const { return started_; }

private:
    AudioCapture(webrtc::scoped_refptr<XrtcAudioDeviceModule> adm,
                 std::string device_id);

    //确保XrtcAudioDeviceModule是否初始化
    bool ensure_init();
    /// @brief 根据设备名称或者设备id获得音频设备对应的下标
    /// @param device_id 设备的名称或者id
    int resolve_index(const std::string& device_id) const;
    /// @brief 根据当前提供的设备id或者设备名称,将当前的adm_设置这个设备
    bool apply_device(const std::string& device_id);
    bool open_on_worker();
    bool start_recording_on_worker();
    bool stop_on_worker();

    webrtc::scoped_refptr<XrtcAudioDeviceModule> adm_;
    ///当前麦克风的id或者名称
    std::string device_id_;
    ///当前使用的麦克风设备索引
    int device_index_ = -1;
    bool opened_ = false;
    bool started_ = false;       // 是否由本类调用了 StartRecording
    bool level_tapping_ = false; // 是否已挂旁路 listener
    int64_t last_level_ms_ = 0;
};

}  // namespace xrtc
