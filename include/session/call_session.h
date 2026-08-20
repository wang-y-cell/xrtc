#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "api/scoped_refptr.h"
#include "concurrency/signal_and_slots/signal_and_slots.h"
#include <janus/janus_client.h>
#include <media/remote_video_sink.h>
#include <media/vcm_capture.h>
#include <media/video_track_source.h>
#include <pc/peer_connection.h>
#include <xrtc/ixrtc_engine.h>
#include <xrtc/xrtc_result.h>

namespace xrtc {

template <typename T = void>
using slots_t = utils::slots_t<T>;

/// 串联 Janus 信令与 Publisher/Subscriber PeerConnection（无 Qt）
class CallSession : public utils::object {
public:
    CallSession();
    ~CallSession() override;

    CallSession(const CallSession&) = delete;
    CallSession& operator=(const CallSession&) = delete;

    void Start(const XRTCJoinConfig& config);
    void Stop();

    void MuteAudio(bool mute);
    void MuteVideo(bool mute);

    bool active() const { return active_; }

private:
    void notifyJoinResult(XRtcError error, const std::string& message);
    void bindJanusSignals();
    slots_t<> onJoinedAsPublisher();
    slots_t<> onPublishers(const std::vector<JanusPublisherInfo>& pubs);
    slots_t<> onPublisherLeft(uint64_t feed_id, const std::string& display);
    slots_t<> onPublisherAnswer(const JanusJsep& jsep);
    slots_t<> onSubscriberOffer(uint64_t feed_id, uint64_t handle_id,
                                const JanusJsep& offer);
    slots_t<> onRemoteCandidate(uint64_t handle_id, const std::string& mid,
                                int idx, const std::string& cand);
    slots_t<> onJanusError(const std::string& err);
    slots_t<> onJanusDestroyed();
    XRtcStatus ensureLocalMedia();
    void createPublisherPc();
    void subscribeFeed(const JanusPublisherInfo& info);
    void onPublisherLocalSdp(const std::string& type, const std::string& sdp);
    void onSubscriberLocalSdp(uint64_t handle_id, const std::string& type,
                              const std::string& sdp);
    void attachRemoteTrack(
        uint64_t feed_id,
        webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track);

    XRTCJoinConfig config_;
    bool active_ = false;
    bool join_notified_ = false;

    /// Beast 线程 emit → Queued 到此 worker；槽内再 PostTask 到 WebRTC api_thread
    std::unique_ptr<utils::worker_thread> signal_thread_;
    std::unique_ptr<JanusClient> janus_;
    std::vector<utils::scoped_connection> janus_conns_;

    VcmCapture* capture_ = nullptr;
    webrtc::scoped_refptr<XrtcVideoTrackSource> video_source_;
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> audio_track_;
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> video_track_;

    std::unique_ptr<PeerConnectionHandler> publisher_pc_;
    std::unordered_map<uint64_t, std::unique_ptr<PeerConnectionHandler>>
        subscriber_pcs_;
    std::unordered_map<uint64_t, uint64_t> handle_to_feed_;
    std::unordered_map<uint64_t, std::unique_ptr<RemoteVideoSink>> remote_sinks_;
};

}  // namespace xrtc
