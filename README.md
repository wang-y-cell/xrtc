# xrtc

基于 WebRTC + Janus 的音视频 SDK。Windows 上用 CMake 编译。

## 编译

需要：

- **Visual Studio 2026**（18.8+，`cl` **19.51** / 工具集 **14.51**）。不要用 VS 2022 / Preview / MinGW
- CMake ≥ 3.19、独立 **Ninja**（不要用 `depot_tools` 里的 ninja）
- Demo 另需 Qt 6.8（目录名仍是 `msvc2022_64`）
- Git（配置时会自动 `git submodule update --init --recursive` 拉取 spdlog / google_test）
- WSS **不需要**额外安装 OpenSSL：TLS 复用 `webrtc.lib` 内置的 BoringSSL。
  不需要时可用 `-DXRTC_ENABLE_WSS=OFF` 关闭（此时 `wss://` 运行期报明确错误）。

用 `vswhere` 找 2026 的 `vcvars64.bat`（不要用 2022 那套）：

```powershell
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -version "[18.0,19.0)" `
  -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
  -property installationPath
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
```

PowerShell 里不要 `& vcvars64.bat`，环境不会留下。在 **一条 cmd** 里加载工具链再配置、编译（路径按本机修改）：

```powershell
Remove-Item -Recurse -Force build\Debug -ErrorAction SilentlyContinue

cmd /c "`"$vcvars`" && cmake -S . -B build/Debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_MAKE_PROGRAM=F:/ninja/ninja.exe -DBUILD_XRTC_DEMO=ON -DQT6_ROOT=F:/Qt/6.8.3/msvc2022_64 && cmake --build build/Debug --target webrtc_test"
```

配置日志里应出现 `MSVC 19.51` 和 `...\MSVC\14.51.xxxxx\...\cl.exe`。不要加 `-DCMAKE_C_COMPILER=cl`（未加载 vcvars 时会找不到编译器）。

- 默认不编 Demo：去掉 `-DBUILD_XRTC_DEMO=ON` 和 `-DQT6_ROOT`
- Release：把 `Debug` 换成 `Release`，`-DCMAKE_BUILD_TYPE=Release`
- json / Boost / WebRTC 预编译包、以及 git submodule（spdlog / google_test）在配置阶段自动处理；国内访问 GitHub 需先设 `$env:HTTPS_PROXY`，git 也需能连 GitHub（可设 `git config --global http.proxy`）

可执行文件：`build/Debug/webrtc_test.exe`。运行 Demo 前把 `QT6_ROOT/bin` 加入 PATH。

## 头文件

对外只需：

```cpp
#include <xrtc/ixrtc_engine.h>
#include <xrtc/ixrtc_media_source.h>
#include <xrtc/xrtc_defines.h>
```

头文件在仓库 `include/xrtc/`。链接 `xrtc` 静态库即可，WebRTC / Boost / json / spdlog 对使用方隔离。

完整接口说明（类型、方法、回调、线程约定、典型场景、已知差异）见 **[docs/api.md](docs/api.md)**。

## 在其他工程里链接本库

本仓库提供静态库目标 `xrtc`，**没有** `find_package(xrtc)`，请用 `add_subdirectory` 把本仓库嵌进对方工程。对方同样必须用 **VS 2026（cl 19.51+）**、x64、`/MD` 或 `/MDd`。

把本仓库放进对方工程后，在对方 `CMakeLists.txt` 里：

```cmake
set(BUILD_XRTC_DEMO OFF CACHE BOOL "" FORCE)
set(BUILD_XRTC_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(path/to/webrtc_test)   # 换成本仓库路径

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE xrtc)
```

C++ 只包含上一节三个头文件。链接 `xrtc` 会带上 `webrtc.lib`、`utils` 以及 `_ITERATOR_DEBUG_LEVEL=0`。

对方 **不必** 传 `-DQT6_ROOT`、`-DCMAKE_MAKE_PROGRAM`；生成器和 Ninja 用他们自己工程的。第一次配置仍会自动拉取 submodule，并下载 WebRTC / json / Boost。有现成 Boost 时可加 `-DBoost_ROOT=`。

配置、编译仍要在 **2026 的 vcvars** 环境里进行（与「编译」一节相同）。

## 运行 Demo：连接参数全部由外部注入

代码里不再内置任何公网地址或 TURN 账号。`webrtc_test.exe` 读取以下环境变量：

| 变量 | 作用 | 示例 |
|------|------|------|
| `XRTC_JANUS_URL` | 预填 Janus 信令地址（可手改），`ws://` 或 `wss://` | `wss://janus.example.com/janus` |
| `XRTC_ROOM_ID` | 预填房间号 | `1234` |
| `XRTC_DISPLAY_NAME` | 预填显示名 | `alice` |
| `XRTC_ICE_URLS` | 逗号分隔的 STUN/TURN URI | `stun:stun.example.com:3478,turn:turn.example.com:3478` |
| `XRTC_ICE_USERNAME` | TURN 用户名（STUN 可留空） | `alice` |
| `XRTC_ICE_PASSWORD` | TURN 密码 | `***` |
| `XRTC_CA_FILE` | WSS 的 CA 证书链（PEM）。为空时依次读 `SSL_CERT_FILE` / `CURL_CA_BUNDLE`，最后用 OpenSSL 默认路径 | `D:\certs\ca.pem` |
| `XRTC_TLS_VERIFY` | `0` 关闭 WSS 证书校验（**仅限自签名联调**），默认开启 | `0` |
| `XRTC_SIGNAL_TIMEOUT_MS` | 单个 Janus 请求超时（默认 10000ms） | `3000` |
| `XRTC_SCREEN_MAX_LONG_EDGE` | 屏幕共享采集长边上限，`0`=不缩放（默认 2560） | `1920` / `3840` |
| `XRTC_SCREEN_FPS` | 屏幕共享帧率（默认 30） | `15` / `30` |
| `XRTC_SCREEN_BITRATE_KBPS` | 屏幕共享发送码率上限 kbps，`0`=自动估算（默认） | `8000` |
| `XRTC_REMOTE_MAX_LONG_EDGE` | 远端画面渲染长边上限，`0`=不缩放（默认，保留原始分辨率） | `1280` |

PowerShell 示例：

```powershell
$env:XRTC_JANUS_URL = "wss://localhost:8989/janus"
$env:XRTC_CA_FILE   = "D:\certs\ca.pem"
$env:XRTC_ICE_URLS  = "turn:turn.example.com:3478"
$env:XRTC_ICE_USERNAME = "alice"
$env:XRTC_ICE_PASSWORD = "secret"
F:\Qt\6.8.3\msvc2022_64\bin 加入 PATH 后运行 build\Debug\demo\webrtc_test.exe
```

## 行为变化（相对旧版本）

- **信令线程**：`JanusClient` 的所有公开方法都投递到它自己的线程执行，
  不再从 WebRTC api 线程直接改 `session_id_` / `subscribe_state_`。
- **WSS**：`wss://` 可用（复用 `webrtc.lib` 内的 BoringSSL），默认校验证书链与目标身份。
  域名按证书 SAN `dNSName` 校验（无 DNS SAN 时回落 `Subject CN`），IP 字面量按 SAN
  `iPAddress` 校验；SNI 只对域名发送。自签名证书用 `XRTC_CA_FILE` 指定 CA，
  或临时 `XRTC_TLS_VERIFY=0`。CA 解析优先级：`XRTCJoinConfig::ca_file` →
  `XRTC_CA_FILE` → `SSL_CERT_FILE` → `CURL_CA_BUNDLE` → 系统默认。
  传输层不再自己读环境变量，也不依赖 `X509_VERIFY_PARAM_set1_host`
  （本 SDK 的 BoringSSL 里该接口会失败，导致 `wss://域名` 连不上）。
- **强制加密**：新增 `XRTCJoinConfig::allow_plaintext_ws`（默认 true）。置 false 后
  `ws://` 在连接前就被拒绝，`on_join_result` 收到 `kInvalidParam`；生产建议置 false。
- **音频处理**：默认启用 AEC3 / 降噪(kHigh) / AGC(kAdaptiveDigital) / 高通，
  不再把 `audio_processing=nullptr` 交给工厂。
- **远端说话检测**：新增 `on_remote_audio_level(feed_id, 0~100)` 与
  `on_remote_talking(feed_id, bool)`。基于远端 PCM 的 RMS+峰值算音量，音量按时间衰减；
  talking 用 release 迟滞防字间闪断，参数由 `XRTCVoiceActivityConfig` 控制
  （阈值 15 / 释放 600ms / 音量回调 100ms）。对端静音或断流时由 200ms 看门狗补
  `talking=false`，解绑 feed 也会补一次，UI 不会一直高亮。
- **ICE**：`ice_servers` 为空时**不再**静默回落 Google STUN，只用本地候选
  （同一网段可用）；跨网段请显式配置 `XRTC_ICE_URLS`。
- **请求超时**：create / attach / join / publish / 订阅 join 都有超时。
  会话级超时会整场失败（`on_join_result` / `on_leave` 收到 `kSignalingFailed`），
  单路订阅超时只重试该路（最多 3 次，退避 0.8s/1.6s/2.4s）。
- **屏幕共享画质**：采集长边上限 2560、30fps、按需自动码率上限；
  BGRA→I420 与缩放改用 libyuv（双线性），不再用最近邻。
  远端默认**不再压到 960**，按对端原始分辨率渲染；需要省 CPU 时用
  `XRTC_REMOTE_MAX_LONG_EDGE` 显式限制。

## 测试

```powershell
ctest --test-dir build\Debug --output-on-failure          # 纯逻辑单测（不需要网络）
```

WSS / 超时冒烟需要本地测试服务端（`build/wss_test_server.py` 只是本地测试脚本，不入库）。

**仓库不随带任何证书**：`cert.pem` / `key.pem` 是本地自签测试证书，生成到 `build/test-certs/`
（`build/` 已被 gitignore，私钥不要提交）。首次或证书过期时执行：

```powershell
powershell -ExecutionPolicy Bypass -File tests\certs\gen_test_certs.ps1   # 加 -Force 覆盖重建
```

脚本需要 openssl（默认找 Git for Windows 自带的），生成 CN=localhost、
SAN=`DNS:localhost, IP:127.0.0.1, IP:::1`、默认 825 天的自签证书，同时当 CA 用。

```powershell
python build\wss_test_server.py --mode echo   --port 8989 --cert build\test-certs\cert.pem --key build\test-certs\key.pem
python build\wss_test_server.py --mode silent --port 8990 --cert build\test-certs\cert.pem --key build\test-certs\key.pem

$env:XRTC_WSS_SMOKE_URL = "wss://localhost:8989/janus"
$env:XRTC_WS_SILENT_URL = "wss://localhost:8990/janus"
$env:XRTC_CA_FILE       = "$PWD\build\test-certs\cert.pem"
build\Debug\xrtc_tests.exe --gtest_filter=WebsocketTls.*:JanusSignaling.*
```

屏幕采集冒烟（需要真实桌面，验证推流分辨率/预览比例）：

```powershell
$env:XRTC_SCREEN_SMOKE = "1"
$env:XRTC_SCREEN_TEST_EDGE = "2560"   # 0 = 不缩放；默认 1280
build\Debug\xrtc_tests.exe --gtest_filter=ScreenCapture.*
# 输出示例：[screen] smoke: edge=2560 push=2560x1440 frames=44 fps=21.8
```
