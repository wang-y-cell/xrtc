#include <session/call_session.h>

#include "api/audio_options.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "rtc_base/thread.h"
#include "rtc_base/time_utils.h"
#include <engine/xrtc_global.h>
#include <media/xrtc_audio_device_module.h>
#include <spdlog/spdlog.h>
#include <vector>

#include "libyuv/convert_argb.h"

namespace xrtc {

CallSession::CallSession() {
    // 新 utils：亲和绑 utils::thread*；跨线程需目标 loop 在泵，否则 emit 会丢。
    // 专用 worker 泵事件：Beast emit → Queued → 本线程槽 → PostTask(api_thread)
    signal_thread_ = std::make_unique<utils::worker_thread>();
    signal_thread_->start();
    move_to_thread(signal_thread_.get());

    janus_ = std::make_unique<JanusClient>();
    janus_->move_to_thread(signal_thread_.get());
    bindJanusSignals();
}

CallSession::~CallSession() {
    invalidate();
    Stop();
    janus_conns_.clear();
    janus_.reset();
    if (signal_thread_) {
        signal_thread_->stop();
        signal_thread_.reset();
    }
}

void CallSession::bindJanusSignals() {
    janus_conns_.clear();

    janus_conns_.emplace_back(utils::connect(
        janus_->joined_as_publisher, this, &CallSession::onJoinedAsPublisher));
    janus_conns_.emplace_back(
        utils::connect(janus_->publishers, this, &CallSession::onPublishers));
    janus_conns_.emplace_back(utils::connect(janus_->publisher_left, this,
                                             &CallSession::onPublisherLeft));
    janus_conns_.emplace_back(utils::connect(
        janus_->publisher_answer, this, &CallSession::onPublisherAnswer));
    janus_conns_.emplace_back(utils::connect(
        janus_->subscriber_offer, this, &CallSession::onSubscriberOffer));
    janus_conns_.emplace_back(utils::connect(
        janus_->remote_candidate, this, &CallSession::onRemoteCandidate));
    janus_conns_.emplace_back(
        utils::connect(janus_->error, this, &CallSession::onJanusError));
    janus_conns_.emplace_back(
        utils::connect(janus_->destroyed, this, &CallSession::onJanusDestroyed));
}

slots_t<> CallSession::onPublishers(
    const std::vector<JanusPublisherInfo>& pubs) {
        const uint64_t gen = life_.generation;
    XRtcGlobal::instance().api_thread()->PostTask([this, pubs, gen]() {
        if (!isCurrentGeneration(gen)) {
            return;
        }
        for (const auto& p : pubs) {
            subscribeFeed(p);
        }
    });
    return {};
}

slots_t<> CallSession::onPublisherLeft(uint64_t feed_id,
                                              const std::string&) {
        const uint64_t gen = life_.generation;
    XRtcGlobal::instance().api_thread()->PostTask([this, feed_id, gen]() {
        if (!isCurrentGeneration(gen)) {
            return;
        }
        // 先卸 sink，再关 subscriber PC，避免帧回调写已释放内存
        detachRemoteMedia(feed_id);
        if (janus_) {
            janus_->DetachSubscriber(feed_id);
        }
        for (auto it = handle_to_feed_.begin(); it != handle_to_feed_.end();) {
            if (it->second == feed_id) {
                subscriber_pcs_.erase(it->first);
                it = handle_to_feed_.erase(it);
            } else {
                ++it;
            }
        }
        if (auto* obs = XRtcGlobal::instance().observer()) {
            XRTCRemoteUser user;
            user.feed_id = feed_id;
            obs->on_remote_user_left(user);
        }
    });
    return {};
}

slots_t<> CallSession::onPublisherAnswer(const JanusJsep& jsep) {
        const uint64_t gen = life_.generation;
    XRtcGlobal::instance().api_thread()->PostTask([this, jsep, gen]() {
        if (!isCurrentGeneration(gen)) {
            return;
        }
        if (publisher_pc_) {
            publisher_pc_->SetRemoteDescription(jsep.type, jsep.sdp);
        }
    });
    return {};
}

slots_t<> CallSession::onSubscriberOffer(uint64_t feed_id,
                                                uint64_t handle_id,
                                                const JanusJsep& offer) {
        const uint64_t gen = life_.generation;
    XRtcGlobal::instance().api_thread()->PostTask(
        [this, feed_id, handle_id, offer, gen]() {
            if (!isCurrentGeneration(gen)) {
                return;
            }
            handle_to_feed_[handle_id] = feed_id;

            PeerConnectionHandler::Callbacks pcb;
            pcb.on_local_description =
                [this, handle_id, gen](const std::string& type,
                                       const std::string& sdp) {
                    if (!isCurrentGeneration(gen)) {
                        return;
                    }
                    onSubscriberLocalSdp(handle_id, type, sdp);
                };
            pcb.on_ice_candidate = [this, handle_id, gen](const std::string& mid,
                                                          int idx,
                                                          const std::string& cand) {
                if (!isCurrentGeneration(gen)) {
                    return;
                }
                janus_->SendTrickle(handle_id, mid, idx, cand);
            };
            pcb.on_ice_gathering_complete = [this, handle_id, gen]() {
                if (!isCurrentGeneration(gen)) {
                    return;
                }
                janus_->SendTrickleComplete(handle_id);
            };
            pcb.on_track =
                [this, feed_id, gen](
                    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface>
                        track) {
                    if (!isCurrentGeneration(gen)) {
                        return;
                    }
                    attachRemoteTrack(feed_id, track);
                };
            pcb.on_error = [this, gen](const std::string& err) {
                if (!isCurrentGeneration(gen)) {
                    return;
                }
                spdlog::error("Subscriber PC error: {}", err);
                if (auto* obs = XRtcGlobal::instance().observer()) {
                    obs->on_connection_state(XRTCConnectionState::kFailed);
                }
            };

            auto factory =
                XRtcGlobal::instance().GetOrCreatePeerConnectionFactory();
            auto pc =
                std::make_unique<PeerConnectionHandler>(factory, std::move(pcb));
            if (!pc->Init(config_.ice_servers)) {
                spdlog::error("[session] subscriber pc init failed feed={}",
                              feed_id);
                if (!life_.join_notified) {
                    failJoin(XRtcError::kPeerConnectionFailed,
                             "subscriber pc init failed");
                }
                return;
            }
            pc->SetRemoteDescription(offer.type, offer.sdp);
            pc->CreateAnswer();
            subscriber_pcs_[handle_id] = std::move(pc);

            if (auto* obs = XRtcGlobal::instance().observer()) {
                XRTCRemoteUser user;
                user.feed_id = feed_id;
                obs->on_remote_user_joined(user);
            }
        });
    return {};
}

slots_t<> CallSession::onRemoteCandidate(uint64_t handle_id,
                                                const std::string& mid, int idx,
                                                const std::string& cand) {
        const uint64_t gen = life_.generation;
    XRtcGlobal::instance().api_thread()->PostTask(
        [this, handle_id, mid, idx, cand, gen]() {
            if (!isCurrentGeneration(gen)) {
                return;
            }
            if (handle_id == janus_->publisher_handle()) {
                if (publisher_pc_) {
                    publisher_pc_->AddIceCandidate(mid, idx, cand);
                }
                return;
            }
            auto it = subscriber_pcs_.find(handle_id);
            if (it != subscriber_pcs_.end()) {
                it->second->AddIceCandidate(mid, idx, cand);
            }
        });
    return {};
}

slots_t<> CallSession::onJanusError(const std::string& err) {
    const uint64_t gen = life_.generation;
    XRtcGlobal::instance().api_thread()->PostTask([this, err, gen]() {
        if (gen != life_.generation) {
            return;
        }
        spdlog::error("[session] signaling error: {}", err);
        const std::string msg =
            err.empty() ? "signaling failed (empty detail)" : err;
        if (life_.join_notified) {
            failAfterJoined(XRtcError::kSignalingFailed, msg);
        } else {
            failJoin(XRtcError::kSignalingFailed, msg);
        }
    });
    return {};
}

slots_t<> CallSession::onJanusDestroyed() {
    const uint64_t gen = life_.generation;
    XRtcGlobal::instance().api_thread()->PostTask([this, gen]() {
        if (gen != life_.generation && !life_.active) {
            // 世代已变且已 inactive：补一次幂等清理即可
            cleanupMediaResources();
            return;
        }
        spdlog::info("[session] Janus connection destroyed active={} tearing={}",
                     life_.active, life_.tearing_down);
        auto action = life_.On(SessionLifecycle::Event::kQuietDestroy,
                               "connection closed");
        applyLifecycleAction(action, /*already_disconnected=*/true);
    });
    return {};
}

void CallSession::Start(const XRTCJoinConfig& config) {
    auto start_action = life_.On(SessionLifecycle::Event::kStartOk);
    if (start_action.ignore) {
        notifyJoinResult(XRtcError::kAlreadyInCall, "already in call");
        return;
    }
    if (config.janus_ws_url.empty()) {
        life_.active = false;
        notifyJoinResult(XRtcError::kInvalidParam, "empty janus_ws_url");
        return;
    }

    config_ = config;
    auto st = janus_->Connect(config_);
    if (!st) {
        life_.active = false;
        cleanupMediaResources();
        notifyJoinResult(st.error(),
                         std::string(XRtcErrorToString(st.error())));
    }
}

void CallSession::Stop() {
    auto action = life_.On(SessionLifecycle::Event::kUserStop);
    applyLifecycleAction(action, /*already_disconnected=*/false);
}

void CallSession::cleanupMediaResources() {
    auto run = [this]() {
        detachAllRemoteMedia();
        publisher_pc_.reset();
        subscriber_pcs_.clear();
        handle_to_feed_.clear();
        audio_track_ = nullptr;
        video_track_ = nullptr;
        video_source_ = nullptr;
        local_video_capturing_ = false;
        local_audio_capturing_ = false;
        if (audio_capture_) {
            audio_capture_->stop();
            audio_capture_.reset();
        }
        if (capture_) {
            capture_->stop();
            capture_.reset();
        }
    };
    auto* api = XRtcGlobal::instance().api_thread();
    if (api && webrtc::Thread::Current() != api) {
        api->BlockingCall(run);
        return;
    }
    run();
}

void CallSession::applyLifecycleAction(const SessionLifecycle::Action& action,
                                       bool already_disconnected) {
    if (action.ignore && !action.cleanup_media) {
        life_.FinishTeardown();
        return;
    }
    if (action.disconnect && !already_disconnected && janus_) {
        janus_->Disconnect();
    }
    if (action.cleanup_media) {
        cleanupMediaResources();
    }
    if (action.notify_join_fail) {
        notifyJoinResult(action.leave_or_join_error, action.message);
    }
    if (action.notify_leave) {
        if (auto* obs = XRtcGlobal::instance().observer()) {
            if (action.leave_or_join_error != XRtcError::kNOERROR) {
                obs->on_connection_state(XRTCConnectionState::kFailed);
            }
            obs->on_leave(action.leave_or_join_error);
        }
    }
    life_.FinishTeardown();
}

void CallSession::failJoin(XRtcError error, const std::string& message) {
    auto action = life_.On(SessionLifecycle::Event::kFailJoin, message);
    action.leave_or_join_error = error;
    action.message = message;
    // On() 已按 join_notified 决定；此处强制使用调用方错误码
    if (!life_.join_notified) {
        action.notify_join_fail = true;
        action.notify_leave = false;
    }
    applyLifecycleAction(action, /*already_disconnected=*/false);
}

void CallSession::failAfterJoined(XRtcError error, const std::string& message) {
    auto action = life_.On(SessionLifecycle::Event::kSignalingError, message);
    action.leave_or_join_error = error;
    action.message = message;
    applyLifecycleAction(action, /*already_disconnected=*/false);
}

void CallSession::notifyJoinResult(XRtcError error,
                                   const std::string& message) {
    if (life_.join_notified) {
        return;
    }
    life_.MarkJoinNotified();
    if (auto* obs = XRtcGlobal::instance().observer()) {
        obs->on_join_result(error, message);
    }
}

///@brief 静音音频
///@param mute 是否静音
void CallSession::MuteAudio(bool mute) {
    auto run = [this, mute]() {
        if (publisher_pc_) {
            publisher_pc_->MuteAudio(mute);
        }
    };
    auto* api = XRtcGlobal::instance().api_thread();
    if (api && webrtc::Thread::Current() != api) {
        api->BlockingCall(run);
        return;
    }
    run();
}

void CallSession::MuteVideo(bool mute) {
    auto run = [this, mute]() {
        if (publisher_pc_) {
            publisher_pc_->MuteVideo(mute);
        }
    };
    auto* api = XRtcGlobal::instance().api_thread();
    if (api && webrtc::Thread::Current() != api) {
        api->BlockingCall(run);
        return;
    }
    run();
}

void CallSession::muteLocalTracks(bool mute) {
    if (publisher_pc_) {
        publisher_pc_->MuteAudio(mute);
        publisher_pc_->MuteVideo(mute);
    } else {
        if (audio_track_) {
            audio_track_->set_enabled(!mute);
        }
        if (video_track_) {
            video_track_->set_enabled(!mute);
        }
    }
}

bool CallSession::StartLocalVideo() {
    auto run = [this]() -> bool {
        if (!capture_) {
            spdlog::error("[session] StartLocalVideo: capture not ready");
            return false;
        }
        if (!capture_->start()) {
            spdlog::error("[session] StartLocalVideo: capture start failed");
            return false;
        }
        local_video_capturing_ = true;
        spdlog::info("[session] local video capture started");
        return true;
    };
    auto* api = XRtcGlobal::instance().api_thread();
    if (api && webrtc::Thread::Current() != api) {
        return api->BlockingCall(run);
    }
    return run();
}

bool CallSession::StopLocalVideo() {
    auto run = [this]() -> bool {
        // 关采集前推一帧黑图，清本地预览并避免远端卡在最后一帧
        if (video_source_) {
            int w = config_.width > 0 ? config_.width : 640;
            int h = config_.height > 0 ? config_.height : 480;
            if (capture_) {
                const auto fmt = capture_->capture_format();
                if (fmt.width > 0) {
                    w = fmt.width;
                }
                if (fmt.height > 0) {
                    h = fmt.height;
                }
            }
            auto buffer = webrtc::I420Buffer::Create(w, h);
            webrtc::I420Buffer::SetBlack(buffer.get());
            const webrtc::VideoFrame frame =
                webrtc::VideoFrame::Builder()
                    .set_video_frame_buffer(buffer)
                    .set_timestamp_rtp(0)
                    .set_timestamp_ms(webrtc::TimeMillis())
                    .build();
            video_source_->PushFrame(frame);

            if (auto* obs = XRtcGlobal::instance().observer()) {
                auto argb = std::make_shared<std::vector<uint8_t>>(
                    static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
                libyuv::I420ToARGB(buffer->DataY(), buffer->StrideY(),
                                   buffer->DataU(), buffer->StrideU(),
                                   buffer->DataV(), buffer->StrideV(),
                                   argb->data(), w * 4, w, h);
                XRTCVideoFrame vf;
                vf.width = w;
                vf.height = h;
                vf.argb = std::move(argb);
                obs->on_video_frame(capture_.get(), vf);
            }
        }
        if (capture_) {
            capture_->stop();
        }
        local_video_capturing_ = false;
        spdlog::info("[session] local video capture stopped");
        return true;
    };
    auto* api = XRtcGlobal::instance().api_thread();
    if (api && webrtc::Thread::Current() != api) {
        return api->BlockingCall(run);
    }
    return run();
}

bool CallSession::StartLocalAudio() {
    auto run = [this]() -> bool {
        if (!audio_capture_) {
            spdlog::error("[session] StartLocalAudio: audio_capture not ready");
            return false;
        }
        if (!audio_capture_->start()) {
            spdlog::error("[session] StartLocalAudio: start failed");
            return false;
        }
        local_audio_capturing_ = true;
        spdlog::info("[session] local audio capture started");
        return true;
    };
    auto* api = XRtcGlobal::instance().api_thread();
    if (api && webrtc::Thread::Current() != api) {
        return api->BlockingCall(run);
    }
    return run();
}

bool CallSession::StopLocalAudio() {
    auto run = [this]() -> bool {
        if (audio_capture_) {
            audio_capture_->StopHardwareRecording();
        }
        local_audio_capturing_ = false;
        spdlog::info("[session] local audio capture stopped");
        return true;
    };
    auto* api = XRtcGlobal::instance().api_thread();
    if (api && webrtc::Thread::Current() != api) {
        return api->BlockingCall(run);
    }
    return run();
}

XRtcStatus CallSession::ensureLocalMedia() {
    //WebRTC 全局工厂，用来创建 AudioTrack、VideoTrack 等。拿不到就返回 kMediaStartFailed
    auto factory = XRtcGlobal::instance().GetOrCreatePeerConnectionFactory();
    if (!factory) {
        return xrtc_err(XRtcError::kMediaStartFailed);
    }

    //创建视频源,如果没有创建就创建
    if (!video_source_) {
        video_source_ = XrtcVideoTrackSource::Create();
    }

    //校验摄像头id,如果没有摄像头id,返回错误参数
    if (config_.video_device_id.empty()) {
        return xrtc_err(XRtcError::kInvalidParam);
    }

    // 创建摄像头采集器（默认不 start，由 StartLocalVideo 手动开）
    if (!capture_) {
        capture_ = VcmCapture::Create(
            static_cast<size_t>(config_.width),
            static_cast<size_t>(config_.height), config_.fps,
            config_.video_device_id, config_.select_strategy);
        if (!capture_) {
            return xrtc_err(XRtcError::kMediaStartFailed);
        }
        capture_->set_track_source(video_source_);
    }

    // 选麦 + 挂音量旁路；不在此 StartRecording
    if (!audio_capture_) {
        auto base = XRtcGlobal::instance().audio_device();
        auto* raw = static_cast<XrtcAudioDeviceModule*>(base.get());
        if (!raw) {
            return xrtc_err(XRtcError::kMediaStartFailed);
        }
        webrtc::scoped_refptr<XrtcAudioDeviceModule> xrtc_adm(raw);
        audio_capture_ =
            AudioCapture::Create(std::move(xrtc_adm), config_.audio_device_id);
        if (!audio_capture_) {
            return xrtc_err(XRtcError::kMediaStartFailed);
        }
        if (!audio_capture_->open()) {
            spdlog::error("[session] AudioCapture open failed, device_id={}",
                          config_.audio_device_id);
            return xrtc_err(XRtcError::kMediaStartFailed);
        }
    }

    // 创建音频轨道
    if (!audio_track_) {
        spdlog::info("[session] CreateAudioSource/Track");
        auto audio_source = factory->CreateAudioSource(webrtc::AudioOptions());
        audio_track_ = factory->CreateAudioTrack("audio0", audio_source.get());
        audio_track_->set_enabled(false); //关闭推流
    }
    if (!video_track_) {
        video_track_ = factory->CreateVideoTrack(video_source_, "video0");
        video_track_->set_enabled(false); //关闭推流
    }
    if (!audio_track_ || !video_track_) {
        return xrtc_err(XRtcError::kMediaStartFailed);
    }
    return xrtc_ok();
}

void CallSession::createPublisherPc() {
        const uint64_t gen = life_.generation;
    PeerConnectionHandler::Callbacks pcb;
    pcb.on_local_description = [this, gen](const std::string& type,
                                           const std::string& sdp) {
        if (!isCurrentGeneration(gen)) {
            return;
        }
        onPublisherLocalSdp(type, sdp);
    };
    pcb.on_ice_candidate = [this, gen](const std::string& mid, int idx,
                                       const std::string& cand) {
        if (!isCurrentGeneration(gen)) {
            return;
        }
        janus_->SendTrickle(janus_->publisher_handle(), mid, idx, cand);
    };
    pcb.on_ice_gathering_complete = [this, gen]() {
        if (!isCurrentGeneration(gen)) {
            return;
        }
        janus_->SendTrickleComplete(janus_->publisher_handle());
    };
    pcb.on_connection_state = [this, gen](XRTCConnectionState state) {
        if (!isCurrentGeneration(gen)) {
            return;
        }
        if (auto* obs = XRtcGlobal::instance().observer()) {
            obs->on_connection_state(state);
        }
        if (state == XRTCConnectionState::kFailed) {
            spdlog::error("[session] publisher PC entered failed state");
            if (!life_.join_notified) {
                failJoin(XRtcError::kPeerConnectionFailed, "ice/pc failed");
            } else {
                failAfterJoined(XRtcError::kPeerConnectionFailed,
                                "ice/pc failed");
            }
        }
    };
    pcb.on_error = [this, gen](const std::string& err) {
        if (gen != life_.generation) {
            return;
        }
        spdlog::error("[session] publisher PC error: {}", err);
        if (!life_.join_notified) {
            failJoin(XRtcError::kPeerConnectionFailed, err);
            return;
        }
        failAfterJoined(XRtcError::kPeerConnectionFailed, err);
    };

    auto factory = XRtcGlobal::instance().GetOrCreatePeerConnectionFactory();
    publisher_pc_ =
        std::make_unique<PeerConnectionHandler>(factory, std::move(pcb));
    if (!publisher_pc_->Init(config_.ice_servers)) {
        publisher_pc_.reset();
        failJoin(XRtcError::kPeerConnectionFailed, "publisher pc init failed");
        return;
    }

    if (!publisher_pc_->AddTrack(audio_track_, {"stream0"})) {
        publisher_pc_.reset();
        failJoin(XRtcError::kPeerConnectionFailed, "add audio track failed");
        return;
    }
    if (!publisher_pc_->AddTrack(video_track_, {"stream0"})) {
        publisher_pc_.reset();
        failJoin(XRtcError::kPeerConnectionFailed, "add video track failed");
        return;
    }
    // 默认禁推流；WebRTC 可能已 StartRecording，立即停掉等手动开麦
    muteLocalTracks(true);
    if (audio_capture_) {
        audio_capture_->StopHardwareRecording();
    }
    local_video_capturing_ = false;
    local_audio_capturing_ = false;
    publisher_pc_->CreateOffer();
}

slots_t<> CallSession::onJoinedAsPublisher() {
        const uint64_t gen = life_.generation;
    XRtcGlobal::instance().api_thread()->PostTask([this, gen]() {
        if (!isCurrentGeneration(gen)) {
            return;
        }
        spdlog::info(
            "[session] onJoinedAsPublisher: prepare local media + PC "
            "(capture deferred)");
        auto st = ensureLocalMedia();
        if (!st) {
            spdlog::error("[session] ensureLocalMedia failed: {}",
                              XRtcErrorToString(st.error()));
            failJoin(st.error(), "failed to start local media");
            return;
        }
        createPublisherPc();
        if (!life_.active) {
            return;
        }
        spdlog::info("[session] notify join success");
        notifyJoinResult(XRtcError::kNOERROR, "joined");
    });
    return {};
}

void CallSession::onPublisherLocalSdp(const std::string& type,
                                      const std::string& sdp) {
    JanusJsep jsep;
    jsep.type = type;
    jsep.sdp = sdp;
    janus_->Publish(jsep);
}

void CallSession::onSubscriberLocalSdp(uint64_t handle_id,
                                       const std::string& type,
                                       const std::string& sdp) {
    JanusJsep jsep;
    jsep.type = type;
    jsep.sdp = sdp;
    janus_->StartSubscriber(handle_id, jsep);
}

void CallSession::subscribeFeed(const JanusPublisherInfo& info) {
    janus_->Subscribe(info.feed_id);
}

void CallSession::detachRemoteVideo(uint64_t feed_id) {
    auto vit = remote_videos_.find(feed_id);
    if (vit != remote_videos_.end()) {
        vit->second.Detach();
        remote_videos_.erase(vit);
    }
}

void CallSession::detachRemoteMedia(uint64_t feed_id) {
    detachRemoteVideo(feed_id);
    remote_audio_tracks_.erase(feed_id);
}

void CallSession::detachAllRemoteMedia() {
    for (auto& [id, att] : remote_videos_) {
        (void)id;
        att.Detach();
    }
    remote_videos_.clear();
    remote_audio_tracks_.clear();
}

void CallSession::attachRemoteTrack(
    uint64_t feed_id,
    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track) {
    if (!track) {
        return;
    }

    // 远端音频：启用并持有轨即可。扬声器由 WebRTC AudioState 在 worker
    // 线程上 InitPlayout/StartPlayout，切勿在 api_thread 上手动调 ADM。
    if (track->kind() == webrtc::MediaStreamTrackInterface::kAudioKind) {
        auto audio =
            webrtc::scoped_refptr<webrtc::AudioTrackInterface>(
                static_cast<webrtc::AudioTrackInterface*>(track.get()));
        audio->set_enabled(true);
        remote_audio_tracks_[feed_id] = std::move(audio);
        spdlog::info("[session] remote audio attached, feed={}", feed_id);
        return;
    }

    if (track->kind() != webrtc::MediaStreamTrackInterface::kVideoKind) {
        return;
    }

    auto video = webrtc::scoped_refptr<webrtc::VideoTrackInterface>(
        static_cast<webrtc::VideoTrackInterface*>(track.get()));
    // 同一 feed 重复绑视频时先卸旧 sink；不要动 audio 引用
    detachRemoteVideo(feed_id);

    RemoteVideoAttachment att;
    att.track = video;
    att.sink = std::make_unique<RemoteVideoSink>(
        feed_id, [](uint64_t id, const XRTCVideoFrame& frame) {
            if (auto* obs = XRtcGlobal::instance().observer()) {
                obs->on_remote_video_frame(id, frame);
            }
        });
    video->AddOrUpdateSink(att.sink.get(), webrtc::VideoSinkWants());
    remote_videos_[feed_id] = std::move(att);
}

}  // namespace xrtc
