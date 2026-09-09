#ifndef WIDGET_H
#define WIDGET_H

#include <QLabel>
#include <QWidget>
#include <iostream>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <xrtc/ixrtc_engine.h>
#include <xrtc/ixrtc_media_source.h>
#include <xrtc/xrtc_defines.h>

class YuvGlWidget;

QT_BEGIN_NAMESPACE
namespace Ui {
class Widget;
}
QT_END_NAMESPACE

/**
 * @brief 主界面窗口（XRTC Janus 多人视频通话演示）
 *
 * 本地/远端预览使用 YuvGlWidget（Qt OpenGL）直接上传 I420。
 * 视频帧回调经互斥锁 + 队列调度切回 UI 线程再交给 GL 控件。
 */
class Widget : public QWidget, public xrtc::XRtcEngineObserver {
    Q_OBJECT

public:
    explicit Widget(QWidget* parent = nullptr);
    ~Widget() override;

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    struct RemoteViewUi {
        QWidget* container = nullptr;
        QLabel* name_label = nullptr;
        YuvGlWidget* gl = nullptr;
        QString display;
    };

    struct RemotePending {
        xrtc::XRTCVideoFrame frame;
        bool scheduled = false;
    };

    struct LocalPending {
        xrtc::XRTCVideoFrame frame;
    };

    void init_device_list();
    void on_speaker_changed(int index);
    void init_connection();
    void init_preview();
    void clear_preview();
    void ensure_remote_view(uint64_t feed_id, const QString& display);
    void remove_remote_view(uint64_t feed_id);
    void clear_all_remote_views();
    void render_preview_frame();
    void render_remote_preview_frame(uint64_t feed_id);
    xrtc::XRTCVideoCaptureRequest current_video_request() const;
    void refresh_actual_format_label(const std::string& device_id = {});
    bool handle_rest(const xrtc::Rest<>& st, const QString& fail_text);

    void video_source_start_event(xrtc::IXRtcMediaSource* video_source,
                                  xrtc::XRtcError error) override;
    void video_source_stop_event(xrtc::IXRtcMediaSource* video_source,
                                 xrtc::XRtcError error) override;
    void on_video_frame(xrtc::IXRtcMediaSource* video_source,
                        const xrtc::XRTCVideoFrame& frame) override;
    void on_join_result(xrtc::XRtcError error,
                        const std::string& message) override;
    void on_leave(xrtc::XRtcError error) override;
    void on_connection_state(xrtc::XRTCConnectionState state) override;
    void on_remote_user_joined(const xrtc::XRTCRemoteUser& user) override;
    void on_remote_user_left(const xrtc::XRTCRemoteUser& user) override;
    void on_remote_video_frame(uint64_t feed_id,
                               const xrtc::XRTCVideoFrame& frame) override;
    void on_audio_level(xrtc::IXRtcMediaSource* audio_source,
                        int level) override;

private slots:
    void start_video_source();
    void start_audio_source();
    void join_meeting();
    void leave_meeting();
    void toggle_meeting_video();
    void toggle_meeting_audio();
    void on_video_request_changed();

private:
    void set_meeting_media_buttons(bool in_meeting);
    void reset_meeting_media_button_labels();

    Ui::Widget* ui;
    xrtc::IXRtcEngine* engine = nullptr;
    xrtc::IXRtcMediaSource* video_source = nullptr;
    xrtc::IXRtcMediaSource* audio_source = nullptr;
    bool in_meeting_ = false;
    bool meeting_video_on_ = false;
    bool meeting_audio_on_ = false;

    std::vector<xrtc::XRTCDeviceInfo> video_devices_;
    std::vector<xrtc::XRTCDeviceInfo> audio_devices_;
    std::vector<xrtc::XRTCDeviceInfo> playout_devices_;

    std::mutex preview_mutex_;
    LocalPending pending_preview_;
    bool preview_scheduled_ = false;

    std::unordered_map<uint64_t, RemoteViewUi> remote_views_;
    std::mutex remote_mutex_;
    std::unordered_map<uint64_t, RemotePending> remote_pending_;
};
#endif  // WIDGET_H
