#include <media/video_capability_selector.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>

#include <modules/video_capture/video_capture_factory.h>
#include <spdlog/spdlog.h>

namespace xrtc {
namespace {

int64_t Area(const VideoFormat& f) {
    return static_cast<int64_t>(f.width) * static_cast<int64_t>(f.height);
}

int64_t Score(const VideoFormat& cand, const VideoFormat& requested) {
    const int64_t area_delta = std::llabs(Area(cand) - Area(requested));
    const int64_t fps_delta =
        std::llabs(static_cast<int64_t>(cand.fps) -
                   static_cast<int64_t>(requested.fps));
    // fps 权重略高，避免选到面积接近但帧率过低的模式
    return area_delta + fps_delta * 10000;
}

/// 从caps中找到与requested最接近的格式，并返回
VideoFormat Closest(const std::vector<VideoFormat>& caps,
                    const VideoFormat& requested) {
    VideoFormat best = caps.front();
    int64_t best_score = Score(best, requested);
    for (size_t i = 1; i < caps.size(); ++i) {
        const int64_t s = Score(caps[i], requested);
        if (s < best_score) {
            best_score = s;
            best = caps[i];
        }
    }
    return best;
}

/// 从caps中找到面积最大的格式,如果面积相同，则返回帧率最高的格式
VideoFormat MaxArea(const std::vector<VideoFormat>& caps) {
    return *std::max_element(
        caps.begin(), caps.end(),
        [](const VideoFormat& a, const VideoFormat& b) {
            if (Area(a) != Area(b)) {
                return Area(a) < Area(b);
            }
            return a.fps < b.fps;
        });
}

std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> MakeDeviceInfo() {
    return std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo>(
        webrtc::VideoCaptureFactory::CreateDeviceInfo());
}

}  // namespace

VideoFormat PreferRequestedStrategy::Select(
    const std::vector<VideoFormat>& caps,
    const VideoFormat& requested) const {
    if (caps.empty()) {
        return requested;
    }
    return Closest(caps, requested);
}

VideoFormat PreferStandardStrategy::Select(
    const std::vector<VideoFormat>& caps,
    const VideoFormat& requested) const {
    if (caps.empty()) {
        return requested;
    }

    const PreferRequestedStrategy prefer;
    const VideoFormat hd = prefer.Select(caps, {1280, 720, 30});
    if (hd.width >= 960 && hd.height >= 540) {
        return hd;
    }

    const VideoFormat sd = prefer.Select(caps, {640, 480, 30});
    if (sd.width >= 480 && sd.height >= 360) {
        return sd;
    }

    if (requested.width > 0 && requested.height > 0) {
        return prefer.Select(caps, requested);
    }
    return MaxArea(caps);
}

std::vector<VideoFormat> VideoCapabilitySelector::ListCapabilities(
    const std::string& device_id) {
    std::vector<VideoFormat> out;
    auto info = MakeDeviceInfo();
    if (!info) {
        spdlog::warn("[cap-select] CreateDeviceInfo failed");
        return out;
    }

    const int32_t n = info->NumberOfCapabilities(device_id.c_str());
    for (int32_t i = 0; i < n; ++i) {
        webrtc::VideoCaptureCapability cap;
        if (info->GetCapability(device_id.c_str(), static_cast<uint32_t>(i),
                                cap) != 0) {
            continue;
        }
        out.push_back({cap.width, cap.height, cap.maxFPS});
    }
    return out;
}

VideoFormat VideoCapabilitySelector::Select(
    const std::vector<VideoFormat>& caps,
    const VideoFormat& requested,
    const IVideoSelectStrategy& strategy) {
    if (caps.empty()) {
        return requested;
    }
    return strategy.Select(caps, requested);
}

webrtc::VideoCaptureCapability VideoCapabilitySelector::Resolve(
    const std::string& device_id,
    int width,
    int height,
    int fps,
    const IVideoSelectStrategy* strategy) {
    PreferRequestedStrategy default_strategy;
    const IVideoSelectStrategy& chosen =
        strategy ? *strategy : default_strategy;

    const VideoFormat requested{width, height, fps};
    webrtc::VideoCaptureCapability result;
    result.width = width;
    result.height = height;
    result.maxFPS = fps;
    result.videoType = webrtc::VideoType::kI420;

    auto info = MakeDeviceInfo();
    if (!info) {
        spdlog::warn("[cap-select] CreateDeviceInfo failed, use requested "
                     "{}x{}@{}",
                     width, height, fps);
        return result;
    }

    const int32_t n = info->NumberOfCapabilities(device_id.c_str());
    std::vector<VideoFormat> formats;
    formats.reserve(static_cast<size_t>(std::max(n, 0)));
    for (int32_t i = 0; i < n; ++i) {
        webrtc::VideoCaptureCapability cap;
        if (info->GetCapability(device_id.c_str(), static_cast<uint32_t>(i),
                                cap) != 0) {
            continue;
        }
        formats.push_back({cap.width, cap.height, cap.maxFPS});
    }

    VideoFormat selected = requested;
    if (!formats.empty()) {
        selected = chosen.Select(formats, requested);
    }

    webrtc::VideoCaptureCapability req;
    req.width = selected.width;
    req.height = selected.height;
    req.maxFPS = selected.fps;
    req.videoType = webrtc::VideoType::kI420;

    webrtc::VideoCaptureCapability matched;
    if (info->GetBestMatchedCapability(device_id.c_str(), req, matched) >= 0) {
        spdlog::info(
            "[cap-select] device={} requested={}x{}@{} -> matched={}x{}@{} "
            "type={}",
            device_id, width, height, fps, matched.width, matched.height,
            matched.maxFPS, static_cast<int>(matched.videoType));
        return matched;
    }

    // GetBestMatchedCapability 失败时，从枚举列表取第一项匹配策略结果
    for (int32_t i = 0; i < n; ++i) {
        webrtc::VideoCaptureCapability cap;
        if (info->GetCapability(device_id.c_str(), static_cast<uint32_t>(i),
                                cap) != 0) {
            continue;
        }
        if (cap.width == selected.width && cap.height == selected.height &&
            cap.maxFPS == selected.fps) {
            spdlog::info(
                "[cap-select] device={} requested={}x{}@{} -> enum={}x{}@{} "
                "type={}",
                device_id, width, height, fps, cap.width, cap.height,
                cap.maxFPS, static_cast<int>(cap.videoType));
            return cap;
        }
    }

    if (n > 0) {
        webrtc::VideoCaptureCapability cap;
        if (info->GetCapability(device_id.c_str(), 0, cap) == 0) {
            spdlog::warn(
                "[cap-select] device={} fallback index0 {}x{}@{}", device_id,
                cap.width, cap.height, cap.maxFPS);
            return cap;
        }
    }

    spdlog::warn(
        "[cap-select] device={} no capabilities, keep requested {}x{}@{}",
        device_id, width, height, fps);
    result.width = selected.width;
    result.height = selected.height;
    result.maxFPS = selected.fps;
    return result;
}

}  // namespace xrtc
