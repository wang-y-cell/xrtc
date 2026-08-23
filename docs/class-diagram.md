# xrtc 类图（PlantUML）

**完整类图（一张）**：[plantuml/05-class-diagram.puml](./plantuml/05-class-diagram.puml)

## 预览

1. 安装扩展 **PlantUML**（jebbs）
2. 打开 `docs/plantuml/05-class-diagram.puml`
3. 按 `Alt+D` 预览

或粘贴到 [https://www.plantuml.com/plantuml/uml](https://www.plantuml.com/plantuml/uml)

无本地 Java 时可在 Cursor `settings.json` 中配置：

```json
"plantuml.server": "https://www.plantuml.com/plantuml",
"plantuml.render": "PlantUMLServer"
```

## 图中关系说明

| 关系 | 含义 |
|------|------|
| `<|--` | 继承（如 `XRtcEngine` → `IXRtcEngine`，`Widget` → `XRtcEngineObserver`） |
| `<|..` | 实现接口（如 `VcmCapture` 实现 `VideoSinkInterface`） |
| `*-->` | 组合 / 强拥有（`unique_ptr`） |
| `o-->` | 聚合 / 引用持有 |
| `..>` | 依赖 / 使用 |
| `+--` | 嵌套类型（如 `VideoCaptureModule::DeviceInfo`） |

## `XRtcEngine` 与 WebRTC 设备模块

| 成员 | 类型 | 用途 |
|------|------|------|
| `video_device` | `unique_ptr<webrtc::VideoCaptureModule::DeviceInfo>` | 枚举摄像头（`get_video_device_info`） |
| `audio_device` | `scoped_refptr<webrtc::AudioDeviceModule>` | 枚举麦克风；构造时交给 `XRtcGlobal::adm_` 供 PC Factory 共用 |

其它结构图（分层 / 时序 / 线程）仍在 [architecture-plantuml.md](./architecture-plantuml.md) 与 `plantuml/01~04-*.puml`。
