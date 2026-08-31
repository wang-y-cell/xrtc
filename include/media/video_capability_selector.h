#pragma once

#include <string>
#include <vector>

#include "modules/video_capture/video_capture_defines.h"
#include <xrtc/xrtc_defines.h>

namespace xrtc {

/// 与公开 API 解耦的内部视频格式描述
struct VideoFormat {
    int width = 0;
    int height = 0;
    int fps = 0;
};

/// 在设备已枚举的能力列表中，按策略选出最终格式
class IVideoSelectStrategy {
public:
    virtual ~IVideoSelectStrategy() = default;

    /// @param caps 设备支持的格式列表（非空）
    /// @param requested 上层期望的宽高帧率
    virtual VideoFormat Select(const std::vector<VideoFormat>& caps,
                               const VideoFormat& requested) const = 0;
};

/// 优先贴近 requested：面积差 + fps 差最小
class PreferRequestedStrategy : public IVideoSelectStrategy {
public:
    VideoFormat Select(const std::vector<VideoFormat>& caps,
                       const VideoFormat& requested) const override;
};

/// 优先接近 1280x720@30，其次 640x480，再退回列表中面积最大项
class PreferStandardStrategy : public IVideoSelectStrategy {
public:
    VideoFormat Select(const std::vector<VideoFormat>& caps,
                       const VideoFormat& requested) const override;
};

/// 公开策略 -> 内部策略实例（requested/standard 由调用方提供存储）
inline const IVideoSelectStrategy& StrategyFor(
    XRTCVideoSelectStrategy strategy,
    PreferRequestedStrategy& requested,
    PreferStandardStrategy& standard) {
    return strategy == XRTCVideoSelectStrategy::kPreferStandard
               ? static_cast<const IVideoSelectStrategy&>(standard)
               : static_cast<const IVideoSelectStrategy&>(requested);
}

/// 枚举能力并应用策略；内部还会尝试 WebRTC GetBestMatchedCapability
class VideoCapabilitySelector {
public:
    /// 枚举 device_id 支持的全部格式（可能含重复分辨率不同 pixel format）
    static std::vector<VideoFormat> ListCapabilities(
        const std::string& device_id);

    /// 用策略从列表中选择；caps 为空时返回 requested
    static VideoFormat Select(const std::vector<VideoFormat>& caps,
                              const VideoFormat& requested,
                              const IVideoSelectStrategy& strategy);

    /// 结合 DeviceInfo::GetBestMatchedCapability + 策略，得到可 StartCapture 的 capability。
    /// @param strategy 为空时使用 PreferRequestedStrategy
    static webrtc::VideoCaptureCapability Resolve(
        const std::string& device_id,
        int width,
        int height,
        int fps,
        const IVideoSelectStrategy* strategy = nullptr);

    /// 便捷重载：直接使用公开策略枚举
    static webrtc::VideoCaptureCapability Resolve(
        const std::string& device_id,
        int width,
        int height,
        int fps,
        XRTCVideoSelectStrategy strategy);
};

}  // namespace xrtc
