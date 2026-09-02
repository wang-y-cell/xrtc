#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "api/scoped_refptr.h"
#include "api/media_stream_interface.h"
#include "concurrency/signal_and_slots/signal_and_slots.h"
#include <janus/janus_client.h>
#include <media/remote_video_sink.h>
#include <media/audio_capture.h>
#include <media/vcm_capture.h>
#include <media/video_track_source.h>
#include <pc/peer_connection.h>
#include <xrtc/ixrtc_engine.h>
#include <internal/xrtc_result.h>
#include <session/session_lifecycle.h>

namespace xrtc {

template <typename T = void>
using slots_t = utils::slots_t<T>;

/// 串联 Janus 信令与 Publisher/Subscriber PeerConnection
class CallSession : public utils::object {
public:
    //将当前对象和janusclient绑定到同一个线程,并设置信号与槽连接
    CallSession();
    ~CallSession() override;

    CallSession(const CallSession&) = delete;
    CallSession& operator=(const CallSession&) = delete;

    ///@brief 启动通话,包括连接janus服务器
    ///@param config 加入房间的配置
    void Start(const XRTCJoinConfig& config);
    ///@brief 停止通话,包括关闭Publisher/Subscriber PeerConnection,断开janus服务器
    void Stop();

    ///@brief 静音音频（仅控制推流轨 enable，不管硬件采集）
    void MuteAudio(bool mute);
    ///@brief 静音视频（仅控制推流轨 enable，不管硬件采集）
    void MuteVideo(bool mute);

    /// 仅开启/停止本地视频硬件采集（不含 mute）
    bool StartLocalVideo();
    bool StopLocalVideo();
    /// 仅开启/停止本地音频硬件录音（不含 mute）
    bool StartLocalAudio();
    bool StopLocalAudio();

    bool local_video_capturing() const { return local_video_capturing_; }
    bool local_audio_capturing() const { return local_audio_capturing_; }

    bool active() const { return life_.active; }

private:
    void notifyJoinResult(XRtcError error, const std::string& message);
    /// 进房失败：断信令 + 清媒体 + on_join_result
    void failJoin(XRtcError error, const std::string& message);
    /// 进房后致命失败（信令/安静断线/ICE）：断信令 + 清媒体 + on_leave
    void failAfterJoined(XRtcError error, const std::string& message);
    /// 幂等清理本地 PC / 采集（须在 api 线程或经 BlockingCall）
    void cleanupMediaResources();
    void applyLifecycleAction(const SessionLifecycle::Action& action,
                              bool already_disconnected);
    /// PostTask / PC 回调入口：校验会话仍有效
    bool isCurrentGeneration(uint64_t gen) const {
        return life_.active && gen == life_.generation;
    }
    void bindJanusSignals();
    //本地客户端进入janus房间之后调用
    slots_t<> onJoinedAsPublisher();
    //当客户端进入janus房间之后,如果房间中还有人推流,则调用这个槽函数
    slots_t<> onPublishers(const std::vector<JanusPublisherInfo>& pubs);
    slots_t<> onPublisherLeft(uint64_t feed_id, const std::string& display);
    //收到janus的sdp offer,设置本地sdp描述
    slots_t<> onPublisherAnswer(const JanusJsep& jsep);
    //设置本地sdp描述,并发送给janus
    slots_t<> onSubscriberOffer(uint64_t feed_id, uint64_t handle_id,
                                const JanusJsep& offer);
    slots_t<> onRemoteCandidate(uint64_t handle_id, const std::string& mid,
                                int idx, const std::string& cand);
    slots_t<> onJanusError(const std::string& err);
    slots_t<> onJanusDestroyed();
    ///在 Janus 进房成功后，准备本地轨与采集器（默认不开采，由 StartLocal* 手动开）
    XRtcStatus ensureLocalMedia();
    /// 进房后默认禁推流，等待上层 start_local_*
    void muteLocalTracks(bool mute);
    ///设置peerconnectionHandler回调函数,创建peerconnectionHandler对象,调用init函数创建peerconnection对象,并将视频轨道和音频轨道加入进去
    void createPublisherPc();
    ///通过远端的信息之后发送janus请求订阅对方
    void subscribeFeed(const JanusPublisherInfo& info);
    ///设置本地sdp描述,同时将sdp发送给janus
    void onPublisherLocalSdp(const std::string& type, const std::string& sdp);
    void onSubscriberLocalSdp(uint64_t handle_id, const std::string& type,
                              const std::string& sdp);
    void attachRemoteTrack(
        uint64_t feed_id,
        webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track);
    void detachRemoteVideo(uint64_t feed_id);
    void detachRemoteMedia(uint64_t feed_id);
    void detachAllRemoteMedia();

    XRTCJoinConfig config_;
    SessionLifecycle life_;

    /// Beast 线程 emit → Queued 到此 worker；槽内再 PostTask 到 WebRTC api_thread
    std::unique_ptr<utils::worker_thread> signal_thread_;
    std::unique_ptr<JanusClient> janus_;
    std::vector<utils::scoped_connection> janus_conns_;

    std::unique_ptr<VcmCapture> capture_;
    std::unique_ptr<AudioCapture> audio_capture_;
    /// 接收来自vcmCapture采集的帧，并通过VideoBroadcaster推送到WebRTC
    webrtc::scoped_refptr<XrtcVideoTrackSource> video_source_;
    ///音频轨道
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> audio_track_;
    ///视频轨道
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> video_track_;
    bool local_video_capturing_ = false;
    bool local_audio_capturing_ = false;

    std::unique_ptr<PeerConnectionHandler> publisher_pc_;
    std::unordered_map<uint64_t, std::unique_ptr<PeerConnectionHandler>>
        subscriber_pcs_;
    std::unordered_map<uint64_t, uint64_t> handle_to_feed_;
    std::unordered_map<uint64_t, RemoteVideoAttachment> remote_videos_;
    /// 持有远端音频轨，保证其存活并由 ADM 混音播放
    std::unordered_map<uint64_t,
                       webrtc::scoped_refptr<webrtc::AudioTrackInterface>>
        remote_audio_tracks_;
};

}  // namespace xrtc
