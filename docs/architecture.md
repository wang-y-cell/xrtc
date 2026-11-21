# xrtc 项目结构

本仓库是「无 Qt 的实时音视频 SDK（`xrtc`）+ Qt Demo」。  
在 Cursor / VS Code 中打开本文件，用 Markdown 预览即可查看下方 Mermaid 图。

---

## 1. 整体分层

```mermaid
flowchart TB
  subgraph app [Demo层_Qt]
    Widget[Widget_QApplication]
  end

  subgraph publicApi [对外API_include_xrtc]
    IXRtcEngine
    Observer[XRtcEngineObserver]
  end

  subgraph sdk [xrtc_SDK_无Qt]
    Engine[XRtcEngine]
    Global[XRtcGlobal]
    Session[CallSession]
    Janus[JanusClient]
    WS[WebsocketTransport]
    PC[PeerConnectionHandler]
    Media[VcmCapture_VideoTrack_RemoteSink]
  end

  subgraph deps [依赖]
    WebRTC[libwebrtc]
    Beast[Boost_Beast]
    Utils[utils_signal_result_log]
    JanusGW[Janus_VideoRoom]
  end

  Widget -->|实现Observer| Observer
  Widget -->|create_join_leave| IXRtcEngine
  IXRtcEngine --> Engine
  Engine --> Global
  Engine --> Session
  Session --> Janus
  Session --> PC
  Session --> Media
  Janus --> WS
  WS --> Beast
  Beast -->|ws| JanusGW
  PC --> WebRTC
  Media --> WebRTC
  Session --> Utils
  Janus --> Utils
  WS --> Utils
```

---

## 2. 目录对应关系

| 路径 | 角色 |
|------|------|
| `include/xrtc/` | 对外契约：引擎接口、Observer、数据结构、log/result |
| `include/engine/` | 引擎实现与全局单例 |
| `include/session/` | 通话编排（CallSession） |
| `include/janus/` | Janus 信令客户端 + WebSocket 传输 |
| `include/pc/` | PeerConnection 封装 |
| `include/media/` | 采集 / 视频源 / 远端渲染 sink |
| `src/**` | 与 `include/<module>` 一一对应的实现 |
| `demo/` | Qt UI，实现 `XRtcEngineObserver` |
| `utils/` | 自研框架：信号槽 / expected / log |
| `third_party/` | `spdlog` / `google_test` 为 submodule；json、Boost 由 CMake 下载 Release 包 |

---

## 3. 一次 join 的主链路

```mermaid
sequenceDiagram
  participant UI as Widget
  participant Eng as XRtcEngine
  participant CS as CallSession
  participant JC as JanusClient
  participant WS as WebsocketTransport
  participant PC as PeerConnectionHandler
  participant Obs as XRtcEngineObserver

  UI->>Eng: join(config)
  Eng->>CS: Start(config)
  CS->>JC: Connect
  JC->>WS: open(ws_url)
  WS-->>JC: connected signal
  JC->>JC: create / attach / join
  JC-->>CS: joined_as_publisher
  CS->>CS: PostTask api_thread
  CS->>PC: 本地媒体 + CreateOffer
  CS->>JC: Publish / Trickle
  JC-->>CS: publishers / subscriber_offer
  CS->>PC: Subscribe PC + Answer
  PC-->>Obs: on_remote_video_frame 等
  Obs-->>UI: QueuedConnection 切 UI 渲染
  CS-->>Obs: on_join_result
```

---

## 4. 模块职责（心智图）

```text
        ┌──────────── Demo (Qt) ────────────┐
        │  Widget = Observer + 调 Engine     │
        └───────────────┬───────────────────┘
                        │ IXRtcEngine / Observer
        ┌───────────────▼───────────────────┐
        │           XRtcEngine               │
        │              │                     │
        │         CallSession  ◄── 编排核心   │
        │          /        \                │
        │     JanusClient   PeerConnection   │
        │         │         + 本地/远端媒体   │
        │   WebsocketTransport               │
        └─────────┼─────────────┬────────────┘
                  │             │
              Janus 信令      libwebrtc 媒体
```

| 模块 | 一句话 |
|------|--------|
| **XRtcGlobal** | 单例：WebRTC 三线程、PC Factory、共享 ADM、全局 Observer |
| **XRtcEngine** | `IXRtcEngine` 实现：持有 `VideoCaptureModule::DeviceInfo` + `AudioDeviceModule` 枚举设备，预览源、CallSession |
| **CallSession** | 通话编排：Janus 事件 ↔ PC/采集 ↔ Observer |
| **JanusClient** | VideoRoom 协议状态机 |
| **WebsocketTransport** | Boost.Beast WebSocket（Pimpl） |
| **PeerConnectionHandler** | 单个 PeerConnection（SDP/ICE/Track） |
| **VcmCapture / VideoTrackSource** | 本地摄像头 → 视频轨 |
| **RemoteVideoSink** | 远端轨 → ARGB → Observer |

---

## 5. 事件与线程

```mermaid
flowchart LR
  subgraph beastThread [Beast_io线程]
    WS2[WebsocketTransport_emit]
    JC2[JanusClient_槽]
  end

  subgraph apiThread [WebRTC_api_thread]
    CS2[CallSession_PostTask后]
    PC2[PeerConnection]
  end

  subgraph uiThread [Qt_UI线程]
    W2[Widget_Observer]
  end

  WS2 -->|utils_connect_Direct| JC2
  JC2 -->|utils_connect| CS2
  CS2 -->|Observer| W2
```

| 线程 | 典型工作 |
|------|----------|
| Qt UI | Widget、Demo 里 Queued 回来的 Observer |
| WebRTC api_thread | 建 PC、改 SDP、多数 CallSession 业务 |
| WebRTC worker/network | 编解码、ICE、收发包 |
| Beast io | WebSocket 读写、Janus JSON 解析入口 |
| 采集线程 | VCM 出帧 |

原则：**谁产生事件就在谁的线程 emit；碰 WebRTC 就 PostTask 到 api_thread；碰 UI 就投到 Qt。**

---

## 6. 建议阅读顺序

1. `include/xrtc/ixrtc_engine.h`、`xrtc_defines.h`
2. `demo/widget.cpp`（`join_meeting` / `on_join_result`）
3. `src/session/call_session.cpp`（`Start` + `bindJanusSignals`）
4. `src/janus/janus_client.cpp`
5. `src/janus/websocket_transport.cpp`
6. `src/pc/peer_connection.cpp`

---

## 7. 预览说明

### Mermaid（本文件）

- **Cursor / VS Code**：`Ctrl+Shift+V` 打开预览；若仍显示代码块，需安装 **Markdown Preview Mermaid Support**
- **在线**：[https://mermaid.live](https://mermaid.live)

### PlantUML（推荐，预览更稳）

- **完整类图（一张）**：[plantuml/05-class-diagram.puml](./plantuml/05-class-diagram.puml)（说明见 [class-diagram.md](./class-diagram.md)）
- 其它图：[architecture-plantuml.md](./architecture-plantuml.md)、[plantuml/](./plantuml/)（安装 **PlantUML** 扩展后 `Alt+D` 预览）

---

## 8. 信令传输层（ws / wss）分层

传输层只做「连接 + 收发文本帧」，不关心 Janus 协议；明文与 TLS 的差异收敛在一个
通道接口后面。改这一层不会影响 `CallSession`，换信令协议也不会影响这一层。

```mermaid
flowchart TB
  subgraph upper [调用方]
    CFG[XRTCJoinConfig<br/>janus_ws_url / ca_file / tls_verify / allow_plaintext_ws]
  end

  subgraph janus [janus 模块]
    JC[JanusClient::do_connect]
    POL[CheckWebsocketUrlPolicy<br/>scheme 策略]
    RES[ResolveWebsocketTlsOptions<br/>显式 > XRTC_CA_FILE > SSL_CERT_FILE > CURL_CA_BUNDLE]
    TC[WebsocketTransportConfig<br/>url / subprotocol / tls]
  end

  subgraph transport [WebsocketTransport]
    IMPL[Impl 状态机<br/>解析 / 连接 / 收发 / 错误上报]
    CH[IWebsocketChannel]
    PLAIN[PlainChannel<br/>ws]
    TLS[TlsChannel<br/>wss: TLS + WebSocket]
    VERIFY[verify_peer<br/>链 + X509_check_host / ip_asc]
  end

  CFG --> JC
  JC --> POL
  JC --> RES
  POL --> TC
  RES --> TC
  TC --> IMPL
  IMPL --> CH
  CH --> PLAIN
  CH --> TLS
  TLS --> VERIFY
```

职责边界：

| 层 | 负责 | 不负责 |
|----|------|--------|
| `XRTCJoinConfig`（公开 API） | 声明 URL、CA、是否允许明文 | 不描述 TLS 实现细节 |
| `JanusClient` | 把进房配置翻译成传输参数；scheme 策略校验 | 不做握手、不读环境变量 |
| `WebsocketTransport::open` | 解析 URL、建 TLS 上下文、起网络线程、同步错误返回 | 不认识 Janus 消息格式 |
| `IWebsocketChannel` | 握手顺序（TLS→WS）、读写、SNI | 不认识上层业务 |
| `verify_peer` | 链校验 + 目标身份（SAN dNSName / iPAddress） | 不管理 CA 来源 |

两个实现约束：

1. **身份校验自己写**：本 SDK 附带 BoringSSL 的 `X509_VERIFY_PARAM_set1_host`
   会返回 0，用它会让所有 `wss://域名` 失败；因此统一用 `X509_check_host` /
   `X509_check_ip_asc`，并给出 `depth`/`err` 级别的错误日志。
2. **`#ifdef XRTC_HAVE_WSS` 只出现在通道创建处**：状态机不再同时维护
   `ws_plain` / `ws_tls` 两个成员，也不需要 `with_ws()` 模板分发。

测试锚点：`tests/transport_config_test.cpp`（CA 优先级 / scheme 策略 / 默认子协议，纯逻辑）、
`tests/websocket_tls_test.cpp`（wss 回环、CA 缺失同步失败、明文拒绝、请求超时）。

