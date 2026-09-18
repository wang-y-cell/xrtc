# xrtc

基于 WebRTC + Janus 的音视频 SDK。Windows 上用 CMake 编译。

## 编译

需要：

- **Visual Studio 2026**（18.8+，`cl` **19.51** / 工具集 **14.51**）。不要用 VS 2022 / Preview / MinGW
- CMake ≥ 3.19、独立 **Ninja**（不要用 `depot_tools` 里的 ninja）
- Demo 另需 Qt 6.8（目录名仍是 `msvc2022_64`）
- Git（配置时会自动 `git submodule update --init --recursive` 拉取 spdlog / google_test）

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
