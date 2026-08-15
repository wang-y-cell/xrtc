#include <janus/janus_client.h>

#include <chrono>

#include <spdlog/spdlog.h>

namespace xrtc {

JanusClient::JanusClient() {
    transport_ = std::make_unique<WebsocketTransport>();
    transport_->set_callbacks(
        [this](const std::string& text) { on_ws_message(text); },
        [this]() { on_ws_connected(); },
        [this]() { on_ws_disconnected(); },
        [this](const std::string& err) {
            if (callbacks_.on_error) {
                callbacks_.on_error(err);
            }
        });
}

JanusClient::~JanusClient() {
    Disconnect();
}

void JanusClient::set_callbacks(Callbacks callbacks) {
    callbacks_ = std::move(callbacks);
}

void JanusClient::Connect(const XRTCJoinConfig& config) {
    spdlog::info("[janus] Connect url={} room={} display={}",
                 config.janus_ws_url, config.room_id, config.display_name);
    config_ = config;
    session_id_ = 0;
    pub_handle_ = 0;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        feed_to_handle_.clear();
        tx_ops_.clear();
        tx_feeds_.clear();
    }
    if (!transport_->open(config.janus_ws_url)) {
        spdlog::error("[janus] transport open failed");
        if (callbacks_.on_error) {
            callbacks_.on_error("failed to open websocket");
        }
    }
}

void JanusClient::Disconnect() {
    stop_keepalive();
    if (session_id_ != 0) {
        if (pub_handle_ != 0) {
            json body = {{"request", "unpublish"}};
            send_json({{"janus", "message"},
                       {"session_id", session_id_},
                       {"handle_id", pub_handle_},
                       {"transaction", new_transaction()},
                       {"body", body}});
        }
        send_json({{"janus", "destroy"},
                   {"session_id", session_id_},
                   {"transaction", new_transaction()}});
    }
    transport_->close();
    session_id_ = 0;
    pub_handle_ = 0;
}

void JanusClient::send_json(const json& obj) {
    const std::string text = obj.dump();
    spdlog::info("[janus] TX {}", text.substr(0, 400));
    transport_->send_text(text);
}

std::string JanusClient::new_transaction() {
    const int id = ++tx_seq_;
    return "tx-" + std::to_string(id);
}

void JanusClient::start_keepalive() {
    stop_keepalive();
    keepalive_running_ = true;
    keepalive_thread_ = std::thread([this]() {
        while (keepalive_running_) {
            for (int i = 0; i < 250 && keepalive_running_; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!keepalive_running_ || session_id_ == 0) {
                continue;
            }
            send_json({{"janus", "keepalive"},
                       {"session_id", session_id_},
                       {"transaction", new_transaction()}});
        }
    });
}

void JanusClient::stop_keepalive() {
    keepalive_running_ = false;
    if (keepalive_thread_.joinable()) {
        keepalive_thread_.join();
    }
}

void JanusClient::on_ws_connected() {
    spdlog::info("Janus websocket connected");
    create_session();
}

void JanusClient::on_ws_disconnected() {
    stop_keepalive();
    if (callbacks_.on_destroyed) {
        callbacks_.on_destroyed();
    }
}

void JanusClient::create_session() {
    const std::string tx = new_transaction();
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        tx_ops_[tx] = PendingOp::kCreate;
    }
    send_json({{"janus", "create"}, {"transaction", tx}});
}

void JanusClient::attach_publisher() {
    const std::string tx = new_transaction();
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        tx_ops_[tx] = PendingOp::kAttachPub;
    }
    send_json({{"janus", "attach"},
               {"plugin", "janus.plugin.videoroom"},
               {"session_id", session_id_},
               {"transaction", tx}});
}

void JanusClient::join_as_publisher() {
    const std::string tx = new_transaction();
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        tx_ops_[tx] = PendingOp::kJoinPub;
    }
    json body = {{"request", "join"},
                 {"ptype", "publisher"},
                 {"room", config_.room_id},
                 {"display", config_.display_name}};
    if (!config_.pin.empty()) {
        body["pin"] = config_.pin;
    }
    send_json({{"janus", "message"},
               {"session_id", session_id_},
               {"handle_id", pub_handle_},
               {"transaction", tx},
               {"body", body}});
}

void JanusClient::Publish(const JanusJsep& offer) {
    json body = {{"request", "configure"}, {"audio", true}, {"video", true}};
    json jsep = {{"type", offer.type}, {"sdp", offer.sdp}};
    send_json({{"janus", "message"},
               {"session_id", session_id_},
               {"handle_id", pub_handle_},
               {"transaction", new_transaction()},
               {"body", body},
               {"jsep", jsep}});
}

void JanusClient::SendTrickle(uint64_t handle_id, const std::string& sdp_mid,
                              int mline_index, const std::string& candidate) {
    json cand = {{"candidate", candidate},
                 {"sdpMid", sdp_mid},
                 {"sdpMLineIndex", mline_index}};
    send_json({{"janus", "trickle"},
               {"session_id", session_id_},
               {"handle_id", handle_id},
               {"transaction", new_transaction()},
               {"candidate", cand}});
}

void JanusClient::SendTrickleComplete(uint64_t handle_id) {
    send_json({{"janus", "trickle"},
               {"session_id", session_id_},
               {"handle_id", handle_id},
               {"transaction", new_transaction()},
               {"candidate", {{"completed", true}}}});
}

void JanusClient::Subscribe(uint64_t feed_id) {
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        if (feed_to_handle_.count(feed_id)) {
            return;
        }
    }
    attach_subscriber(feed_id);
}

void JanusClient::attach_subscriber(uint64_t feed_id) {
    const std::string tx = new_transaction();
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        tx_ops_[tx] = PendingOp::kAttachSub;
        tx_feeds_[tx] = feed_id;
    }
    send_json({{"janus", "attach"},
               {"plugin", "janus.plugin.videoroom"},
               {"session_id", session_id_},
               {"transaction", tx}});
}

void JanusClient::StartSubscriber(uint64_t handle_id, const JanusJsep& answer) {
    json body = {{"request", "start"}, {"room", config_.room_id}};
    json jsep = {{"type", answer.type}, {"sdp", answer.sdp}};
    send_json({{"janus", "message"},
               {"session_id", session_id_},
               {"handle_id", handle_id},
               {"transaction", new_transaction()},
               {"body", body},
               {"jsep", jsep}});
}

void JanusClient::parse_jsep(const json& msg, JanusJsep* out) {
    if (!out || !msg.contains("jsep")) {
        return;
    }
    const auto& jsep = msg.at("jsep");
    out->type = jsep.value("type", "");
    out->sdp = jsep.value("sdp", "");
}

void JanusClient::on_ws_message(const std::string& text) {
    spdlog::info("[janus] RX {}", text.substr(0, 500));
    json msg;
    try {
        msg = json::parse(text);
    } catch (const std::exception& e) {
        spdlog::warn("invalid Janus JSON: {}", e.what());
        return;
    }

    const std::string janus = msg.value("janus", "");
    spdlog::info("[janus] message type={}", janus);
    if (janus == "success") {
        handle_success(msg);
    } else if (janus == "event") {
        handle_event(msg);
    } else if (janus == "trickle") {
        handle_trickle(msg);
    } else if (janus == "hangup") {
        const std::string reason = msg.value("reason", "hangup");
        spdlog::warn("[janus] hangup: {}", reason);
        if (callbacks_.on_error) {
            callbacks_.on_error(std::string("hangup: ") + reason);
        }
    } else if (janus == "error" || janus == "timeout") {
        std::string reason = janus;
        if (msg.contains("error") && msg["error"].is_object()) {
            reason = msg["error"].value("reason", janus);
        }
        spdlog::error("[janus] gateway {}: {}", janus, reason);
        if (callbacks_.on_error) {
            callbacks_.on_error(reason);
        }
    }
}

void JanusClient::handle_success(const json& msg) {
    const std::string tx = msg.value("transaction", "");
    PendingOp op = PendingOp::kNone;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        auto it = tx_ops_.find(tx);
        if (it != tx_ops_.end()) {
            op = it->second;
            tx_ops_.erase(it);
        }
    }

    const json data = msg.value("data", json::object());

    if (op == PendingOp::kCreate) {
        session_id_ = data.value("id", static_cast<uint64_t>(0));
        spdlog::info("Janus session {}", session_id_);
        start_keepalive();
        attach_publisher();
        return;
    }

    if (op == PendingOp::kAttachPub) {
        pub_handle_ = data.value("id", static_cast<uint64_t>(0));
        spdlog::info("Janus publisher handle {}", pub_handle_);
        join_as_publisher();
        return;
    }

    if (op == PendingOp::kAttachSub) {
        const uint64_t handle = data.value("id", static_cast<uint64_t>(0));
        uint64_t feed = 0;
        {
            std::lock_guard<std::mutex> lock(map_mutex_);
            auto fit = tx_feeds_.find(tx);
            if (fit != tx_feeds_.end()) {
                feed = fit->second;
                tx_feeds_.erase(fit);
            }
            feed_to_handle_[feed] = handle;
        }

        json body = {{"request", "join"},
                     {"ptype", "subscriber"},
                     {"room", config_.room_id},
                     {"feed", feed}};
        if (!config_.pin.empty()) {
            body["pin"] = config_.pin;
        }
        send_json({{"janus", "message"},
                   {"session_id", session_id_},
                   {"handle_id", handle},
                   {"transaction", new_transaction()},
                   {"body", body}});
    }
}

void JanusClient::handle_event(const json& msg) {
    const json plugindata = msg.value("plugindata", json::object());
    const json data = plugindata.value("data", json::object());
    const std::string videoroom = data.value("videoroom", "");
    const uint64_t handle_id = msg.value("sender", static_cast<uint64_t>(0));

    auto collect_publishers = [](const json& data_obj) {
        std::vector<JanusPublisherInfo> pubs;
        if (!data_obj.contains("publishers") || !data_obj["publishers"].is_array()) {
            return pubs;
        }
        for (const auto& p : data_obj["publishers"]) {
            JanusPublisherInfo info;
            info.feed_id = p.value("id", static_cast<uint64_t>(0));
            info.display = p.value("display", "");
            pubs.push_back(info);
        }
        return pubs;
    };

    if (videoroom == "joined") {
        if (callbacks_.on_joined_as_publisher) {
            callbacks_.on_joined_as_publisher();
        }
        auto pubs = collect_publishers(data);
        if (!pubs.empty() && callbacks_.on_publishers) {
            callbacks_.on_publishers(pubs);
        }
    } else if (videoroom == "event") {
        auto pubs = collect_publishers(data);
        if (!pubs.empty() && callbacks_.on_publishers) {
            callbacks_.on_publishers(pubs);
        }
        if (data.contains("unpublished") || data.contains("leaving")) {
            const uint64_t feed =
                data.contains("unpublished")
                    ? data.value("unpublished", static_cast<uint64_t>(0))
                    : data.value("leaving", static_cast<uint64_t>(0));
            {
                std::lock_guard<std::mutex> lock(map_mutex_);
                feed_to_handle_.erase(feed);
            }
            if (callbacks_.on_publisher_left) {
                callbacks_.on_publisher_left(feed, "");
            }
        }
        if (data.contains("error") || data.contains("error_code")) {
            if (callbacks_.on_error) {
                callbacks_.on_error(data.value("error", "videoroom error"));
            }
        }
    }

    JanusJsep jsep;
    parse_jsep(msg, &jsep);
    if (!jsep.sdp.empty()) {
        if (handle_id == pub_handle_ && jsep.type == "answer") {
            if (callbacks_.on_publisher_answer) {
                callbacks_.on_publisher_answer(jsep);
            }
        } else if (jsep.type == "offer") {
            uint64_t feed = 0;
            {
                std::lock_guard<std::mutex> lock(map_mutex_);
                for (const auto& kv : feed_to_handle_) {
                    if (kv.second == handle_id) {
                        feed = kv.first;
                        break;
                    }
                }
            }
            if (callbacks_.on_subscriber_offer) {
                callbacks_.on_subscriber_offer(feed, handle_id, jsep);
            }
        }
    }

    if (msg.contains("candidate")) {
        const auto& cand = msg.at("candidate");
        if (cand.value("completed", false)) {
            return;
        }
        if (callbacks_.on_remote_candidate) {
            callbacks_.on_remote_candidate(handle_id, cand.value("sdpMid", ""),
                                           cand.value("sdpMLineIndex", 0),
                                           cand.value("candidate", ""));
        }
    }
}

void JanusClient::handle_trickle(const json& msg) {
    // 顶层 janus=trickle（full-trickle），sender 即 handle_id
    const uint64_t handle_id = msg.value("sender", static_cast<uint64_t>(0));
    if (!msg.contains("candidate")) {
        return;
    }
    const auto& cand = msg.at("candidate");
    if (cand.value("completed", false)) {
        spdlog::info("[janus] remote trickle completed handle={}", handle_id);
        return;
    }
    const std::string mid = cand.value("sdpMid", "");
    const int idx = cand.value("sdpMLineIndex", 0);
    const std::string candidate = cand.value("candidate", "");
    spdlog::info("[janus] remote trickle handle={} mid={} idx={} cand={}",
                 handle_id, mid, idx, candidate.substr(0, 120));
    if (callbacks_.on_remote_candidate && !candidate.empty()) {
        callbacks_.on_remote_candidate(handle_id, mid, idx, candidate);
    }
}

}  // namespace xrtc
