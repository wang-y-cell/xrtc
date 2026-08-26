# xrtc / xrtc SDK

基于 WebRTC + Janus Gateway 的音视频通话 SDK，附带 Qt6 演示程序。

## 目录结构

```
include/xrtc/     对外公开 API
include/          内部头文件（PRIVATE，链接方不可见）
src/              SDK 实现（.cpp）
demo/             Qt6 演示程序
webrtc/           预编译 WebRTC SDK（需同步，见下文）
  include/
  lib_debug/      Debug webrtc.lib（/MDd）
  lib_release/    Release webrtc.lib（/MD）
build.py          构建脚本
```

## 前置依赖

| 依赖 | 说明 |
|------|------|
| **MSVC 2022** | x64 工具链（固定） |
| **CMake ≥ 3.19** | |
| **Ninja** | 固定生成器 |
| **Boost ≥ 1.82** | 通过 `--boost_path` / `-DBoost_ROOT` 传入安装前缀 |
| **WebRTC 预编译包** | `lib_debug` + `lib_release`；缺失时从 GitHub Release 自动下载 |
| **Qt 6**（仅 Demo） | 通过 `--qt_lib` / `-DQT6_ROOT` 传入 |

### 同步 WebRTC SDK

Release 资源：[`webrtc.7z`](https://github.com/wang-y-cell/xrtc/releases/download/v1.0.0/webrtc.7z)

压缩包布局（解压后顶层目录必须是 `webrtc/`）：

```
webrtc/
  include/
  lib_debug/webrtc.lib
  lib_release/webrtc.lib
```

CMake 会按 `CMAKE_BUILD_TYPE` 自动选择对应库。本地缺失时，配置阶段会从 GitHub Release 自动下载。

#### 必须使用 VPN / 代理（国内网络）

访问 GitHub Releases 在国内常出现 `Connection was reset` / `HTTP response code said error`。  
**自动下载前请先开 VPN**，并在当前 PowerShell 会话设置代理（端口按你本机 VPN 为准，常见 Clash 为 `7890`）：

```powershell
$env:HTTPS_PROXY = "http://127.0.0.1:7890"
$env:HTTP_PROXY  = "http://127.0.0.1:7890"

# 可选：确认能连上（期望 302，而不是 reset / 404）
curl.exe -I "https://github.com/wang-y-cell/xrtc/releases/download/v1.0.0/webrtc.7z"

python build.py --boost_path=F:/wy/boost_install
# 或
powershell -File scripts/sync_webrtc_sdk.ps1
```

CMake 的 `file(DOWNLOAD)` 会读取 `HTTPS_PROXY` / `HTTP_PROXY`。

#### 代理仍失败时：手动安装

1. 浏览器（同样走 VPN）打开上述 Release 页，下载 `webrtc.7z`
2. 解压到**仓库根目录**（与 `CMakeLists.txt`、`build.py` 同级），最终路径必须是：

```text
<仓库根>/webrtc/include/
<仓库根>/webrtc/lib_debug/webrtc.lib
<仓库根>/webrtc/lib_release/webrtc.lib
```

示例（仓库在 `F:/wy/xrtc` 时）：

```text
F:/wy/xrtc/webrtc/include/
F:/wy/xrtc/webrtc/lib_debug/webrtc.lib
F:/wy/xrtc/webrtc/lib_release/webrtc.lib
```

目录齐全后 CMake **不会再下载**，可直接编译。

## 构建

首次配置必须指定 `--boost_path`（Boost 安装前缀，内含 `include` / `lib` 等）。

```powershell
# Debug SDK（链接 lib_debug）
python build.py --boost_path=F:/wy/boost_install

# Debug Demo
python build.py --boost_path=F:/wy/boost_install --demo --qt_lib=F:/Qt/6.8.3/msvc2022_64

# Release Demo
python build.py --boost_path=F:/wy/boost_install --demo --qt_lib=F:/Qt/6.8.3/msvc2022_64 --config release

# 编译并运行
python build.py --boost_path=F:/wy/boost_install --demo --qt_lib=F:/Qt/6.8.3/msvc2022_64 --run

# 清理后重编
python build.py --boost_path=F:/wy/boost_install --demo --qt_lib=F:/Qt/6.8.3/msvc2022_64 -c
```

可执行文件：`build/Debug/webrtc_test.exe` 或 `build/Release/webrtc_test.exe`。

### 手动 CMake

```powershell
cmake -S . -B build/Debug -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug `
  -DBoost_ROOT=F:/wy/boost_install `
  -DBUILD_XRTC_DEMO=ON `
  -DQT6_ROOT=F:/Qt/6.8.3/msvc2022_64

cmake --build build/Debug --target webrtc_test
```

### 运行前注意

1. 启动 [Janus Gateway](https://janus.conf.meetecho.com/)（默认 `ws://localhost:8188`）
2. `python build.py --demo --run` 会把 `QT6_ROOT/bin` 加入 PATH

## 对外 API

```cpp
#include <xrtc/ixrtc_engine.h>
#include <xrtc/ixrtc_media_source.h>
#include <xrtc/xrtc_defines.h>
```

WebRTC、Boost、spdlog、utils 等第三方依赖已对链接方隔离（PRIVATE）。
