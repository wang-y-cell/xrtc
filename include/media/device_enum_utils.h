#pragma once

#include <algorithm>
#include <cstdint>

namespace xrtc {

/// 将 ADM 返回的设备数量限制到安全枚举上限（避免 >32 时错误变成 0）
inline int ClampDeviceCount(int16_t total, int max_count = 32) {
    if (total <= 0) {
        return 0;
    }
    return std::min(static_cast<int>(total), max_count);
}

}  // namespace xrtc
