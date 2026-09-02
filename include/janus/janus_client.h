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

#include "concurrency/signal_and_slots/signal_and_slots.h"
#include <janus/websocket_transport.h>
#include <janus/subscribe_state.h>
#include <xrtc/xrtc_defines.h>
#include <internal/xrtc_result.h>


/*
 * JanusClient 与 Janus Gateway 的信令协议说明
 *
 * 传输层：WebSocket（WebsocketTransport 负责握手与收发文本帧）
 * 应用层：每条消息为 JSON 文本，通过 "janus" 字段区分类型
 * 关联请求：客户端发送时生成 "transaction"，服务端原样回传，用于匹配异步响应
 *
 * ── 发布者完整流程（Connect 后自动串联）────────────────────────────────
 *
 *   WebSocket 握手
 *     → create（创建 Session）
 *     → attach（挂载 janus.plugin.videoroom，获得 pub_handle_）
 *     → [可选] message/create（create_room_if_missing：房间不存在则建房）
 *     → message/join（以 publisher 身份进房）
 *     → [本地创建 Offer]
 *     → message/configure + jsep（Publish 推流）
 *     → trickle 双向交换 ICE
 *     → event 收到 answer，开始通话
 *
 * ── 订阅者流程（收到 publishers 信号后按需触发）────────────────────────
 *
 *   attach（为每个 feed 新建 subscriber handle）
 *     → message/join（ptype=subscriber, feed=<远端 id>）
 *     → event 收到 offer
 *     → [本地创建 Answer]
 *     → message/start + jsep
 *     → trickle 双向交换 ICE
 *
 * ── 断开流程（Disconnect）──────────────────────────────────────────────
 *
 *   message/unpublish → destroy → 关闭 WebSocket
 *
 * ═══════════════════════════════════════════════════════════════════════
 * 1. WebSocket 握手
 * ═══════════════════════════════════════════════════════════════════════
 *
 * 客户端请求头：
 *   GET /janus HTTP/1.1
 *   Upgrade: websocket
 *   Host: localhost:8188
 *   Origin: http://localhost
 *   Sec-WebSocket-Key: <随机 Base64>
 *   Sec-WebSocket-Version: 13
 *
 * 服务端响应头：
 *   HTTP/1.1 101 Switching Protocols
 *   Upgrade: websocket
 *   Connection: Upgrade
 *   Sec-WebSocket-Accept: <根据 Key 计算的 Base64>
 *
 * 握手成功后触发 on_ws_connected() → create_session()
 *
 * ═══════════════════════════════════════════════════════════════════════
 * 2. 创建 Session（create_session）
 * ═══════════════════════════════════════════════════════════════════════
 *
 * 客户端 → 服务端：
 *   { "janus": "create", "transaction": "tx-1" }
 *
 * 服务端 → 客户端（janus: success）：
 *   {
 *     "janus": "success",
 *     "transaction": "tx-1",
 *     "data": { "id": 5109637625847547 }   // → session_id_
 *   }
 *
 * 成功后：start_keepalive() + attach_publisher()
 *
 * ═══════════════════════════════════════════════════════════════════════
 * 3. 挂载 VideoRoom 插件（attach_publisher / attach_subscriber）
 * ═══════════════════════════════════════════════════════════════════════
 *
 * 客户端 → 服务端：
 *   {
 *     "janus": "attach",
 *     "plugin": "janus.plugin.videoroom",
 *     "session_id": <session_id_>,
 *     "transaction": "tx-2"
 *   }
 *
 * 服务端 → 客户端（janus: success）：
 *   {
 *     "janus": "success",
 *     "transaction": "tx-2",
 *     "data": { "id": 12345678 }            // → pub_handle_ 或 subscriber handle
 *   }
 *
 * 发布者 attach 成功后：
 *   create_room_if_missing → create_room() → success(created|427) → join_as_publisher()
 *   否则直接 → join_as_publisher()
 * 订阅者 attach 成功后 → message/join（ptype=subscriber）
 *
 * ═══════════════════════════════════════════════════════════════════════
 * 3b. 动态创建 VideoRoom（create_room，可选）
 * ═══════════════════════════════════════════════════════════════════════
 *
 * 仅当 XRTCJoinConfig::create_room_if_missing 为 true 时，在 join 前发送：
 *
 * 客户端 → 服务端：
 *   {
 *     "janus": "message",
 *     "session_id": <session_id_>,
 *     "handle_id": <pub_handle_>,
 *     "transaction": "tx-create-room",
 *     "body": {
 *       "request": "create",
 *       "room": <room_id>,
 *       "description": "<可选>",
 *       "publishers": <max_publishers>,
 *       "audiocodec": "opus",
 *       "videocodec": "vp8",
 *       "pin": "<可选>",
 *       "admin_key": "<可选，服务端要求时>"
 *     }
 *   }
 *
 * 服务端 → 客户端（create 为同步请求，走 janus: success + plugindata）：
 *   成功：
 *     {
 *       "janus": "success", "transaction": "...", "sender": <pub_handle>,
 *       "plugindata": { "data": { "videoroom": "created", "room": <id> } }
 *     }
 *   房间已存在：
 *     {
 *       "janus": "success", ...,
 *       "plugindata": { "data": { "videoroom": "event", "error_code": 427, ... } }
 *     }
 *     → 视为可继续 join
 *
 * 前提：Janus VideoRoom 需允许客户端 create；若配置了 admin_key 须在 join config 填写。
 *
 * ═══════════════════════════════════════════════════════════════════════
 * 4. 以发布者身份进房（join_as_publisher）
 * ═══════════════════════════════════════════════════════════════════════
 *
 * 客户端 → 服务端：
 *   {
 *     "janus": "message",
 *     "session_id": <session_id_>,
 *     "handle_id": <pub_handle_>,
 *     "transaction": "tx-3",
 *     "body": {
 *       "request": "join",
 *       "ptype": "publisher",
 *       "room": <room_id>,
 *       "display": "<display_name>",
 *       "pin": "<可选，房间密码>"
 *     }
 *   }
 *
 * 服务端 → 客户端（先 ack，再 event；ack 当前未处理）：
 *   { "janus": "ack", "session_id": ..., "transaction": "tx-3" }
 *
 *   {
 *     "janus": "event",
 *     "session_id": ...,
 *     "sender": <pub_handle_>,
 *     "plugindata": {
 *       "plugin": "janus.plugin.videoroom",
 *       "data": {
 *         "videoroom": "joined",
 *         "room": 1234,
 *         "id": 987654321,                 // 本端 feed id，供他人订阅
 *         "private_id": 12345,             //私有令牌,只有自己知道
 *         "publishers": [                  // 房间内已在推流的其他人（可为空）
 *           { "id": 111, "display": "user-a", "audio_codec": "opus", "video_codec": "vp8" }
 *         ]
 *       }
 *     }
 *   }
 *
 * videoroom=="joined" → joined_as_publisher 信号；有 publishers 则 publishers 信号
 *
 * ═══════════════════════════════════════════════════════════════════════
 * 5. 发布媒体（Publish）
 * ═══════════════════════════════════════════════════════════════════════
 *
 * 客户端 → 服务端：
 *   {
 *     "janus": "message",
 *     "session_id": <session_id_>,
 *     "handle_id": <pub_handle_>,
 *     "transaction": "tx-4",
 *     "body": { "request": "configure", "audio": true, "video": true },
 *     "jsep": { "type": "offer", "sdp": "<本地 SDP>" }
 *   }
 *
 * 服务端 → 客户端（janus: event，含 answer）：
 *   {
 *     "janus": "event",
 *     "session_id": ...,
 *     "sender": <pub_handle_>,
 *     "plugindata": {
 *       "plugin": "janus.plugin.videoroom",
 *       "data": { "videoroom": "event", "configured": "ok" }
 *     },
 *     "jsep": { "type": "answer", "sdp": "<Janus SDP>" }
 *   }
 *
 * jsep.type=="answer" 且 sender==pub_handle_ → publisher_answer 信号
 *
 * ═══════════════════════════════════════════════════════════════════════
 * 6. ICE 候选交换（SendTrickle / SendTrickleComplete）
 * ═══════════════════════════════════════════════════════════════════════
 *
 * 客户端 → 服务端（单个候选）：
 *   {
 *     "janus": "trickle",
 *     "session_id": <session_id_>,
 *     "handle_id": <handle_id>,
 *     "transaction": "tx-5",
 *     "candidate": {
 *       "candidate": "candidate:...",
 *       "sdpMid": "0",
 *       "sdpMLineIndex": 0
 *     }
 *   }
 *
 * 客户端 → 服务端（候选收集完毕）：
 *   {
 *     "janus": "trickle",
 *     "session_id": <session_id_>,
 *     "handle_id": <handle_id>,
 *     "transaction": "tx-6",
 *     "candidate": { "completed": true }
 *   }
 *
 * 服务端 → 客户端（janus: trickle 或 event 内嵌 candidate）：
 *   {
 *     "janus": "trickle",
 *     "session_id": ...,
 *     "sender": <handle_id>,
 *     "candidate": {
 *       "candidate": "candidate:...",
 *       "sdpMid": "0",
 *       "sdpMLineIndex": 0
 *     }
 *   }
 *
 * → remote_candidate 信号
 *
 * ═══════════════════════════════════════════════════════════════════════
 * 7. 订阅远端发布者（Subscribe → attach_subscriber → join → StartSubscriber）
 * ═══════════════════════════════════════════════════════════════════════
 *
 * 7a. attach（同第 3 节，每个 feed 一个 handle）
 *
 * 7b. 以订阅者身份 join：
 *   客户端 → 服务端：
 *     {
 *       "janus": "message",
 *       "session_id": <session_id_>,
 *       "handle_id": <subscriber_handle>,
 *       "transaction": "tx-7",
 *       "body": {
 *         "request": "join",
 *         "ptype": "subscriber",
 *         "room": <room_id>,
 *         "feed": <远端 feed_id>,
 *         "pin": "<可选>"
 *       }
 *     }
 *
 *   服务端 → 客户端（janus: event，含 offer）：
 *     {
 *       "janus": "event",
 *       "sender": <subscriber_handle>,
 *       "plugindata": {
 *         "plugin": "janus.plugin.videoroom",
 *         "data": { "videoroom": "attached" }
 *       },
 *       "jsep": { "type": "offer", "sdp": "<远端 SDP>" }
 *     }
 *
 *   jsep.type=="offer" → subscriber_offer 信号
 *
 * 7c. 发送 Answer 开始拉流（StartSubscriber）：
 *   客户端 → 服务端：
 *     {
 *       "janus": "message",
 *       "session_id": <session_id_>,
 *       "handle_id": <subscriber_handle>,
 *       "transaction": "tx-8",
 *       "body": { "request": "start", "room": <room_id> },
 *       "jsep": { "type": "answer", "sdp": "<本地 SDP>" }
 *     }
 *
 * ═══════════════════════════════════════════════════════════════════════
 * 8. 房间动态事件（handle_event, videoroom=="event"）
 * ═══════════════════════════════════════════════════════════════════════
 *
 * 有人新进房并开始推流（publishers 列表更新）：
 *   { "videoroom": "event", "publishers": [ { "id": ..., "display": "..." } ] }
 *   → publishers 信号
 *
 * 有人停止推流：
 *   { "videoroom": "event", "unpublished": <feed_id> }
 *   → publisher_left 信号
 *
 * 有人离开房间：
 *   { "videoroom": "event", "leaving": <feed_id> }
 *   → publisher_left 信号
 *
 * 插件错误：
 *   { "videoroom": "event", "error_code": 426, "error": "No such room" }
 *   → error 信号
 *
 * ═══════════════════════════════════════════════════════════════════════
 * 9. 心跳保活（start_keepalive，间隔 25s）
 * ═══════════════════════════════════════════════════════════════════════
 *
 * 客户端 → 服务端：
 *   {
 *     "janus": "keepalive",
 *     "session_id": <session_id_>,
 *     "transaction": "tx-N"
 *   }
 *
 * 服务端 → 客户端（janus: ack，当前未处理，可忽略）：
 *   { "janus": "ack", "session_id": ..., "transaction": "tx-N" }
 *
 * keepalive 的 transaction 不写入 tx_ops_，无需等待响应。
 *
 * ═══════════════════════════════════════════════════════════════════════
 * 10. 断开连接（Disconnect）
 * ═══════════════════════════════════════════════════════════════════════
 *
 * 客户端 → 服务端（先 unpublish，再 destroy）：
 *   {
 *     "janus": "message",
 *     "session_id": <session_id_>,
 *     "handle_id": <pub_handle_>,
 *     "transaction": "tx-9",
 *     "body": { "request": "unpublish" }
 *   }
 *   { "janus": "destroy", "session_id": <session_id_>, "transaction": "tx-10" }
 *
 * 随后关闭 WebSocket → on_ws_disconnected() → destroyed 信号
 *
 * ═══════════════════════════════════════════════════════════════════════
 * 11. 网关级错误响应
 * ═══════════════════════════════════════════════════════════════════════
 *
 *   { "janus": "error", "transaction": "tx-X",
 *     "error": { "code": 458, "reason": "..." } }
 *   { "janus": "timeout", "transaction": "tx-X", ... }
 *   { "janus": "hangup", "session_id": ..., "reason": "..." }
 *
 * → error 信号
 *
 * ═══════════════════════════════════════════════════════════════════════
 * 关键 ID 说明
 * ═══════════════════════════════════════════════════════════════════════
 *
 *   session_id_  : create 返回，标识与 Janus 的会话
 *   pub_handle_  : attach videoroom 返回，发布者插件句柄
 *   feed_id      : join 后 Janus 分配的房间内发布者 id（订阅时用）
 *   handle_id    : 每个 attach 返回的插件句柄（发布/订阅各一个）
 */




namespace xrtc {

///SDP(Session Description Protocol)是WebRTC中用于描述会话的协议
struct JanusJsep {
    std::string type; //sdp类型,offer或answer
    std::string sdp; //完整的sdp文本
};

///表示房间里某个远端发布者的简要信息
struct JanusPublisherInfo {
    uint64_t feed_id = 0; ///Janus VideoRoom 里该发布者的 id（订阅时 join 的 feed）
    std::string display; ///对方进房时填的显示名
};

///@brief JanusClient是客户端与Janus服务器建立WebSocket连接的类
class JanusClient : public utils::object {
public:
    JanusClient();
    ~JanusClient() override;

    JanusClient(const JanusClient&) = delete;
    JanusClient& operator=(const JanusClient&) = delete;

    utils::signal<> joined_as_publisher{this};
    utils::signal<std::vector<JanusPublisherInfo>> publishers{this};
    utils::signal<uint64_t, std::string> publisher_left{this};
    utils::signal<JanusJsep> publisher_answer{this};
    utils::signal<uint64_t, uint64_t, JanusJsep> subscriber_offer{this};
    utils::signal<uint64_t, std::string, int, std::string> remote_candidate{this};
    utils::signal<std::string> error{this};
    utils::signal<> destroyed{this};

    //连接janus服务器,websocket完成,可以发送和接收数据
    XRtcStatus Connect(const XRTCJoinConfig& config);
    void Disconnect();

	///发送offer给janus服务器,将客户端打包的sdp发送给janus服务器
    void Publish(const JanusJsep& offer);
	///发送ICE数据包给janus服务器,将客户端打包的ICE数据包发送给janus服务器
	///@param handle_id 发布者句柄
	///@param sdp_mid 媒体描述的 mid
	///@param mline_index 媒体描述的 mline 索引
	///@param candidate ICE 候选者
    void SendTrickle(uint64_t handle_id, const std::string& sdp_mid,
                     int mline_index, const std::string& candidate);
	///@brief 发送ICE数据包完成信号,表示这个ice已经发送完成了,发送完成信号给janus服务器
	///@param handle_id 发布者句柄
    void SendTrickleComplete(uint64_t handle_id);

	///@brief 开始订阅远端发布者,订阅远端发布者,发送attach请求,附加订阅者插件,拿到订阅者用的 plugin handle
    void Subscribe(uint64_t feed_id);
	/// 远端离开时 detach 对应 subscriber handle，释放 Janus 侧资源
    void DetachSubscriber(uint64_t feed_id);
	///开始订阅远端发布者,订阅远端发布者,发送attach请求,附加订阅者插件,拿到订阅者用的 plugin handle
	///把本地生成的 SDP answer 发给 Janus，正式开始拉该路流
    void StartSubscriber(uint64_t handle_id, const JanusJsep& answer);

	///获取发布者句柄
    uint64_t publisher_handle() const { return pub_handle_; }

private:
    using json = nlohmann::json;

    //待处理的操作
    enum class PendingOp {
        kNone,
        kCreate,
        kAttachPub,
        kCreateRoom,
        kJoinPub,
        kAttachSub,
    };

    //绑定websocket的信号与槽
    void bind_transport_signals();
    //将要发送给服务端的json数据发送给janus服务端
    void send_json(const json& obj);
    //生成一个唯一的transaction字段的值
    std::string new_transaction();
	///websocket握手成功之后调用,创建会话,开启之后的所有操作
    ///后面的操作基于on_ws_message槽函数中
    utils::slots_t<> on_ws_connected();
    //获得从websocket中获得网络数据
    utils::slots_t<> on_ws_message(const std::string& text);
	///disconnect信号发出之后调用
    utils::slots_t<> on_ws_disconnected();
	///error信号发出之后调用
    utils::slots_t<> on_ws_error(const std::string& err);
	///处理janus服务端返回的success响应,处理创建会话、附加发布者插件、加入房间等成功响应
    /// 响应数据类型: 创建janus会话,请求插件,进入房间请求,获得远端请求
    void handle_success(const json& msg);
	///处理janus服务端返回的event响应
    /// 响应数据类型: 加入房间,房间情况变换
    void handle_event(const json& msg);
	///处理janus服务端返回的trickle响应,处理发送ICE数据包
    void handle_trickle(const json& msg);
	///给janus服务器发送create请求
    void create_session();
	///给janus服务器发送attach请求,附加发布者插件,拿到发布者用的 plugin handle
    void attach_publisher();
	/// 可选：VideoRoom create，房间不存在时建房；完成后调用 join_as_publisher
    void create_room();
	/// 处理 create 房间的插件结果（success/plugindata 或 event）
    void handle_create_room_result(const json& data);
	///以发布者身份进房,发送join请求,janus服务端返回event响应
    void join_as_publisher();
	///附加订阅者插件,拿到订阅者用的 plugin handle,
	// 这里发送attach请求,附加订阅者插件,拿到订阅者用的 plugin handle
    void attach_subscriber(uint64_t feed_id);
	//解析sdp信息,从msg读取sdp信息给out
    void parse_jsep(const json& msg, JanusJsep* out);
    ///保持会话的连接,每隔25秒发送一个数据包给服务端,等待时间使用单独线程
    void start_keepalive();
    ///停止保持会话的连接,停止发送心跳数据包的线程
    void stop_keepalive();

    //指向websocket网络处理的指针,在构造函数中创建
    std::unique_ptr<WebsocketTransport> transport_;
    //每个信号与槽的连接
    std::vector<utils::scoped_connection> transport_conns_;
    //加入janus的配置
    XRTCJoinConfig config_;

	///会话id,发送create之后,janus发送响应中包含的id,将这个id赋值给这个id
    uint64_t session_id_ = 0;
    ///发布者句柄,发送attach 使用videoroom插件之后,janus发送响应中包含的id,将这个id赋值给这个句柄
	///这是本地的发布者句柄,表示一个连接
    uint64_t pub_handle_ = 0;
    /// 正在等待 create 房间的 event（created 或 427）
    bool create_room_pending_ = false;
    ///生成随机的transaction字段的值的序号,
    ///用来保持会话的连接信息的transaction字段的值
    std::atomic<int> tx_seq_{0};

    ///待处理的操作,存储发送给janus服务器的transaction字段的值和对应的操作
    std::unordered_map<std::string, PendingOp> tx_ops_;
	///待处理的操作,存储发送给janus服务器的transaction字段的值和对应的feed_id
    std::unordered_map<std::string, uint64_t> tx_feeds_;
    /// 订阅去重 / feed→handle（含 pending attach）
    SubscribeState subscribe_state_;
    std::mutex map_mutex_;

    //保持心跳的线程
    std::thread keepalive_thread_;
    ///保持会话的连接的线程是否在运行
    std::atomic<bool> keepalive_running_{false};
};

}  // namespace xrtc
