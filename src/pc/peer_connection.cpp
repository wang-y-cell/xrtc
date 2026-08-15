#include <pc/peer_connection.h>

#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/rtp_sender_interface.h"
#include "api/set_remote_description_observer_interface.h"
#include "rtc_base/logging.h"

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

class CreateSdpObserver : public webrtc::CreateSessionDescriptionObserver {
public:
    CreateSdpObserver(
        webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc,
        PeerConnectionHandler::Callbacks callbacks,
        bool /*is_offer*/)
        : pc_(std::move(pc)), callbacks_(std::move(callbacks)) {}

    void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
        std::string sdp;
        desc->ToString(&sdp);
        const std::string type = webrtc::SdpTypeToString(desc->GetType());
        pc_->SetLocalDescription(
            webrtc::make_ref_counted<LocalSetObserver>(callbacks_, type, sdp)
                .get(),
            desc);
    }

    void OnFailure(webrtc::RTCError error) override {
        RTC_LOG(LS_ERROR) << "CreateSessionDescription failed: "
                          << error.message();
        if (callbacks_.on_error) {
            callbacks_.on_error(std::string("CreateSDP: ") + error.message());
        }
    }

private:
    class LocalSetObserver : public webrtc::SetSessionDescriptionObserver {
    public:
        LocalSetObserver(PeerConnectionHandler::Callbacks callbacks,
                         std::string type,
                         std::string sdp)
            : callbacks_(std::move(callbacks)),
              type_(std::move(type)),
              sdp_(std::move(sdp)) {}

        void OnSuccess() override {
            if (callbacks_.on_local_description) {
                callbacks_.on_local_description(type_, sdp_);
            }
        }

        void OnFailure(webrtc::RTCError error) override {
            RTC_LOG(LS_ERROR) << "SetLocalDescription failed: "
                              << error.message();
            if (callbacks_.on_error) {
                callbacks_.on_error(std::string("SetLocal: ") + error.message());
            }
        }

    private:
        PeerConnectionHandler::Callbacks callbacks_;
        std::string type_;
        std::string sdp_;
    };

    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_;
    PeerConnectionHandler::Callbacks callbacks_;
};

class RemoteSetObserver
    : public webrtc::SetRemoteDescriptionObserverInterface {
public:
    explicit RemoteSetObserver(PeerConnectionHandler::Callbacks callbacks)
        : callbacks_(std::move(callbacks)) {}

    void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
        if (!error.ok()) {
            RTC_LOG(LS_ERROR) << "SetRemoteDescription failed: "
                              << error.message();
            if (callbacks_.on_error) {
                callbacks_.on_error(std::string("SetRemote: ") +
                                    error.message());
            }
        }
    }

private:
    PeerConnectionHandler::Callbacks callbacks_;
};

}  // namespace

PeerConnectionHandler::PeerConnectionHandler(
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory,
    Callbacks callbacks)
    : factory_(std::move(factory)), callbacks_(std::move(callbacks)) {}

PeerConnectionHandler::~PeerConnectionHandler() {
    Close();
}

bool PeerConnectionHandler::Init(
    const std::vector<XRTCIceServer>& ice_servers) {
    if (!factory_) {
        return false;
    }

    webrtc::PeerConnectionInterface::RTCConfiguration config;
    config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;

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

    webrtc::PeerConnectionDependencies deps(this);
    auto result =
        factory_->CreatePeerConnectionOrError(config, std::move(deps));
    if (!result.ok()) {
        RTC_LOG(LS_ERROR) << "CreatePeerConnection failed: "
                          << result.error().message();
        return false;
    }
    pc_ = std::move(result.value());
    return pc_ != nullptr;
}

void PeerConnectionHandler::Close() {
    if (pc_) {
        pc_->Close();
        pc_ = nullptr;
    }
}

bool PeerConnectionHandler::AddTrack(
    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track,
    const std::vector<std::string>& stream_ids) {
    if (!pc_ || !track) {
        return false;
    }
    auto result = pc_->AddTrack(track, stream_ids);
    if (!result.ok()) {
        RTC_LOG(LS_ERROR) << "AddTrack failed: " << result.error().message();
        return false;
    }
    return true;
}

void PeerConnectionHandler::CreateOffer() {
    if (!pc_) {
        return;
    }
    pc_->CreateOffer(
        webrtc::make_ref_counted<CreateSdpObserver>(pc_, callbacks_, true)
            .get(),
        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
}

void PeerConnectionHandler::CreateAnswer() {
    if (!pc_) {
        return;
    }
    pc_->CreateAnswer(
        webrtc::make_ref_counted<CreateSdpObserver>(pc_, callbacks_, false)
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
    pc_->SetRemoteDescription(std::move(desc),
                              webrtc::make_ref_counted<RemoteSetObserver>(
                                  callbacks_));
}

void PeerConnectionHandler::AddIceCandidate(const std::string& sdp_mid,
                                            int mline_index,
                                            const std::string& candidate) {
    if (!pc_ || candidate.empty()) {
        return;
    }
    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::IceCandidate> ice(
        webrtc::CreateIceCandidate(sdp_mid, mline_index, candidate, &error));
    if (!ice) {
        RTC_LOG(LS_WARNING) << "CreateIceCandidate failed: " << error.description;
        return;
    }
    pc_->AddIceCandidate(ice.get());
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

void PeerConnectionHandler::OnIceGatheringChange(
    webrtc::PeerConnectionInterface::IceGatheringState new_state) {
    if (new_state ==
            webrtc::PeerConnectionInterface::kIceGatheringComplete &&
        callbacks_.on_ice_gathering_complete) {
        callbacks_.on_ice_gathering_complete();
    }
}

void PeerConnectionHandler::OnIceCandidate(
    const webrtc::IceCandidate* candidate) {
    if (!candidate || !callbacks_.on_ice_candidate) {
        return;
    }
    std::string cand;
    candidate->ToString(&cand);
    callbacks_.on_ice_candidate(candidate->sdp_mid(),
                                candidate->sdp_mline_index(), cand);
}

void PeerConnectionHandler::OnTrack(
    webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
    if (!transceiver || !callbacks_.on_track) {
        return;
    }
    auto receiver = transceiver->receiver();
    if (!receiver) {
        return;
    }
    callbacks_.on_track(receiver->track());
}

void PeerConnectionHandler::OnConnectionChange(
    webrtc::PeerConnectionInterface::PeerConnectionState new_state) {
    if (callbacks_.on_connection_state) {
        callbacks_.on_connection_state(ToXrtcState(new_state));
    }
}

}  // namespace xrtc
