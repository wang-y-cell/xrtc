#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace xrtc {

/// 把一帧 16-bit PCM 折算成 0~100 的音量值。
///
/// 本地麦克风（AudioCapture）与远端说话检测（VoiceActivityDetector）共用同一个
/// 公式，保证“本地音量条”和“远端说话指示”的刻度一致：
///   0.7 * RMS + 0.3 * 峰值（兼顾平均响度与瞬间大声）
///   → 按 16-bit 满量程归一化 → ×1.8 放大 → clamp 到 0~100
/// 全静音返回 0，满量程方波约为 100。
inline int AudioLevelFromPcm16(const int16_t* samples, size_t count) {
    if (!samples || count == 0) {
        return 0;
    }
    double sum_sq = 0.0;
    int peak = 0;
    for (size_t i = 0; i < count; ++i) {
        const int v = samples[i];
        const int a = v >= 0 ? v : -v;
        if (a > peak) {
            peak = a;
        }
        sum_sq += static_cast<double>(v) * static_cast<double>(v);
    }
    const double rms = std::sqrt(sum_sq / static_cast<double>(count));
    const double mixed = 0.7 * rms + 0.3 * static_cast<double>(peak);
    return std::clamp(static_cast<int>(mixed / 32768.0 * 100.0 * 1.8), 0, 100);
}

}  // namespace xrtc
