# webrtc_test / xrtc SDK

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
| **Boost ≥ 1.82** | 默认路径 `F:/wy/boost_install`（见 `CMakeLists.txt`） |
| **WebRTC 预编译包** | `lib_debug` + `lib_release`；缺失时从 GitHub Release 自动下载 |
| **Qt 6**（仅 Demo） | 通过 `--qt_lib` / `-DQT6_ROOT` 传入 |

### 同步 WebRTC SDK

Release 资源：[`webrtc.7z`](https://github.com/wang-y-cell/xrtc/releases/download/v1.0.0/webrtc.7z)

压缩包布局：

```
webrtc/
  include/
  lib_debug/webrtc.lib
  lib_release/webrtc.lib
```

CMake 会按 `CMAKE_BUILD_TYPE` 自动选择对应库。缺失时配置阶段自动下载。

```powershell
python build.py
# 或手动
powershell -File scripts/sync_webrtc_sdk.ps1
```

## 构建

```powershell
# Debug SDK（链接 lib_debug）
python build.py

# Debug Demo
python build.py --demo --qt_lib=F:/Qt/6.8.3/msvc2022_64

# Release Demo
python build.py --demo --qt_lib=F:/Qt/6.8.3/msvc2022_64 --config release

# 编译并运行
python build.py --demo --qt_lib=F:/Qt/6.8.3/msvc2022_64 --run

# 清理后重编
python build.py --demo --qt_lib=F:/Qt/6.8.3/msvc2022_64 -c
```

可执行文件：`build/Debug/webrtc_test.exe` 或 `build/Release/webrtc_test.exe`。

### 手动 CMake

```powershell
cmake -S . -B build/Debug -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug `
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
