#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <xrtc/xrtc_defines.h>

// 屏幕采集使用 WebRTC DesktopCapturer。系统差异（DXGI / WGC / GDI 等）留在 WebRTC 内。
#include "modules/desktop_capture/desktop_and_cursor_composer.h"
#include "modules/desktop_capture/desktop_capture_options.h"
#include "modules/desktop_capture/desktop_capturer.h"
#include "modules/desktop_capture/desktop_frame.h"
#include "modules/desktop_capture/desktop_geometry.h"
#include "modules/desktop_capture/win/screen_capture_utils.h"

namespace xrtc {

/// 一帧桌面画面（BGRA，紧凑排列）。WebRTC 实现里光标已经合成进画面。
/// 指针只在下一次 poll/close 之前有效。
struct ScreenDesktopFrame {
    int width = 0;
    int height = 0;
    const uint8_t* bgra = nullptr;
    /// 该显示器在虚拟桌面上的矩形，用于把光标画回画面
    int origin_x = 0;
    int origin_y = 0;
    int origin_right = 0;
    int origin_bottom = 0;
};

/// 屏幕采集的平台接口。调用方不依赖具体系统 API。
class ScreenGrabber {
public:
    virtual ~ScreenGrabber() = default;

    enum class PollResult {
        kTimeout = 0,  /// 桌面没有新画面
        kNewFrame,     /// clean 缓冲已更新
        kRestarted,    /// 采集会话丢失后已重建，本轮没有新画面
        kFailed,       /// 无法继续采集
    };

    /// 打开 screen_id 对应的显示器。id 须已由 resolve_screen_id 解析
    virtual bool open(const std::string& screen_id) = 0;
    /// 释放本次采集占用的系统资源
    virtual void close() = 0;
    /// 等待最多 timeout_ms 毫秒，尝试取一帧桌面
    virtual PollResult poll(int timeout_ms) = 0;
    /// 最近一帧缩放后的桌面。尚未采到画面时 bgra 可能为空，宽高仍有效
    virtual ScreenDesktopFrame frame() const = 0;
    /// 是否已经至少成功采到一帧
    virtual bool has_frame() const = 0;
    /// 光标显隐或位置相对上次调用是否变化
    virtual bool consume_cursor_change() = 0;
    /// 丢弃已记录的光标位置（采集会话重建后调用）
    virtual void reset_cursor_tracking() = 0;
    /// 把系统光标画进调用方的 BGRA 缓冲。WebRTC 实现已在采集时合成光标，此处为空操作
    virtual void draw_cursor(uint8_t* bgra, int width, int height, int origin_x,
                             int origin_y, int origin_right,
                             int origin_bottom) = 0;
};

/// 枚举显示器，主屏在前
std::vector<XRTCScreenInfo> list_screens();
/// 空 id 解析为主屏。找不到时返回空字符串
std::string resolve_screen_id(const std::string& screen_id);
/// 创建当前系统的屏幕采集实现。没有实现时返回 nullptr
std::unique_ptr<ScreenGrabber> create_screen_grabber();

}  // namespace xrtc
