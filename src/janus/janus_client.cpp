#include <janus/janus_client.h>
#include <janus/janus_event_utils.h>

#include <chrono>
#include <utility>

#include <spdlog/spdlog.h>

namespace xrtc {
namespace {

/// 默认单次 Janus 请求超时（毫秒）；可由 XRTCJoinConfig::signal_timeout_ms 覆盖
constexpr uint32_t kDefaultRequestTimeoutMs = 10000;

/// 把 join 配置里的 TLS 选项转成传输层参数
WebsocketTlsOptions MakeTlsOptions(const XRTCJoinConfig& config) {
    WebsocketTlsOptions tls;
    tls.verify = config.tls_verify;
    tls.ca_file = config.ca_file;
    return tls;
}

}  // namespace

// =============================================================================
// 线程收口
//
// JanusClient 被 move_to_thread(signal_thread_)。所有公开方法都必须在亲和线程
// 执行：session_id_ / tx_ops_ / subscribe_state_ 等状态只允许由该线程读写，
// 避免与 on_ws_message 并发。
// =============================================================================
bool JanusClient::on_janus_thread() const {
    return thread() == utils::ensure_thread();
}

void JanusClient::run_on_janus_thread_blocking(
    const std::function<void()>& fn) {
    if (!fn) {
        return;
    }
    if (on_janus_thread()) {
        fn();
        return;
    }
    auto loop = loop_shared();
    if (loop && loop->post_blocking(fn)) {
        return;
    }
    // 走到这里只有两种可能：亲和线程已停（没有并发方），或本线程正在泵
    // 同一 loop（本来就在目标线程上）。两种情况就地执行都是安全的。
    spdlog::warn("[janus] janus thread unavailable, running inline");
    fn();
}

void JanusClient::post_to_janus_thread(std::function<void()> fn) {
    if (!fn) {
        return;
    }
    if (on_janus_thread()) {
        fn();
        return;
    }
    auto shared = std::make_shared<std::function<void()>>(std::move(fn));
    auto loop = loop_shared();
    if (loop && loop->post([shared]() { (*shared)(); })) {
        return;
    }
    spdlog::warn("[janus] janus thread unavailable, posting inline");
    (*shared)();
}

JanusClient::JanusClient() {
    transport_ = std::make_unique<WebsocketTransport>();
    // transport 的信号在本对象的亲和线程上处理（跨线程时由 utils 自动排队）
    bind_transport_signals();
}

JanusClient::~JanusClient() {
    invalidate();
    Disconnect();
}

// =============================================================================
// 请求超时
// =============================================================================
void JanusClient::arm_request_timeout(const std::string& tx, uint64_t feed_id,
                                      const std::string& what) {
    auto* l = loop();
    if (!l || request_timeout_ms_ == 0 || tx.empty()) {
        return;
    }
    const uint32_t timeout_ms =
        request_timeout_ms_ > 0 ? request_timeout_ms_ : kDefaultRequestTimeoutMs;
    const auto id = l->post_delayed(
        std::chrono::milliseconds(timeout_ms),
        [this, tx, feed_id, what]() { on_request_timeout(tx, feed_id, what); });
    if (id != 0) {
        tx_timers_[tx] = id;
    }
}

void JanusClient::clear_request_timeout(const std::string& tx) {
    if (tx.empty()) {
        return;
    }
    auto it = tx_timers_.find(tx);
    if (it == tx_timers_.end()) {
        return;
    }
    if (auto* l = loop()) {
        l->cancel_timer(it->second);
    }
    tx_timers_.erase(it);
}

void JanusClient::complete_tx(const std::string& tx) {
    if (tx.empty()) {
        return;
    }
    clear_request_timeout(tx);
    std::lock_guard<std::mutex> lock(map_mutex_);
    tx_ops_.erase(tx);
    tx_feeds_.erase(tx);
}

void JanusClient::on_request_timeout(std::string tx, uint64_t feed_id,
                                     std::string what) {
    tx_timers_.erase(tx);
    bool pending = false;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        if (tx_ops_.erase(tx) > 0) {
            pending = true;
        }
        auto fit = tx_feeds_.find(tx);
        if (fit != tx_feeds_.end()) {
            feed_id = fit->second;
            tx_feeds_.erase(fit);
        }
    }
    if (create_room_tx_ == tx) {
        create_room_tx_.clear();
        create_room_pending_ = false;
    }
    if (join_tx_ == tx) {
        join_tx_.clear();
    }
    if (publish_tx_ == tx) {
        publish_tx_.clear();
    }
    for (auto it = sub_join_tx_.begin(); it != sub_join_tx_.end();) {
        if (it->second == tx) {
            if (feed_id == 0) {
                feed_id = it->first;
            }
            it = sub_join_tx_.erase(it);
        } else {
            ++it;
        }
    }
    if (!pending) {
        return;
    }

    spdlog::warn("[janus] request timeout tx={} feed={} what={} ({} ms)", tx,
                 feed_id, what, request_timeout_ms_);
    // 订阅类超时只影响该路，交给 CallSession 重试；会话级超时整场失败
    if (feed_id != 0) {
        subscribe_state_.OnAttachFailure(feed_id);
    }
    request_timeout.emit(feed_id, what);
}

void JanusClient::clear_all_request_timeouts() {
    if (auto* l = loop()) {
        for (const auto& kv : tx_timers_) {
            l->cancel_timer(kv.second);
        }
    }
    tx_timers_.clear();
    create_room_tx_.clear();
    join_tx_.clear();
    publish_tx_.clear();
    sub_join_tx_.clear();
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

Rest<> JanusClient::Connect(const XRTCJoinConfig& config) {
    Rest<> result = xrtc_ok();
    run_on_janus_thread_blocking(
        [this, &config, &result]() { result = do_connect(config); });
    return result;
}

Rest<> JanusClient::do_connect(const XRTCJoinConfig& config) {
    spdlog::info("[janus] Connect url={} room={} display={} timeout_ms={}",
                 config.janus_ws_url, config.room_id, config.display_name,
                 config.signal_timeout_ms);
    //设置加入janus的配置
    config_ = config;
    request_timeout_ms_ = config.signal_timeout_ms > 0
                              ? config.signal_timeout_ms
                              : kDefaultRequestTimeoutMs;
    session_id_ = 0;             //初始化会话id为0
    pub_handle_.store(0);        //初始化发布者句柄为0
    create_room_pending_ = false;
    clear_all_request_timeouts();
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        subscribe_state_.Clear();
        tx_ops_.clear();
        tx_feeds_.clear();
    }
    auto st = transport_->open(config.janus_ws_url, MakeTlsOptions(config));
    //如果连接服务器失败,则返回错误
    if (!st) {
        spdlog::error("[janus] transport open failed: {}",
                          XRtcErrorToString(st.error()));
        return st;
    }
    return xrtc_ok();
}

void JanusClient::Disconnect() {
    run_on_janus_thread_blocking([this]() { do_disconnect(); });
}

void JanusClient::do_disconnect() {
    //停止保持会话的连接的线程
    stop_keepalive();
    // 关闭前先撤掉所有请求超时，避免拆会话过程中再触发超时回调
    clear_all_request_timeouts();
    //如果会话id不为0,则发送解除发布的请求和销毁会话的请求
    if (session_id_ != 0) {
        //如果发布者句柄不为0,则发送解除发布的请求
        if (pub_handle_.load() != 0) {
            json body = {{"request", "unpublish"}};
            send_json({{"janus", "message"},
                       {"session_id", session_id_},
                       {"handle_id", pub_handle_.load()},
                       {"transaction", new_transaction()},
                       {"body", body}});
        }
        send_json({{"janus", "destroy"},
                   {"session_id", session_id_},
                   {"transaction", new_transaction()}});
    }
    // 给上面两帧留出写入 socket 的时间，避免 destroy 还没发出就 close
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    transport_->close();
    session_id_ = 0;
    pub_handle_.store(0);
    create_room_pending_ = false;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        subscribe_state_.Clear();
        tx_ops_.clear();
        tx_feeds_.clear();
    }
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
            //如果当前不需要保持会话的连接，则跳过
            if (!keepalive_running_) {
                continue;
            }
            // 心跳统一投递到 janus 线程发送：session_id_/transport 只由该线程访问
            post_to_janus_thread([this]() {
                if (session_id_ == 0) {
                    return;
                }
                send_json({{"janus", "keepalive"},
                           {"session_id", session_id_},
                           {"transaction", new_transaction()}});
                //没有写入tx_ops_中,因为keepalive请求不需要等待服务端的响应
                //但是服务端还是发送的信息给客户端
            });
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
    arm_request_timeout(tx, 0, "create-session");
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
    arm_request_timeout(tx, 0, "attach-publisher");
}

void JanusClient::create_room() {
    create_room_pending_ = true;
    const std::string tx = new_transaction();
    create_room_tx_ = tx;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        tx_ops_[tx] = PendingOp::kCreateRoom;
    }
    json body = {{"request", "create"},
                 {"room", config_.room_id},
                 {"publishers", config_.max_publishers},
                 {"audiocodec", "opus"},
                 {"videocodec", "vp8"}};
    if (!config_.room_description.empty()) {
        body["description"] = config_.room_description;
    }
    if (!config_.pin.empty()) {
        body["pin"] = config_.pin;
    }
    if (!config_.admin_key.empty()) {
        body["admin_key"] = config_.admin_key;
    }
    spdlog::info("[janus] create room {} (if missing)", config_.room_id);
    send_json({{"janus", "message"},
               {"session_id", session_id_},
               {"handle_id", pub_handle_.load()},
               {"transaction", tx},
               {"body", body}});
    arm_request_timeout(tx, 0, "create-room");
}

void JanusClient::handle_create_room_result(const json& data) {
    if (!create_room_pending_) {
        return;
    }
    create_room_pending_ = false;
    // 建房请求已有结论（created / 427 / error），撤掉超时并清记账
    complete_tx(create_room_tx_);
    create_room_tx_.clear();

    const std::string videoroom = data.value("videoroom", "");
    const int error_code = data.value("error_code", 0);

    if (videoroom == "created") {
        spdlog::info("[janus] room {} created, joining as publisher",
                     config_.room_id);
        join_as_publisher();
        return;
    }
    // 427 Room already exists → 仍可 join
    if (error_code == 427) {
        spdlog::info("[janus] room {} already exists, joining as publisher",
                     config_.room_id);
        join_as_publisher();
        return;
    }
    if (data.contains("error") || data.contains("error_code")) {
        error.emit(data.value("error", "videoroom create failed"));
        return;
    }
    // 未知成功形态：仍尝试 join，避免卡在 joining
    spdlog::warn("[janus] unexpected create room response: {}", data.dump());
    join_as_publisher();
}

void JanusClient::join_as_publisher() {
    const std::string tx = new_transaction();
    join_tx_ = tx;
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
               {"handle_id", pub_handle_.load()},
               {"transaction", tx},
               {"body", body}});
    arm_request_timeout(tx, 0, "join-publisher");
}

void JanusClient::Publish(const JanusJsep& offer) {
    post_to_janus_thread([this, offer]() { do_publish(offer); });
}

void JanusClient::do_publish(const JanusJsep& offer) {
    if (session_id_ == 0 || pub_handle_.load() == 0) {
        spdlog::warn("[janus] Publish ignored: no session/handle");
        return;
    }
    // 记录本次 publish 的 transaction，answer 到达时取消超时
    publish_tx_ = new_transaction();
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        tx_ops_[publish_tx_] = PendingOp::kPublish;
    }
    arm_request_timeout(publish_tx_, 0, "publish");
    json body = {{"request", "configure"}, {"audio", true}, {"video", true}};
    json jsep = {{"type", offer.type}, {"sdp", offer.sdp}};
    send_json({{"janus", "message"},
               {"session_id", session_id_},
               {"handle_id", pub_handle_.load()},
               {"transaction", publish_tx_},
               {"body", body},
               {"jsep", jsep}});
}

void JanusClient::SendTrickle(uint64_t handle_id, const std::string& sdp_mid,
                              int mline_index, const std::string& candidate) {
    post_to_janus_thread([this, handle_id, sdp_mid, mline_index, candidate]() {
        do_send_trickle(handle_id, sdp_mid, mline_index, candidate);
    });
}

void JanusClient::do_send_trickle(uint64_t handle_id,
                                  const std::string& sdp_mid, int mline_index,
                                  const std::string& candidate) {
    if (session_id_ == 0) {
        return;
    }
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
    post_to_janus_thread(
        [this, handle_id]() { do_send_trickle_complete(handle_id); });
}

void JanusClient::do_send_trickle_complete(uint64_t handle_id) {
    if (session_id_ == 0) {
        return;
    }
    send_json({{"janus", "trickle"},
               {"session_id", session_id_},
               {"handle_id", handle_id},
               {"transaction", new_transaction()},
               {"candidate", {{"completed", true}}}});
}

void JanusClient::Subscribe(uint64_t feed_id) {
    run_on_janus_thread_blocking([this, feed_id]() { do_subscribe(feed_id); });
}

void JanusClient::do_subscribe(uint64_t feed_id) {
    if (!subscribe_state_.TryBeginSubscribe(feed_id)) {
        return;
    }
    attach_subscriber(feed_id);
}

void JanusClient::DetachSubscriber(uint64_t feed_id) {
    run_on_janus_thread_blocking(
        [this, feed_id]() { do_detach_subscriber(feed_id); });
}

void JanusClient::do_detach_subscriber(uint64_t feed_id) {
    const auto handle = subscribe_state_.OnFeedLeft(feed_id);
    if (auto it = sub_join_tx_.find(feed_id); it != sub_join_tx_.end()) {
        complete_tx(it->second);
        sub_join_tx_.erase(it);
    }
    if (!handle || session_id_ == 0 || *handle == 0) {
        return;
    }
    send_json({{"janus", "detach"},
               {"session_id", session_id_},
               {"handle_id", *handle},
               {"transaction", new_transaction()}});
}

void JanusClient::attach_subscriber(uint64_t feed_id) {
    const std::string tx = new_transaction();
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        tx_ops_[tx] = PendingOp::kAttachSub;
        tx_feeds_[tx] = feed_id;
    }
    //作为订阅者发送attach,在已有的session上再挂一个VideoRoom插件
    send_json({{"janus", "attach"},
               {"plugin", "janus.plugin.videoroom"},
               {"session_id", session_id_},
               {"transaction", tx}});
    arm_request_timeout(tx, feed_id, "attach-subscriber");
}

void JanusClient::StartSubscriber(uint64_t handle_id, uint64_t feed_id,
                                  const JanusJsep& answer) {
    post_to_janus_thread([this, handle_id, feed_id, answer]() {
        do_start_subscriber(handle_id, feed_id, answer);
    });
}

void JanusClient::do_start_subscriber(uint64_t handle_id, uint64_t feed_id,
                                      const JanusJsep& answer) {
    (void)feed_id;  // 保留参数：Janus 的 start 没有独立确认，不做超时记账
    if (session_id_ == 0) {
        return;
    }
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
        //创建 Session
        //挂载 VideoRoom 插件
        //动态创建 VideoRoom
        handle_success(msg);
    } else if (janus == "event") {
        //以发布者身份进房
        //有人开始推流,退出,房间出错,都会进入这里
        handle_event(msg);
    } else if (janus == "trickle") { //接收janus的ice候选
        //ICE 候选交换
        handle_trickle(msg);
    } else if (janus == "hangup") {
        const uint64_t handle_id =
            msg.value("sender", static_cast<uint64_t>(0));
        const std::string reason = msg.value("reason", "hangup");
        spdlog::warn("[janus] hangup handle={} reason={}", handle_id, reason);
        // 该 handle 对应的订阅 join 超时不再有意义（hangup 本身就是终态）
        if (handle_id != pub_handle_.load() && handle_id != 0) {
            const uint64_t feed =
                subscribe_state_.FeedForHandle(handle_id).value_or(0);
            if (auto it = sub_join_tx_.find(feed); it != sub_join_tx_.end()) {
                complete_tx(it->second);
                sub_join_tx_.erase(it);
            }
        }
        hangup.emit(handle_id, reason);
    } else if (janus == "error" || janus == "timeout") {
        const std::string tx = msg.value("transaction", "");
        // gateway 明确报错/超时：撤掉本地定时器，避免二次上报
        clear_request_timeout(tx);
        {
            std::lock_guard<std::mutex> lock(map_mutex_);
            auto fit = tx_feeds_.find(tx);
            if (fit != tx_feeds_.end()) {
                subscribe_state_.OnAttachFailure(fit->second);
                tx_feeds_.erase(fit);
            }
            auto oit = tx_ops_.find(tx);
            if (oit != tx_ops_.end()) {
                tx_ops_.erase(oit);
            }
        }
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
    // 收到响应即取消该请求的超时
    clear_request_timeout(tx);

    const json data = msg.value("data", json::object());

    //如果之前请求的是创建会话，则设置session_id_的值，并调用start_keepalive和attach_publisher
    if (op == PendingOp::kCreate) {
        session_id_ = data.value("id", static_cast<uint64_t>(0));
        spdlog::info("Janus session {}", session_id_);
        start_keepalive();
        attach_publisher();
        return;
    }

    //如果之前请求的是附加发布者插件，则设置pub_handle_的值
    // create_room_if_missing 时先建房，否则直接以 publisher 进房
    if (op == PendingOp::kAttachPub) {
        pub_handle_.store(data.value("id", static_cast<uint64_t>(0)));
        spdlog::info("Janus publisher handle {}", pub_handle_.load());
        if (config_.create_room_if_missing) {
            create_room();
        } else {
            join_as_publisher();
        }
        return;
    }

    //你加入房间之后,作为订阅者发送attach,在已有的session上再挂一个VideoRoom插件
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
        }
        subscribe_state_.OnAttachSuccess(feed, handle);

        //以订阅者的身份进入房间；该请求也要有超时，否则 Janus 不回应会永久卡住
        const std::string join_tx = new_transaction();
        {
            std::lock_guard<std::mutex> lock(map_mutex_);
            tx_ops_[join_tx] = PendingOp::kSubJoin;
            tx_feeds_[join_tx] = feed;
        }
        sub_join_tx_[feed] = join_tx;
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
                   {"transaction", join_tx},
                   {"body", body}});
        arm_request_timeout(join_tx, feed, "join-subscriber");
        return;
    }

    // VideoRoom create 是同步请求：响应为 janus:success + plugindata
    if (op == PendingOp::kCreateRoom) {
        const json plugindata = msg.value("plugindata", json::object());
        const json pdata = plugindata.value("data", json::object());
        handle_create_room_result(pdata);
    }
}

void JanusClient::handle_event(const json& msg) {
    /*
    /// 作为发布者进入房间之后,janus发送给我们的消息
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
    //你进入这个房间号
    const std::string videoroom = data.value("videoroom", "");
    //你进入这个房间之后,在这个房间的feed编号,可以出现在对方收到的 publishers 列表里
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
        // 进房已完成：取消 join-publisher 的超时并清记账
        complete_tx(join_tx_);
        join_tx_.clear();
        //发送call_session的joined_as_publisher信号,生成sdp offer
        joined_as_publisher.emit();
        auto pubs = collect_publishers(data); //收集发布者信息,这些发布者是会议中存在的人
        //如果发布者信息不为空,则发送发布者信息给call_session
        if (!pubs.empty()) {
            //将发布者信息发送给call_session
            publishers.emit(pubs); //这里连接的是call_session的槽函数,逐个订阅房间中存在的发布者,也就是发送attach,需要获得janus的success信息
        }
    } else if (videoroom == "created") {
        // 少数部署可能仍以 event 返回 create 结果
        if (create_room_pending_ &&
            handle_id == pub_handle_.load()) {
            handle_create_room_result(data);
        }
    } else if (videoroom == "event") { //进房之后的房间动态
        //有人开始推流,退出,房间出错,都会进入这里
        auto pubs = collect_publishers(data);
        if (!pubs.empty()) {
            publishers.emit(pubs);
        }
        // 有人停止推流或退出；本端 unpublish 成功为 "unpublished":"ok"，应忽略
        if (data.contains("unpublished") || data.contains("leaving")) {
            const auto feed = ParsePublisherLeftFeed(data);
            if (feed) {
                publisher_left.emit(*feed, "");
            } else {
                spdlog::info(
                    "[janus] ignore unpublished/leaving non-feed event");
            }
        }
        // 建房流程中：427 等错误也可能走 event（兼容）
        if (create_room_pending_ && handle_id == pub_handle_.load() &&
            (data.contains("error") || data.contains("error_code"))) {
            handle_create_room_result(data);
        } else if (data.contains("error") || data.contains("error_code")) {
            //如果房间出错,则发送错误信息给call_session
            error.emit(data.value("error", "videoroom error"));
        }
    }

    // videoroom == attached
    //解析janus服务端返回的jsep响应
    JanusJsep jsep;
    //解析sdp信息,从msg读取sdp信息给out
    parse_jsep(msg, &jsep);
    if (!jsep.sdp.empty()) { //如果sdp信息不为空
        //如果当前是我们作为发布者发送的消息并且是janus回复的 "answer"
        if (handle_id == pub_handle_.load() && jsep.type == "answer") {
            // answer 到达：取消 publish 的超时并清记账
            complete_tx(publish_tx_);
            publish_tx_.clear();
            //收到janus对我们的确认回复,这里是我们发送sdp之后,janus回复我们的消息
            //调用槽函数,我们需要将janus的sdp注册到我们本地中
            publisher_answer.emit(jsep);
        } else if (jsep.type == "offer") { //作为订阅者收到远端对象的sdp offer
            /*
            {
                "janus": "event",
                "session_id": 5941003581048891,
                "sender": 2881316821259561,
                "plugindata": {
                    "plugin": "janus.plugin.videoroom",
                    "data": {
                    "videoroom": "attached",
                    "room": 1234
                    }
                },
                "jsep": {
                    "type": "offer",
                    "sdp": "v=0\r\no=- ...\r\ns=...\r\n..."
                }
            } 
            */
            //获得我们要订阅的远端对象的feed_id
            uint64_t feed = subscribe_state_.FeedForHandle(handle_id).value_or(0);
            // 收到远端 offer：订阅 join 完成，取消该 feed 的超时
            if (auto it = sub_join_tx_.find(feed); it != sub_join_tx_.end()) {
                complete_tx(it->second);
                sub_join_tx_.erase(it);
            }
            //发送订阅者offer信号,将我们要订阅的远端对象的feed_id和handle_id发送给call_session
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
