#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include <xrtc/xrtc_defines.h>

namespace xrtc {

class PeerConnectionHandler : public webrtc::PeerConnectionObserver {
public:
    struct Callbacks {
        std::function<void(const std::string& type, const std::string& sdp)>
            on_local_description;
        std::function<void(const std::string& sdp_mid, int mline_index,
                           const std::string& candidate)>
            on_ice_candidate;
        std::function<void()> on_ice_gathering_complete;
        std::function<void(
            webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface>)>
            on_track;
        std::function<void(XRTCConnectionState)> on_connection_state;
        std::function<void(const std::string& error)> on_error;
    };

    PeerConnectionHandler(
        webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory,
        Callbacks callbacks);
    ~PeerConnectionHandler() override;

    bool Init(const std::vector<XRTCIceServer>& ice_servers);
    void Close();

    /// Close 后置 false；异步 observer 持有 shared_ptr 副本，避免 UAF
    std::shared_ptr<std::atomic<bool>> alive_flag() const { return alive_; }

    bool AddTrack(
        webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track,
        const std::vector<std::string>& stream_ids);

    void CreateOffer();
    void CreateAnswer();
    void SetRemoteDescription(const std::string& type, const std::string& sdp);
    void AddIceCandidate(const std::string& sdp_mid, int mline_index,
                         const std::string& candidate);

    void MuteAudio(bool mute);
    void MuteVideo(bool mute);

    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc() const {
        return pc_;
    }

    void OnSignalingChange(
        webrtc::PeerConnectionInterface::SignalingState) override {}
    void OnDataChannel(
        webrtc::scoped_refptr<webrtc::DataChannelInterface>) override {}
    void OnRenegotiationNeeded() override {}
    void OnIceConnectionChange(
        webrtc::PeerConnectionInterface::IceConnectionState) override {}
    void OnIceGatheringChange(
        webrtc::PeerConnectionInterface::IceGatheringState new_state) override;
    void OnIceCandidate(const webrtc::IceCandidate* candidate) override;
    void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>
                     transceiver) override;
    void OnConnectionChange(
        webrtc::PeerConnectionInterface::PeerConnectionState new_state) override;

private:
    friend class RemoteSetObserver;

    struct PendingIceCandidate {
        std::string sdp_mid;
        int mline_index = 0;
        std::string candidate;
    };

    void ApplyIceCandidate(const std::string& sdp_mid, int mline_index,
                           const std::string& candidate);
    void FlushPendingIceCandidates();

    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_;
    Callbacks callbacks_;
    bool remote_description_set_ = false;
    std::vector<PendingIceCandidate> pending_remote_candidates_;
    std::shared_ptr<std::atomic<bool>> alive_ =
        std::make_shared<std::atomic<bool>>(true);
};

}  // namespace xrtc
