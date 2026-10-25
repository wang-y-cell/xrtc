#include <platform/screen_grabber.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace xrtc {
namespace {

// 与 webrtc.lib 的 ABI 绑定：DesktopCaptureOptions 尺寸变化时编译期报错，
// 避免静默的栈损坏（RTC_ENABLE_WIN_WGC 等编译宏必须与 SDK 一致）。
static_assert(sizeof(webrtc::DesktopCaptureOptions) == 0x50,
              "DesktopCaptureOptions layout must match webrtc.lib");

struct ListedScreen {
    XRTCScreenInfo info;
    webrtc::DesktopCapturer::SourceId source_id = webrtc::kInvalidScreenId;
};

std::vector<ListedScreen> EnumerateScreens() {
    webrtc::DesktopCapturer::SourceList screens;
    std::vector<std::string> device_names;
    if (!webrtc::GetScreenList(&screens, &device_names) ||
        screens.size() != device_names.size()) {
        return {};
    }

    std::vector<ListedScreen> listed;
    listed.reserve(screens.size());
    int secondary = 1;
    for (size_t i = 0; i < screens.size(); ++i) {
        const webrtc::DesktopRect rect =
            webrtc::GetScreenRect(screens[i].id, std::nullopt);
        if (rect.is_empty()) {
            continue;
        }
        ListedScreen item;
        item.source_id = screens[i].id;
        item.info.screen_id = device_names[i];
        item.info.width = rect.width();
        item.info.height = rect.height();
        item.info.primary = rect.left() == 0 && rect.top() == 0;
        item.info.name = item.info.primary ? "主屏幕"
                                           : "屏幕" + std::to_string(secondary++);
        listed.push_back(std::move(item));
    }
    std::stable_partition(listed.begin(), listed.end(),
                          [](const ListedScreen& screen) {
                              return screen.info.primary;
                          });
    if (!listed.empty() && !listed.front().info.primary) {
        listed.front().info.primary = true;
        listed.front().info.name = "主屏幕";
    }
    return listed;
}

class WebrtcScreenGrabber : public ScreenGrabber,
                            public webrtc::DesktopCapturer::Callback {
public:
    WebrtcScreenGrabber() = default;
    ~WebrtcScreenGrabber() override { close(); }

    bool open(const std::string& screen_id, int max_fps) override {
        close();
        std::vector<ListedScreen> screens = EnumerateScreens();
        const auto found = std::find_if(
            screens.begin(), screens.end(),
            [&](const ListedScreen& screen) {
                return screen.info.screen_id == screen_id;
            });
        if (found == screens.end()) {
            spdlog::error("[screen] webrtc source not found id={}", screen_id);
            return false;
        }
        webrtc::DesktopCaptureOptions options =
            webrtc::DesktopCaptureOptions::CreateDefault();
        options.set_allow_directx_capturer(true);
        options.set_prefer_cursor_embedded(false);

        std::unique_ptr<webrtc::DesktopCapturer> raw =
            webrtc::DesktopCapturer::CreateScreenCapturer(options);
        if (!raw) {
            spdlog::error("[screen] CreateScreenCapturer failed");
            return false;
        }
        // DesktopAndCursorComposer 会把系统光标合成进每一帧，
        // 因此调用方不需要再自己画光标，也不需要整幅拷贝。
        capturer_ = std::make_unique<webrtc::DesktopAndCursorComposer>(
            std::move(raw), options);
        if (!capturer_->SelectSource(found->source_id)) {
            spdlog::error("[screen] SelectSource failed id={}", screen_id);
            capturer_.reset();
            return false;
        }
        capturer_->Start(this);
        max_fps_ = std::max(1, max_fps);
        capturer_->SetMaxFrameRate(static_cast<uint32_t>(max_fps_));

        screen_id_ = screen_id;
        source_id_ = found->source_id;
        // 这里只记录源分辨率；缩放/转 I420 由 ScreenCapture 用 libyuv 完成
        width_ = found->info.width;
        height_ = found->info.height;
        const webrtc::DesktopRect rect =
            webrtc::GetScreenRect(source_id_, std::nullopt);
        if (!rect.is_empty()) {
            origin_x_ = rect.left();
            origin_y_ = rect.top();
            origin_right_ = rect.right();
            origin_bottom_ = rect.bottom();
        }
        spdlog::info("[screen] webrtc capturer opened id={} source={} {}x{} fps={}",
                     screen_id_, static_cast<long long>(source_id_), width_,
                     height_, max_fps);
        return width_ > 0 && height_ > 0;
    }

    void close() override {
        capturer_.reset();
        last_frame_.reset();
        screen_id_.clear();
        source_id_ = webrtc::kInvalidScreenId;
        width_ = 0;
        height_ = 0;
    }

    PollResult poll(int timeout_ms) override {
        (void)timeout_ms;
        if (!capturer_) {
            return PollResult::kFailed;
        }
        got_frame_ = false;
        last_result_ = webrtc::DesktopCapturer::Result::ERROR_PERMANENT;
        capturer_->CaptureFrame();
        // DXGI 采集器在 CaptureFrame() 内同步回调；WGC 若改成异步需另行处理
        if (last_result_ == webrtc::DesktopCapturer::Result::SUCCESS &&
            got_frame_) {
            return PollResult::kNewFrame;
        }
        if (last_result_ == webrtc::DesktopCapturer::Result::ERROR_TEMPORARY) {
            return PollResult::kTimeout;
        }
        const std::string id = screen_id_;
        spdlog::warn("[screen] capture failed, reopening id={}", id);
        if (id.empty() || !open(id, max_fps_)) {
            return PollResult::kFailed;
        }
        return PollResult::kRestarted;
    }

    ScreenDesktopFrame frame() const override {
        ScreenDesktopFrame out;
        if (!last_frame_ || !last_frame_->data()) {
            out.width = width_;
            out.height = height_;
            return out;
        }
        out.width = last_frame_->size().width();
        out.height = last_frame_->size().height();
        out.bgra = last_frame_->data();
        const webrtc::DesktopVector top_left = last_frame_->top_left();
        out.origin_x = top_left.x();
        out.origin_y = top_left.y();
        out.origin_right = out.origin_x + out.width;
        out.origin_bottom = out.origin_y + out.height;
        return out;
    }

    bool has_frame() const override { return last_frame_ != nullptr; }

    bool consume_cursor_change() override { return false; }

    void reset_cursor_tracking() override {}

    void draw_cursor(uint8_t* bgra, int width, int height, int origin_x,
                     int origin_y, int origin_right, int origin_bottom) override {
        // 光标已由 DesktopAndCursorComposer 合成，保留空实现兼容接口
        (void)bgra;
        (void)width;
        (void)height;
        (void)origin_x;
        (void)origin_y;
        (void)origin_right;
        (void)origin_bottom;
    }

    void OnCaptureResult(webrtc::DesktopCapturer::Result result,
                         std::unique_ptr<webrtc::DesktopFrame> frame) override {
        last_result_ = result;
        if (result != webrtc::DesktopCapturer::Result::SUCCESS || !frame ||
            !frame->data()) {
            return;
        }
        const int src_w = frame->size().width();
        const int src_h = frame->size().height();
        if (src_w <= 0 || src_h <= 0) {
            return;
        }
        // 只持有这一帧，不做整幅拷贝/缩放：下一次 poll 之前指针有效
        last_frame_ = std::move(frame);
        width_ = src_w;
        height_ = src_h;
        got_frame_ = true;
    }

private:
    std::unique_ptr<webrtc::DesktopCapturer> capturer_;
    std::string screen_id_;
    webrtc::DesktopCapturer::SourceId source_id_ = webrtc::kInvalidScreenId;
    /// 最近一次成功采集的原始帧（BGRA，源分辨率）
    std::unique_ptr<webrtc::DesktopFrame> last_frame_;
    int width_ = 0;
    int height_ = 0;
    int origin_x_ = 0;
    int origin_y_ = 0;
    int origin_right_ = 0;
    int origin_bottom_ = 0;
    int max_fps_ = 30;
    bool got_frame_ = false;
    webrtc::DesktopCapturer::Result last_result_ =
        webrtc::DesktopCapturer::Result::ERROR_PERMANENT;
};

}  // namespace

std::vector<XRTCScreenInfo> list_screens() {
    const std::vector<ListedScreen> screens = EnumerateScreens();
    std::vector<XRTCScreenInfo> out;
    out.reserve(screens.size());
    for (const ListedScreen& screen : screens) {
        out.push_back(screen.info);
    }
    return out;
}

std::string resolve_screen_id(const std::string& screen_id) {
    const std::vector<ListedScreen> screens = EnumerateScreens();
    if (screens.empty()) {
        return {};
    }
    if (screen_id.empty()) {
        return screens.front().info.screen_id;
    }
    const auto found = std::find_if(
        screens.begin(), screens.end(), [&](const ListedScreen& screen) {
            return screen.info.screen_id == screen_id;
        });
    if (found == screens.end()) {
        return {};
    }
    return found->info.screen_id;
}

std::unique_ptr<ScreenGrabber> create_screen_grabber() {
    return std::make_unique<WebrtcScreenGrabber>();
}

}  // namespace xrtc
