#include <session/call_session.h>

#include "api/audio_options.h"
#include <engine/xrtc_global.h>
#include <xrtc/xrtc_log.h>

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
    XRtcGlobal::instance().api_thread()->PostTask([this, pubs]() {
        //逐个订阅发布者,如果之前已经订阅了就跳过,订阅的过程中会发送相应的信息给janus
        for (const auto& p : pubs) {
            subscribeFeed(p);
        }
    });
    return {};
}

slots_t<> CallSession::onPublisherLeft(uint64_t feed_id,
                                              const std::string&) {
    XRtcGlobal::instance().api_thread()->PostTask([this, feed_id]() {
        for (auto it = handle_to_feed_.begin(); it != handle_to_feed_.end();) {
            if (it->second == feed_id) {
                subscriber_pcs_.erase(it->first);
                it = handle_to_feed_.erase(it);
            } else {
                ++it;
            }
        }
        remote_sinks_.erase(feed_id);
        if (auto* obs = XRtcGlobal::instance().observer()) {
            XRTCRemoteUser user;
            user.feed_id = feed_id;
            obs->on_remote_user_left(user);
        }
    });
    return {};
}

slots_t<> CallSession::onPublisherAnswer(const JanusJsep& jsep) {
    XRtcGlobal::instance().api_thread()->PostTask([this, jsep]() {
        if (publisher_pc_) {
            publisher_pc_->SetRemoteDescription(jsep.type, jsep.sdp);
        }
    });
    return {};
}

slots_t<> CallSession::onSubscriberOffer(uint64_t feed_id,
                                                uint64_t handle_id,
                                                const JanusJsep& offer) {
    XRtcGlobal::instance().api_thread()->PostTask(
        [this, feed_id, handle_id, offer]() {
            handle_to_feed_[handle_id] = feed_id;

            PeerConnectionHandler::Callbacks pcb;
            pcb.on_local_description =
                [this, handle_id](const std::string& type,
                                  const std::string& sdp) {
                    onSubscriberLocalSdp(handle_id, type, sdp);
                };
            pcb.on_ice_candidate = [this, handle_id](const std::string& mid,
                                                     int idx,
                                                     const std::string& cand) {
                janus_->SendTrickle(handle_id, mid, idx, cand);
            };
            pcb.on_ice_gathering_complete = [this, handle_id]() {
                janus_->SendTrickleComplete(handle_id);
            };
            pcb.on_track =
                [this, feed_id](
                    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface>
                        track) { attachRemoteTrack(feed_id, track); };
            pcb.on_error = [](const std::string& err) {
                spdlog::error("Subscriber PC error: {}", err);
            };

            auto factory =
                XRtcGlobal::instance().GetOrCreatePeerConnectionFactory();
            auto pc =
                std::make_unique<PeerConnectionHandler>(factory, std::move(pcb));
            if (!pc->Init(config_.ice_servers)) {
                notifyJoinResult(XRtcError::kPeerConnectionFailed,
                                 "subscriber pc init failed");
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
    XRtcGlobal::instance().api_thread()->PostTask(
        [this, handle_id, mid, idx, cand]() {
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
    XRtcGlobal::instance().api_thread()->PostTask([this, err]() {
        spdlog::error("[session] signaling error: {}", err);
        active_ = false;
        if (janus_) {
            janus_->Disconnect();
        }
        const std::string msg =
            err.empty() ? "signaling failed (empty detail)" : err;
        notifyJoinResult(XRtcError::kSignalingFailed, msg);
    });
    return {};
}

slots_t<> CallSession::onJanusDestroyed() {
    spdlog::info("Janus connection destroyed");
    return {};
}

void CallSession::Start(const XRTCJoinConfig& config) {
    //如果已经处于通话状态,则通知上层已经处于通话状态
    if (active_) {
        notifyJoinResult(XRtcError::kAlreadyInCall, "already in call");
        return;
    }
    //如果janus_ws_url为空,则通知上层参数错误
    if (config.janus_ws_url.empty()) {
        notifyJoinResult(XRtcError::kInvalidParam, "empty janus_ws_url");
        return;
    }

    config_ = config;
    join_notified_ = false;
    active_ = true;
    //连接janus服务器
    auto st = janus_->Connect(config_);
    if (!st) {
        //如果连接失败,则通知上层连接失败
        active_ = false;
        notifyJoinResult(st.error(),
                         std::string(XRtcErrorToString(st.error())));
    }
}

void CallSession::Stop() {
    active_ = false;

    if (janus_) {
        janus_->Disconnect();
    }

    auto cleanup = [this]() {
        publisher_pc_.reset();
        subscriber_pcs_.clear();
        handle_to_feed_.clear();
        remote_sinks_.clear();
        audio_track_ = nullptr;
        video_track_ = nullptr;
        video_source_ = nullptr;
        if (capture_) {
            capture_->stop();
            delete capture_;
            capture_ = nullptr;
        }
    };

    if (webrtc::Thread::Current() == XRtcGlobal::instance().api_thread()) {
        cleanup();
    } else {
        XRtcGlobal::instance().api_thread()->BlockingCall(cleanup);
    }

    if (auto* obs = XRtcGlobal::instance().observer()) {
        obs->on_leave(XRtcError::kNOERROR);
    }
}

///@brief 静音音频
///@param mute 是否静音
void CallSession::MuteAudio(bool mute) {
    XRtcGlobal::instance().api_thread()->PostTask([this, mute]() {
        if (publisher_pc_) {
            publisher_pc_->MuteAudio(mute);
        }
    });
}

void CallSession::MuteVideo(bool mute) {
    XRtcGlobal::instance().api_thread()->PostTask([this, mute]() {
        if (publisher_pc_) {
            publisher_pc_->MuteVideo(mute);
        }
    });
}

void CallSession::notifyJoinResult(XRtcError error,
                                   const std::string& message) {
    if (join_notified_) {
        return;
    }
    join_notified_ = true;
    if (auto* obs = XRtcGlobal::instance().observer()) {
        obs->on_join_result(error, message);
    }
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

    //创建并启动摄像头采集, 如果没有创建就创建
    if (!capture_) {
        capture_ = VcmCapture::create(static_cast<size_t>(config_.width),
                                      static_cast<size_t>(config_.height),
                                      config_.fps, config_.video_device_id);
        //如果创建失败,返回错误
        if (!capture_) {
            return xrtc_err(XRtcError::kMediaStartFailed);
        }
        //设置视频源
        capture_->set_track_source(video_source_);
        //启动摄像头采集
        capture_->start();
    }

    //创建音频轨道
    if (!audio_track_) {
        //用平台默认麦克风创建音频轨（AudioOptions() 为空表示默认配置）
        auto audio_source = factory->CreateAudioSource(webrtc::AudioOptions());
        audio_track_ = factory->CreateAudioTrack("audio0", audio_source.get());
    }
    //创建视频轨道
    if (!video_track_) {
        video_track_ = factory->CreateVideoTrack(video_source_, "video0");
    }
    if (!audio_track_ || !video_track_) {
        return xrtc_err(XRtcError::kMediaStartFailed);
    }
    return xrtc_ok();
}

void CallSession::createPublisherPc() {
    PeerConnectionHandler::Callbacks pcb;
    pcb.on_local_description = [this](const std::string& type,
                                      const std::string& sdp) {
        onPublisherLocalSdp(type, sdp);
    };
    pcb.on_ice_candidate = [this](const std::string& mid, int idx,
                                  const std::string& cand) {
        janus_->SendTrickle(janus_->publisher_handle(), mid, idx, cand);
    };
    pcb.on_ice_gathering_complete = [this]() {
        janus_->SendTrickleComplete(janus_->publisher_handle());
    };
    pcb.on_connection_state = [](XRTCConnectionState state) {
        if (auto* obs = XRtcGlobal::instance().observer()) {
            obs->on_connection_state(state);
        }
    };
    pcb.on_error = [this](const std::string& err) {
        notifyJoinResult(XRtcError::kPeerConnectionFailed, err);
    };

    auto factory = XRtcGlobal::instance().GetOrCreatePeerConnectionFactory();
    publisher_pc_ =
        std::make_unique<PeerConnectionHandler>(factory, std::move(pcb));
    if (!publisher_pc_->Init(config_.ice_servers)) {
        notifyJoinResult(XRtcError::kPeerConnectionFailed,
                         "publisher pc init failed");
        return;
    }

    publisher_pc_->AddTrack(audio_track_, {"stream0"});
    publisher_pc_->AddTrack(video_track_, {"stream0"});
    publisher_pc_->CreateOffer();
}

slots_t<> CallSession::onJoinedAsPublisher() {
    XRtcGlobal::instance().api_thread()->PostTask([this]() {
        spdlog::info(
            "[session] onJoinedAsPublisher: starting local media + PC");
        //创建并开启摄像头,启动视频采集
        auto st = ensureLocalMedia();
        if (!st) {
            spdlog::error("[session] ensureLocalMedia failed: {}",
                              XRtcErrorToString(st.error()));
            notifyJoinResult(st.error(), "failed to start local media");
            return;
        }
        //创建音频源和视频源,添加视频轨道和音频轨道,创建offer,设置本地sdp描述,并发送给janus
        createPublisherPc();
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

void CallSession::attachRemoteTrack(
    uint64_t feed_id,
    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track) {
    if (!track) {
        return;
    }
    if (track->kind() != webrtc::MediaStreamTrackInterface::kVideoKind) {
        return;
    }
    auto* video = static_cast<webrtc::VideoTrackInterface*>(track.get());
    auto sink = std::make_unique<RemoteVideoSink>(
        feed_id, [](uint64_t id, const XRTCVideoFrame& frame) {
            if (auto* obs = XRtcGlobal::instance().observer()) {
                obs->on_remote_video_frame(id, frame);
            }
        });
    video->AddOrUpdateSink(sink.get(), webrtc::VideoSinkWants());
    remote_sinks_[feed_id] = std::move(sink);
}

}  // namespace xrtc
