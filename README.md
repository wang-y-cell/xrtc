# xrtc / xrtc SDK

基于 WebRTC + Janus Gateway 的音视频通话 SDK，附带 Qt6 演示程序。

## 目录结构

```
include/xrtc/          对外公开 API
include/               内部头文件（PRIVATE，链接方不可见）
src/                   SDK 实现（.cpp）
demo/                  Qt6 演示程序（CMake 子目录，默认不编）
third_party/
  google_test/         git submodule
  spdlog/              git submodule
  json/                CMake 从 Release 下载 include.zip
  boost-*/             CMake 从 Release 下载 Boost 源码包
webrtc/                预编译 WebRTC SDK（CMake 分别下载 include / Debug / Release）
  include/             webrtc_include.7z
  lib_debug/           Debug webrtc.lib（/MDd）
  lib_release/         Release webrtc.lib（/MD）
```

## 前置依赖

| 依赖 | 说明 |
|------|------|
| **MSVC 2022** | x64 工具链 |
| **CMake ≥ 3.19** | 唯一构建入口（已不再使用 `build.py`） |
| **Ninja** | 推荐生成器 |
| **Git submodule** | `spdlog`、`google_test`：`git submodule update --init` |
| **nlohmann/json 3.12.0** | 配置时下载 `include.zip`，header-only |
| **Boost 1.92.0** | 配置时下载 `boost-1.92.0-b2-nodocs.7z`；Beast/Asio 只用头文件，不跑 b2 |
| **WebRTC 预编译包** | 缺失时从 GitHub Release 自动下载 |
| **Qt 6**（仅 Demo） | `-DBUILD_XRTC_DEMO=ON -DQT6_ROOT=...` |

克隆后先拉子模块：

```powershell
git submodule update --init --recursive
```

### 自动下载的 Release

| 依赖 | 地址 |
|------|------|
| WebRTC 头文件 | https://github.com/wang-y-cell/xrtc/releases/download/v1.0.0/webrtc_include.7z |
| WebRTC Debug `.lib` | https://github.com/wang-y-cell/xrtc/releases/download/v1.0.0/webrtc_win_msvc2022_x64_debug.7z |
| WebRTC Release `.lib` | https://github.com/wang-y-cell/xrtc/releases/download/v1.0.0/webrtc_win_msvc2022_x64_release.7z |
| nlohmann/json | https://github.com/nlohmann/json/releases/download/v3.12.0/include.zip |
| Boost | https://github.com/boostorg/boost/releases/download/boost-1.92.0/boost-1.92.0-b2-nodocs.7z |

本地已有对应头文件/库时，CMake **不会再下载**。已有 Boost 安装也可 `-DBoost_ROOT=...` 覆盖。

Ninja 等单配置生成器会下载 `webrtc_include.7z` + 当前 `CMAKE_BUILD_TYPE` 对应的那一个 `.lib`；Visual Studio 多配置会两个库都下。解压后放到：

```
webrtc/include/
webrtc/lib_debug/webrtc.lib
webrtc/lib_release/webrtc.lib
```

CMake 会按 `CMAKE_BUILD_TYPE` 选择库。

#### 必须使用 VPN / 代理（国内网络）

访问 GitHub Releases 在国内常出现 `Connection was reset` / `HTTP response code said error`。  
**自动下载前请先开 VPN**，并在当前 PowerShell 会话设置代理（端口按你本机 VPN 为准，常见 Clash 为 `7890`）：

```powershell
$env:HTTPS_PROXY = "http://127.0.0.1:7890"
$env:HTTP_PROXY  = "http://127.0.0.1:7890"

# 可选：确认能连上（期望 302，而不是 reset / 404）
curl.exe -I "https://github.com/wang-y-cell/xrtc/releases/download/v1.0.0/webrtc_win_msvc2022_x64_debug.7z"
```

CMake 的 `file(DOWNLOAD)` 会读取 `HTTPS_PROXY` / `HTTP_PROXY`。

#### 代理仍失败时：手动安装

1. 浏览器（同样走 VPN）打开对应 Release 页下载压缩包
2. WebRTC：`webrtc_include.7z` 解压到 `webrtc/include/`，`.lib` 解压到对应目录：

```text
<仓库根>/webrtc/include/                 # webrtc_include.7z
<仓库根>/webrtc/lib_debug/webrtc.lib     # debug.7z
<仓库根>/webrtc/lib_release/webrtc.lib   # release.7z
```

json 解压到 `third_party/json/`（需能 `#include <nlohmann/json.hpp>`）。  
Boost 解压到 `third_party/`（目录内需有 `boost/version.hpp`）。

目录齐全后 CMake **不会再下载**，可直接编译。

## 构建

只使用 CMake。**Windows 必须用 MSVC 2022 x64**（`webrtc.lib` 与 Qt `msvc2022_64`），不要用 MinGW。

PowerShell **不能**直接 `& vcvars64.bat`：bat 里设置的 PATH 不会留在当前会话，所以会看到初始化横幅，但 `where cl` 仍找不到。

用 VS 自带的 PowerShell 脚本（你这台是 Preview）：

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Preview\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -SkipAutomaticLocation

where.exe cl    # 应是 ...\MSVC\...\bin\Hostx64\x64\cl.exe
where.exe ninja # 第一行不要是 depot_tools；可用 F:\ninja\ninja.exe
```

或开开始菜单里的 **x64 Native Tools Command Prompt for VS 2022 Preview**，在 cmd 里编译。

**默认不编译 Demo**，只有显式打开 `BUILD_XRTC_DEMO` 才会进入 `demo/` 子目录。

```powershell
# 若之前用 MinGW 配过，先删构建目录
Remove-Item -Recurse -Force build/Debug

cmake -S . -B build/Debug -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_C_COMPILER=cl `
  -DCMAKE_CXX_COMPILER=cl `
  -DCMAKE_MAKE_PROGRAM=F:/ninja/ninja.exe `
  -DBUILD_XRTC_DEMO=ON `
  -DQT6_ROOT=F:/Qt/6.8.3/msvc2022_64

cmake --build build/Debug --target webrtc_test
```

可执行文件：`build/Debug/webrtc_test.exe` 或 `build/Release/webrtc_test.exe`。

运行 Demo 前把 `QT6_ROOT/bin` 加入 PATH，并启动 [Janus Gateway](https://janus.conf.meetecho.com/)。

### 链接报 `__std_rotate` / LNK2019

`webrtc.lib` 引用了较新 MSVC STL 的向量化辅助函数。最终链接用的工具集必须 **≥ 编 webrtc.lib 的那套**。  
VS 2022 Preview `17.14.0-pre.1.1`（MSVC 14.44.34823）的 `msvcprtd.lib` 里没有这些符号。

处理：Visual Studio Installer 把 Preview **更新到最新**，或安装带更新工具集的 VS；更新后删掉 `build/Debug` 再配置。  
C4068（未知杂注 clang）来自 WebRTC 头文件，可忽略。

## 对外 API

```cpp
#include <xrtc/ixrtc_engine.h>
#include <xrtc/ixrtc_media_source.h>
#include <xrtc/xrtc_defines.h>
```

WebRTC、Boost、spdlog、utils 等第三方依赖已对链接方隔离（PRIVATE）。
