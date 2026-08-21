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
#include <xrtc/xrtc_defines.h>
#include <xrtc/xrtc_result.h>


/*
客户端与Janus服务器建立WebSocket连接的请求头

GET /janus HTTP/1.1
Upgrade: websocket
Host: localhost:8188
Origin: http://localhost
Sec-WebSocket-Key: <随机生成的Base64字符串>
Sec-WebSocket-Version: 13

janus服务器的响应头

HTTP/1.1 101 Switching Protocols
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Accept: <根据客户端Key计算出的Base64字符串>

客户端发送的创建会话的请求

{
  "janus": "create",
  "transaction": "aAMxRfsTsVVQ"
}

janus服务器返回的创建会话的响应

{
  "janus": "success",
  "transaction": "aAMxRfsTsVVQ",
  "data": {
    "id": 5109637625847547
  }
}


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
        kJoinPub,
        kAttachSub,
    };

    //绑定websocket的信号与槽
    void bind_transport_signals();
    //将要发送给服务端的json数据发送给janus服务端
    void send_json(const json& obj);
    //生成一个唯一的transaction字段的值
    std::string new_transaction();
	///websocket握手成功之后调用,创建会话
    utils::slots_t<> on_ws_connected();
    //获得从websocket中获得网络数据
    utils::slots_t<> on_ws_message(const std::string& text);
	///disconnect信号发出之后调用
    utils::slots_t<> on_ws_disconnected();
	///error信号发出之后调用
    utils::slots_t<> on_ws_error(const std::string& err);
	///处理janus服务端返回的success响应,处理创建会话、附加发布者插件、加入房间等成功响应
    void handle_success(const json& msg);
	///处理janus服务端返回的event响应,加入房间成功服务端返回event响应,客户端处理event响应
    void handle_event(const json& msg);
	///处理janus服务端返回的trickle响应,处理发送ICE数据包
    void handle_trickle(const json& msg);
	///给janus服务器发送create请求
    void create_session();
	///给janus服务器发送attach请求,附加发布者插件,拿到发布者用的 plugin handle
    void attach_publisher();
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
    ///生成随机的transaction字段的值的序号,
    ///用来保持会话的连接信息的transaction字段的值
    std::atomic<int> tx_seq_{0};

    ///待处理的操作,存储发送给janus服务器的transaction字段的值和对应的操作
    std::unordered_map<std::string, PendingOp> tx_ops_;
	///待处理的操作,存储发送给janus服务器的transaction字段的值和对应的feed_id
    std::unordered_map<std::string, uint64_t> tx_feeds_;
	///存储feed_id和发布者句柄的映射,feed_id是janus服务端返回的id,发布者句柄是客户端分配的id
	///feed_id是房间里某个远端发布者的id,发布者句柄是客户端分配的id
	/// handle_id是客户端连接videoroom插件之后,janus服务端返回的id,这个id表示一个连接
	//每个远端的发布者都有一个handle_id,如果我要获得多个远端的视频,我就需要多个handle_id
    std::unordered_map<uint64_t, uint64_t> feed_to_handle_;
    std::mutex map_mutex_;

    //保持心跳的线程
    std::thread keepalive_thread_;
    ///保持会话的连接的线程是否在运行
    std::atomic<bool> keepalive_running_{false};
};

}  // namespace xrtc
