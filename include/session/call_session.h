#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "api/scoped_refptr.h"
#include "component/signal_and_slots/signal_and_slots.h"
#include <janus/janus_client.h>
#include <media/remote_video_sink.h>
#include <media/vcm_capture.h>
#include <media/video_track_source.h>
#include <pc/peer_connection.h>
#include <xrtc/ixrtc_engine.h>
#include <xrtc/xrtc_result.h>

namespace xrtc {

/// 串联 Janus 信令与 Publisher/Subscriber PeerConnection（无 Qt）
class CallSession {
public:
    CallSession();
    ~CallSession();

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
    void onJoinedAsPublisher();
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
