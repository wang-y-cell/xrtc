#pragma once

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

    /// 初始化,根据ice_servers配置创建peerconnection
    /// @return 如果创建成功返回true
    bool Init(const std::vector<XRTCIceServer>& ice_servers);
    /// 关闭peerconnection
    void Close();

    ///把本地媒体轨道加到当前peerconnection,准备随时将offer推给对端
    ///@param track 媒体轨道
    ///@param stream_ids 流ID
    ///@return 是否成功
    bool AddTrack(
        webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track,
        const std::vector<std::string>& stream_ids);

    /// 创建offer,准备将offer推给对端
    ///根据本端已 AddTrack 的音视频等，生成 offer SDP,用于本端主动推流
    void CreateOffer();
    /// 创建answer,准备将answer推给对端
    ///通常在已 SetRemoteDescription(offer) 之后，针对对端 offer 生成 answer SDP
    ///用于：本端拉流（subscriber 回答 Janus 下发的 offer）
    void CreateAnswer();
    /// 把对端（这里一般是 Janus）发来的 SDP，设成当前 PeerConnection 的「远端会话描述」，让本端 WebRTC 知道对方怎么收发、用什么编码、ICE/DTLS 参数是什么
    ///@param type 描述类型
    ///@param sdp 描述
    void SetRemoteDescription(const std::string& type, const std::string& sdp);
    /// 添加ice候选者,将ice候选者设置到peerconnection中
    ///@param sdp_mid 描述ID
    ///@param mline_index 媒体线索引
    ///@param candidate ice候选者
    void AddIceCandidate(const std::string& sdp_mid, int mline_index,
                         const std::string& candidate);

    /// 静音音频
    ///@param mute 是否静音
    void MuteAudio(bool mute);
    /// 静音视频
    ///@param mute 是否静音
    void MuteVideo(bool mute);

    /// 获取peerconnection
    ///@return peerconnection
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc() const {
        return pc_;
    }

    /// 信号变化回调
    ///@param state 信号状态
    void OnSignalingChange(
        webrtc::PeerConnectionInterface::SignalingState) override {}
    /// 数据通道回调
    ///@param channel 数据通道
    void OnDataChannel(
        webrtc::scoped_refptr<webrtc::DataChannelInterface>) override {}
    /// 重新协商回调
    void OnRenegotiationNeeded() override {}
    /// ice连接状态变化回调
    ///@param state ice连接状态
    void OnIceConnectionChange(
        webrtc::PeerConnectionInterface::IceConnectionState) override {}
    /// ice收集状态变化回调
    ///@param state ice收集状态
    void OnIceGatheringChange(
        webrtc::PeerConnectionInterface::IceGatheringState new_state) override;
    /// ice候选者回调
    ///@param candidate ice候选者
    void OnIceCandidate(const webrtc::IceCandidate* candidate) override;
    /// 轨道回调
    ///@param transceiver 轨道
    void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>
                     transceiver) override;
    /// 连接状态变化回调
    ///@param state 连接状态
    void OnConnectionChange(
        webrtc::PeerConnectionInterface::PeerConnectionState new_state) override;

private:
    friend class RemoteSetObserver;

    //表示暂时还不可以加进peerconnection 的远端ice候选
    struct PendingIceCandidate {
        std::string sdp_mid; //候选属于哪条媒体线的 mid（如 "0" 音频、"1" 视频）
        int mline_index = 0;//SDP 里第几条 m= 行的下标（0、1…）
        std::string candidate; //候选字符串，如 candidate:8421... typ host
    };

    /// 应用ice候选者,将ice候选者设置到peerconnection中
    ///@param sdp_mid 描述ID
    ///@param mline_index 媒体线索引
    ///@param candidate ice候选者
    void ApplyIceCandidate(const std::string& sdp_mid, int mline_index,
                           const std::string& candidate);
    /// 刷新待处理的ice候选者,将待处理的ice候选者设置到peerconnection中
    void FlushPendingIceCandidates();

    /// 创建PeerConnection工厂,在构造函数指定,它本身是创建peerConnection的
    /// 工厂,是用来创建PeerConnection的,不是PeerConnection
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
    /// @brief peerconnection对象,
    /// 在Init函数中创建,init中将ice和sdp的基础配置都配置好了
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_;
    /// 回调函数,在构造函数指定
    Callbacks callbacks_;
    bool remote_description_set_ = false;
    /// 所有待添加的ice
    std::vector<PendingIceCandidate> pending_remote_candidates_;
};

}  // namespace xrtc
