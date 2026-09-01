#ifndef WIDGET_H
#define WIDGET_H

#include <QWidget>
#include <QImage>
#include <QGraphicsScene>
#include <QGraphicsPixmapItem>
#include <mutex>
#include <vector>
#include <iostream>
#include <xrtc/ixrtc_engine.h>
#include <xrtc/ixrtc_media_source.h>
#include <xrtc/xrtc_defines.h>

QT_BEGIN_NAMESPACE
namespace Ui {
class Widget;
}
QT_END_NAMESPACE

/**
 * @brief 主界面窗口（XRTC Janus 1:1 视频通话演示）
 *
 * 职责：
 *  - 展示本地摄像头预览与远端视频画面；
 *  - 提供加入/离开会议、开关本地预览等交互按钮；
 *  - 实现 XRtcEngineObserver 回调，将引擎层事件（加入结果、
 *    连接状态、远端用户进出、视频帧等）更新到界面上。
 *
 * 线程模型：
 *  - 视频帧回调可能来自引擎的采集/渲染线程，本类通过
 *    “互斥锁 + 待处理图像 + QMetaObject 队列调度” 的方式，
 *    把图像安全地切回 UI 线程再渲染，避免直接在回调里操作控件。
 */
class Widget : public QWidget, public xrtc::XRtcEngineObserver {
    Q_OBJECT

public:
    /// 构造函数：创建引擎、初始化 UI、预览场景和设备列表
    explicit Widget(QWidget* parent = nullptr);
    /// 析构函数：离开会议、释放视频源和引擎资源
    ~Widget() override;

protected:
    /// 窗口尺寸变化时，让画面按比例自适应显示区域
    void resizeEvent(QResizeEvent* event) override;

private:
    /// 枚举本地摄像头 / 音频设备，并填充到下拉框中
    void init_device_list();
    /// 连接按钮的点击信号与对应槽函数
    void init_connection();
    /// 初始化本地预览与远端预览所用的 QGraphicsScene / Item
    void init_preview();
    /// 清空本地预览（置空待渲染图像并清空画面）
    void clear_preview();
    /// 清空远端预览（置空待渲染图像并清空画面）
    void clear_remote_preview();
    /// 在 UI 线程渲染本地预览帧（由 on_video_frame 调度触发）
    void render_preview_frame();
    /// 在 UI 线程渲染远端预览帧（由 on_remote_video_frame 调度触发）
    void render_remote_preview_frame();
    /// 从界面读取当前采集请求并同步到 engine
    xrtc::XRTCVideoCaptureRequest current_video_request() const;
    /// 刷新「实际格式」标签（可传入设备 id；为空则仅显示请求值）
    void refresh_actual_format_label(const std::string& device_id = {});

    // ---------- XRtcEngineObserver 回调（引擎线程） ----------

    /// 视频采集源启动完成回调
    void video_source_start_event(xrtc::IXRtcMediaSource* video_source,
                                  xrtc::XRtcError error) override;
    /// 视频采集源停止回调
    void video_source_stop_event(xrtc::IXRtcMediaSource* video_source,
                                 xrtc::XRtcError error) override;
    /// 本地视频帧回调：拷贝一帧图像待 UI 线程渲染
    void on_video_frame(xrtc::IXRtcMediaSource* video_source,
                        const xrtc::XRTCVideoFrame& frame) override;
    /// 加入会议结果回调
    void on_join_result(xrtc::XRtcError error,
                        const std::string& message) override;
    /// 离开会议回调
    void on_leave(xrtc::XRtcError error) override;
    /// 连接状态变化回调
    void on_connection_state(xrtc::XRTCConnectionState state) override;
    /// 远端用户加入回调
    void on_remote_user_joined(const xrtc::XRTCRemoteUser& user) override;
    /// 远端用户离开回调
    void on_remote_user_left(const xrtc::XRTCRemoteUser& user) override;
    /// 远端视频帧回调：拷贝一帧图像待 UI 线程渲染
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

    std::vector<xrtc::XRTCDeviceInfo> video_devices_;  ///< 摄像头设备列表
    std::vector<xrtc::XRTCDeviceInfo> audio_devices_;  ///< 麦克风设备列表

    // ---- 本地预览相关 ----
    QGraphicsScene* preview_scene_ = nullptr;      ///< 本地预览场景
    QGraphicsPixmapItem* preview_item_ = nullptr;  ///< 本地预览图像项
    std::mutex preview_mutex_;                     ///< 保护 pending_preview_ 的锁
    QImage pending_preview_;                       ///< 待渲染的本地帧（引擎线程写入）
    bool preview_scheduled_ = false;               ///< 是否已排队一次渲染任务

    // ---- 远端预览相关 ----
    QGraphicsScene* remote_scene_ = nullptr;       ///< 远端预览场景
    QGraphicsPixmapItem* remote_item_ = nullptr;   ///< 远端预览图像项
    std::mutex remote_mutex_;                      ///< 保护 pending_remote_ 的锁
    QImage pending_remote_;                        ///< 待渲染的远端帧（引擎线程写入）
    bool remote_scheduled_ = false;                ///< 是否已排队一次渲染任务
};
#endif  // WIDGET_H
