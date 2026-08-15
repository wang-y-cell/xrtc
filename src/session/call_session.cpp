#include <session/call_session.h>

#include "api/audio_options.h"
#include <engine/xrtc_global.h>
#include <spdlog/spdlog.h>

namespace xrtc {

CallSession::CallSession() {
    janus_ = std::make_unique<JanusClient>();
    setupJanusCallbacks();
}

CallSession::~CallSession() {
    Stop();
}

void CallSession::setupJanusCallbacks() {
    JanusClient::Callbacks cb;
    cb.on_joined_as_publisher = [this]() {
        XRtcGlobal::instance().api_thread()->PostTask(
            [this]() { onJoinedAsPublisher(); });
    };
    cb.on_publishers = [this](const std::vector<JanusPublisherInfo>& pubs) {
        XRtcGlobal::instance().api_thread()->PostTask([this, pubs]() {
            for (const auto& p : pubs) {
                subscribeFeed(p);
            }
        });
    };
    cb.on_publisher_left = [this](uint64_t feed_id, const std::string&) {
        XRtcGlobal::instance().api_thread()->PostTask([this, feed_id]() {
            for (auto it = handle_to_feed_.begin();
                 it != handle_to_feed_.end();) {
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
    };
    cb.on_publisher_answer = [this](const JanusJsep& jsep) {
        XRtcGlobal::instance().api_thread()->PostTask([this, jsep]() {
            if (publisher_pc_) {
                publisher_pc_->SetRemoteDescription(jsep.type, jsep.sdp);
            }
        });
    };
    cb.on_subscriber_offer =
        [this](uint64_t feed_id, uint64_t handle_id, const JanusJsep& offer) {
            XRtcGlobal::instance().api_thread()->PostTask(
                [this, feed_id, handle_id, offer]() {
                    handle_to_feed_[handle_id] = feed_id;

                    PeerConnectionHandler::Callbacks pcb;
                    pcb.on_local_description =
                        [this, handle_id](const std::string& type,
                                          const std::string& sdp) {
                            onSubscriberLocalSdp(handle_id, type, sdp);
                        };
                    pcb.on_ice_candidate =
                        [this, handle_id](const std::string& mid, int idx,
                                          const std::string& cand) {
                            janus_->SendTrickle(handle_id, mid, idx, cand);
                        };
                    pcb.on_ice_gathering_complete = [this, handle_id]() {
                        janus_->SendTrickleComplete(handle_id);
                    };
                    pcb.on_track =
                        [this, feed_id](
                            webrtc::scoped_refptr<
                                webrtc::MediaStreamTrackInterface> track) {
                            attachRemoteTrack(feed_id, track);
                        };
                    pcb.on_error = [](const std::string& err) {
                        spdlog::error("Subscriber PC error: {}", err);
                    };

                    auto factory =
                        XRtcGlobal::instance().GetOrCreatePeerConnectionFactory();
                    auto pc = std::make_unique<PeerConnectionHandler>(
                        factory, std::move(pcb));
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
        };
    cb.on_remote_candidate = [this](uint64_t handle_id, const std::string& mid,
                                    int idx, const std::string& cand) {
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
    };
    cb.on_error = [this](const std::string& err) {
        XRtcGlobal::instance().api_thread()->PostTask([this, err]() {
            notifyJoinResult(XRtcError::kSignalingFailed, err);
        });
    };
    cb.on_destroyed = []() { spdlog::info("Janus connection destroyed"); };
    janus_->set_callbacks(std::move(cb));
}

void CallSession::Start(const XRTCJoinConfig& config) {
    if (active_) {
        notifyJoinResult(XRtcError::kAlreadyInCall, "already in call");
        return;
    }
    if (config.janus_ws_url.empty()) {
        notifyJoinResult(XRtcError::kInvalidParam, "empty janus_ws_url");
        return;
    }

    config_ = config;
    join_notified_ = false;
    active_ = true;
    janus_->Connect(config_);
}

void CallSession::Stop() {
    if (!active_ && !publisher_pc_ && !capture_) {
        return;
    }
    active_ = false;

    janus_->Disconnect();

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
    join_notified_ = true;
    if (auto* obs = XRtcGlobal::instance().observer()) {
        obs->on_join_result(error, message);
    }
}

bool CallSession::ensureLocalMedia() {
    auto factory = XRtcGlobal::instance().GetOrCreatePeerConnectionFactory();
    if (!factory) {
        return false;
    }

    if (!video_source_) {
        video_source_ = XrtcVideoTrackSource::Create();
    }

    if (config_.video_device_id.empty()) {
        return false;
    }

    if (!capture_) {
        capture_ = VcmCapture::create(static_cast<size_t>(config_.width),
                                      static_cast<size_t>(config_.height),
                                      config_.fps, config_.video_device_id);
        if (!capture_) {
            return false;
        }
        capture_->set_track_source(video_source_);
        capture_->start();
    }

    if (!audio_track_) {
        auto audio_source = factory->CreateAudioSource(webrtc::AudioOptions());
        audio_track_ = factory->CreateAudioTrack("audio0", audio_source.get());
    }
    if (!video_track_) {
        video_track_ = factory->CreateVideoTrack(video_source_, "video0");
    }
    return audio_track_ != nullptr && video_track_ != nullptr;
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

void CallSession::onJoinedAsPublisher() {
    if (!ensureLocalMedia()) {
        notifyJoinResult(XRtcError::kMediaStartFailed,
                         "failed to start local media");
        return;
    }
    createPublisherPc();
    notifyJoinResult(XRtcError::kNOERROR, "joined");
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
