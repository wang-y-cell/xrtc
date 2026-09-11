#include <session/call_session.h>

#include "api/audio_options.h"
#include "api/units/time_delta.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_source_interface.h"
#include "rtc_base/thread.h"
#include "rtc_base/time_utils.h"
#include <engine/xrtc_global.h>
#include <media/i420_frame.h>
#include <media/xrtc_audio_device_module.h>
#include <spdlog/spdlog.h>
#include <vector>

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
        utils::connect(janus_->hangup, this, &CallSession::onJanusHangup));
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
            if (p.feed_id != 0 && !p.display.empty()) {
                feed_to_display_[p.feed_id] = p.display;
            }
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
                clearPendingRemoteIce(it->first);
                subscriber_pcs_.erase(it->first);
                it = handle_to_feed_.erase(it);
            } else {
                ++it;
            }
        }
        std::string display;
        if (auto dit = feed_to_display_.find(feed_id); dit != feed_to_display_.end()) {
            display = dit->second;
            feed_to_display_.erase(dit);
        }
        subscriber_retry_count_.erase(feed_id);
        remote_joined_notified_.erase(feed_id);
        if (auto* obs = XRtcGlobal::instance().observer()) {
            XRTCRemoteUser user;
            user.feed_id = feed_id;
            user.display = std::move(display);
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
            spdlog::info(
                "[session] subscriber offer feed={} handle={} type={} "
                "sdp_bytes={}",
                feed_id, handle_id, offer.type, offer.sdp.size());

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
                spdlog::info(
                    "[session] send subscriber trickle handle={} mid={} idx={} "
                    "cand={}",
                    handle_id, mid, idx, cand.substr(0, 80));
                janus_->SendTrickle(handle_id, mid, idx, cand);
            };
            pcb.on_ice_gathering_complete = [this, handle_id, gen]() {
                if (!isCurrentGeneration(gen)) {
                    return;
                }
                spdlog::info(
                    "[session] send subscriber trickle-complete handle={}",
                    handle_id);
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
            pcb.on_connection_state =
                [this, handle_id, feed_id, gen](XRTCConnectionState state) {
                    if (!isCurrentGeneration(gen)) {
                        return;
                    }
                    if (state != XRTCConnectionState::kFailed) {
                        return;
                    }
                    spdlog::warn(
                        "[session] subscriber PC failed feed={} handle={}",
                        feed_id, handle_id);
                    dropSubscriber(handle_id, "subscriber pc failed",
                                   /*retry=*/true);
                };
            pcb.on_error = [this, handle_id, feed_id, gen](const std::string& err) {
                if (!isCurrentGeneration(gen)) {
                    return;
                }
                spdlog::error("[session] subscriber PC error feed={} err={}",
                              feed_id, err);
                dropSubscriber(handle_id, err, /*retry=*/true);
            };

            auto factory =
                XRtcGlobal::instance().GetOrCreatePeerConnectionFactory();
            auto pc =
                std::make_unique<PeerConnectionHandler>(factory, std::move(pcb));
            pc->SetLabel("sub-feed" + std::to_string(feed_id) + "-h" +
                         std::to_string(handle_id));
            if (auto st = pc->Init(config_.ice_servers); !st) {
                spdlog::error("[session] subscriber pc init failed feed={} err={}",
                              feed_id, XRtcErrorToString(st.error()));
                clearPendingRemoteIce(handle_id);
                if (!life_.join_notified) {
                    failJoin(st.error(), "subscriber pc init failed");
                } else {
                    // 已进房：单路订阅失败不拆会，允许后续 publishers 事件再订
                    janus_->DetachSubscriber(feed_id);
                }
                return;
            }
            // 先入 map，再 SetRemote/Answer，避免 Janus trickle 早到被丢
            auto* pc_ptr = pc.get();
            subscriber_pcs_[handle_id] = std::move(pc); //pc创建完成,保存到map中,key为handle_id,value为pc
            flushPendingRemoteIce(handle_id); //查看是否有保存的janus发送的trickle,如果有,则加入ice候选
            pc_ptr->SetRemoteDescription(offer.type, offer.sdp); //同时将远端的sdp设置到pc中
            pc_ptr->CreateAnswer(); //创建answer,answer是本地的sdp描述,用来和远端的sdp进行匹配

            if (remote_joined_notified_[feed_id]) {
                spdlog::info(
                    "[session] skip duplicate remote joined notify feed={} "
                    "(retry)",
                    feed_id);
            } else if (auto* obs = XRtcGlobal::instance().observer()) {
                XRTCRemoteUser user;
                user.feed_id = feed_id;
                if (auto it = feed_to_display_.find(feed_id);
                    it != feed_to_display_.end()) {
                    user.display = it->second;
                }
                remote_joined_notified_[feed_id] = true;
                spdlog::info("[session] remote user joined feed={} display={}",
                             feed_id, user.display);
                obs->on_remote_user_joined(user);
            }
        });
    return {};
}

void CallSession::flushPendingRemoteIce(uint64_t handle_id) {
    auto pit = pending_remote_ice_by_handle_.find(handle_id);
    if (pit == pending_remote_ice_by_handle_.end()) {
        return;
    }
    auto sit = subscriber_pcs_.find(handle_id);
    if (sit == subscriber_pcs_.end() || !sit->second) {
        return;
    }
    spdlog::info(
        "[session] flush {} early remote ICE for handle={}", pit->second.size(),
        handle_id);
    for (const auto& c : pit->second) {
        sit->second->AddIceCandidate(c.mid, c.idx, c.cand);
    }
    pending_remote_ice_by_handle_.erase(pit);
}

void CallSession::clearPendingRemoteIce(uint64_t handle_id) {
    if (pending_remote_ice_by_handle_.erase(handle_id) > 0) {
        spdlog::info("[session] clear pending remote ICE handle={}", handle_id);
    }
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
            //作为发布者收到远端ice候选
            if (handle_id == janus_->publisher_handle()) {
                if (publisher_pc_) {
                    publisher_pc_->AddIceCandidate(mid, idx, cand);
                } else {
                    spdlog::warn(
                        "[session] drop publisher remote ICE: no PC yet mid={} "
                        "cand={}",
                        mid, cand.substr(0, 80));
                }
                return;
            }
            //作为订阅者收到远端ice候选
            auto it = subscriber_pcs_.find(handle_id); //找到对应的订阅者pc,查看是否创建pc
            //janus发送trickle的时候,本地的pc可能还没有建立好,所以这个为空,我们先保存这个trickle,随后处理
            if (it != subscriber_pcs_.end() && it->second) { //如果pc已经创建,则直接加入ice候选
                it->second->AddIceCandidate(mid, idx, cand);
                return;
            }
            spdlog::warn(
                "[session] queue remote ICE (no subscriber PC yet) handle={} "
                "mid={} idx={} cand={}",
                handle_id, mid, idx, cand.substr(0, 80));

            //到了这里表示这个trickle确实是本地的pc还没有建立好,我们保存这个handle_id对应的trickle,随后处理
            pending_remote_ice_by_handle_[handle_id].push_back(
                PendingRemoteIce{mid, idx, cand});
        });
    return {};
}

slots_t<> CallSession::onJanusHangup(uint64_t handle_id,
                                     const std::string& reason) {
    const uint64_t gen = life_.generation;
    XRtcGlobal::instance().api_thread()->PostTask(
        [this, handle_id, reason, gen]() {
            if (gen != life_.generation) {
                return;
            }
            const std::string msg =
                reason.empty() ? "hangup" : ("hangup: " + reason);

            // 发布者挂断 → 整场失败
            if (handle_id != 0 && handle_id == janus_->publisher_handle()) {
                spdlog::error("[session] publisher hangup: {}", msg);
                if (life_.join_notified) {
                    failAfterJoined(XRtcError::kSignalingFailed, msg);
                } else {
                    failJoin(XRtcError::kSignalingFailed, msg);
                }
                return;
            }

            // 订阅者挂断（常见 ICE failed）→ 只拆该路并重试
            if (subscriber_pcs_.count(handle_id) > 0 ||
                handle_to_feed_.count(handle_id) > 0) {
                spdlog::warn("[session] subscriber hangup handle={} reason={}",
                             handle_id, reason);
                dropSubscriber(handle_id, msg, /*retry=*/true);
                return;
            }

            // 未知 handle：可能是已清理的订阅，或异常；已进房则不拆整场
            spdlog::warn("[session] ignore hangup for unknown handle={} reason={}",
                         handle_id, reason);
            if (!life_.join_notified) {
                failJoin(XRtcError::kSignalingFailed, msg);
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

void CallSession::dropSubscriber(uint64_t handle_id, const std::string& reason,
                                 bool retry) {
    // 幂等：hangup 与 PC failed 可能连续到达
    if (subscriber_pcs_.count(handle_id) == 0 &&
        handle_to_feed_.count(handle_id) == 0) {
        clearPendingRemoteIce(handle_id);
        return;
    }

    uint64_t feed_id = 0;
    if (auto it = handle_to_feed_.find(handle_id); it != handle_to_feed_.end()) {
        feed_id = it->second;
        handle_to_feed_.erase(it);
    }
    subscriber_pcs_.erase(handle_id);
    clearPendingRemoteIce(handle_id);

    if (feed_id == 0) {
        spdlog::warn("[session] dropSubscriber: no feed for handle={} ({})",
                     handle_id, reason);
        return;
    }

    detachRemoteMedia(feed_id);
    // 释放 Janus 侧 handle，并清 subscribe_state，允许重新 Subscribe
    janus_->DetachSubscriber(feed_id);

    int retries = 0;
    if (auto rit = subscriber_retry_count_.find(feed_id);
        rit != subscriber_retry_count_.end()) {
        retries = rit->second;
    }

    if (retry && life_.active && retries < kMaxSubscriberRetries) {
        subscriber_retry_count_[feed_id] = retries + 1;
        spdlog::info(
            "[session] retry subscribe feed={} attempt={}/{} after: {}",
            feed_id, retries + 1, kMaxSubscriberRetries, reason);
        JanusPublisherInfo info;
        info.feed_id = feed_id;
        if (auto dit = feed_to_display_.find(feed_id);
            dit != feed_to_display_.end()) {
            info.display = dit->second;
        }
        const uint64_t gen = life_.generation;
        // 稍后再订，避开 Janus 刚 hangup 的瞬态
        XRtcGlobal::instance().api_thread()->PostDelayedTask(
            [this, info, gen]() {
                if (!isCurrentGeneration(gen) || !life_.active) {
                    return;
                }
                // 对端已离开则不再重订
                if (feed_to_display_.find(info.feed_id) ==
                    feed_to_display_.end()) {
                    subscriber_retry_count_.erase(info.feed_id);
                    return;
                }
                subscribeFeed(info);
            },
            webrtc::TimeDelta::Millis(800 * (retries + 1)));
        return;
    }

    spdlog::warn("[session] give up subscribe feed={} after: {}", feed_id,
                 reason);
    subscriber_retry_count_.erase(feed_id);
    remote_joined_notified_.erase(feed_id);
    if (auto* obs = XRtcGlobal::instance().observer()) {
        XRTCRemoteUser user;
        user.feed_id = feed_id;
        if (auto dit = feed_to_display_.find(feed_id);
            dit != feed_to_display_.end()) {
            user.display = dit->second;
            feed_to_display_.erase(dit);
        }
        obs->on_remote_user_left(user);
    } else {
        feed_to_display_.erase(feed_id);
    }
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
        feed_to_display_.clear();
        subscriber_retry_count_.clear();
        remote_joined_notified_.clear();
        pending_remote_ice_by_handle_.clear();
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
        // mute 时仍可能在采：关掉预览回调，推流由 track enable 控制
        if (capture_) {
            capture_->set_local_preview_enabled(!mute && local_video_capturing_);
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

Rest<> CallSession::StartLocalVideo() {
    auto run = [this]() -> Rest<> {
        if (!capture_) {
            spdlog::error("[session] StartLocalVideo: capture not ready");
            return xrtc_err(XRtcError::kVideoSourceNotInit);
        }
        capture_->set_local_preview_enabled(true);
        auto st = capture_->start();
        if (!st) {
            spdlog::error("[session] StartLocalVideo: capture start failed: {}",
                          XRtcErrorToString(st.error()));
            return st;
        }
        local_video_capturing_ = true;
        spdlog::info("[session] local video capture started");
        return xrtc_ok();
    };
    auto* api = XRtcGlobal::instance().api_thread();
    if (api && webrtc::Thread::Current() != api) {
        return api->BlockingCall(run);
    }
    return run();
}

Rest<> CallSession::StopLocalVideo() {
    auto run = [this]() -> Rest<> {
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
                obs->on_video_frame(capture_.get(),
                                    MakeXRTCVideoFrame(buffer, frame));
            }
        }
        if (capture_) {
            capture_->set_local_preview_enabled(false);
            (void)capture_->stop();
        }
        local_video_capturing_ = false;
        spdlog::info("[session] local video capture stopped");
        return xrtc_ok();
    };
    auto* api = XRtcGlobal::instance().api_thread();
    if (api && webrtc::Thread::Current() != api) {
        return api->BlockingCall(run);
    }
    return run();
}

Rest<> CallSession::StartLocalAudio() {
    auto run = [this]() -> Rest<> {
        if (!audio_capture_) {
            spdlog::error("[session] StartLocalAudio: audio_capture not ready");
            return xrtc_err(XRtcError::kMediaStartFailed);
        }
        auto st = audio_capture_->start();
        if (!st) {
            spdlog::error("[session] StartLocalAudio: start failed: {}",
                          XRtcErrorToString(st.error()));
            return st;
        }
        local_audio_capturing_ = true;
        spdlog::info("[session] local audio capture started");
        return xrtc_ok();
    };
    auto* api = XRtcGlobal::instance().api_thread();
    if (api && webrtc::Thread::Current() != api) {
        return api->BlockingCall(run);
    }
    return run();
}

Rest<> CallSession::StopLocalAudio() {
    auto run = [this]() -> Rest<> {
        if (audio_capture_) {
            (void)audio_capture_->StopHardwareRecording();
        }
        local_audio_capturing_ = false;
        spdlog::info("[session] local audio capture stopped");
        return xrtc_ok();
    };
    auto* api = XRtcGlobal::instance().api_thread();
    if (api && webrtc::Thread::Current() != api) {
        return api->BlockingCall(run);
    }
    return run();
}

Rest<> CallSession::SwitchAudioDevice(const std::string& device_id) {
    auto run = [this, device_id]() -> Rest<> {
        if (device_id.empty()) {
            spdlog::warn("[session] SwitchAudioDevice: empty device_id");
            return xrtc_err(XRtcError::kInvalidParam);
        }
        config_.audio_device_id = device_id;
        if (!audio_capture_) {
            spdlog::info(
                "[session] SwitchAudioDevice: remembered id={} (capture not "
                "ready)",
                device_id);
            return xrtc_ok();
        }
        auto st = audio_capture_->device_switch(device_id);
        if (!st) {
            spdlog::error("[session] SwitchAudioDevice failed id={} err={}",
                          device_id, XRtcErrorToString(st.error()));
        } else {
            spdlog::info("[session] SwitchAudioDevice ok id={}", device_id);
        }
        return st;
    };
    auto* api = XRtcGlobal::instance().api_thread();
    if (api && webrtc::Thread::Current() != api) {
        return api->BlockingCall(run);
    }
    return run();
}

Rest<> CallSession::SwitchVideoDevice(const std::string& device_id) {
    auto run = [this, device_id]() -> Rest<> {
        if (device_id.empty()) {
            spdlog::warn("[session] SwitchVideoDevice: empty device_id");
            return xrtc_err(XRtcError::kInvalidParam);
        }
        config_.video_device_id = device_id;
        if (!capture_) {
            spdlog::info(
                "[session] SwitchVideoDevice: remembered id={} (capture not "
                "ready)",
                device_id);
            return xrtc_ok();
        }
        auto st = capture_->device_switch(device_id);
        if (!st) {
            spdlog::error("[session] SwitchVideoDevice failed id={} err={}",
                          device_id, XRtcErrorToString(st.error()));
        } else {
            spdlog::info("[session] SwitchVideoDevice ok id={}", device_id);
        }
        return st;
    };
    auto* api = XRtcGlobal::instance().api_thread();
    if (api && webrtc::Thread::Current() != api) {
        return api->BlockingCall(run);
    }
    return run();
}

Rest<> CallSession::ensureLocalMedia() {
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
        auto open_st = audio_capture_->open();
        if (!open_st) {
            spdlog::error("[session] AudioCapture open failed, device_id={}",
                          config_.audio_device_id);
            return open_st;
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
        const auto handle = janus_->publisher_handle();
        spdlog::info(
            "[session] send publisher trickle handle={} mid={} idx={} cand={}",
            handle, mid, idx, cand.substr(0, 80));
        janus_->SendTrickle(handle, mid, idx, cand);
    };
    pcb.on_ice_gathering_complete = [this, gen]() {
        if (!isCurrentGeneration(gen)) {
            return;
        }
        spdlog::info("[session] send publisher trickle-complete handle={}",
                     janus_->publisher_handle());
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
    publisher_pc_->SetLabel("publisher");
    if (auto st = publisher_pc_->Init(config_.ice_servers); !st) {
        publisher_pc_.reset();
        failJoin(st.error(), "publisher pc init failed");
        return;
    }

    if (auto st = publisher_pc_->AddTrack(audio_track_, {"stream0"}); !st) {
        publisher_pc_.reset();
        failJoin(st.error(), "add audio track failed");
        return;
    }
    if (auto st = publisher_pc_->AddTrack(video_track_, {"stream0"}); !st) {
        publisher_pc_.reset();
        failJoin(st.error(), "add video track failed");
        return;
    }
    publisher_pc_->ConfigureVideoSend(config_.width, config_.height,
                                      config_.fps);
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
    spdlog::info("[session] publisher local SDP type={} bytes={} -> Publish",
                 type, sdp.size());
    JanusJsep jsep;
    jsep.type = type;
    jsep.sdp = sdp;
    janus_->Publish(jsep);
}

void CallSession::onSubscriberLocalSdp(uint64_t handle_id,
                                       const std::string& type,
                                       const std::string& sdp) {
    spdlog::info(
        "[session] subscriber local SDP handle={} type={} bytes={} -> start",
        handle_id, type, sdp.size());
    JanusJsep jsep;
    jsep.type = type;
    jsep.sdp = sdp;
    janus_->StartSubscriber(handle_id, jsep);
}

void CallSession::subscribeFeed(const JanusPublisherInfo& info) {
    if (info.feed_id != 0 && !info.display.empty()) {
        feed_to_display_[info.feed_id] = info.display;
    }
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
    video->AddOrUpdateSink(att.sink.get(), [] {
        webrtc::VideoSinkWants wants;
        // 远端渲染限到约 720p，避免多路全分辨率解码+上传
        wants.max_pixel_count = 1280 * 720;
        wants.target_pixel_count = 960 * 540;
        wants.max_framerate_fps = 30;
        wants.rotation_applied = true;
        return wants;
    }());
    remote_videos_[feed_id] = std::move(att);
    subscriber_retry_count_.erase(feed_id);
    spdlog::info("[session] remote video attached, feed={}", feed_id);
}

}  // namespace xrtc
