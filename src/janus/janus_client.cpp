#include <janus/janus_client.h>

#include <chrono>

#include <xrtc/xrtc_log.h>

namespace xrtc {

JanusClient::JanusClient() {
    transport_ = std::make_unique<WebsocketTransport>();
    bind_transport_signals();
}

JanusClient::~JanusClient() {
    invalidate();
    Disconnect();
}

///绑定与websocket的信号与槽
void JanusClient::bind_transport_signals() {
    transport_conns_.clear();
    transport_conns_.emplace_back(
        utils::connect(transport_->message, this, &JanusClient::on_ws_message));
    transport_conns_.emplace_back(utils::connect(
        transport_->connected, this, &JanusClient::on_ws_connected));
    transport_conns_.emplace_back(utils::connect(
        transport_->disconnected, this, &JanusClient::on_ws_disconnected));
    transport_conns_.emplace_back(
        utils::connect(transport_->error, this, &JanusClient::on_ws_error));
}

XRtcStatus JanusClient::Connect(const XRTCJoinConfig& config) {
    spdlog::info("[janus] Connect url={} room={} display={}",
                     config.janus_ws_url, config.room_id, config.display_name);
    //设置加入janus的配置
    config_ = config;
    session_id_ = 0; //初始化会话id为0
    pub_handle_ = 0; //初始化发布者句柄为0
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        feed_to_handle_.clear();
        tx_ops_.clear();
        tx_feeds_.clear();
    }
    auto st = transport_->open(config.janus_ws_url);
    //如果连接服务器失败,则返回错误
    if (!st) {
        spdlog::error("[janus] transport open failed: {}",
                          XRtcErrorToString(st.error()));
        error.emit("failed to open websocket");
        return st;
    }
    return xrtc_ok();
}

void JanusClient::Disconnect() {
    //停止保持会话的连接的线程
    stop_keepalive();
    //如果会话id不为0,则发送解除发布的请求和销毁会话的请求
    if (session_id_ != 0) {
        //如果发布者句柄不为0,则发送解除发布的请求
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
    //生成一个随机的transaction字段的值
    const int id = ++tx_seq_;
    return "tx-" + std::to_string(id);
}

void JanusClient::start_keepalive() {
    stop_keepalive();
    keepalive_running_ = true;
    keepalive_thread_ = std::thread([this]() {
        while (keepalive_running_) {
            //间隔25秒
            for (int i = 0; i < 250 && keepalive_running_; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            //如果当前不需要保持会话的连接，或者会话id为0，则跳过
            if (!keepalive_running_ || session_id_ == 0) {
                continue;
            }
            //发送保持会话的连接的请求
            send_json({{"janus", "keepalive"},
                       {"session_id", session_id_},
                       {"transaction", new_transaction()}});
            //没有写入tx_ops_中,因为keepalive请求不需要等待服务端的响应
            //但是服务端还是发送的信息给客户端
        }
    });
}

void JanusClient::stop_keepalive() {
    keepalive_running_ = false;
    if (keepalive_thread_.joinable()) {
        keepalive_thread_.join();
    }
}

utils::slots_t<> JanusClient::on_ws_connected() {
    spdlog::info("Janus websocket connected");
    create_session();
    return {};
}

utils::slots_t<> JanusClient::on_ws_disconnected() {
    stop_keepalive();
    destroyed.emit();
    return {};
}

utils::slots_t<> JanusClient::on_ws_error(const std::string& err) {
    error.emit(err);
    return {};
}

void JanusClient::create_session() {
    const std::string tx = new_transaction();
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        tx_ops_[tx] = PendingOp::kCreate;
    }
    send_json({{"janus", "create"}, {"transaction", tx}});
}

///create 只有 session，还不能进房推流。必须先 attach 到 janus.plugin.videoroom，
///服务端返回一个 handle id，客户端存成 pub_handle_，后面才能：
///join_as_publisher（以发布者进房）
///Publish（发 offer）
///SendTrickle（发 ICE）
///janus返回的success响应中包含的id,将这个id赋值给pub_handle_
void JanusClient::attach_publisher() {
    const std::string tx = new_transaction();
    {
        //注册请求到attach_publisher的请求的transaction字段的值
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
        //如果我已经在订阅这个远端的发布者了,则直接返回
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

utils::slots_t<> JanusClient::on_ws_message(const std::string& text) {
    spdlog::info("[janus] RX {}", text.substr(0, 500));
    json msg;
    try {
        msg = json::parse(text);
    } catch (const std::exception& e) {
        spdlog::warn("invalid Janus JSON: {}", e.what());
        return {};
    }

    //安全读取janus字段的值,如果存在则读取,不存在则返回空字符串
    //janus后面的字段是服务端对客户端的请求的内容的响应
    const std::string janus = msg.value("janus", "");
    spdlog::info("[janus] message type={}", janus);
    if (janus == "success") { //如果服务端发送的消息是成功响应，则调用handle_success处理
        handle_success(msg);
    } else if (janus == "event") {
        handle_event(msg);
    } else if (janus == "trickle") {
        handle_trickle(msg);
    } else if (janus == "hangup") {
        const std::string reason = msg.value("reason", "hangup");
        spdlog::warn("[janus] hangup: {}", reason);
        error.emit(std::string("hangup: ") + reason);
    } else if (janus == "error" || janus == "timeout") {
        std::string reason = janus;
        if (msg.contains("error") && msg["error"].is_object()) {
            reason = msg["error"].value("reason", janus);
        }
        spdlog::error("[janus] gateway {}: {}", janus, reason);
        error.emit(reason);
    }
    return {};
}

void JanusClient::handle_success(const json& msg) {
    //安全读取transaction字段的值,如果存在则读取,不存在则返回空字符串
    //Janus 的所有消息都是异步的。当客户端发送一个请求（例如创建会话 create）时，
    // Janus 网关可能会同时处理来自多个客户端或同一个客户端的多个请求。
    // 客户端在发送请求时生成一个随机的字符串（如这里的 "aAMxRfsTsVVQ"），
    // Janus 在处理完毕后，会在返回的响应中原封不动地带上这个字符串。这样，客户端收到响应时，
    // 就能准确知道这个响应是针对自己刚才发出的哪一个请求的。
    const std::string tx = msg.value("transaction", ""); //获取客户端发送的请求的transaction字段的值
    PendingOp op = PendingOp::kNone;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        auto it = tx_ops_.find(tx); //找到客户端存放的之前发送的请求的transaction字段的值
        if (it != tx_ops_.end()) { //如果找到了之前发送的请求的transaction字段的值
            op = it->second; //获取之前发送的请求的PendingOp的值
            tx_ops_.erase(it); //删除之前发送的请求的transaction字段的值
        }
    }

    const json data = msg.value("data", json::object());

    //如果之前请求的是创建会话，则设置session_id_的值，并调用start_keepalive和attach_publisher
    if (op == PendingOp::kCreate) {
        session_id_ = data.value("id", static_cast<uint64_t>(0));
        spdlog::info("Janus session {}", session_id_);
        start_keepalive();
        attach_publisher();
        return;
    }

    //如果之前请求的是附加发布者插件，则设置pub_handle_的值，并调用join_as_publisher
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
    /*
    {
        "janus": "event",
        "session_id": ...,
        "sender": <pub_handle>,
        "plugindata": {
            "plugin": "janus.plugin.videoroom",
            "data": {
                "videoroom": "joined",
                "room": 1234,
                "id": <你的 publisher id / feed>,
                "publishers": [ ... ]
            }
        }
    }
    */
    const json plugindata = msg.value("plugindata", json::object());
    const json data = plugindata.value("data", json::object());
    const std::string videoroom = data.value("videoroom", "");
    const uint64_t handle_id = msg.value("sender", static_cast<uint64_t>(0));

    ///收集发布者信息,这些发布者是会议中存在的人
    auto collect_publishers = [](const json& data_obj) {
        std::vector<JanusPublisherInfo> pubs;
        if (!data_obj.contains("publishers") ||
            !data_obj["publishers"].is_array()) {
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

    //如果刚加入房间成功了,将发布者信息发送给call_session
    if (videoroom == "joined") {
        //发送call_session的joined_as_publisher信号,生成sdp offer
        joined_as_publisher.emit();
        auto pubs = collect_publishers(data); //收集发布者信息,这些发布者是会议中存在的人
        //如果发布者信息不为空,则发送发布者信息给call_session
        if (!pubs.empty()) {
            //将发布者信息发送给call_session
            publishers.emit(pubs); //这里连接的是call_session的槽函数,逐个订阅房间中存在的发布者,也就是发送attach,需要获得janus的success信息
        }
    } else if (videoroom == "event") { //进房之后的房间动态
        //有人开始推流,退出,房间出错,都会进入这里
        auto pubs = collect_publishers(data);
        if (!pubs.empty()) {
            publishers.emit(pubs);
        }
        //有人停止推流或者退出房间,将发布者信息发送给call_session
        if (data.contains("unpublished") || data.contains("leaving")) {
            const uint64_t feed =
                data.contains("unpublished")
                    ? data.value("unpublished", static_cast<uint64_t>(0))
                    : data.value("leaving", static_cast<uint64_t>(0));
            {
                std::lock_guard<std::mutex> lock(map_mutex_);
                feed_to_handle_.erase(feed);
            }
            //发送call_session的publisher_left信号
            publisher_left.emit(feed, "");
        }
        //如果房间出错,则发送错误信息给call_session
        if (data.contains("error") || data.contains("error_code")) {
            error.emit(data.value("error", "videoroom error"));
        }
    }

    //解析janus服务端返回的jsep响应
    JanusJsep jsep;
    //解析sdp信息,从msg读取sdp信息给out
    parse_jsep(msg, &jsep);
    if (!jsep.sdp.empty()) { //如果sdp信息不为空
        //如果当前是我们的消息并且是janus给我们的回复
        if (handle_id == pub_handle_ && jsep.type == "answer") {
            //收到janus对我们的确认回复,这里是我们发送sdp之后,janus回复我们的消息
            //调用槽函数,我们需要将janus的sdp注册到我们本地中
            publisher_answer.emit(jsep);
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
            subscriber_offer.emit(feed, handle_id, jsep);
        }
    }

    //ice解析
    if (msg.contains("candidate")) {
        const auto& cand = msg.at("candidate");
        if (cand.value("completed", false)) {
            return;
        }
        remote_candidate.emit(handle_id, cand.value("sdpMid", ""),
                              cand.value("sdpMLineIndex", 0),
                              cand.value("candidate", ""));
    }
}

void JanusClient::handle_trickle(const json& msg) {
    const uint64_t handle_id = msg.value("sender", static_cast<uint64_t>(0));
    if (!msg.contains("candidate")) {
        return;
    }
    const auto& cand = msg.at("candidate");
    if (cand.value("completed", false)) {
        spdlog::info("[janus] remote trickle completed handle={}",
                         handle_id);
        return;
    }
    const std::string mid = cand.value("sdpMid", "");
    const int idx = cand.value("sdpMLineIndex", 0);
    const std::string candidate = cand.value("candidate", "");
    spdlog::info("[janus] remote trickle handle={} mid={} idx={} cand={}",
                     handle_id, mid, idx, candidate.substr(0, 120));
    if (!candidate.empty()) {
        remote_candidate.emit(handle_id, mid, idx, candidate);
    }
}

}  // namespace xrtc
