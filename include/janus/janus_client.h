#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "component/signal_and_slots/signal_and_slots.h"
#include <janus/websocket_transport.h>
#include <xrtc/xrtc_defines.h>
#include <xrtc/xrtc_result.h>

namespace xrtc {

struct JanusJsep {
    std::string type;
    std::string sdp;
};

struct JanusPublisherInfo {
    uint64_t feed_id = 0;
    std::string display;
};

class JanusClient : public utils::object {
public:
    JanusClient();
    ~JanusClient() override;

    JanusClient(const JanusClient&) = delete;
    JanusClient& operator=(const JanusClient&) = delete;

    utils::signal<> joined_as_publisher;
    utils::signal<std::vector<JanusPublisherInfo>> publishers;
    utils::signal<uint64_t, std::string> publisher_left;
    utils::signal<JanusJsep> publisher_answer;
    utils::signal<uint64_t, uint64_t, JanusJsep> subscriber_offer;
    utils::signal<uint64_t, std::string, int, std::string> remote_candidate;
    utils::signal<std::string> error;
    utils::signal<> destroyed;

    XRtcStatus Connect(const XRTCJoinConfig& config);
    void Disconnect();

    void Publish(const JanusJsep& offer);
    void SendTrickle(uint64_t handle_id, const std::string& sdp_mid,
                     int mline_index, const std::string& candidate);
    void SendTrickleComplete(uint64_t handle_id);

    void Subscribe(uint64_t feed_id);
    void StartSubscriber(uint64_t handle_id, const JanusJsep& answer);

    uint64_t publisher_handle() const { return pub_handle_; }

private:
    using json = nlohmann::json;

    enum class PendingOp {
        kNone,
        kCreate,
        kAttachPub,
        kJoinPub,
        kAttachSub,
    };

    void bind_transport_signals();
    void send_json(const json& obj);
    std::string new_transaction();
    void on_ws_connected();
    void on_ws_message(const std::string& text);
    void on_ws_disconnected();
    void on_ws_error(const std::string& err);
    void handle_success(const json& msg);
    void handle_event(const json& msg);
    void handle_trickle(const json& msg);
    void create_session();
    void attach_publisher();
    void join_as_publisher();
    void attach_subscriber(uint64_t feed_id);
    void parse_jsep(const json& msg, JanusJsep* out);
    void start_keepalive();
    void stop_keepalive();

    std::unique_ptr<WebsocketTransport> transport_;
    std::vector<utils::scoped_connection> transport_conns_;
    XRTCJoinConfig config_;

    uint64_t session_id_ = 0;
    uint64_t pub_handle_ = 0;
    std::atomic<int> tx_seq_{0};

    std::unordered_map<std::string, PendingOp> tx_ops_;
    std::unordered_map<std::string, uint64_t> tx_feeds_;
    std::unordered_map<uint64_t, uint64_t> feed_to_handle_;
    std::mutex map_mutex_;

    std::thread keepalive_thread_;
    std::atomic<bool> keepalive_running_{false};
};

}  // namespace xrtc
