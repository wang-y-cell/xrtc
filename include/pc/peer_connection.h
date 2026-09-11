#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include <internal/xrtc_result.h>
#include <xrtc/xrtc_defines.h>

namespace xrtc {

class PeerConnectionHandler : public webrtc::PeerConnectionObserver {
public:
    struct Callbacks {
        /// 本地sdp描述,当本地sdp描述就绪后,会调用这个回调函数,将sdp描述发送给janus服务器
        std::function<void(const std::string& type, const std::string& sdp)>
            on_local_description;
        /// 当收到远端ice候选时,会调用这个回调函数,将本地的ice候选发送给janus服务器
        std::function<void(const std::string& sdp_mid, int mline_index,
                           const std::string& candidate)>
            on_ice_candidate;
        /// 当ice收集完成时,会调用这个回调函数,将ice收集完成信号发送给janus服务器
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

    Rest<> Init(const std::vector<XRTCIceServer>& ice_servers);
    void Close();

    /// 仅用于日志区分 publisher / subscriber
    void SetLabel(std::string label) { label_ = std::move(label); }
    const std::string& label() const { return label_; }

    /// Close 后置 false；异步 observer 持有 shared_ptr 副本，避免 UAF
    std::shared_ptr<std::atomic<bool>> alive_flag() const { return alive_; }

    Rest<> AddTrack(
        webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track,
        const std::vector<std::string>& stream_ids);

    /// 为视频 Sender 设置码率/帧率上限与降级策略（AddTrack 视频后调用）
    void ConfigureVideoSend(int width, int height, int fps);

    void CreateOffer();
    /// 若 SetRemote 尚未完成则延后到完成后再 CreateAnswer
    void CreateAnswer();
    void SetRemoteDescription(const std::string& type, const std::string& sdp);
    void AddIceCandidate(const std::string& sdp_mid, int mline_index,
                         const std::string& candidate);

    /// SetLocal 成功后：先通知信令，再放行本地 trickle
    void OnLocalDescriptionReady(const std::string& type,
                                 const std::string& sdp);

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
        webrtc::PeerConnectionInterface::IceConnectionState new_state) override;
    void OnIceGatheringChange(
        webrtc::PeerConnectionInterface::IceGatheringState new_state) override;
    void OnIceCandidate(const webrtc::IceCandidate* candidate) override;
    void OnIceCandidateError(const std::string& address,
                             int port,
                             const std::string& url,
                             int error_code,
                             const std::string& error_text) override;
    void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>
                     transceiver) override;
    void OnConnectionChange(
        webrtc::PeerConnectionInterface::PeerConnectionState new_state) override;

private:
    friend class RemoteSetObserver;
    friend class CreateSdpObserver;

    struct PendingIceCandidate {
        std::string sdp_mid;
        int mline_index = 0;
        std::string candidate;
    };

    void DoCreateAnswer();
    void ApplyIceCandidate(const std::string& sdp_mid, int mline_index,
                           const std::string& candidate);
    void FlushPendingRemoteIceCandidates();
    void FlushPendingLocalIceCandidates();

    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_;
    Callbacks callbacks_;
    std::string label_ = "pc";

    bool remote_description_set_ = false;
    bool create_answer_after_remote_ = false;
    /// 本地 SDP 已通过回调交给信令（Publish / StartSubscriber）之后才 trickle
    bool local_description_notified_ = false;
    bool gathering_complete_pending_ = false;

    std::vector<PendingIceCandidate> pending_remote_candidates_;
    std::vector<PendingIceCandidate> pending_local_candidates_;
    std::shared_ptr<std::atomic<bool>> alive_ =
        std::make_shared<std::atomic<bool>>(true);
};

}  // namespace xrtc
