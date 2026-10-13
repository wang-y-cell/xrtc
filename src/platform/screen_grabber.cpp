#include <platform/screen_grabber.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace xrtc {
namespace {

static_assert(sizeof(webrtc::DesktopCaptureOptions) == 0x50,
              "DesktopCaptureOptions layout must match webrtc.lib");

constexpr int kMaxLongEdge = 1920;

void FitOutputSize(int src_w, int src_h, int* out_w, int* out_h) {
    int w = src_w;
    int h = src_h;
    const int long_edge = std::max(w, h);
    if (long_edge > kMaxLongEdge && long_edge > 0) {
        if (w >= h) {
            w = kMaxLongEdge;
            h = std::max(2, src_h * kMaxLongEdge / src_w);
        } else {
            h = kMaxLongEdge;
            w = std::max(2, src_w * kMaxLongEdge / src_h);
        }
    }
    w &= ~1;
    h &= ~1;
    if (w < 2) {
        w = 2;
    }
    if (h < 2) {
        h = 2;
    }
    *out_w = w;
    *out_h = h;
}

void ScaleBgraNearest(const uint8_t* src, int src_stride, int src_w, int src_h,
                      uint8_t* dst, int dst_w, int dst_h) {
    if (src_w == dst_w && src_h == dst_h) {
        for (int y = 0; y < src_h; ++y) {
            std::memcpy(dst + static_cast<size_t>(y) * dst_w * 4,
                        src + static_cast<size_t>(y) * src_stride,
                        static_cast<size_t>(src_w) * 4);
        }
        return;
    }
    for (int y = 0; y < dst_h; ++y) {
        const int sy = y * src_h / dst_h;
        const uint8_t* row = src + static_cast<size_t>(sy) * src_stride;
        uint8_t* out = dst + static_cast<size_t>(y) * dst_w * 4;
        for (int x = 0; x < dst_w; ++x) {
            const int sx = x * src_w / dst_w;
            std::memcpy(out + static_cast<size_t>(x) * 4,
                        row + static_cast<size_t>(sx) * 4, 4);
        }
    }
}

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

    bool open(const std::string& screen_id) override {
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
        capturer_ = std::make_unique<webrtc::DesktopAndCursorComposer>(
            std::move(raw), options);
        if (!capturer_->SelectSource(found->source_id)) {
            spdlog::error("[screen] SelectSource failed id={}", screen_id);
            capturer_.reset();
            return false;
        }
        capturer_->Start(this);
        capturer_->SetMaxFrameRate(15);

        screen_id_ = screen_id;
        source_id_ = found->source_id;
        FitOutputSize(found->info.width, found->info.height, &width_, &height_);
        origin_x_ = 0;
        origin_y_ = 0;
        const webrtc::DesktopRect rect =
            webrtc::GetScreenRect(source_id_, std::nullopt);
        if (!rect.is_empty()) {
            origin_x_ = rect.left();
            origin_y_ = rect.top();
            origin_right_ = rect.right();
            origin_bottom_ = rect.bottom();
        }
        spdlog::info("[screen] webrtc capturer opened id={} source={} {}x{}",
                     screen_id_, static_cast<long long>(source_id_), width_,
                     height_);
        return width_ > 0 && height_ > 0;
    }

    void close() override {
        capturer_.reset();
        bgra_.clear();
        have_frame_ = false;
        screen_id_.clear();
        source_id_ = webrtc::kInvalidScreenId;
    }

    PollResult poll(int timeout_ms) override {
        (void)timeout_ms;
        if (!capturer_) {
            return PollResult::kFailed;
        }
        size_changed_ = false;
        got_frame_ = false;
        last_result_ = webrtc::DesktopCapturer::Result::ERROR_PERMANENT;
        capturer_->CaptureFrame();
        if (last_result_ == webrtc::DesktopCapturer::Result::SUCCESS &&
            got_frame_) {
            return size_changed_ ? PollResult::kRestarted
                                 : PollResult::kNewFrame;
        }
        if (last_result_ == webrtc::DesktopCapturer::Result::ERROR_TEMPORARY) {
            return PollResult::kTimeout;
        }
        const std::string id = screen_id_;
        spdlog::warn("[screen] capture failed, reopening id={}", id);
        if (id.empty() || !open(id)) {
            return PollResult::kFailed;
        }
        return PollResult::kRestarted;
    }

    ScreenDesktopFrame frame() const override {
        ScreenDesktopFrame out;
        out.width = width_;
        out.height = height_;
        out.bgra = have_frame_ ? bgra_.data() : nullptr;
        out.origin_x = origin_x_;
        out.origin_y = origin_y_;
        out.origin_right = origin_right_;
        out.origin_bottom = origin_bottom_;
        return out;
    }

    bool has_frame() const override { return have_frame_; }

    bool consume_cursor_change() override { return false; }

    void reset_cursor_tracking() override {}

    void draw_cursor(uint8_t* bgra, int width, int height, int origin_x,
                     int origin_y, int origin_right, int origin_bottom) override {
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
        int dst_w = 0;
        int dst_h = 0;
        FitOutputSize(src_w, src_h, &dst_w, &dst_h);
        if (have_frame_ && (dst_w != width_ || dst_h != height_)) {
            size_changed_ = true;
        }
        bgra_.resize(static_cast<size_t>(dst_w) * dst_h * 4);
        ScaleBgraNearest(frame->data(), frame->stride(), src_w, src_h,
                         bgra_.data(), dst_w, dst_h);
        width_ = dst_w;
        height_ = dst_h;
        origin_x_ = frame->top_left().x();
        origin_y_ = frame->top_left().y();
        origin_right_ = origin_x_ + src_w;
        origin_bottom_ = origin_y_ + src_h;
        have_frame_ = true;
        got_frame_ = true;
    }

private:
    std::unique_ptr<webrtc::DesktopCapturer> capturer_;
    std::string screen_id_;
    webrtc::DesktopCapturer::SourceId source_id_ = webrtc::kInvalidScreenId;
    std::vector<uint8_t> bgra_;
    int width_ = 0;
    int height_ = 0;
    int origin_x_ = 0;
    int origin_y_ = 0;
    int origin_right_ = 0;
    int origin_bottom_ = 0;
    bool have_frame_ = false;
    bool got_frame_ = false;
    bool size_changed_ = false;
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
