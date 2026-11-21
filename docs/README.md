# xrtc 文档

本目录保存 xrtc（WebRTC + Janus 音视频 SDK）的文档。已在 `.gitignore` 中解禁，随代码一起提交。

| 文档 | 面向 | 内容 |
|------|------|------|
| [api.md](./api.md) | **SDK 使用方** | 三个公开头文件的完整接口说明：类型、方法、回调、线程约定、典型场景、已知差异 |
| [architecture.md](./architecture.md) | 维护者 | 整体分层、模块职责、线程模型、一次 join 的主链路（含 Mermaid 图） |
| [流程.md](./流程.md) | 维护者 | Janus 信令与 UDP 打洞流程、本地进房/订阅流程的逐步讲解 |
| [class-diagram.md](./class-diagram.md) | 维护者 | 类图说明；图源见 [plantuml/](./plantuml/) |

其它入口：

- 编译、工具链、Demo 的 `XRTC_*` 环境变量 → 仓库根目录 [README.md](../README.md)
- 早期欠缺项清单（部分内容已被实现推翻，仅作历史参考） → [update_note.md](../update_note.md)

> 维护约定：改动 `include/xrtc/` 下的公开头时，同步更新 `api.md`；
> 只改内部实现（engine / session / janus / pc / media）时不必动 `api.md`。
