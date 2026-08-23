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

}  // namespace

// Janus full-trickle 常在 answer 之前下发 candidate；须排队等 SetRemote 完成。
class RemoteSetObserver
    : public webrtc::SetRemoteDescriptionObserverInterface {
public:
    RemoteSetObserver(PeerConnectionHandler* handler,
                      PeerConnectionHandler::Callbacks callbacks)
        : handler_(handler), callbacks_(std::move(callbacks)) {}

    void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
        if (!error.ok()) {
            RTC_LOG(LS_ERROR) << "SetRemoteDescription failed: "
                              << error.message();
            if (callbacks_.on_error) {
                callbacks_.on_error(std::string("SetRemote: ") +
                                    error.message());
            }
            return;
        }
        if (handler_ && handler_->pc_) {
            handler_->remote_description_set_ = true;
            handler_->FlushPendingIceCandidates();
        }
    }

private:
    PeerConnectionHandler* handler_;
    PeerConnectionHandler::Callbacks callbacks_;
};

PeerConnectionHandler::PeerConnectionHandler(
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory,
    Callbacks callbacks)
    : factory_(std::move(factory)), callbacks_(std::move(callbacks)) {}

PeerConnectionHandler::~PeerConnectionHandler() {
    Close();
}

bool PeerConnectionHandler::Init
(const std::vector<XRTCIceServer>& ice_servers) {
    //在构造函数指定,如果还是nullptr,则返回false
    if (!factory_) {
        return false;
    }

    //创建peerconnection是使用的配置结构体
    webrtc::PeerConnectionInterface::RTCConfiguration config;
    //使用统一计划SDP语义,使用 Unified Plan SDP（现代 WebRTC 默认方式）
    config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;

    //如果传入的ice_servers为空,则使用默认的STUN服务器
    if (ice_servers.empty()) {
        //创建一个ice服务器结构体
        webrtc::PeerConnectionInterface::IceServer stun;
        //设置ice服务器的服务器地址,这里的默认地址是google的stun服务器
        stun.uri = "stun:stun.l.google.com:19302";
        //将ice服务器结构体添加到配置结构体中
        config.servers.push_back(stun);
    } else { //如果我们自己填写了ice服务器,则将ice服务器结构体添加到配置结构体中
        for (const auto& s : ice_servers) { //遍历ice服务器结构体
            webrtc::PeerConnectionInterface::IceServer server; //创建一个ice服务器结构体
            server.uri = s.uri; //设置ice服务器的服务器地址
            server.username = s.username; //设置ice服务器的服务器用户名
            server.password = s.password; //设置ice服务器的服务器密码
            config.servers.push_back(server); //将ice服务器结构体添加到配置结构体中
        }
    }
    //创建peerconnection依赖的结构体,告诉工厂观察者是谁,可选的自定义工厂有哪些
    //构造函数需要一个 PeerConnectionObserver*。
    //PeerConnectionHandler 继承了 webrtc::PeerConnectionObserver
    webrtc::PeerConnectionDependencies deps(this);
    //创建peerconnection,返回一个结果,如果结果是ok的,则将peerconnection赋值给pc_
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
    pending_remote_candidates_.clear();
    remote_description_set_ = false;
    if (pc_) {
        pc_->Close();
        pc_ = nullptr;
    }
}

bool PeerConnectionHandler::AddTrack(
    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track,
    const std::vector<std::string>& stream_ids) {
    //如果peerconnection为空或者媒体轨道为空,则返回false
    if (!pc_ || !track) {
        return false;
    }
    //将媒体轨道添加到peerconnection中,返回一个结果,如果结果是ok的,则返回true
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
    pc_->SetRemoteDescription(
        std::move(desc),
        webrtc::make_ref_counted<RemoteSetObserver>(this, callbacks_));
}

void PeerConnectionHandler::AddIceCandidate(const std::string& sdp_mid,
                                            int mline_index,
                                            const std::string& candidate) {
    if (!pc_ || candidate.empty()) {
        return;
    }
    if (!remote_description_set_) {
        RTC_LOG(LS_INFO) << "Queue remote ICE candidate until SetRemoteDescription: "
                         << candidate.substr(0, 80);
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
        RTC_LOG(LS_WARNING) << "CreateIceCandidate failed: " << error.description;
        return;
    }
    if (!pc_->AddIceCandidate(ice.get())) {
        RTC_LOG(LS_WARNING) << "AddIceCandidate rejected: " << candidate.substr(0, 80);
    } else {
        RTC_LOG(LS_INFO) << "AddIceCandidate ok: " << candidate.substr(0, 80);
    }
}

void PeerConnectionHandler::FlushPendingIceCandidates() {
    if (pending_remote_candidates_.empty()) {
        return;
    }
    RTC_LOG(LS_INFO) << "Flushing " << pending_remote_candidates_.size()
                     << " queued remote ICE candidate(s)";
    auto pending = std::move(pending_remote_candidates_);
    pending_remote_candidates_.clear();
    for (const auto& c : pending) {
        ApplyIceCandidate(c.sdp_mid, c.mline_index, c.candidate);
    }
}

void PeerConnectionHandler::MuteAudio(bool mute) {
    //没有pc就返回
    if (!pc_) {
        return;
    }
    //遍历所有 Sender（本端往外发的轨）
    for (const auto& sender : pc_->GetSenders()) {
        //找到 音频轨（kAudioKind）
        auto track = sender->track();
        if (track && track->kind() == webrtc::MediaStreamTrackInterface::kAudioKind) {
            //设置音频轨是否启用
            track->set_enabled(!mute);
        }
    }
}

void PeerConnectionHandler::MuteVideo(bool mute) {
    //没有pc就返回
    if (!pc_) {
        return;
    }
    //遍历所有 Sender（本端往外发的轨）
    for (const auto& sender : pc_->GetSenders()) {
        //找到 视频轨（kVideoKind）
        auto track = sender->track();
        if (track && track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
            //设置视频轨是否启用
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
    // Janus 本机关闭了 IPv6；跳过无用的 IPv6 candidate
    const auto typ_pos = cand.find(" typ ");
    const std::string head =
        typ_pos == std::string::npos ? cand : cand.substr(0, typ_pos);
    if (head.find(':') != std::string::npos &&
        head.find('.') == std::string::npos) {
        RTC_LOG(LS_INFO) << "Skip IPv6 ICE candidate: " << cand.substr(0, 80);
        return;
    }
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
