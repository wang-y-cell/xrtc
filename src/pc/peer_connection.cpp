#include <pc/peer_connection.h>

#include <atomic>
#include <utility>

#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/media_stream_interface.h"
#include "api/rtp_parameters.h"
#include "api/rtp_sender_interface.h"
#include "api/set_remote_description_observer_interface.h"
#include "rtc_base/logging.h"
#include <spdlog/spdlog.h>

namespace xrtc {
namespace {

XRTCConnectionState ToXrtcState(
    webrtc::PeerConnectionInterface::PeerConnectionState state) {
    switch (state) {
        case webrtc::PeerConnectionInterface::PeerConnectionState::kNew:
            return XRTCConnectionState::kNew;
        case webrtc::PeerConnectionInterface::PeerConnectionState::kConnecting:
            return XRTCConnectionState::kConnecting;
        case webrtc::PeerConnectionInterface::PeerConnectionState::kConnected:
            return XRTCConnectionState::kConnected;
        case webrtc::PeerConnectionInterface::PeerConnectionState::kDisconnected:
            return XRTCConnectionState::kDisconnected;
        case webrtc::PeerConnectionInterface::PeerConnectionState::kFailed:
            return XRTCConnectionState::kFailed;
        case webrtc::PeerConnectionInterface::PeerConnectionState::kClosed:
            return XRTCConnectionState::kClosed;
    }
    return XRTCConnectionState::kNew;
}

std::string IceGatheringStateName(
    webrtc::PeerConnectionInterface::IceGatheringState state) {
    switch (state) {
        case webrtc::PeerConnectionInterface::kIceGatheringNew:
            return "new";
        case webrtc::PeerConnectionInterface::kIceGatheringGathering:
            return "gathering";
        case webrtc::PeerConnectionInterface::kIceGatheringComplete:
            return "complete";
    }
    return "unknown";
}

}  // namespace

/// CreateOffer / CreateAnswer 异步完成后的回调
class CreateSdpObserver : public webrtc::CreateSessionDescriptionObserver {
public:
    CreateSdpObserver(
        PeerConnectionHandler* handler,
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
        PeerConnectionHandler::Callbacks callbacks,
        std::shared_ptr<std::atomic<bool>> alive)
        : handler_(handler),
          pc_(std::move(pc)),
          callbacks_(std::move(callbacks)),
          alive_(std::move(alive)) {}

    void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
        if (!alive_ || !alive_->load(std::memory_order_acquire) || !pc_) {
            delete desc;
            return;
        }
        std::string sdp;
        desc->ToString(&sdp);
        const std::string type = webrtc::SdpTypeToString(desc->GetType());
        spdlog::info("[pc:{}] Create{} ok, SetLocalDescription",
                     handler_ ? handler_->label() : "?", type);
        pc_->SetLocalDescription(
            webrtc::make_ref_counted<LocalSetObserver>(handler_, callbacks_,
                                                       alive_, type, sdp)
                .get(),
            desc);
    }

    void OnFailure(webrtc::RTCError error) override {
        if (!alive_ || !alive_->load(std::memory_order_acquire)) {
            return;
        }
        spdlog::error("[pc:{}] CreateSessionDescription failed: {}",
                      handler_ ? handler_->label() : "?", error.message());
        RTC_LOG(LS_ERROR) << "CreateSessionDescription failed: "
                          << error.message();
        if (callbacks_.on_error) {
            callbacks_.on_error(std::string("CreateSDP: ") + error.message());
        }
    }

private:
    class LocalSetObserver : public webrtc::SetSessionDescriptionObserver {
    public:
        LocalSetObserver(PeerConnectionHandler* handler,
                         PeerConnectionHandler::Callbacks callbacks,
                         std::shared_ptr<std::atomic<bool>> alive,
                         std::string type,
                         std::string sdp)
            : handler_(handler),
              callbacks_(std::move(callbacks)),
              alive_(std::move(alive)),
              type_(std::move(type)),
              sdp_(std::move(sdp)) {}

        void OnSuccess() override {
            if (!alive_ || !alive_->load(std::memory_order_acquire)) {
                return;
            }
            if (handler_) {
                handler_->OnLocalDescriptionReady(type_, sdp_);
            } else if (callbacks_.on_local_description) {
                callbacks_.on_local_description(type_, sdp_);
            }
        }

        void OnFailure(webrtc::RTCError error) override {
            if (!alive_ || !alive_->load(std::memory_order_acquire)) {
                return;
            }
            spdlog::error("[pc:{}] SetLocalDescription failed: {}",
                          handler_ ? handler_->label() : "?", error.message());
            RTC_LOG(LS_ERROR) << "SetLocalDescription failed: "
                              << error.message();
            if (callbacks_.on_error) {
                callbacks_.on_error(std::string("SetLocal: ") + error.message());
            }
        }

    private:
        PeerConnectionHandler* handler_;
        PeerConnectionHandler::Callbacks callbacks_;
        std::shared_ptr<std::atomic<bool>> alive_;
        std::string type_;
        std::string sdp_;
    };

    PeerConnectionHandler* handler_;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_;
    PeerConnectionHandler::Callbacks callbacks_;
    std::shared_ptr<std::atomic<bool>> alive_;
};

// Janus full-trickle 常在 answer 之前下发 candidate；须排队等 SetRemote 完成。
class RemoteSetObserver
    : public webrtc::SetRemoteDescriptionObserverInterface {
public:
    RemoteSetObserver(PeerConnectionHandler* handler,
                      PeerConnectionHandler::Callbacks callbacks,
                      std::shared_ptr<std::atomic<bool>> alive)
        : handler_(handler),
          callbacks_(std::move(callbacks)),
          alive_(std::move(alive)) {}

    void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
        if (!alive_ || !alive_->load(std::memory_order_acquire)) {
            return;
        }
        if (!error.ok()) {
            spdlog::error("[pc:{}] SetRemoteDescription failed: {}",
                          handler_ ? handler_->label() : "?", error.message());
            RTC_LOG(LS_ERROR) << "SetRemoteDescription failed: "
                              << error.message();
            if (callbacks_.on_error) {
                callbacks_.on_error(std::string("SetRemote: ") +
                                    error.message());
            }
            return;
        }
        if (!handler_ || !handler_->pc_) {
            return;
        }
        handler_->remote_description_set_ = true;
        spdlog::info(
            "[pc:{}] SetRemoteDescription ok; flush {} queued remote ICE; "
            "create_answer_pending={}",
            handler_->label(), handler_->pending_remote_candidates_.size(),
            handler_->create_answer_after_remote_);
        handler_->FlushPendingRemoteIceCandidates();
        if (handler_->create_answer_after_remote_) {
            handler_->create_answer_after_remote_ = false;
            handler_->DoCreateAnswer();
        }
    }

private:
    PeerConnectionHandler* handler_;
    PeerConnectionHandler::Callbacks callbacks_;
    std::shared_ptr<std::atomic<bool>> alive_;
};

PeerConnectionHandler::PeerConnectionHandler(
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory,
    Callbacks callbacks)
    : factory_(std::move(factory)), callbacks_(std::move(callbacks)) {}

PeerConnectionHandler::~PeerConnectionHandler() {
    Close();
}

Rest<> PeerConnectionHandler::Init(const std::vector<XRTCIceServer>& ice_servers) {
    if (!factory_) {
        return xrtc_err(XRtcError::kPeerConnectionFailed);
    }

    webrtc::PeerConnectionInterface::RTCConfiguration config;
    config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
    // Janus 要求 RTCP-mux；显式 Require 避免协商偏差
    config.rtcp_mux_policy =
        webrtc::PeerConnectionInterface::kRtcpMuxPolicyRequire;
    config.bundle_policy =
        webrtc::PeerConnectionInterface::kBundlePolicyMaxBundle;

    if (ice_servers.empty()) {
        webrtc::PeerConnectionInterface::IceServer stun;
        stun.uri = "stun:stun.l.google.com:19302";
        config.servers.push_back(stun);
    } else {
        for (const auto& s : ice_servers) {
            webrtc::PeerConnectionInterface::IceServer server;
            server.uri = s.uri;
            server.username = s.username;
            server.password = s.password;
            config.servers.push_back(server);
        }
    }
    spdlog::info("[pc:{}] Init ice_servers={} bundle=max rtcp_mux=require",
                 label_, config.servers.size());

    webrtc::PeerConnectionDependencies deps(this);
    auto result =
        factory_->CreatePeerConnectionOrError(config, std::move(deps));
    if (!result.ok()) {
        spdlog::error("[pc:{}] CreatePeerConnection failed: {}", label_,
                      result.error().message());
        RTC_LOG(LS_ERROR) << "CreatePeerConnection failed: "
                          << result.error().message();
        return xrtc_err(XRtcError::kPeerConnectionFailed);
    }
    pc_ = std::move(result.value());
    if (!pc_) {
        return xrtc_err(XRtcError::kPeerConnectionFailed);
    }
    return xrtc_ok();
}

void PeerConnectionHandler::Close() {
    if (alive_) {
        alive_->store(false, std::memory_order_release);
    }
    callbacks_ = {};
    pending_remote_candidates_.clear();
    pending_local_candidates_.clear();
    remote_description_set_ = false;
    create_answer_after_remote_ = false;
    local_description_notified_ = false;
    gathering_complete_pending_ = false;
    if (pc_) {
        pc_->Close();
        pc_ = nullptr;
    }
}

Rest<> PeerConnectionHandler::AddTrack(
    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track,
    const std::vector<std::string>& stream_ids) {
    if (!pc_ || !track) {
        return xrtc_err(XRtcError::kInvalidParam);
    }
    auto result = pc_->AddTrack(track, stream_ids);
    if (!result.ok()) {
        RTC_LOG(LS_ERROR) << "AddTrack failed: " << result.error().message();
        return xrtc_err(XRtcError::kPeerConnectionFailed);
    }
    return xrtc_ok();
}

void PeerConnectionHandler::ConfigureVideoSend(int width, int height, int fps) {
    if (!pc_) {
        return;
    }
    // 不设 max_bitrate_bps：交给 WebRTC 拥塞控制自适应，便于排查卡顿是否被硬上限卡住
    const double max_fps = fps > 0 ? static_cast<double>(fps) : 30.0;

    for (const auto& sender : pc_->GetSenders()) {
        auto track = sender->track();
        if (!track ||
            track->kind() != webrtc::MediaStreamTrackInterface::kVideoKind) {
            continue;
        }

        auto* video =
            static_cast<webrtc::VideoTrackInterface*>(track.get());
        video->set_content_hint(
            webrtc::VideoTrackInterface::ContentHint::kFluid);

        webrtc::RtpParameters params = sender->GetParameters();
        params.degradation_preference =
            webrtc::DegradationPreference::BALANCED;
        if (!params.encodings.empty()) {
            params.encodings[0].max_bitrate_bps.reset();
            params.encodings[0].max_framerate = max_fps;
        }
        const webrtc::RTCError err = sender->SetParameters(params);
        if (!err.ok()) {
            spdlog::warn("[pc:{}] ConfigureVideoSend SetParameters failed: {}",
                         label_, err.message());
        } else {
            spdlog::info(
                "[pc:{}] ConfigureVideoSend {}x{}@{} max_bitrate=unlimited",
                label_, width, height, static_cast<int>(max_fps));
        }
        break;
    }
}

void PeerConnectionHandler::CreateOffer() {
    if (!pc_) {
        return;
    }
    spdlog::info("[pc:{}] CreateOffer", label_);
    pc_->CreateOffer(
        webrtc::make_ref_counted<CreateSdpObserver>(this, pc_, callbacks_,
                                                    alive_)
            .get(),
        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
}

void PeerConnectionHandler::CreateAnswer() {
    if (!pc_) {
        return;
    }
    if (!remote_description_set_) {
        create_answer_after_remote_ = true;
        spdlog::info(
            "[pc:{}] defer CreateAnswer until SetRemoteDescription completes",
            label_);
        return;
    }
    DoCreateAnswer();
}

void PeerConnectionHandler::DoCreateAnswer() {
    if (!pc_) {
        return;
    }
    spdlog::info("[pc:{}] CreateAnswer", label_);
    pc_->CreateAnswer(
        webrtc::make_ref_counted<CreateSdpObserver>(this, pc_, callbacks_,
                                                    alive_)
            .get(),
        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
}

void PeerConnectionHandler::SetRemoteDescription(const std::string& type,
                                                 const std::string& sdp) {
    if (!pc_) {
        return;
    }
    auto sdp_type = webrtc::SdpTypeFromString(type);
    if (!sdp_type) {
        if (callbacks_.on_error) {
            callbacks_.on_error("Unknown SDP type: " + type);
        }
        return;
    }
    auto desc = webrtc::CreateSessionDescription(*sdp_type, sdp);
    if (!desc) {
        if (callbacks_.on_error) {
            callbacks_.on_error("Failed to parse remote SDP");
        }
        return;
    }
    spdlog::info("[pc:{}] SetRemoteDescription type={} sdp_bytes={}", label_,
                 type, sdp.size());
    pc_->SetRemoteDescription(
        std::move(desc),
        webrtc::make_ref_counted<RemoteSetObserver>(this, callbacks_, alive_));
}

void PeerConnectionHandler::OnLocalDescriptionReady(const std::string& type,
                                                    const std::string& sdp) {
    spdlog::info(
        "[pc:{}] local description ready type={} sdp_bytes={}; notify signaling "
        "then flush local ICE",
        label_, type, sdp.size());
    if (callbacks_.on_local_description) {
        callbacks_.on_local_description(type, sdp);
    }
    local_description_notified_ = true;
    FlushPendingLocalIceCandidates();
    if (gathering_complete_pending_) {
        gathering_complete_pending_ = false;
        spdlog::info("[pc:{}] emit deferred ice gathering complete", label_);
        if (callbacks_.on_ice_gathering_complete) {
            callbacks_.on_ice_gathering_complete();
        }
    }
}

void PeerConnectionHandler::AddIceCandidate(const std::string& sdp_mid,
                                            int mline_index,
                                            const std::string& candidate) {
    if (!pc_ || candidate.empty()) {
        return;
    }
    if (!remote_description_set_) {
        spdlog::info(
            "[pc:{}] queue remote ICE until SetRemote mid={} idx={} cand={}",
            label_, sdp_mid, mline_index, candidate.substr(0, 80));
        pending_remote_candidates_.push_back(
            PendingIceCandidate{sdp_mid, mline_index, candidate});
        return;
    }
    ApplyIceCandidate(sdp_mid, mline_index, candidate);
}

void PeerConnectionHandler::ApplyIceCandidate(const std::string& sdp_mid,
                                              int mline_index,
                                              const std::string& candidate) {
    if (!pc_ || candidate.empty()) {
        return;
    }
    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::IceCandidate> ice(
        webrtc::CreateIceCandidate(sdp_mid, mline_index, candidate, &error));
    if (!ice) {
        spdlog::warn("[pc:{}] CreateIceCandidate failed: {} cand={}", label_,
                     error.description, candidate.substr(0, 80));
        RTC_LOG(LS_WARNING) << "CreateIceCandidate failed: " << error.description;
        return;
    }
    // 回调版挂到 operations chain，避免旧 bool API 在错误状态下静默失败
    auto* raw = ice.get();
    const std::string mid = raw->sdp_mid();
    const int idx = raw->sdp_mline_index();
    const std::string cand_preview = candidate.substr(0, 80);
    pc_->AddIceCandidate(
        std::move(ice),
        [label = label_, alive = alive_, mid, idx, cand_preview](
            webrtc::RTCError err) {
            if (!alive || !alive->load(std::memory_order_acquire)) {
                return;
            }
            if (!err.ok()) {
                spdlog::warn(
                    "[pc:{}] AddIceCandidate rejected mid={} idx={} err={} "
                    "cand={}",
                    label, mid, idx, err.message(), cand_preview);
            } else {
                spdlog::info("[pc:{}] AddIceCandidate ok mid={} idx={} cand={}",
                             label, mid, idx, cand_preview);
            }
        });
}

void PeerConnectionHandler::FlushPendingRemoteIceCandidates() {
    if (pending_remote_candidates_.empty()) {
        return;
    }
    spdlog::info("[pc:{}] flushing {} queued remote ICE candidate(s)", label_,
                 pending_remote_candidates_.size());
    auto pending = std::move(pending_remote_candidates_);
    pending_remote_candidates_.clear();
    for (const auto& c : pending) {
        ApplyIceCandidate(c.sdp_mid, c.mline_index, c.candidate);
    }
}

void PeerConnectionHandler::FlushPendingLocalIceCandidates() {
    if (pending_local_candidates_.empty()) {
        return;
    }
    spdlog::info(
        "[pc:{}] flushing {} queued local ICE candidate(s) after local SDP",
        label_, pending_local_candidates_.size());
    auto pending = std::move(pending_local_candidates_);
    pending_local_candidates_.clear();
    for (const auto& c : pending) {
        if (callbacks_.on_ice_candidate) {
            callbacks_.on_ice_candidate(c.sdp_mid, c.mline_index, c.candidate);
        }
    }
}

void PeerConnectionHandler::MuteAudio(bool mute) {
    if (!pc_) {
        return;
    }
    for (const auto& sender : pc_->GetSenders()) {
        auto track = sender->track();
        if (track && track->kind() == webrtc::MediaStreamTrackInterface::kAudioKind) {
            track->set_enabled(!mute);
        }
    }
}

void PeerConnectionHandler::MuteVideo(bool mute) {
    if (!pc_) {
        return;
    }
    for (const auto& sender : pc_->GetSenders()) {
        auto track = sender->track();
        if (track && track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
            track->set_enabled(!mute);
        }
    }
}

void PeerConnectionHandler::OnIceConnectionChange(
    webrtc::PeerConnectionInterface::IceConnectionState new_state) {
    if (!alive_ || !alive_->load(std::memory_order_acquire)) {
        return;
    }
    spdlog::info("[pc:{}] ice_connection_state={}", label_,
                 std::string(webrtc::PeerConnectionInterface::AsString(
                     new_state)));
}

void PeerConnectionHandler::OnIceGatheringChange(
    webrtc::PeerConnectionInterface::IceGatheringState new_state) {
    if (!alive_ || !alive_->load(std::memory_order_acquire)) {
        return;
    }
    spdlog::info("[pc:{}] ice_gathering_state={}", label_,
                 IceGatheringStateName(new_state));
    if (new_state != webrtc::PeerConnectionInterface::kIceGatheringComplete) {
        return;
    }
    if (!local_description_notified_) {
        gathering_complete_pending_ = true;
        spdlog::info(
            "[pc:{}] defer ice gathering complete until local SDP notified",
            label_);
        return;
    }
    if (callbacks_.on_ice_gathering_complete) {
        callbacks_.on_ice_gathering_complete();
    }
}

void PeerConnectionHandler::OnIceCandidate(
    const webrtc::IceCandidate* candidate) {
    if (!alive_ || !alive_->load(std::memory_order_acquire)) {
        return;
    }
    if (!candidate || !callbacks_.on_ice_candidate) {
        return;
    }
    std::string cand;
    candidate->ToString(&cand);
    // Janus 本机关闭了 IPv6；跳过无用的 IPv6 candidate
    const auto typ_pos = cand.find(" typ ");
    const std::string head =
        typ_pos == std::string::npos ? cand : cand.substr(0, typ_pos);
    if (head.find(':') != std::string::npos &&
        head.find('.') == std::string::npos) {
        spdlog::info("[pc:{}] skip IPv6 ICE candidate: {}", label_,
                     cand.substr(0, 80));
        return;
    }
    const std::string mid = candidate->sdp_mid();
    const int idx = candidate->sdp_mline_index();
    if (!local_description_notified_) {
        spdlog::info(
            "[pc:{}] queue local ICE until local SDP sent mid={} idx={} cand={}",
            label_, mid, idx, cand.substr(0, 80));
        pending_local_candidates_.push_back(
            PendingIceCandidate{mid, idx, std::move(cand)});
        return;
    }
    spdlog::info("[pc:{}] local ICE mid={} idx={} cand={}", label_, mid, idx,
                 cand.substr(0, 80));
    callbacks_.on_ice_candidate(mid, idx, cand);
}

void PeerConnectionHandler::OnIceCandidateError(const std::string& address,
                                                int port,
                                                const std::string& url,
                                                int error_code,
                                                const std::string& error_text) {
    if (!alive_ || !alive_->load(std::memory_order_acquire)) {
        return;
    }
    spdlog::warn(
        "[pc:{}] ice_candidate_error addr={}:{} url={} code={} text={}", label_,
        address, port, url, error_code, error_text);
}

void PeerConnectionHandler::OnTrack(
    webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
    if (!alive_ || !alive_->load(std::memory_order_acquire)) {
        return;
    }
    if (!transceiver || !callbacks_.on_track) {
        return;
    }
    auto receiver = transceiver->receiver();
    if (!receiver) {
        return;
    }
    auto track = receiver->track();
    spdlog::info("[pc:{}] OnTrack kind={}", label_,
                 track ? track->kind() : "?");
    callbacks_.on_track(track);
}

void PeerConnectionHandler::OnConnectionChange(
    webrtc::PeerConnectionInterface::PeerConnectionState new_state) {
    if (!alive_ || !alive_->load(std::memory_order_acquire)) {
        return;
    }
    spdlog::info("[pc:{}] connection_state={}", label_,
                 std::string(webrtc::PeerConnectionInterface::AsString(
                     new_state)));
    if (callbacks_.on_connection_state) {
        callbacks_.on_connection_state(ToXrtcState(new_state));
    }
}

}  // namespace xrtc
