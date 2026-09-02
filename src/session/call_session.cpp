#include <session/call_session.h>

#include "api/audio_options.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
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
        // 先卸 sink，再关 subscriber PC，避免帧回调写已释放内存
        detachRemoteMedia(feed_id);
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
            //如果这个ice是我推流的ice的回复
            if (handle_id == janus_->publisher_handle()) {
                if (publisher_pc_) {
                    publisher_pc_->AddIceCandidate(mid, idx, cand);
                }
                return;
            }
            //如果这个ice是拉流的回复
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
        const bool already_joined = join_notified_;
        active_ = false;
        if (janus_) {
            janus_->Disconnect();
        }
        const std::string msg =
            err.empty() ? "signaling failed (empty detail)" : err;
        if (already_joined) {
            // 进房成功后的 hangup/ICE failed：不能再走 on_join_result（已被吞掉）
            if (auto* obs = XRtcGlobal::instance().observer()) {
                obs->on_connection_state(XRTCConnectionState::kFailed);
                obs->on_leave(XRtcError::kSignalingFailed);
            }
        } else {
            notifyJoinResult(XRtcError::kSignalingFailed, msg);
        }
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
    const bool was_active = active_;
    active_ = false;

    if (janus_) {
        janus_->Disconnect();
    }

    auto cleanup = [this]() {
        // 1) 先从 VideoTrack 卸掉 sink，杜绝退房/退出时的堆损坏
        detachAllRemoteMedia();
        // 2) 再关 PC（Close 内会清空回调）
        publisher_pc_.reset();
        subscriber_pcs_.clear();
        handle_to_feed_.clear();
        // 3) 本地采集与轨
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

    if (webrtc::Thread::Current() == XRtcGlobal::instance().api_thread()) {
        cleanup();
    } else {
        XRtcGlobal::instance().api_thread()->BlockingCall(cleanup);
    }

    if (was_active) {
        if (auto* obs = XRtcGlobal::instance().observer()) {
            obs->on_leave(XRtcError::kNOERROR);
        }
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
    XRtcGlobal::instance().api_thread()->PostTask([this]() {
        spdlog::info(
            "[session] onJoinedAsPublisher: prepare local media + PC "
            "(capture deferred)");
        // 准备采集器与轨道，默认不开采、不推流
        auto st = ensureLocalMedia();
        if (!st) {
            spdlog::error("[session] ensureLocalMedia failed: {}",
                              XRtcErrorToString(st.error()));
            notifyJoinResult(st.error(), "failed to start local media");
            return;
        }
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
