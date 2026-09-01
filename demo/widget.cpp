#include "widget.h"
#include "ui_widget.h"

#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QResizeEvent>
#include <QString>
#include <QSizePolicy>
#include <QVBoxLayout>

#include <algorithm>

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
    ui->btn_audio_preview->setStyleSheet(btnStart);
    ui->btn_meeting_video->setStyleSheet(btnStart);
    ui->btn_meeting_audio->setStyleSheet(btnStart);
    init_preview();
    init_connection();
    init_device_list();
    on_video_request_changed();
    set_meeting_media_buttons(false);
}

xrtc::XRTCVideoCaptureRequest Widget::current_video_request() const {
    xrtc::XRTCVideoCaptureRequest request;
    request.format.width = ui->video_width->value();
    request.format.height = ui->video_height->value();
    request.format.fps = ui->video_fps->value();
    request.strategy = ui->video_strategy->currentIndex() == 1
                           ? xrtc::XRTCVideoSelectStrategy::kPreferStandard
                           : xrtc::XRTCVideoSelectStrategy::kPreferRequested;
    return request;
}

void Widget::refresh_actual_format_label(const std::string& device_id) {
    const auto request = current_video_request();
    xrtc::XRTCVideoFormat actual = request.format;
    if (!device_id.empty()) {
        actual = engine->select_video_format(device_id, request.format,
                                             request.strategy);
    }
    ui->label_actual_format->setText(
        QString::fromUtf8("实际格式: %1×%2@%3")
            .arg(actual.width)
            .arg(actual.height)
            .arg(actual.fps));
}

void Widget::on_video_request_changed() {
    engine->set_video_capture_request(current_video_request());

    std::string device_id;
    const int index = ui->camera_list->currentIndex();
    if (index >= 0 && index < static_cast<int>(video_devices_.size())) {
        device_id = video_devices_[index].device_id;
    }
    refresh_actual_format_label(device_id);

    if (video_source && !in_meeting_) {
        // 公开 API 不暴露 VcmCapture；改请求后重建预览源以应用新分辨率
        const auto request = current_video_request();
        std::string device_id;
        const int cam = ui->camera_list->currentIndex();
        if (cam >= 0 && cam < static_cast<int>(video_devices_.size())) {
            device_id = video_devices_[cam].device_id;
        }
        video_source->stop();
        engine->destroy_video_source(video_source);
        video_source = nullptr;

        xrtc::ixrtc_video_config video_config;
        video_config.device_id = device_id;
        video_config.width = request.format.width;
        video_config.height = request.format.height;
        video_config.fps = request.format.fps;
        video_config.select_strategy = request.strategy;
        video_source = engine->create_video_source(video_config);
        if (video_source && video_source->start()) {
            const auto actual = engine->select_video_format(
                device_id, request.format, request.strategy);
            ui->label_actual_format->setText(
                QString::fromUtf8("实际格式: %1×%2@%3 (采集中)")
                    .arg(actual.width)
                    .arg(actual.height)
                    .arg(actual.fps));
        } else {
            spdlog::warn("[ui] recreate video source after format change failed");
            if (video_source) {
                engine->destroy_video_source(video_source);
                video_source = nullptr;
            }
            ui->video_capture->setStyleSheet(btnStart);
            ui->video_capture->setText(QString::fromUtf8("开启本地预览"));
        }
    }
}

/**
 * @brief 连接界面按钮的点击信号到对应的槽函数
 */
void Widget::init_connection() {
    connect(ui->video_capture, &QPushButton::clicked, this,
            &Widget::start_video_source);
    connect(ui->btn_audio_preview, &QPushButton::clicked, this,
            &Widget::start_audio_source);
    connect(ui->btn_join, &QPushButton::clicked, this, &Widget::join_meeting);
    connect(ui->btn_leave, &QPushButton::clicked, this, &Widget::leave_meeting);
    connect(ui->btn_meeting_video, &QPushButton::clicked, this,
            &Widget::toggle_meeting_video);
    connect(ui->btn_meeting_audio, &QPushButton::clicked, this,
            &Widget::toggle_meeting_audio);
    connect(ui->speaker_list, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &Widget::on_speaker_changed);
    connect(ui->video_width, qOverload<int>(&QSpinBox::valueChanged), this,
            &Widget::on_video_request_changed);
    connect(ui->video_height, qOverload<int>(&QSpinBox::valueChanged), this,
            &Widget::on_video_request_changed);
    connect(ui->video_fps, qOverload<int>(&QSpinBox::valueChanged), this,
            &Widget::on_video_request_changed);
    connect(ui->video_strategy, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &Widget::on_video_request_changed);
    connect(ui->camera_list, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &Widget::on_video_request_changed);
}

/**
 * @brief 初始化本地预览窗口
 * 远端窗口在用户加入时按 feed_id 动态创建。
 */
void Widget::init_preview() {
    preview_scene_ = new QGraphicsScene(this);
    ui->display->setScene(preview_scene_);
    ui->display->setRenderHint(QPainter::SmoothPixmapTransform);
    preview_item_ = preview_scene_->addPixmap(QPixmap());
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
 * @brief 确保指定 feed 的远端窗口存在（UI 线程）
 * 新用户加入时在底部横向追加一个窗口；已存在则更新显示名。
 */
void Widget::ensure_remote_view(uint64_t feed_id, const QString& display) {
    auto it = remote_views_.find(feed_id);
    if (it != remote_views_.end()) {
        if (!display.isEmpty() && it->second.name_label) {
            it->second.display = display;
            it->second.name_label->setText(display);
        }
        return;
    }

    const QString title =
        display.isEmpty() ? QString("feed %1").arg(feed_id) : display;

    auto* container = new QWidget();
    container->setMinimumWidth(240);
    container->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    auto* name = new QLabel(title);
    name->setAlignment(Qt::AlignCenter);
    name->setWordWrap(true);

    auto* view = new QGraphicsView();
    view->setMinimumSize(220, 140);
    view->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    view->setRenderHint(QPainter::SmoothPixmapTransform);
    auto* scene = new QGraphicsScene(container);
    view->setScene(scene);
    auto* item = scene->addPixmap(QPixmap());

    layout->addWidget(name);
    layout->addWidget(view, 1);

    // 插在末尾 spacer 之前，保证窗口横向排列、右侧仍可留白
    const int insert_at =
        std::max(0, ui->remote_layout->count() - 1);
    ui->remote_layout->insertWidget(insert_at, container, 1);

    RemoteViewUi remote;
    remote.container = container;
    remote.name_label = name;
    remote.view = view;
    remote.scene = scene;
    remote.item = item;
    remote.display = title;
    remote_views_[feed_id] = remote;
}

/**
 * @brief 移除指定 feed 的远端窗口（UI 线程）
 */
void Widget::remove_remote_view(uint64_t feed_id) {
    {
        std::lock_guard<std::mutex> lock(remote_mutex_);
        remote_pending_.erase(feed_id);
    }

    auto it = remote_views_.find(feed_id);
    if (it == remote_views_.end()) {
        return;
    }
    if (it->second.container) {
        ui->remote_layout->removeWidget(it->second.container);
        it->second.container->deleteLater();
    }
    remote_views_.erase(it);
}

/**
 * @brief 清空全部远端预览窗口
 */
void Widget::clear_all_remote_views() {
    {
        std::lock_guard<std::mutex> lock(remote_mutex_);
        remote_pending_.clear();
    }

    for (auto& [feed_id, remote] : remote_views_) {
        (void)feed_id;
        if (remote.container) {
            ui->remote_layout->removeWidget(remote.container);
            remote.container->deleteLater();
        }
    }
    remote_views_.clear();
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

    audio_devices_ = engine->get_audio_device_info();
    for (const auto& device : audio_devices_) {
        ui->audio_list->addItem(QString::fromUtf8(device.device_name.c_str()));
    }

    playout_devices_ = engine->get_playout_device_info();
    for (const auto& device : playout_devices_) {
        ui->speaker_list->addItem(QString::fromUtf8(device.device_name.c_str()));
    }
}

void Widget::on_speaker_changed(int index) {
    if (!engine || index < 0 ||
        index >= static_cast<int>(playout_devices_.size())) {
        return;
    }
    if (!engine->set_playout_device(playout_devices_[index].device_id)) {
        ui->status_label->setText(QString::fromUtf8("状态: 切换扬声器失败"));
        return;
    }
    ui->status_label->setText(
        QString::fromUtf8("状态: 扬声器已切换为 %1")
            .arg(QString::fromUtf8(playout_devices_[index].device_name.c_str())));
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
    if (audio_source) {
        audio_source->stop();
        engine->destroy_audio_source(audio_source);
        audio_source = nullptr;
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
    for (auto& [feed_id, remote] : remote_views_) {
        (void)feed_id;
        if (remote.item && remote.view && !remote.item->pixmap().isNull()) {
            remote.view->fitInView(remote.item, Qt::KeepAspectRatio);
        }
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
        engine->set_video_capture_request(current_video_request());
        video_source = engine->create_video_source(video_config);
        if (!video_source) {
            QMessageBox::warning(this, "Warning", "Failed to create video source");
            ui->video_capture->setDisabled(false);
            return;
        }
        {
            const auto req = current_video_request();
            const auto actual = engine->select_video_format(
                video_config.device_id, req.format, req.strategy);
            ui->label_actual_format->setText(
                QString::fromUtf8("实际格式: %1×%2@%3 (采集中)")
                    .arg(actual.width)
                    .arg(actual.height)
                    .arg(actual.fps));
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

void Widget::start_audio_source() {
    if (in_meeting_) {
        return;
    }
    ui->btn_audio_preview->setDisabled(true);
    if (!audio_source) {
        const int index = ui->audio_list->currentIndex();
        if (index < 0 || index >= static_cast<int>(audio_devices_.size())) {
            QMessageBox::warning(this, "Warning", "No microphone device");
            ui->btn_audio_preview->setDisabled(false);
            return;
        }
        xrtc::ixrtc_audio_config cfg;
        cfg.device_id = audio_devices_[index].device_id;
        audio_source = engine->create_audio_source(cfg);
        if (!audio_source) {
            QMessageBox::warning(this, "Warning",
                                 "Failed to create audio source");
            ui->btn_audio_preview->setDisabled(false);
            return;
        }
        if (!audio_source->start()) {
            QMessageBox::warning(this, "Warning",
                                 "Failed to start audio source");
            engine->destroy_audio_source(audio_source);
            audio_source = nullptr;
            ui->btn_audio_preview->setDisabled(false);
            return;
        }
        ui->btn_audio_preview->setStyleSheet(btnStop);
        ui->btn_audio_preview->setText(QString::fromUtf8("停止麦克风预览"));
    } else {
        audio_source->stop();
        engine->destroy_audio_source(audio_source);
        audio_source = nullptr;
        ui->mic_level_bar->setValue(0);
        ui->btn_audio_preview->setStyleSheet(btnStart);
        ui->btn_audio_preview->setText(QString::fromUtf8("开启麦克风预览"));
    }
    ui->btn_audio_preview->setDisabled(false);
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
    ui->btn_audio_preview->setEnabled(false);

    // join 会自行采集；若已有预览源先停掉，避免双开摄像头/麦克风
    if (video_source) {
        video_source->stop();
        engine->destroy_video_source(video_source);
        video_source = nullptr;
        clear_preview();
    }
    if (audio_source) {
        audio_source->stop();
        engine->destroy_audio_source(audio_source);
        audio_source = nullptr;
        ui->mic_level_bar->setValue(0);
        ui->btn_audio_preview->setText(QString::fromUtf8("开启麦克风预览"));
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
    const int audio_index = ui->audio_list->currentIndex();
    if (audio_index >= 0 &&
        audio_index < static_cast<int>(audio_devices_.size())) {
        config.audio_device_id = audio_devices_[audio_index].device_id;
    }
    const int speaker_index = ui->speaker_list->currentIndex();
    if (speaker_index >= 0 &&
        speaker_index < static_cast<int>(playout_devices_.size())) {
        config.playout_device_id = playout_devices_[speaker_index].device_id;
    }
    engine->set_video_capture_request(current_video_request());

    // 本机 coturn（勿留空，否则会退回 Google STUN）
    config.ice_servers.push_back(
        {"stun:8.153.155.18:3478", "", ""});
    config.ice_servers.push_back(
        {"turn:8.153.155.18:3478", "wang_y", "wang_y"});
    config.ice_servers.push_back(
        {"turn:8.153.155.18:3478?transport=tcp", "wang_y", "wang_y"});

    spdlog::info("[ui] join_meeting url={} room={} name={} create_if_missing={} ice={} video={}x{}@{} mic={}",
                 config.janus_ws_url, config.room_id, config.display_name,
                 config.create_room_if_missing, config.ice_servers.size(),
                 engine->get_video_capture_request().format.width,
                 engine->get_video_capture_request().format.height,
                 engine->get_video_capture_request().format.fps,
                 config.audio_device_id);
    ui->status_label->setText(QString::fromUtf8("状态: joining..."));
    ui->btn_join->setEnabled(false);
    clear_all_remote_views();
    engine->join(config);
}

/**
 * @brief 槽：离开会议
 * 通知引擎离开当前会议，并清空远端画面、恢复界面状态。
 */
void Widget::leave_meeting() {
    engine->leave();
    clear_all_remote_views();
    in_meeting_ = false;
    meeting_video_on_ = false;
    meeting_audio_on_ = false;
    set_meeting_media_buttons(false);
    reset_meeting_media_button_labels();
    ui->video_capture->setEnabled(true);
    ui->btn_audio_preview->setEnabled(true);
    ui->btn_join->setEnabled(true);
    ui->status_label->setText(QString::fromUtf8("状态: left"));
}

void Widget::set_meeting_media_buttons(bool in_meeting) {
    ui->btn_meeting_video->setEnabled(in_meeting);
    ui->btn_meeting_audio->setEnabled(in_meeting);
}

void Widget::reset_meeting_media_button_labels() {
    ui->btn_meeting_video->setStyleSheet(btnStart);
    ui->btn_meeting_video->setText(QString::fromUtf8("会议：开启摄像头"));
    ui->btn_meeting_audio->setStyleSheet(btnStart);
    ui->btn_meeting_audio->setText(QString::fromUtf8("会议：开启麦克风"));
}

void Widget::toggle_meeting_video() {
    if (!in_meeting_ || !engine) {
        return;
    }
    if (!meeting_video_on_) {
        engine->start_local_video();
        meeting_video_on_ = true;
        ui->btn_meeting_video->setStyleSheet(btnStop);
        ui->btn_meeting_video->setText(QString::fromUtf8("会议：关闭摄像头"));
        ui->status_label->setText(QString::fromUtf8("状态: 摄像头已开"));
    } else {
        engine->stop_local_video();
        meeting_video_on_ = false;
        clear_preview();
        ui->btn_meeting_video->setStyleSheet(btnStart);
        ui->btn_meeting_video->setText(QString::fromUtf8("会议：开启摄像头"));
        ui->status_label->setText(QString::fromUtf8("状态: 摄像头已关"));
    }
}

void Widget::toggle_meeting_audio() {
    if (!in_meeting_ || !engine) {
        return;
    }
    if (!meeting_audio_on_) {
        engine->start_local_audio();
        meeting_audio_on_ = true;
        ui->btn_meeting_audio->setStyleSheet(btnStop);
        ui->btn_meeting_audio->setText(QString::fromUtf8("会议：关闭麦克风"));
        ui->status_label->setText(QString::fromUtf8("状态: 麦克风已开"));
    } else {
        engine->stop_local_audio();
        meeting_audio_on_ = false;
        ui->btn_meeting_audio->setStyleSheet(btnStart);
        ui->btn_meeting_audio->setText(QString::fromUtf8("会议：开启麦克风"));
        ui->mic_level_bar->setValue(0);
        ui->status_label->setText(QString::fromUtf8("状态: 麦克风已关"));
    }
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
                meeting_video_on_ = false;
                meeting_audio_on_ = false;
                ui->video_capture->setEnabled(false);
                ui->btn_audio_preview->setEnabled(false);
                set_meeting_media_buttons(true);
                reset_meeting_media_button_labels();
                ui->status_label->setText(
                    QString::fromUtf8("状态: joined - %1（请手动开摄像头/麦克风）")
                        .arg(QString::fromStdString(message)));
            } else {
                in_meeting_ = false;
                meeting_video_on_ = false;
                meeting_audio_on_ = false;
                set_meeting_media_buttons(false);
                reset_meeting_media_button_labels();
                ui->video_capture->setEnabled(true);
                ui->btn_audio_preview->setEnabled(true);
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
            meeting_video_on_ = false;
            meeting_audio_on_ = false;
            set_meeting_media_buttons(false);
            reset_meeting_media_button_labels();
            ui->video_capture->setEnabled(true);
            ui->btn_audio_preview->setEnabled(true);
            ui->btn_join->setEnabled(true);
            ui->status_label->setText(QString::fromUtf8("状态: left"));
            clear_all_remote_views();
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
 * 切回 UI 线程：在底部横向追加窗口，并更新状态栏。
 */
void Widget::on_remote_user_joined(const xrtc::XRTCRemoteUser& user) {
    QMetaObject::invokeMethod(
        this,
        [this, user]() {
            const QString display = QString::fromStdString(user.display);
            ensure_remote_view(user.feed_id, display);
            ui->status_label->setText(
                QString("状态: remote joined feed=%1 %2 (共%3人)")
                    .arg(user.feed_id)
                    .arg(display)
                    .arg(remote_views_.size()));
        },
        Qt::QueuedConnection);
}

/**
 * @brief 回调：远端用户离开
 * 切回 UI 线程：移除对应窗口并更新状态栏。
 */
void Widget::on_remote_user_left(const xrtc::XRTCRemoteUser& user) {
    QMetaObject::invokeMethod(
        this,
        [this, user]() {
            remove_remote_view(user.feed_id);
            ui->status_label->setText(
                QString("状态: remote left feed=%1 (剩余%2人)")
                    .arg(user.feed_id)
                    .arg(remote_views_.size()));
        },
        Qt::QueuedConnection);
}

/**
 * @brief 回调：远端视频帧到达（可能来自非 UI 线程）
 * 按 feed_id 分别缓存待渲染帧，并调度 UI 线程渲染对应窗口。
 */
void Widget::on_remote_video_frame(uint64_t feed_id,
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
        auto& pending = remote_pending_[feed_id];
        pending.image = std::move(copied);
        if (!pending.scheduled) {
            pending.scheduled = true;
            schedule = true;
        }
    }

    if (schedule) {
        QMetaObject::invokeMethod(
            this,
            [this, feed_id]() {
                // 帧可能先于 joined 回调到达，补建窗口
                ensure_remote_view(feed_id, QString());
                render_remote_preview_frame(feed_id);
            },
            Qt::QueuedConnection);
    }
}

/**
 * @brief 在 UI 线程渲染指定 feed 的远端预览帧
 */
void Widget::render_remote_preview_frame(uint64_t feed_id) {
    QImage image;
    {
        std::lock_guard<std::mutex> lock(remote_mutex_);
        auto it = remote_pending_.find(feed_id);
        if (it == remote_pending_.end()) {
            return;
        }
        image = std::move(it->second.image);
        it->second.scheduled = false;
    }

    auto vit = remote_views_.find(feed_id);
    if (image.isNull() || vit == remote_views_.end() || !vit->second.item ||
        !vit->second.view || !vit->second.scene) {
        return;
    }

    vit->second.item->setPixmap(QPixmap::fromImage(image));
    vit->second.scene->setSceneRect(vit->second.item->boundingRect());
    vit->second.view->fitInView(vit->second.item, Qt::KeepAspectRatio);
}

void Widget::on_audio_level(xrtc::IXRtcMediaSource* /*audio_source*/,
                            int level) {
    QMetaObject::invokeMethod(
        this,
        [this, level]() {
            if (!ui || !ui->mic_level_bar) {
                return;
            }
            int v = level;
            // 预览仅测试用：放大显示；会议内保持 SDK 原始 level
            if (audio_source && !in_meeting_) {
                v = level * 2;
            }
            ui->mic_level_bar->setValue(std::clamp(v, 0, 100));
        },
        Qt::QueuedConnection);
}
