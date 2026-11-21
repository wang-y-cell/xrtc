# xrtc SDK 对外 API 参考

面向使用方（业务客户端）的接口说明。只描述 `include/xrtc/` 下的公开契约；
`include/engine|session|janus|pc|media|platform` 与 `src/` 属于实现细节，
可能随版本调整，不应在你的工程里直接依赖。

- 头文件入口：`include/xrtc/ixrtc_engine.h`、`ixrtc_media_source.h`、`xrtc_defines.h`
- 命名空间：`xrtc`
- 构建产物：静态库 `xrtc.lib`（内部已带 WebRTC / Boost / json / spdlog，使用方不需要单独链接）

---

## 0. 先看这六条（最容易踩的约定）

1. **只包含三个头文件**，链接 `xrtc` 即可；不要 include `<engine/...>`、`<janus/...>` 等内部头。
2. **一个进程一个引擎**：`create_xrtc_engine()` 会把观察者登记到进程级单例，多个引擎实例会互相覆盖
   observer / 声卡模块 / PC 工厂。需要多实例前先确认没有此需求。
3. **`join()` 是异步的**，结果从 `on_join_result()` 回来；`ok` 只表示"本地媒体与推流 PC 已就绪、
   offer 已发出"，**不代表 ICE 已连通**。真正的连通状态看 `on_connection_state(kConnected)`。
4. **进房后不会自动开摄像头/麦克风**。进房只是建好轨道和采集器（默认不采集、不推流），
   要调 `start_local_video()` / `start_local_audio()` 才真正推流。
5. `mute_audio/mute_video` **只切推流开关，不停硬件采集**；要停采集用 `stop_local_*`。
6. **所有回调都不保证在你调用 API 的线程**（详见第 5 节），回调里不要做耗时操作，
   需要更新 UI 时先切回自己的线程。

---

## 1. 头文件与链接

| 头文件 | 内容 |
|--------|------|
| `<xrtc/ixrtc_engine.h>` | `IXRtcEngine` 接口、`XRtcEngineObserver` 回调、`create_xrtc_engine` / `destroy_xrtc_engine` |
| `<xrtc/ixrtc_media_source.h>` | `IXRtcMediaSource` 抽象媒体源（`start` / `stop` / `device_switch`） |
| `<xrtc/xrtc_defines.h>` | 全部数据结构与枚举（设备、格式、画质、进房配置、错误码、视频帧） |

`Rest<>`（统一可失败返回值）定义在 `include/internal/xrtc_result.h`，由 `ixrtc_engine.h` 间接引入，
你不需要也不应该直接 include 它。

### CMake 接入

本仓库**没有** `find_package(xrtc)`，用 `add_subdirectory` 嵌进来：

```cmake
set(BUILD_XRTC_DEMO OFF CACHE BOOL "" FORCE)
set(BUILD_XRTC_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(path/to/webrtc_test)   # 换成本仓库路径

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE xrtc)
```

### 工具链要求（硬约束）

| 项 | 要求 | 不满足的后果 |
|----|------|--------------|
| 编译器 | MSVC x64，`cl` 19.51+（VS 2026 / 工具集 14.51） | 链接 `webrtc.lib` 失败或运行期崩溃 |
| 运行库 | `/MD` 或 `/MDd`，与 `webrtc.lib` 一致 | 重复符号 / 堆不一致 |
| 迭代器调试 | `_ITERATOR_DEBUG_LEVEL=0`（`xrtc` 以 PUBLIC 传递，**不要覆盖**） | `LNK2038`：`_ITERATOR_DEBUG_LEVEL` 不匹配 |
| 配置 | 在 vcvars64（2026）环境里 configure / build | CMake 选不到 `cl`，或被 MinGW 抢走 |

Debug 请用 Debug 版 `webrtc.lib`，Release 用 Release 版，仓库的 CMake 会按
`CMAKE_BUILD_TYPE` 自动选。

---

## 2. 快速开始

```cpp
#include <xrtc/ixrtc_engine.h>
#include <xrtc/xrtc_defines.h>

class MyObserver : public xrtc::XRtcEngineObserver {
public:
    void on_join_result(xrtc::XRtcError error, const std::string& message) override {
        if (error != xrtc::XRtcError::kNOERROR) {
            // message 里是原因，可展示给用户
            return;
        }
        // 进房成功：切到自己的线程后再开麦开摄像头
    }

    void on_leave(xrtc::XRtcError error) override { /* 会话结束（含异常断开） */ }

    void on_connection_state(xrtc::XRTCConnectionState state) override {
        // kConnected 才是真正推拉流可用
    }

    void on_remote_user_joined(const xrtc::XRTCRemoteUser& user) override {
        // user.feed_id 唯一标识一路远端；user.display 是对端 display_name
    }
    void on_remote_user_left(const xrtc::XRTCRemoteUser& user) override {}

    void on_remote_video_frame(uint64_t feed_id, const xrtc::XRTCVideoFrame& frame) override {
        // frame.valid() 为真才可用；如需长期持有请自行拷贝
    }

    void on_video_frame(xrtc::IXRtcMediaSource* source,
                        const xrtc::XRTCVideoFrame& frame) override {
        // 本地画面（预览源，或会议内本端采集）
    }

    void on_audio_level(xrtc::IXRtcMediaSource* source, int level) override {
        // level 0~100，本地麦克风音量
    }
};

int main() {
    MyObserver observer;                       // 生命周期必须覆盖整个 engine
    xrtc::IXRtcEngine* engine = xrtc::create_xrtc_engine(&observer);

    xrtc::XRTCJoinConfig cfg;
    cfg.janus_ws_url = "wss://janus.example.com/janus";
    cfg.room_id = 1234;
    cfg.display_name = "alice";
    cfg.ice_servers = {{"stun:stun.example.com:3478", "", ""}};
    cfg.ca_file = "D:/certs/ca.pem";           // wss 自签证书时用；正式证书可留空
    engine->join(cfg);

    // ……在 on_join_result 成功后：
    // engine->start_local_audio();
    // engine->start_local_video();

    engine->leave();
    xrtc::destroy_xrtc_engine(engine);         // 内部会先摘 observer 再 leave
    return 0;
}
```

要点：

- `observer` 必须活得比 engine 长；`destroy_xrtc_engine` 会先把观察者置空再 `leave`，
  避免销毁过程中的回调打到已释放对象。
- 进房前建议先 `get_video_device_info()` 等枚举好设备，并在 UI 上列出。
- 预览与会议**不要同时开**同一摄像头/麦克风：进房前先 `stop()` + `destroy_*_source()` 掉预览源。

---

## 3. 数据类型

### 3.1 设备、格式与画质

| 结构 / 枚举 | 字段 | 说明 |
|-------------|------|------|
| `XRTCDeviceInfo` | `device_name`、`device_id` | 设备展示名与系统 ID；`device_id` 用于所有 `*_device_id` 参数 |
| `XRTCVideoFormat` | `width`、`height`、`fps` | 采集格式；`get_video_capabilities` 返回的是设备支持列表 |
| `XRTCScreenInfo` | `name`、`screen_id`、`width`、`height`、`primary` | `screen_id` 形如 `\\.\DISPLAY1`，传空表示主屏 |
| `XRTCVideoSelectStrategy` | `kPreferRequested`（默认）、`kPreferStandard` | 在设备能力里选格式的策略；`kPreferStandard` 优先 1280x720@30，其次 640x480 |
| `XRTCVideoCaptureRequest` | `format{640,480,30}`、`strategy` | 引擎级默认采集请求，`create_video_source`/`join` 里宽高帧率为 0 时用它 |
| `ixrtc_video_config` | `width`、`height`、`fps`、`device_id`、`select_strategy` | 预览源参数；宽高帧率为 0 表示沿用引擎当前请求，`device_id` 为空用第一个摄像头 |
| `ixrtc_audio_config` | `device_id` | 预览音频源参数；为空用默认麦克风 |
| `XRTCScreenShareConfig` | `max_long_edge=2560`、`fps=30`、`max_bitrate_kbps=0`、`remote_max_long_edge=0` | 屏幕共享画质 + 远端渲染上限（见下） |

`XRTCScreenShareConfig` 各字段语义：

| 字段 | 默认 | 说明 |
|------|------|------|
| `max_long_edge` | 2560 | 采集长边上限，`0` = 不缩放（按显示器原始分辨率）；4K 屏会等比缩到 2560 |
| `fps` | 30 | 采集与发送帧率上限（1~60） |
| `max_bitrate_kbps` | 0 | 发送码率上限；`<=0` 按 宽×高×帧率 自动估算（约 1~20 Mbps 之间） |
| `remote_max_long_edge` | 0 | 收到远端画面后的渲染长边上限，`0` = 保留原始分辨率；用于多人高分辨率时省 CPU |

采集类参数**下次** `start_local_screen()` 生效；`remote_max_long_edge` 对新订阅立即生效。

### 3.2 XRTCIceServer

| 字段 | 说明 |
|------|------|
| `uri` | `stun:host:port` 或 `turn:host:port`，也支持 `turns:` |
| `username` | TURN 用户名；STUN 留空 |
| `password` | TURN 密码；STUN 留空 |

**注意**：`ice_servers` 为空时**不会**回落公共 STUN，只用本地 host 候选（同一局域网可用），
跨网段必须显式配置 STUN/TURN。

### 3.3 XRTCJoinConfig（进房配置）

| 字段 | 默认 | 说明 |
|------|------|------|
| `janus_ws_url` | 空 | `ws://` 或 `wss://` 的 Janus 地址；为空直接 `on_join_result(kInvalidParam)` |
| `room_id` | 1234 | VideoRoom 房间号 |
| `display_name` | 空 | 显示名；为空时用 `xrtc-user`（会议里作为对端看到的 `display`） |
| `pin` | 空 | 房间 pin（房间设置了才需要） |
| `create_room_if_missing` | false | 为 true 时先尝试创建房间；房间已存在（427）仍继续进房 |
| `room_description` | 空 | 创建房间时的描述 |
| `max_publishers` | 6 | 创建房间时允许的最大发布者数 |
| `admin_key` | 空 | Janus VideoRoom 管理密钥，服务端要求时填（不要下发到不可信客户端） |
| `video_device_id` | 空 | 摄像头；为空用设备列表第一个 |
| `audio_device_id` | 空 | 麦克风；为空用设备列表第一个 |
| `playout_device_id` | 空 | 远端音频播放设备；为空用系统默认 |
| `width` / `height` / `fps` | 0 | 采集请求；0 表示沿用引擎当前请求 |
| `select_strategy` | `kPreferRequested` | 格式选择策略 |
| `ice_servers` | 空 | STUN/TURN 列表，语义见 3.2 |
| `signal_timeout_ms` | 10000 | 单个 Janus 请求超时（create/attach/join/publish/订阅 join）；`<=0` 用默认值 |
| `ca_file` | 空 | `wss://` 的 CA 链（PEM）；解析优先级：本字段 → `XRTC_CA_FILE` → `SSL_CERT_FILE` → `CURL_CA_BUNDLE` → OpenSSL 默认路径 |
| `tls_verify` | true | `false` 只用于自签证书联调，**生产必须 true**；开启时校验链 + 目标身份 |
| `allow_plaintext_ws` | true | 是否允许 `ws://`。置 false 后明文地址在连接前就被拒绝（`on_join_result(kInvalidParam)`）；生产建议 false |

超时的两种后果：会话级请求（create/attach/join/publish）超时 **整场失败**
（`on_join_result` 失败或 `on_leave(kSignalingFailed)`）；单路订阅 join 超时只重试该路
（最多 3 次，退避 0.8s / 1.6s / 2.4s）。

#### wss:// 的证书与明文策略

- **scheme 决定加密**：`wss://` 走 TLS，`ws://` 明文；不额外传布尔开关。
- **身份校验**：域名按证书 SAN `dNSName` 校验（证书没有 DNS SAN 时回落到 `Subject CN`）；
  IP 字面量按 SAN `iPAddress` 校验。SNI 只对域名发送（RFC 6066，IP 不进 SNI）。
- **自签证书**：`ca_file` 指向自签 CA（或直接是服务器证书）；`tls_verify=false` 只用于联调。
- **错误可读**：链校验失败会打印 `depth` 与 `err=18(self signed certificate)` 这类原因，
  身份不匹配会打印目标 host 与 `X509_check_host` 的返回值；`open()` 阶段 CA 文件加载失败
  会同步返回失败（`kSignalingFailed`），不会拖到握手超时。
- **强制加密**：`allow_plaintext_ws=false` 时 `ws://` 在解析阶段就被拒绝，不会发起 TCP 连接。
- 编译期依赖：wss 需要 `-DXRTC_ENABLE_WSS=ON`（默认 ON，复用 `webrtc.lib` 内的 BoringSSL）；
  关闭后 `wss://` 会返回明确的 "wss not supported" 错误，而不是笼统的握手失败。
- **SDK 不随带任何证书 / CA 包**：连公网服务用系统 CA（`ca_file` 留空即可）；
  自建 Janus 用自签证书时，把 CA（通常是服务器证书本身）交给 `ca_file`。
  联调用的自签证书由 `tests/certs/gen_test_certs.ps1` 生成到 `build/test-certs/`
  （`cert.pem` + `key.pem`，私钥只落本地、不入库）。

### 3.4 XRTCVideoFrame（视频帧）

| 字段 | 说明 |
|------|------|
| `width`、`height` | 像素宽高 |
| `stride_y`、`stride_u`、`stride_v` | 各平面行跨度，**可能大于** `width` / `chroma_width` |
| `data_y`、`data_u`、`data_v` | I420 三个平面指针 |
| `storage` | 持有底层缓冲；`data_*` 只在其存活期间有效 |
| `timestamp_us` | 单调时钟微秒（与 `webrtc::TimeMicros` 同一时基） |
| `ntp_time_ms` | NTP 毫秒，便于与音频对齐；可能为 0 |
| `rtp_timestamp` | RTP 90kHz 时间戳 |
| `render_time_ms` | 建议渲染时刻；可能为 0 |
| `rotation_degrees` | 旋转角；SDK 交给回调前已把像素转正，通常为 0 |
| `valid()` | `storage && data_* && width/height/stride > 0` |

**生命周期**：帧只在回调执行期间有效。要异步渲染，请拷贝三平面（或自建 `shared_ptr` 持有
`storage`），不要只保存指针。

格式固定 I420（不是 ARGB），可直接上传 OpenGL 纹理三个平面，避免一次颜色转换。

### 3.5 错误模型

所有可能失败的同步接口返回 `Rest<>`（成功无值、失败带 `XRtcError`）：

```cpp
auto st = engine->start_local_video();
if (!st) {
    // st.error() 是 XRtcError；XRtcErrorToString(st.error()) 得到可读串
    printf("start video failed: %s\n",
           std::string(xrtc::XRtcErrorToString(st.error())).c_str());
}
```

| `XRtcError` | 值 | 含义 |
|-------------|----|------|
| `kNOERROR` | 0 | 成功 |
| `kVideoSourceNotInit` | -999 | 视频源还没建好（未进房就调 `start_local_video` 之类） |
| `kVideoSourceStartFailed` | -998 | 视频源启动失败 |
| `kAlreadyInCall` | -997 | 已在通话中（重复 `join`） |
| `kNotInCall` | -996 | 当前没有通话（未进房/已退房） |
| `kInvalidParam` | -995 | 参数非法（如空 `janus_ws_url`、空 `device_id`） |
| `kSignalingFailed` | -994 | 信令失败/超时/连接断开 |
| `kPeerConnectionFailed` | -993 | PC 创建、SDP、ICE 失败 |
| `kMediaStartFailed` | -992 | 媒体启动失败（设备占用、工厂未就绪等） |

`XRTCConnectionState`：`kNew`(0)、`kConnecting`、`kConnected`、`kDisconnected`、`kFailed`、`kClosed`。

### 3.6 XRTCRemoteUser

| 字段 | 说明 |
|------|------|
| `feed_id` | 远端这一路流的唯一 ID，**渲染/区分多路都靠它** |
| `display` | 对端的 `display_name`；可能为空 |

---

## 4. 接口详解

### 4.1 创建与销毁

```cpp
IXRtcEngine* create_xrtc_engine(XRtcEngineObserver* observer);
void destroy_xrtc_engine(IXRtcEngine* engine);
```

- `observer`：进程级登记（见第 0 节约定 2），可为 `nullptr`（表示不关心回调）。
- `destroy_xrtc_engine`：先摘 observer，再 `leave()`，最后删除引擎；传入 `nullptr` 安全。
- 引擎内部复用进程共享的声卡模块与 PC 工厂，销毁引擎**不会**释放它们（下次创建更快）。

### 4.2 设备枚举

| 方法 | 说明 |
|------|------|
| `std::vector<XRTCDeviceInfo> get_video_device_info()` | 摄像头列表（VCM 枚举） |
| `std::vector<XRTCDeviceInfo> get_audio_device_info()` | 麦克风列表 |
| `std::vector<XRTCDeviceInfo> get_playout_device_info()` | 扬声器/播放设备列表（与麦克风分开） |
| `std::vector<XRTCScreenInfo> get_screen_info()` | 显示器列表，用于屏幕共享选择 |

均可在进房前后调用；进房期间调用同样安全（内部会投递到 WebRTC API 线程执行）。

### 4.3 采集格式 / 画质配置

| 方法 | 说明 |
|------|------|
| `std::vector<XRTCVideoFormat> get_video_capabilities(device_id)` | 摄像头支持的采集格式（可能含重复分辨率、不同像素格式）；`device_id` 为空返回空列表 |
| `XRTCVideoFormat select_video_format(device_id, requested, strategy)` | 按策略在设备能力里选出"最终会用的格式"，建议在创建源/进房前调，用于 UI 显示真实分辨率；能力为空时原样返回 `requested` |
| `void set_video_capture_request(request)` / `XRTCVideoCaptureRequest get_video_capture_request() const` | 读写引擎级默认采集请求 |
| `void set_screen_share_config(config)` / `XRTCScreenShareConfig get_screen_share_config() const` | 读写屏幕共享画质；会议中设置也会下发到当前会话 |

### 4.4 预览媒体源

用于"进房前看自己/听自己"：本地预览、音量条、设备调试。

| 方法 | 返回 | 说明 |
|------|------|------|
| `IXRtcMediaSource* create_video_source(ixrtc_video_config)` | 源指针，失败可能为 `nullptr` | 创建摄像头预览源；**创建后仍需 `start()` 才开始采集** |
| `IXRtcMediaSource* create_screen_source(screen_id)` | 源指针 | 创建屏幕预览源；`screen_id` 为空用主屏 |
| `IXRtcMediaSource* create_audio_source(ixrtc_audio_config)` | 源指针 | 创建麦克风预览源（同时提供 `on_audio_level`） |
| `void destroy_video_source(IXRtcMediaSource*)` | - | 销毁摄像头/屏幕预览源（两者共用一个销毁入口） |
| `void destroy_audio_source(IXRtcMediaSource*)` | - | 销毁音频预览源 |

`IXRtcMediaSource`：

| 方法 | 说明 |
|------|------|
| `Rest<> start()` | 开始采集；成功后回调 `video_source_start_event(kNOERROR)` |
| `Rest<> stop()` | 停止采集；回调 `video_source_stop_event` |
| `Rest<> device_switch(device_id)` | 预览中切换设备；`device_id` 为空由实现决定默认设备 |

约定：

- 源由引擎创建、**由你销毁**；`destroy_*_source` 内部会 delete，无需再 `stop()`（但建议先 `stop()`）。
- 预览与会议同时开同一设备会冲突（Windows 下第二个采集器可能拿不到设备）：
  进房前先 `stop()` + `destroy_*_source()`。
- 会议期间引擎会在内部另建采集器，**预览源不会自动参与会议推流**。

### 4.5 会话控制

| 方法 | 返回 | 说明 |
|------|------|------|
| `void join(const XRTCJoinConfig&)` | 无（异步） | 连接 Janus → create/attach/join → 建本地轨与 Publisher PC → 发 offer。结果走 `on_join_result`；已在通话中会以 `kAlreadyInCall` 失败回调 |
| `void leave()` | 无（同步阻塞到拆完） | 断信令、关 PC、停采集，触发 `on_leave(kNOERROR)`；未进房时调用安全 |
| `void mute_audio(bool)` | 无 | 只切音频推流轨的 enable，不碰硬件采集 |
| `void mute_video(bool)` | 无 | 只切视频推流轨的 enable，不碰硬件采集 |

`join()` 的补充语义：

- 摄像头/麦克风/扬声器 ID 为空时自动取设备列表第一项；`display_name` 为空用 `xrtc-user`。
- `playout_device_id` 会在进房前设置；设置失败只打警告，不阻断进房。
- **加密由 URL scheme 决定**：`wss://` 走 TLS，`ws://` 明文。`allow_plaintext_ws=false`
  时 `ws://` 在连接前就被拒绝（失败码 `kInvalidParam`），不会发起 TCP 连接。
- 进房后**默认不采集、不推流**（轨已建好但被静音），等 `start_local_*`。
- 进房任务入队后立刻再 `join`：只有最后一次真正执行（旧任务按世代号丢弃）；
  已经进房成功后再 `join` 则直接以 `kAlreadyInCall` 失败回调。

### 4.6 会议内媒体控制

| 方法 | 前置 | 说明 |
|------|------|------|
| `Rest<> start_local_video()` | 已在会议中 | 开摄像头采集并开始推流（内部会解除视频静音）。**若正在共享屏幕，会先停屏幕** |
| `Rest<> stop_local_video()` | 已在会议中 | 停推流 + 停摄像头与屏幕采集；关采集前会推一帧黑图，避免对端卡在最后一帧 |
| `Rest<> start_local_screen(screen_id)` | 已在会议中 | 共享屏幕并推到同一条视频轨；`screen_id` 为空用主屏；**会停掉摄像头采集** |
| `Rest<> stop_local_screen()` | 已在会议中 | 只停屏幕采集；当前推的是屏幕时同时停视频推流 |
| `Rest<> start_local_audio()` | 已在会议中 | 开麦采集并解除音频静音 |
| `Rest<> stop_local_audio()` | 已在会议中 | 停推流 + 停硬件录音 |

错误返回：未进房 → `kNotInCall`；采集器未就绪 → `kVideoSourceNotInit` / `kMediaStartFailed`；
设备被占用等由底层返回并以日志体现。

### 4.7 设备切换（会议中）

| 方法 | 说明 |
|------|------|
| `Rest<> set_video_device(device_id)` | 会中切换摄像头；若采集器尚未创建（例如刚进房还没开摄像头）只记住 ID，下次 `start_local_video` 生效 |
| `Rest<> set_audio_device(device_id)` | 会中切换麦克风；语义同上 |
| `Rest<> set_playout_device(device_id)` | 切换远端音频播放设备，可在进房前或会议中调；`device_id` 为空表示系统默认 |

---

## 5. 观察者回调

| 回调 | 触发时机 | 线程 | 注意 |
|------|----------|------|------|
| `video_source_start_event(source, error)` | 采集源 `start()` 完成 | 采集/调用线程 | 预览源与会话内采集器都会触发；用 `source` 指针区分 |
| `video_source_stop_event(source, error)` | 采集源 `stop()` 完成 | 同上 | 同上 |
| `on_video_frame(source, frame)` | 本地采集出帧（预览源开始采集后；会议内本端采集） | 采集线程 | `frame` 仅回调期间有效；**必须尽快返回** |
| `on_remote_video_frame(feed_id, frame)` | 收到远端视频帧 | WebRTC 视频线程 | 多路用 `feed_id` 区分；按 `render_time_ms` 调度渲染 |
| `on_audio_level(source, level)` | 本地麦克风音量变化，`level` 0~100 | 采集线程 | 无远端音量回调 |
| `on_join_result(error, message)` | `join` 成功/失败 | WebRTC API 线程 | `ok` 只代表本地已就绪并发出 offer；失败时 `message` 可用于展示 |
| `on_leave(error)` | 主动 `leave`、信令断开、ICE 失败等会话终止 | WebRTC API 线程 | 与 `on_join_result` 二选一语义：进房成功后才会走这里 |
| `on_connection_state(state)` | **Publisher PC** 连接状态变化 | WebRTC 线程 | 订阅侧失败只以 `on_leave(kPeerConnectionFailed)` 体现 |
| `on_remote_user_joined(user)` | 某路远端订阅开始（Subscriber PC 建立、answer 生成时） | WebRTC API 线程 | 同一 feed 重试订阅不会重复通知 |
| `on_remote_user_left(user)` | 远端停止推流、离开，或该路订阅被放弃 | WebRTC API 线程 | 之后不会再收到该 feed 的帧 |

回调使用原则：

1. **不要在回调里阻塞**（不要 sleep、不要等锁、不要同步做重活）。
2. 需要 UI/渲染时，把数据（帧要拷贝）投递到自己的线程——Demo 用
   `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`。
3. 回调里可以调用引擎的同步接口，但不要在这里 `destroy_xrtc_engine`；
   卸载请走自己的事件循环。

---

## 6. 典型场景

### 6.1 进房并推音视频

```cpp
engine->join(cfg);

// on_join_result(error == kNOERROR) 之后：
auto a = engine->start_local_audio();      // 先开麦
auto v = engine->start_local_video();      // 再开摄像头
if (!v) { /* 提示"摄像头启动失败"，可让用户改选设备后重试 */ }
```

### 6.2 屏幕共享（与摄像头互斥）

```cpp
auto screens = engine->get_screen_info();  // 列出显示器供选择
auto st = engine->start_local_screen(screens[0].screen_id);  // 空串 = 主屏
// 停共享：
engine->stop_local_screen();   // 只停屏幕；想恢复摄像头再 start_local_video()
```

共享中途改画质：`set_screen_share_config({1920, 15, 0, 0})`，
下次 `start_local_screen` 生效（会重建采集器）。

### 6.3 静音 vs 停止采集

| 需求 | 调用 | 效果 |
|------|------|------|
| 说话但暂时不让别人听 | `mute_audio(true)` | 麦克风仍在采集，只是不推流；可立即恢复 |
| 彻底关麦（释放设备、显示静音灯） | `stop_local_audio()` | 停硬件录音 + 停推流 |
| 关摄像头但保留采集（如虚拟背景） | `mute_video(true)` | 采集继续，不推流 |
| 关摄像头（省资源、释放设备） | `stop_local_video()` | 停采集 + 停推流，并推一帧黑图 |

### 6.4 会议中切换设备

```cpp
auto devices = engine->get_video_device_info();
engine->set_video_device(devices[1].device_id);   // 已开摄像头则立即切换
```

### 6.5 多路远端渲染

```cpp
// on_remote_user_joined: 为 feed_id 建一个渲染视图
// on_remote_video_frame(feed_id, frame): 只更新对应 feed_id 的视图
// on_remote_user_left: 销毁对应视图，后续不要再等该 feed 的帧
```

### 6.6 预览 → 进房切换

```cpp
// 进房前：预览
IXRtcMediaSource* preview = engine->create_video_source({0, 0, 0, device_id});
preview->start();

// 进房前：先拆预览，避免同一摄像头被双开
preview->stop();
engine->destroy_video_source(preview);
preview = nullptr;

engine->join(cfg);
```

---

## 7. 会话状态机（文字版）

```
idle --join()--> joining --on_join_result(ok)--> joined
                        \--on_join_result(err)--> idle
joined --leave()------------------------------> idle（on_leave(kNOERROR)）
joined --信令断开/超时------------------------> idle（on_leave(kSignalingFailed)）
joined --Publisher PC failed------------------> idle（on_leave(kPeerConnectionFailed)）
idle   --join() 且已在通话中-------------------> 保持 joined（on_join_result(kAlreadyInCall)）
```

一次通话内的世代号（generation）会作废所有迟到的异步回调：`leave` 之后到达的 offer、
ICE、超时都不会再影响新会话。

---

## 8. 已知差异与坑（当前实现为准）

1. **`set_audio_device` / `set_video_device` 在未进房时返回 `kNotInCall`**，
   不会"记住供下次进房使用"——这与 `ixrtc_engine.h` 里的注释不一致，以实际行为为准；
   进房时请直接填 `XRTCJoinConfig::video_device_id` / `audio_device_id`。
2. **`ice_servers` 为空时不内置任何公网 STUN**，只用本地 host 候选（同一局域网可用）；
   跨网段必须显式配置 STUN/TURN。
3. **会丢弃 IPv6 ICE candidate**（假设服务端侧关闭 IPv6），纯 IPv6 网络无法连通。
4. **`on_connection_state` 只反映 Publisher PC**；订阅 PC 失败以 `on_leave` 体现。
5. **`on_join_result(ok)` 早于 ICE 连通**，不要用它判断"对方能看到我"。
6. **`on_video_frame` 的 `source` 指针与会话内采集器不同**：预览时等于你创建的预览源；
   会议内是引擎内部采集器。不要把 `source` 当作可长期保存的标识。
7. **`Rest<>` 类型来自内部头** `internal/xrtc_result.h`，随公开头一起暴露；
   若需要跨 TU 传递 `Rest<>`，注意它与 ABI 绑定（升级 SDK 时重新编译）。
8. 进程内**只支持一个引擎实例**（见第 0 节）。
9. 屏幕共享依赖 Windows 桌面捕获（WGC/GDI），部分受保护内容（DRM 播放器等）会黑屏。
10. **wss 的证书身份由 SDK 自己校验**（`X509_check_host` / `X509_check_ip_asc`），
    没用 OpenSSL 的 `X509_VERIFY_PARAM_set1_host`：本 SDK 附带的 BoringSSL 里该接口
    会失败，用它会导致所有 `wss://域名` 连不上。自定义 CA 请走 `ca_file`，
    不要依赖系统证书库里的自签证书。

---

## 9. 相关文档

| 文档 | 内容 |
|------|------|
| `README.md` | 编译方法、工具链、Demo 的 `XRTC_*` 环境变量、行为变化 |
| `docs/architecture.md` | 分层、模块职责、线程模型、一次 join 的时序 |
| `docs/流程.md` | Janus 信令与 UDP 打洞流程、本地进房流程（偏实现讲解） |
| `docs/class-diagram.md` + `docs/plantuml/` | PlantUML 类图 |
| `update_note.md` | 早期欠缺项清单，**部分内容已被实现推翻**，仅作历史参考 |

---

*维护约定：修改 `include/xrtc/` 下任何公开头时，同步更新本文件的对应章节与"已知差异"一节；
内部实现（engine/session/janus/pc/media）的改动不需要改这里。*
