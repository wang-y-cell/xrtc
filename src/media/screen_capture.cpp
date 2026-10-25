#include <media/screen_capture.h>

#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "libyuv/convert.h"
#include "libyuv/scale.h"
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

constexpr int kKeepAliveMs = 1000;
constexpr int kMaxPreviewLongEdge = 640;

/// 按长边上限计算输出宽高（保持比例、取偶数）。
/// max_long_edge<=0 表示不缩放。
void ComputeOutputSize(int src_w, int src_h, int max_long_edge, int* out_w,
                       int* out_h) {
    int w = src_w;
    int h = src_h;
    const int long_edge = std::max(w, h);
    if (max_long_edge > 0 && long_edge > max_long_edge && long_edge > 0) {
        if (w >= h) {
            w = max_long_edge;
            h = std::max(2, src_h * max_long_edge / src_w);
        } else {
            h = max_long_edge;
            w = std::max(2, src_w * max_long_edge / src_h);
        }
    }
    w &= ~1;
    h &= ~1;
    *out_w = std::max(2, w);
    *out_h = std::max(2, h);
}

}  // namespace

ScreenCapture::ScreenCapture(std::string screen_id, int max_long_edge, int fps)
    : screen_id_(std::move(screen_id)),
      fps_(std::clamp(fps, 1, 60)),
      max_long_edge_(max_long_edge <= 0 ? 0 : std::max(320, max_long_edge)) {}

ScreenCapture::~ScreenCapture() {
    stop();
}

std::vector<XRTCScreenInfo> ScreenCapture::get_screen_info() {
    return list_screens();
}

std::unique_ptr<ScreenCapture> ScreenCapture::Create(const std::string& screen_id,
                                                     int max_long_edge,
                                                     int fps) {
    const std::string resolved = resolve_screen_id(screen_id);
    if (resolved.empty()) {
        spdlog::error("[screen] no monitor for id='{}'", screen_id);
        return nullptr;
    }
    return std::unique_ptr<ScreenCapture>(
        new ScreenCapture(resolved, max_long_edge, fps));
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
    spdlog::info("[screen] capture started id={} {}x{}@{} edge_cap={}",
                 screen_id_, out_width_, out_height_, fps_, max_long_edge_);
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
    if (!grabber_ || !grabber_->open(screen_id_, fps_)) {
        return false;
    }
    const ScreenDesktopFrame desktop = grabber_->frame();
    refresh_output_size(desktop.width, desktop.height);
    std::lock_guard<std::mutex> lock(cb_mu_);
    return out_width_ > 0 && out_height_ > 0;
}

void ScreenCapture::refresh_output_size(int src_w, int src_h) {
    int w = 0;
    int h = 0;
    ComputeOutputSize(src_w, src_h, max_long_edge_, &w, &h);
    std::lock_guard<std::mutex> lock(cb_mu_);
    out_width_ = w;
    out_height_ = h;
}

void ScreenCapture::capture_loop() {
    if (!grabber_) {
        return;
    }
    const auto frame_interval =
        std::chrono::milliseconds(std::max(1, 1000 / fps_));
    auto last_push = std::chrono::steady_clock::now() -
                     std::chrono::milliseconds(kKeepAliveMs);

    while (!stop_.load()) {
        const auto tick = std::chrono::steady_clock::now();
        const ScreenGrabber::PollResult polled = grabber_->poll(50);
        if (polled == ScreenGrabber::PollResult::kFailed) {
            break;
        }
        if (polled == ScreenGrabber::PollResult::kRestarted) {
            grabber_->reset_cursor_tracking();
            const ScreenDesktopFrame desktop = grabber_->frame();
            refresh_output_size(desktop.width, desktop.height);
            continue;
        }

        const bool desktop_changed =
            polled == ScreenGrabber::PollResult::kNewFrame;
        const ScreenDesktopFrame desktop = grabber_->frame();
        const auto now = std::chrono::steady_clock::now();
        const bool keepalive =
            grabber_->has_frame() &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_push)
                    .count() >= kKeepAliveMs;
        if (grabber_->has_frame() && desktop.bgra &&
            (desktop_changed || keepalive)) {
            const int src_w = desktop.width;
            const int src_h = desktop.height;
            int dst_w = 0;
            int dst_h = 0;
            ComputeOutputSize(src_w, src_h, max_long_edge_, &dst_w, &dst_h);
            if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
                continue;
            }

            // 源分辨率 I420 中间缓冲（采集线程私有，可复用）
            if (!full_buffer_ || full_width_ != src_w || full_height_ != src_h) {
                full_buffer_ = webrtc::I420Buffer::Create(src_w, src_h);
                full_width_ = src_w;
                full_height_ = src_h;
            }
            // libyuv SIMD：BGRA(小端 ARGB) → I420，替代逐像素手写循环
            libyuv::ARGBToI420(desktop.bgra, src_w * 4,
                               full_buffer_->MutableDataY(),
                               full_buffer_->StrideY(),
                               full_buffer_->MutableDataU(),
                               full_buffer_->StrideU(),
                               full_buffer_->MutableDataV(),
                               full_buffer_->StrideV(), src_w, src_h);

            // 需要缩小时用双线性（不是最近邻），文字边缘明显更稳
            webrtc::scoped_refptr<webrtc::I420BufferInterface> view =
                full_buffer_;
            if (dst_w != src_w || dst_h != src_h) {
                auto scaled = webrtc::I420Buffer::Create(dst_w, dst_h);
                libyuv::I420Scale(
                    full_buffer_->DataY(), full_buffer_->StrideY(),
                    full_buffer_->DataU(), full_buffer_->StrideU(),
                    full_buffer_->DataV(), full_buffer_->StrideV(), src_w,
                    src_h, scaled->MutableDataY(), scaled->StrideY(),
                    scaled->MutableDataU(), scaled->StrideU(),
                    scaled->MutableDataV(), scaled->StrideV(), dst_w, dst_h,
                    libyuv::kFilterBilinear);
                view = scaled;
            }

            const webrtc::VideoFrame frame =
                webrtc::VideoFrame::Builder()
                    .set_video_frame_buffer(view)
                    .set_timestamp_rtp(0)
                    .set_timestamp_ms(webrtc::TimeMillis())
                    .build();

            webrtc::scoped_refptr<XrtcVideoTrackSource> track;
            bool preview = false;
            {
                std::lock_guard<std::mutex> lock(cb_mu_);
                track = track_source_;
                preview = preview_enabled_;
                out_width_ = view->width();
                out_height_ = view->height();
            }
            if (track) {
                track->PushFrame(frame);
            }
            if (preview) {
                if (auto* observer = XRtcGlobal::instance().observer()) {
                    // 本地预览只用于自看，压到 640 省 CPU；不影响推流分辨率
                    webrtc::scoped_refptr<webrtc::I420BufferInterface> view_buf =
                        view;
                    const int long_edge =
                        std::max(view_buf->width(), view_buf->height());
                    if (long_edge > kMaxPreviewLongEdge) {
                        int pw = view_buf->width();
                        int ph = view_buf->height();
                        if (pw >= ph) {
                            pw = kMaxPreviewLongEdge;
                            ph = std::max(2, view_buf->height() *
                                                 kMaxPreviewLongEdge /
                                                 view_buf->width());
                        } else {
                            ph = kMaxPreviewLongEdge;
                            pw = std::max(2, view_buf->width() *
                                                 kMaxPreviewLongEdge /
                                                 view_buf->height());
                        }
                        pw &= ~1;
                        ph &= ~1;
                        auto scaled = webrtc::I420Buffer::Create(pw, ph);
                        scaled->ScaleFrom(*view_buf);
                        view_buf = scaled;
                    }
                    XRTCVideoFrame out =
                        MakeXRTCVideoFrame(std::move(view_buf), frame);
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
