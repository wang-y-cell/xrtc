#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "api/scoped_refptr.h"
#include "api/media_stream_interface.h"
#include "concurrency/signal_and_slots.h"
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
///
/// 职责概览：
/// - 以 Publisher 身份进房、建推流 PC；对每个远端 feed 建一路 Subscriber PC 拉流；
/// - 转发 SDP / trickle ICE；处理 hangup、订阅失败重试与远端轨挂载；
/// - 本地采集默认不开，由 StartLocal* / Mute* 控制；
/// - 生命周期由 SessionLifecycle（life_）管理世代与进房/退房通知。
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
    Rest<> StartLocalVideo();
    Rest<> StopLocalVideo();
    /// 仅开启/停止本地音频硬件录音（不含 mute）
    Rest<> StartLocalAudio();
    Rest<> StopLocalAudio();

    /// 切换麦克风；更新 config，若采集器已创建则 device_switch
    Rest<> SwitchAudioDevice(const std::string& device_id);
    /// 切换摄像头；更新 config，若采集器已创建则 device_switch
    Rest<> SwitchVideoDevice(const std::string& device_id);

    /// 本地摄像头硬件是否正在采集
    bool local_video_capturing() const { return local_video_capturing_; }
    /// 本地麦克风硬件是否正在录音
    bool local_audio_capturing() const { return local_audio_capturing_; }

    /// 会话是否仍处于活跃（未进入拆除流程）
    bool active() const { return life_.active; }

private:
    /// 通知上层进房结果（成功或失败），并标记 join_notified
    void notifyJoinResult(XRtcError error, const std::string& message);
    /// 进房失败：断信令 + 清媒体 + on_join_result
    void failJoin(XRtcError error, const std::string& message);
    /// 进房后致命失败（信令/安静断线/ICE）：断信令 + 清媒体 + on_leave
    void failAfterJoined(XRtcError error, const std::string& message);
    /// 幂等清理本地 PC / 采集（须在 api 线程或经 BlockingCall）
    void cleanupMediaResources();
    /// 根据 SessionLifecycle::Action 执行断信令、清媒体、回调上层等
    void applyLifecycleAction(const SessionLifecycle::Action& action,
                              bool already_disconnected);
    /// PostTask / PC 回调入口：校验会话仍有效
    bool isCurrentGeneration(uint64_t gen) const {
        return life_.active && gen == life_.generation;
    }
    /// 连接 JanusClient 信号到本类槽（须在 signal_thread_ 上）
    void bindJanusSignals();
    //本地客户端进入janus房间之后调用
    /// 准备本地媒体（默认不开采）并 createPublisherPc，成功则 notifyJoinResult
    slots_t<> onJoinedAsPublisher();
    //当客户端进入janus房间之后,如果房间中还有人推流,则调用这个槽函数
    /// 对 publishers 列表逐个 subscribeFeed
    slots_t<> onPublishers(const std::vector<JanusPublisherInfo>& pubs);
    /// 远端停止推流或离开：卸 sink、detach 订阅、通知 on_remote_user_left
    slots_t<> onPublisherLeft(uint64_t feed_id, const std::string& display);
    //收到janus的sdp offer,设置本地sdp描述
    /// 实际为 Publisher 的 remote answer：SetRemoteDescription(answer)
    slots_t<> onPublisherAnswer(const JanusJsep& jsep);
    //设置本地sdp描述,并发送给janus,作为订阅者
    /// 建 Subscriber PC → SetRemote(offer) → CreateAnswer；处理 early ICE 排队
    slots_t<> onSubscriberOffer(uint64_t feed_id, uint64_t handle_id,
                                const JanusJsep& offer);
    /// Janus trickle：写入对应 publisher/subscriber PC；无 PC 时按 handle 暂存
    slots_t<> onRemoteCandidate(uint64_t handle_id, const std::string& mid,
                                int idx, const std::string& cand);
    /// publisher hangup → 整场失败；subscriber hangup → dropSubscriber 可重试
    slots_t<> onJanusHangup(uint64_t handle_id, const std::string& reason);
    /// 信令层错误：按是否已进房走 failJoin / failAfterJoined
    slots_t<> onJanusError(const std::string& err);
    /// WebSocket/会话销毁（含安静断线）：走 SessionLifecycle 清理
    slots_t<> onJanusDestroyed();
    /// 订阅路失败：只拆该 PC，可选稍后重试 Subscribe（不拆整场）
    void dropSubscriber(uint64_t handle_id, const std::string& reason,
                        bool retry);
    ///在 Janus 进房成功后，准备本地轨与采集器（默认不开采，由 StartLocal* 手动开）
    Rest<> ensureLocalMedia();
    /// 进房后默认禁推流，等待上层 start_local_*
    void muteLocalTracks(bool mute);
    ///设置peerconnectionHandler回调函数,创建peerconnectionHandler对象,调用init函数创建peerconnection对象,并将视频轨道和音频轨道加入进去
    void createPublisherPc();
    ///通过远端的信息之后发送janus请求订阅对方
    void subscribeFeed(const JanusPublisherInfo& info);
    ///设置本地sdp描述,同时将sdp发送给janus
    /// Publisher：本地 offer 就绪后 JanusClient::Publish
    void onPublisherLocalSdp(const std::string& type, const std::string& sdp);
    /// Subscriber：本地 answer 就绪后 JanusClient::StartSubscriber
    void onSubscriberLocalSdp(uint64_t handle_id, const std::string& type,
                              const std::string& sdp);
    /// OnTrack：挂远端音/视频（视频加 RemoteVideoSink → UI 回调）
    void attachRemoteTrack(
        uint64_t feed_id,
        webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track);
    /// 仅卸该 feed 的远端视频 sink
    void detachRemoteVideo(uint64_t feed_id);
    /// 卸该 feed 的远端音视频
    void detachRemoteMedia(uint64_t feed_id);
    /// 卸所有远端音视频
    void detachAllRemoteMedia();
    /// 将 pending_remote_ice_by_handle_ 中该 handle 的候选加入已建好的 subscriber PC
    /// 客户端创建好pc之后,查看是否有保存的janus发送的trickle,如果有,则加入ice候选
    void flushPendingRemoteIce(uint64_t handle_id);
    /// 丢弃该 handle 上尚未应用的 early trickle
    void clearPendingRemoteIce(uint64_t handle_id);

    XRTCJoinConfig config_;
    /// 进房/退房/失败世代与通知决策
    SessionLifecycle life_;

    /// Janus 在 Subscriber PC 创建前下发的 trickle（按 handle 暂存）
    struct PendingRemoteIce {
        std::string mid;
        int idx = 0;
        std::string cand;
    };

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

    /// 本端推流 PeerConnection（一路）
    std::unique_ptr<PeerConnectionHandler> publisher_pc_;
    /// 拉流 PeerConnection：handle_id -> PeerConnectionHandler,创建pc的时候设置,用来让我们知道远端发送ice的时候是否有对应的pc
    std::unordered_map<uint64_t, std::unique_ptr<PeerConnectionHandler>>
        subscriber_pcs_;
    /// subscriber handle_id → 远端 feed_id
    std::unordered_map<uint64_t, uint64_t> handle_to_feed_;
    /// feed_id → Janus display（会议客户端用 userId 字符串作 display）
    std::unordered_map<uint64_t, std::string> feed_to_display_;
    /// 订阅 ICE 失败后的重试次数（按 feed）
    std::unordered_map<uint64_t, int> subscriber_retry_count_;
    /// 已通知 UI「远端加入」的 feed（重试订阅时避免重复 joined）
    std::unordered_map<uint64_t, bool> remote_joined_notified_;
    /// janus发送她的trickle的时候如果PC尚未创建,就按 handle 暂存 Janus trickle,等PC创建好之后再加入ice候选
    std::unordered_map<uint64_t, std::vector<PendingRemoteIce>>
        pending_remote_ice_by_handle_;
    /// feed → 远端视频 sink 附着
    std::unordered_map<uint64_t, RemoteVideoAttachment> remote_videos_;
    /// 持有远端音频轨，保证其存活并由 ADM 混音播放
    std::unordered_map<uint64_t,
                       webrtc::scoped_refptr<webrtc::AudioTrackInterface>>
        remote_audio_tracks_;

    /// 单路订阅 ICE/PC 失败后的最大自动重试次数
    static constexpr int kMaxSubscriberRetries = 3;
};

}  // namespace xrtc
