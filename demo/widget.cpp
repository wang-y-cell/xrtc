#include "widget.h"
#include "ui_widget.h"

#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QResizeEvent>
#include <QString>

#include <spdlog/spdlog.h>

namespace {
// 按钮样式：停止时红色、启动时蓝色
const QString btnStop = "background-color:#d84a38";
const QString btnStart = "background-color:#007aac";

/// 将连接状态枚举转换为可读字符串，用于状态栏显示
const char* ConnectionStateName(xrtc::XRTCConnectionState state) {
    switch (state) {
        case xrtc::XRTCConnectionState::kNew:
            return "new";
        case xrtc::XRTCConnectionState::kConnecting:
            return "connecting";
        case xrtc::XRTCConnectionState::kConnected:
            return "connected";
        case xrtc::XRTCConnectionState::kDisconnected:
            return "disconnected";
        case xrtc::XRTCConnectionState::kFailed:
            return "failed";
        case xrtc::XRTCConnectionState::kClosed:
            return "closed";
    }
    return "unknown";
}
}  // namespace

/**
 * @brief 构造函数
 * 创建 XRTC 引擎（以本窗口作为观察者接收回调），
 * 然后依次初始化界面、预览场景、按钮连接和设备列表。
 */
Widget::Widget(QWidget* parent)
    : QWidget(parent),
      ui(new Ui::Widget),
      engine(xrtc::create_xrtc_engine(this)) {
    ui->setupUi(this);
    ui->video_capture->setStyleSheet(btnStart);
    init_preview();
    init_connection();
    init_device_list();
}

/**
 * @brief 连接界面按钮的点击信号到对应的槽函数
 */
void Widget::init_connection() {
    connect(ui->video_capture, &QPushButton::clicked, this,
            &Widget::start_video_source);
    connect(ui->btn_join, &QPushButton::clicked, this, &Widget::join_meeting);
    connect(ui->btn_leave, &QPushButton::clicked, this, &Widget::leave_meeting);
}

/**
 * @brief 初始化预览窗口界面
 * 为本地预览和远端预览分别创建 QGraphicsScene 和
 * 一个空的 QGraphicsPixmapItem，后续把视频帧设置到 Item 上即可显示。
 */
void Widget::init_preview() {
    preview_scene_ = new QGraphicsScene(this);
    ui->display->setScene(preview_scene_);
    ui->display->setRenderHint(QPainter::SmoothPixmapTransform);
    preview_item_ = preview_scene_->addPixmap(QPixmap());

    remote_scene_ = new QGraphicsScene(this);
    ui->remote_display->setScene(remote_scene_);
    ui->remote_display->setRenderHint(QPainter::SmoothPixmapTransform);
    remote_item_ = remote_scene_->addPixmap(QPixmap());
}

/**
 * @brief 清空本地预览
 * 加锁清掉待渲染帧，并清空画面上的图像。
 */
void Widget::clear_preview() {
    {
        std::lock_guard<std::mutex> lock(preview_mutex_);
        pending_preview_ = QImage();
        preview_scheduled_ = false;
    }
    if (preview_item_) {
        preview_item_->setPixmap(QPixmap());
    }
}

/**
 * @brief 清空远端预览
 * 加锁清掉待渲染帧，并清空画面上的图像。
 */
void Widget::clear_remote_preview() {
    {
        std::lock_guard<std::mutex> lock(remote_mutex_);
        pending_remote_ = QImage();
        remote_scheduled_ = false;
    }
    if (remote_item_) {
        remote_item_->setPixmap(QPixmap());
    }
}

/**
 * @brief 初始化设备列表
 * 从引擎获取摄像头和音频设备信息，填充到对应的下拉框中。
 */
void Widget::init_device_list() {
    video_devices_ = engine->get_video_device_info();
    for (const auto& device : video_devices_) {
        ui->camera_list->addItem(QString::fromUtf8(device.device_name.c_str()));
    }

    std::vector<xrtc::XRTCDeviceInfo> audio_device_info =
        engine->get_audio_device_info();
    for (const auto& device : audio_device_info) {
        ui->audio_list->addItem(QString::fromUtf8(device.device_name.c_str()));
    }
}

/**
 * @brief 析构函数
 * 依次离开会议、停止并销毁本地视频源、销毁引擎、释放界面对象。
 */
Widget::~Widget() {
    if (engine) {
        engine->leave();
    }
    if (video_source) {
        video_source->stop();
        engine->destroy_video_source(video_source);
        video_source = nullptr;
    }
    xrtc::destroy_xrtc_engine(engine);
    engine = nullptr;
    delete ui;
}

/**
 * @brief 窗口尺寸变化事件
 * 若当前有画面，则让画面按等比缩放适配新的显示区域。
 */
void Widget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (preview_item_ && !preview_item_->pixmap().isNull()) {
        ui->display->fitInView(preview_item_, Qt::KeepAspectRatio);
    }
    if (remote_item_ && !remote_item_->pixmap().isNull()) {
        ui->remote_display->fitInView(remote_item_, Qt::KeepAspectRatio);
    }
}

/**
 * @brief 槽：开启 / 停止本地视频预览（按钮点击）
 *
 * 如果当前没有视频源：校验所选摄像头 -> 创建视频源并启动；
 * 如果已有视频源：停止并销毁视频源，清空预览画面，恢复按钮样式。
 */
void Widget::start_video_source() {
    if (in_meeting_) {
        return;
    }
    ui->video_capture->setDisabled(true);
    if (!video_source) {
        const int index = ui->camera_list->currentIndex();
        if (index < 0 || index >= static_cast<int>(video_devices_.size())) {
            QMessageBox::warning(this, "Warning", "No camera device");
            ui->video_capture->setDisabled(false);
            return;
        }
        xrtc::ixrtc_video_config video_config;
        video_config.device_id = video_devices_[index].device_id;
        if (video_config.device_id.empty()) {
            QMessageBox::warning(this, "Warning", "Empty camera device id");
            ui->video_capture->setDisabled(false);
            return;
        }
        video_source = engine->create_video_source(video_config);
        if (!video_source) {
            QMessageBox::warning(this, "Warning", "Failed to create video source");
            ui->video_capture->setDisabled(false);
            return;
        }
        video_source->start();
    } else {
        video_source->stop();
        engine->destroy_video_source(video_source);
        video_source = nullptr;
        clear_preview();
        ui->video_capture->setStyleSheet(btnStart);
        ui->video_capture->setText(QString::fromUtf8("开启本地预览"));
    }
    ui->video_capture->setDisabled(false);
}

/**
 * @brief 槽：加入会议
 * join 会自行采集；若已有预览源先停掉，避免双开摄像头。
 * 然后从界面控件读取配置并调用引擎的 join 接口。
 */
void Widget::join_meeting() {
    // 一进房流程即禁用预览，避免会话采集回调改掉按钮文案
    in_meeting_ = true;
    ui->video_capture->setEnabled(false);

    // join 会自行采集；若已有预览源先停掉，避免双开摄像头
    if (video_source) {
        video_source->stop();
        engine->destroy_video_source(video_source);
        video_source = nullptr;
        clear_preview();
    }

    xrtc::XRTCJoinConfig config;
    // Windows 下 toStdString() 可能走本地代码页；URL/名字用 UTF-8
    config.janus_ws_url =
        ui->janus_url->text().trimmed().toUtf8().constData();
    config.room_id = ui->room_id->text().trimmed().toULongLong();
    config.display_name =
        ui->display_name->text().trimmed().toUtf8().constData();
    config.create_room_if_missing = ui->create_room_if_missing->isChecked();
    config.room_description = config.display_name.empty()
                                  ? ("room-" + std::to_string(config.room_id))
                                  : (config.display_name + "'s room");
    const int index = ui->camera_list->currentIndex();
    if (index >= 0 && index < static_cast<int>(video_devices_.size())) {
        config.video_device_id = video_devices_[index].device_id;
    }

    // 本机 coturn（勿留空，否则会退回 Google STUN）
    config.ice_servers.push_back(
        {"stun:8.153.155.18:3478", "", ""});
    config.ice_servers.push_back(
        {"turn:8.153.155.18:3478", "wang_y", "wang_y"});
    config.ice_servers.push_back(
        {"turn:8.153.155.18:3478?transport=tcp", "wang_y", "wang_y"});

    spdlog::info("[ui] join_meeting url={} room={} name={} create_if_missing={} ice={}",
                 config.janus_ws_url, config.room_id, config.display_name,
                 config.create_room_if_missing, config.ice_servers.size());
    ui->status_label->setText(QString::fromUtf8("状态: joining..."));
    ui->btn_join->setEnabled(false);
    engine->join(config);
}

/**
 * @brief 槽：离开会议
 * 通知引擎离开当前会议，并清空远端画面、恢复界面状态。
 */
void Widget::leave_meeting() {
    engine->leave();
    clear_remote_preview();
    in_meeting_ = false;
    ui->video_capture->setEnabled(true);
    ui->btn_join->setEnabled(true);
    ui->status_label->setText(QString::fromUtf8("状态: left"));
}

/**
 * @brief 回调：视频采集源启动完成
 * 通过队列方式切回 UI 线程，成功则切换为“停止预览”按钮，
 * 失败则弹出提示框。
 */
void Widget::video_source_start_event(xrtc::IXRtcMediaSource*,
                                      xrtc::XRtcError error) {
    QMetaObject::invokeMethod(
        this,
        [this, error]() {
            // 会议内采集也会回调此处，不改预览按钮文案
            if (in_meeting_) {
                return;
            }
            if (error == xrtc::XRtcError::kNOERROR) {
                ui->video_capture->setStyleSheet(btnStop);
                ui->video_capture->setText(QString::fromUtf8("停止本地预览"));
            } else {
                QMessageBox::warning(
                    this, "Warning",
                    QString("本地预览启动失败: %1")
                        .arg(static_cast<int>(error)));
            }
        },
        Qt::QueuedConnection);
}

/**
 * @brief 回调：视频采集源停止
 * 切回 UI 线程，成功后恢复“开启预览”按钮并清空预览画面。
 */
void Widget::video_source_stop_event(xrtc::IXRtcMediaSource*,
                                     xrtc::XRtcError error) {
    QMetaObject::invokeMethod(
        this,
        [this, error]() {
            if (in_meeting_) {
                return;
            }
            if (error == xrtc::XRtcError::kNOERROR) {
                ui->video_capture->setStyleSheet(btnStart);
                ui->video_capture->setText(QString::fromUtf8("开启本地预览"));
                clear_preview();
            }
        },
        Qt::QueuedConnection);
}

/**
 * @brief 回调：本地视频帧到达（可能来自非 UI 线程）
 * 校验帧数据后拷贝一份 ARGB 图像，加锁放入待渲染队列；
 * 若当前没有排队的渲染任务，则通过队列方式触发一次
 * render_preview_frame，保证渲染始终在 UI 线程执行。
 */
void Widget::on_video_frame(xrtc::IXRtcMediaSource*,
                            const xrtc::XRTCVideoFrame& frame) {
    if (!frame.argb || frame.width <= 0 || frame.height <= 0) {
        return;
    }

    QImage image(frame.argb->data(), frame.width, frame.height, frame.width * 4,
                 QImage::Format_ARGB32);
    QImage copied = image.copy();

    bool schedule = false;
    {
        std::lock_guard<std::mutex> lock(preview_mutex_);
        pending_preview_ = std::move(copied);
        if (!preview_scheduled_) { //如果没有排队渲染任务,设置渲染任务为true
            preview_scheduled_ = true;
            schedule = true;
        }
    }

    if (schedule) { //没有渲染任务了,就渲染当前帧
        QMetaObject::invokeMethod(
            this, [this]() { render_preview_frame(); }, Qt::QueuedConnection);
    }
}

/**
 * @brief 在 UI 线程渲染本地预览帧pending_preview_
 * pending_preview_是通过onframe函数的帧
 * 取出待渲染图像并设置到 pixmap item 上，同时让画面自适应显示区域。
 */
void Widget::render_preview_frame() {
    QImage image;
    {
        std::lock_guard<std::mutex> lock(preview_mutex_);
        image = std::move(pending_preview_);
        preview_scheduled_ = false; //没有待渲染的帧了
    }
    if (image.isNull() || !preview_item_) {
        return;
    }
    preview_item_->setPixmap(QPixmap::fromImage(image));
    preview_scene_->setSceneRect(preview_item_->boundingRect());
    ui->display->fitInView(preview_item_, Qt::KeepAspectRatio);
}

/**
 * @brief 回调：加入会议结果
 * 切回 UI 线程：成功则恢复“加入会议”按钮并更新状态栏，
 * 失败则提示错误信息。
 */
void Widget::on_join_result(xrtc::XRtcError error, const std::string& message) {
    spdlog::info("[ui] on_join_result error={} msg={}", static_cast<int>(error),
                 message);
    QMetaObject::invokeMethod(
        this,
        [this, error, message]() {
            ui->btn_join->setEnabled(error != xrtc::XRtcError::kNOERROR);
            if (error == xrtc::XRtcError::kNOERROR) {
                in_meeting_ = true;
                ui->video_capture->setEnabled(false);
                ui->status_label->setText(
                    QString::fromUtf8("状态: joined - %1")
                        .arg(QString::fromStdString(message)));
            } else {
                in_meeting_ = false;
                ui->video_capture->setEnabled(true);
                ui->status_label->setText(
                    QString::fromUtf8("状态: join failed - %1")
                        .arg(QString::fromStdString(message)));
                QMessageBox::warning(
                    this, "Join failed",
                    QString::fromStdString(message));
            }
        },
        Qt::QueuedConnection);
}

/**
 * @brief 回调：已离开会议
 * 切回 UI 线程：恢复加入按钮、更新状态栏并清空远端画面。
 */
void Widget::on_leave(xrtc::XRtcError) {
    QMetaObject::invokeMethod(
        this,
        [this]() {
            in_meeting_ = false;
            ui->video_capture->setEnabled(true);
            ui->btn_join->setEnabled(true);
            ui->status_label->setText(QString::fromUtf8("状态: left"));
            clear_remote_preview();
        },
        Qt::QueuedConnection);
}

/**
 * @brief 回调：WebRTC 连接状态变化
 * 切回 UI 线程，把状态转换为可读字符串显示在状态栏。
 */
void Widget::on_connection_state(xrtc::XRTCConnectionState state) {
    QMetaObject::invokeMethod(
        this,
        [this, state]() {
            ui->status_label->setText(
                QString("状态: pc=%1").arg(ConnectionStateName(state)));
        },
        Qt::QueuedConnection);
}

/**
 * @brief 回调：远端用户加入
 * 切回 UI 线程，在状态栏显示远端用户信息。
 */
void Widget::on_remote_user_joined(const xrtc::XRTCRemoteUser& user) {
    QMetaObject::invokeMethod(
        this,
        [this, user]() {
            ui->status_label->setText(
                QString("状态: remote joined feed=%1 %2")
                    .arg(user.feed_id)
                    .arg(QString::fromStdString(user.display)));
        },
        Qt::QueuedConnection);
}

/**
 * @brief 回调：远端用户离开
 * 切回 UI 线程，更新状态栏并清空远端画面。
 */
void Widget::on_remote_user_left(const xrtc::XRTCRemoteUser& user) {
    QMetaObject::invokeMethod(
        this,
        [this, user]() {
            ui->status_label->setText(
                QString("状态: remote left feed=%1").arg(user.feed_id));
            clear_remote_preview();
        },
        Qt::QueuedConnection);
}

/**
 * @brief 回调：远端视频帧到达（可能来自非 UI 线程）
 * 与 on_video_frame 类似：拷贝一帧图像放入待渲染队列，
 * 并调度一次 render_remote_preview_frame 在 UI 线程渲染。
 */
void Widget::on_remote_video_frame(uint64_t /*feed_id*/,
                                   const xrtc::XRTCVideoFrame& frame) {
    if (!frame.argb || frame.width <= 0 || frame.height <= 0) {
        return;
    }

    QImage image(frame.argb->data(), frame.width, frame.height, frame.width * 4,
                 QImage::Format_ARGB32);
    QImage copied = image.copy();

    bool schedule = false;
    {
        std::lock_guard<std::mutex> lock(remote_mutex_);
        pending_remote_ = std::move(copied);
        if (!remote_scheduled_) {
            remote_scheduled_ = true;
            schedule = true;
        }
    }

    if (schedule) {
        QMetaObject::invokeMethod(
            this, [this]() { render_remote_preview_frame(); },
            Qt::QueuedConnection);
    }
}

/**
 * @brief 在 UI 线程渲染远端预览帧
 * 取出待渲染图像并设置到 pixmap item 上，同时让画面自适应显示区域。
 */
void Widget::render_remote_preview_frame() {
    QImage image;
    {
        std::lock_guard<std::mutex> lock(remote_mutex_);
        image = std::move(pending_remote_);
        remote_scheduled_ = false;
    }
    if (image.isNull() || !remote_item_) {
        return;
    }
    remote_item_->setPixmap(QPixmap::fromImage(image));
    remote_scene_->setSceneRect(remote_item_->boundingRect());
    ui->remote_display->fitInView(remote_item_, Qt::KeepAspectRatio);
}
