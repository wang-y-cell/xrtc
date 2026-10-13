#include <media/screen_capture.h>

#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "rtc_base/time_utils.h"

#include <engine/xrtc_global.h>
#include <media/i420_frame.h>
#include <platform/screen_grabber.h>
#include <xrtc/ixrtc_engine.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <future>
#include <vector>

namespace xrtc {
namespace {

constexpr int kCaptureFps = 15;
constexpr int kKeepAliveMs = 1000;

uint8_t ClampToByte(int v) {
    if (v < 0) {
        return 0;
    }
    if (v > 255) {
        return 255;
    }
    return static_cast<uint8_t>(v);
}

void BgraToI420(const uint8_t* src, int src_stride, int width, int height,
                uint8_t* dst_y, int y_stride, uint8_t* dst_u, int u_stride,
                uint8_t* dst_v, int v_stride) {
    for (int y = 0; y < height; y += 2) {
        for (int x = 0; x < width; x += 2) {
            int sum_u = 0;
            int sum_v = 0;
            for (int dy = 0; dy < 2; ++dy) {
                const uint8_t* row =
                    src + static_cast<size_t>(y + dy) * src_stride +
                    static_cast<size_t>(x) * 4;
                uint8_t* yrow = dst_y + static_cast<size_t>(y + dy) * y_stride + x;
                for (int dx = 0; dx < 2; ++dx) {
                    const int b = row[dx * 4 + 0];
                    const int g = row[dx * 4 + 1];
                    const int r = row[dx * 4 + 2];
                    yrow[dx] =
                        ClampToByte(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
                    sum_u += ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                    sum_v += ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                }
            }
            dst_u[static_cast<size_t>(y / 2) * u_stride + x / 2] =
                ClampToByte(sum_u / 4);
            dst_v[static_cast<size_t>(y / 2) * v_stride + x / 2] =
                ClampToByte(sum_v / 4);
        }
    }
}

}  // namespace

ScreenCapture::ScreenCapture(std::string screen_id)
    : screen_id_(std::move(screen_id)), fps_(kCaptureFps) {}

ScreenCapture::~ScreenCapture() {
    stop();
}

std::vector<XRTCScreenInfo> ScreenCapture::get_screen_info() {
    return list_screens();
}

std::unique_ptr<ScreenCapture> ScreenCapture::Create(const std::string& screen_id) {
    const std::string resolved = resolve_screen_id(screen_id);
    if (resolved.empty()) {
        spdlog::error("[screen] no monitor for id='{}'", screen_id);
        return nullptr;
    }
    return std::unique_ptr<ScreenCapture>(new ScreenCapture(resolved));
}

void ScreenCapture::set_track_source(
    webrtc::scoped_refptr<XrtcVideoTrackSource> track_source) {
    std::lock_guard<std::mutex> lock(cb_mu_);
    track_source_ = std::move(track_source);
}

void ScreenCapture::set_local_preview_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(cb_mu_);
    preview_enabled_ = enabled;
}

XRTCVideoFormat ScreenCapture::capture_format() const {
    std::lock_guard<std::mutex> lock(cb_mu_);
    return XRTCVideoFormat{out_width_, out_height_, fps_};
}

Rest<> ScreenCapture::start() {
    std::lock_guard<std::mutex> lock(life_mu_);
    return start_locked();
}

Rest<> ScreenCapture::stop() {
    std::lock_guard<std::mutex> lock(life_mu_);
    stop_locked();
    return xrtc_ok();
}

Rest<> ScreenCapture::device_switch(const std::string& device_id) {
    std::lock_guard<std::mutex> lock(life_mu_);
    const std::string resolved = resolve_screen_id(device_id);
    if (resolved.empty()) {
        spdlog::error("[screen] device_switch: unknown id='{}'", device_id);
        return xrtc_err(XRtcError::kInvalidParam);
    }
    if (resolved == screen_id_) {
        return xrtc_ok();
    }
    const bool was_running = running_;
    if (was_running) {
        stop_locked();
    }
    screen_id_ = resolved;
    if (was_running) {
        return start_locked();
    }
    return xrtc_ok();
}

Rest<> ScreenCapture::start_locked() {
    if (running_) {
        return xrtc_ok();
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    stop_.store(false);
    std::promise<bool> ready;
    std::future<bool> opened = ready.get_future();
    thread_ = std::thread([this, ready = std::move(ready)]() mutable {
        const bool ok = open_grabber();
        ready.set_value(ok);
        if (!ok) {
            close_grabber();
            return;
        }
        capture_loop();
        close_grabber();
    });
    const bool ok = opened.get();
    if (!ok) {
        if (thread_.joinable()) {
            thread_.join();
        }
        if (auto* observer = XRtcGlobal::instance().observer()) {
            observer->video_source_start_event(this, XRtcError::kMediaStartFailed);
        }
        return xrtc_err(XRtcError::kMediaStartFailed);
    }
    running_ = true;
    {
        webrtc::scoped_refptr<XrtcVideoTrackSource> track;
        {
            std::lock_guard<std::mutex> lock(cb_mu_);
            track = track_source_;
        }
        if (track) {
            track->SetLive(true);
        }
    }
    if (auto* observer = XRtcGlobal::instance().observer()) {
        observer->video_source_start_event(this, XRtcError::kNOERROR);
    }
    spdlog::info("[screen] capture started id={}", screen_id_);
    return xrtc_ok();
}

void ScreenCapture::stop_locked() {
    const bool was_running = running_;
    stop_.store(true);
    if (thread_.joinable()) {
        thread_.join();
    }
    running_ = false;
    close_grabber();
    if (!was_running) {
        return;
    }
    webrtc::scoped_refptr<XrtcVideoTrackSource> track;
    {
        std::lock_guard<std::mutex> lock(cb_mu_);
        track = track_source_;
    }
    if (track) {
        track->SetLive(false);
    }
    if (auto* observer = XRtcGlobal::instance().observer()) {
        observer->video_source_stop_event(this, XRtcError::kNOERROR);
    }
}

void ScreenCapture::close_grabber() {
    if (grabber_) {
        grabber_->close();
    }
}

bool ScreenCapture::open_grabber() {
    if (!grabber_) {
        grabber_ = create_screen_grabber();
    }
    if (!grabber_ || !grabber_->open(screen_id_)) {
        return false;
    }
    const ScreenDesktopFrame desktop = grabber_->frame();
    std::lock_guard<std::mutex> lock(cb_mu_);
    out_width_ = desktop.width;
    out_height_ = desktop.height;
    return desktop.width > 0 && desktop.height > 0;
}

void ScreenCapture::capture_loop() {
    if (!grabber_) {
        return;
    }
    const auto frame_interval = std::chrono::milliseconds(std::max(1, 1000 / fps_));
    auto last_push =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(kKeepAliveMs);

    while (!stop_.load()) {
        const auto tick = std::chrono::steady_clock::now();
        const ScreenGrabber::PollResult polled = grabber_->poll(50);
        if (polled == ScreenGrabber::PollResult::kFailed) {
            break;
        }
        if (polled == ScreenGrabber::PollResult::kRestarted) {
            grabber_->reset_cursor_tracking();
            const ScreenDesktopFrame desktop = grabber_->frame();
            std::lock_guard<std::mutex> lock(cb_mu_);
            out_width_ = desktop.width;
            out_height_ = desktop.height;
            continue;
        }

        const bool desktop_changed = polled == ScreenGrabber::PollResult::kNewFrame;
        const bool cursor_changed = grabber_->consume_cursor_change();
        const ScreenDesktopFrame desktop = grabber_->frame();
        const auto now = std::chrono::steady_clock::now();
        const bool keepalive =
            grabber_->has_frame() &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_push)
                    .count() >= kKeepAliveMs;
        if (grabber_->has_frame() && desktop.bgra &&
            (desktop_changed || cursor_changed || keepalive)) {
            std::vector<uint8_t> painted(
                desktop.bgra,
                desktop.bgra + static_cast<size_t>(desktop.width) * desktop.height * 4);
            grabber_->draw_cursor(painted.data(), desktop.width, desktop.height,
                                  desktop.origin_x, desktop.origin_y,
                                  desktop.origin_right, desktop.origin_bottom);
            auto buffer = webrtc::I420Buffer::Create(desktop.width, desktop.height);
            BgraToI420(painted.data(), desktop.width * 4, desktop.width, desktop.height,
                       buffer->MutableDataY(), buffer->StrideY(), buffer->MutableDataU(),
                       buffer->StrideU(), buffer->MutableDataV(), buffer->StrideV());

            const webrtc::VideoFrame frame =
                webrtc::VideoFrame::Builder()
                    .set_video_frame_buffer(buffer)
                    .set_timestamp_rtp(0)
                    .set_timestamp_ms(webrtc::TimeMillis())
                    .build();

            webrtc::scoped_refptr<XrtcVideoTrackSource> track;
            bool preview = false;
            {
                std::lock_guard<std::mutex> lock(cb_mu_);
                track = track_source_;
                preview = preview_enabled_;
            }
            if (track) {
                track->PushFrame(frame);
            }
            if (preview) {
                if (auto* observer = XRtcGlobal::instance().observer()) {
                    webrtc::scoped_refptr<webrtc::I420BufferInterface> view = buffer;
                    constexpr int kMaxPreviewLongEdge = 640;
                    const int long_edge = std::max(view->width(), view->height());
                    if (long_edge > kMaxPreviewLongEdge) {
                        int pw = view->width();
                        int ph = view->height();
                        if (pw >= ph) {
                            pw = kMaxPreviewLongEdge;
                            ph = std::max(2, view->height() * kMaxPreviewLongEdge /
                                                 view->width());
                        } else {
                            ph = kMaxPreviewLongEdge;
                            pw = std::max(2, view->width() * kMaxPreviewLongEdge /
                                                 view->height());
                        }
                        pw &= ~1;
                        ph &= ~1;
                        auto scaled = webrtc::I420Buffer::Create(pw, ph);
                        scaled->ScaleFrom(*view);
                        view = scaled;
                    }
                    XRTCVideoFrame out = MakeXRTCVideoFrame(std::move(view), frame);
                    out.rotation_degrees = 0;
                    observer->on_video_frame(this, out);
                }
            }
            last_push = now;
        }

        const auto spent = std::chrono::steady_clock::now() - tick;
        if (spent < frame_interval) {
            std::this_thread::sleep_for(frame_interval - spent);
        }
    }
}

}  // namespace xrtc
