cmake_minimum_required(VERSION 3.19)
project(webrtc_test LANGUAGES C CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(WEBRTC_INCLUDE "F:\\wy\\webrtc\\src")
set(WETC_LIB "F:\\wy\\webrtc\\src\\out\\Default\\obj")

# ---------- third_party: nlohmann/json (header-only) ----------
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/single_include)

# ---------- third_party: spdlog (header-only, demo log backend only) ----------
add_library(spdlog_header INTERFACE)
target_include_directories(spdlog_header INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/spdlog/include)

# ---------- utils framework (header-only: signal / result / log) ----------
add_subdirectory(utils EXCLUDE_FROM_ALL)

# ---------- Boost (Beast/Asio header-only WebSocket) ----------
# 本机安装：F:/wy/boost_install（MinGW 编译的 .dll 仅 header-only 组件可与 MSVC 共用）
if(POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW)
endif()
set(Boost_ROOT "F:/wy/boost_install" CACHE PATH "Boost install prefix")
set(Boost_NO_SYSTEM_PATHS ON)
list(APPEND CMAKE_PREFIX_PATH "${Boost_ROOT}")
find_package(Boost 1.82 REQUIRED CONFIG)
message(STATUS "Boost ${Boost_VERSION} found: ${Boost_DIR}")

# ---------- xrtc SDK (no Qt) ----------
# 信令 WebSocket 使用 Boost.Beast，不再依赖手写 Winsock / libwebsockets
# include/<module>/*.h 与 src/<module>/*.cpp 一一对应
add_library(xrtc STATIC
    src/engine/xrtc_global.cpp
    src/engine/xrtc_engine.cpp
    src/pc/peer_connection.cpp
    src/media/video_track_source.cpp
    src/media/remote_video_sink.cpp
    src/media/vcm_capture.cpp
    src/session/call_session.cpp
    src/janus/websocket_transport.cpp
    src/janus/janus_client.cpp
)

target_include_directories(xrtc
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${WEBRTC_INCLUDE}/third_party/abseil-cpp
        ${WEBRTC_INCLUDE}/third_party/libyuv/include
        ${WEBRTC_INCLUDE}
)

target_link_directories(xrtc PUBLIC ${WETC_LIB})

target_link_libraries(xrtc
    PUBLIC
        webrtc
        utils::utils
    PRIVATE
        nlohmann_json
        Boost::headers
        ws2_32
        winmm
        strmiids
        msdmo
        dmoguids
        wmcodecdspuuid
        iphlpapi
        secur32
        crypt32
)

target_compile_definitions(xrtc
    PUBLIC
        WEBRTC_WIN
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        $<$<CONFIG:Debug>:_ITERATOR_DEBUG_LEVEL=0>
        $<$<CONFIG:Debug>:_HAS_ITERATOR_DEBUGGING=0>
    PRIVATE
        # Beast/Asio 为 header-only，禁止 MSVC 自动链接 MinGW 版 Boost 二进制
        BOOST_ALL_NO_LIB
)

if(MSVC)
    target_compile_options(xrtc PRIVATE /utf-8 /bigobj)
    # Boost.Asio 需要明确的 Windows 目标版本
    target_compile_definitions(xrtc PRIVATE _WIN32_WINNT=0x0A00)
endif()

# ---------- Qt Demo ----------
qt_standard_project_setup()

qt_add_executable(webrtc_test
    WIN32 MACOSX_BUNDLE
    demo/main.cpp
    demo/widget.cpp
    demo/widget.h
    demo/widget.ui
)

target_link_libraries(webrtc_test
    PRIVATE
        xrtc
        spdlog_header
        Qt::Core
        Qt::Widgets
)

install(TARGETS webrtc_test
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET webrtc_test
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
