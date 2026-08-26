cmake_minimum_required(VERSION 3.19)
project(webrtc_test LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_BUILD_TYPE Debug)

# WebRTC 预编译 SDK
# 布局：webrtc/include + webrtc/lib_debug/webrtc.lib + webrtc/lib_release/webrtc.lib
# 缺失时从 GitHub Release 自动下载 webrtc.7z
set(WEBRTC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/webrtc" CACHE PATH
    "WebRTC SDK root (expect include/ + lib_debug/ + lib_release/)")
set(WEBRTC_RELEASE_URL
    "https://github.com/wang-y-cell/xrtc/releases/download/v1.0.0/webrtc.7z"
    CACHE STRING "GitHub Release URL for webrtc.7z")
set(WEBRTC_INCLUDE "${WEBRTC_ROOT}/include")
set(WEBRTC_LIB_DEBUG_DIR "${WEBRTC_ROOT}/lib_debug")
set(WEBRTC_LIB_RELEASE_DIR "${WEBRTC_ROOT}/lib_release")

function(xrtc_ensure_webrtc_sdk)
    if(EXISTS "${WEBRTC_INCLUDE}/api/peer_connection_interface.h"
       AND EXISTS "${WEBRTC_LIB_DEBUG_DIR}/webrtc.lib"
       AND EXISTS "${WEBRTC_LIB_RELEASE_DIR}/webrtc.lib")
        return()
    endif()

    message(STATUS "WebRTC SDK missing under ${WEBRTC_ROOT}, downloading...")
    set(_archive "${CMAKE_BINARY_DIR}/webrtc.7z")
    file(DOWNLOAD "${WEBRTC_RELEASE_URL}" "${_archive}"
        SHOW_PROGRESS
        STATUS _dl_status
        TLS_VERIFY ON)
    list(GET _dl_status 0 _dl_code)
    if(NOT _dl_code EQUAL 0)
        list(GET _dl_status 1 _dl_msg)
        message(FATAL_ERROR
            "Failed to download WebRTC SDK from ${WEBRTC_RELEASE_URL}: ${_dl_msg}")
    endif()

    # webrtc.7z 顶层目录为 webrtc/，解压到源码根目录
    file(ARCHIVE_EXTRACT INPUT "${_archive}"
        DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}")
    file(REMOVE "${_archive}")

    if(NOT EXISTS "${WEBRTC_INCLUDE}/api/peer_connection_interface.h"
       OR NOT EXISTS "${WEBRTC_LIB_DEBUG_DIR}/webrtc.lib"
       OR NOT EXISTS "${WEBRTC_LIB_RELEASE_DIR}/webrtc.lib")
        message(FATAL_ERROR
            "Downloaded archive did not produce expected layout under ${WEBRTC_ROOT}. "
            "Expect webrtc/include, webrtc/lib_debug/webrtc.lib, "
            "webrtc/lib_release/webrtc.lib inside the .7z.")
    endif()
    message(STATUS "WebRTC SDK ready: ${WEBRTC_ROOT}")
endfunction()

xrtc_ensure_webrtc_sdk()

# 按配置选择 Debug(/MDd) 或 Release(/MD) 的 webrtc.lib
add_library(webrtc STATIC IMPORTED GLOBAL)
set_target_properties(webrtc PROPERTIES
    IMPORTED_LOCATION_DEBUG "${WEBRTC_LIB_DEBUG_DIR}/webrtc.lib"
    IMPORTED_LOCATION_RELEASE "${WEBRTC_LIB_RELEASE_DIR}/webrtc.lib"
    IMPORTED_LOCATION_RELWITHDEBINFO "${WEBRTC_LIB_RELEASE_DIR}/webrtc.lib"
    IMPORTED_LOCATION_MINSIZEREL "${WEBRTC_LIB_RELEASE_DIR}/webrtc.lib"
)
# Ninja 等单配置生成器使用 IMPORTED_LOCATION
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_property(TARGET webrtc PROPERTY IMPORTED_LOCATION
        "${WEBRTC_LIB_DEBUG_DIR}/webrtc.lib")
else()
    set_property(TARGET webrtc PROPERTY IMPORTED_LOCATION
        "${WEBRTC_LIB_RELEASE_DIR}/webrtc.lib")
endif()
get_target_property(_webrtc_lib webrtc IMPORTED_LOCATION)
message(STATUS "WebRTC lib (${CMAKE_BUILD_TYPE}): ${_webrtc_lib}")

# ---------- third_party: nlohmann/json (header-only) ----------
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/single_include)

# ---------- third_party: spdlog ----------
if(TARGET spdlog::spdlog)
    set(_XRTC_SPDLOG_LIB spdlog::spdlog)
    set(_XRTC_SPDLOG_DEF SPDLOG_COMPILED_LIB)
else()
    add_library(spdlog_header INTERFACE)
    target_include_directories(spdlog_header INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/spdlog/include)
    set(_XRTC_SPDLOG_LIB spdlog_header)
endif()

# ---------- utils framework (header-only: signal / result / concurrency) ----------
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

# ---------- xrtc SDK（对外仅暴露 include/xrtc，第三方对链接方彻底隔离）----------
# 公开头：include/xrtc/*.h（无 WebRTC / utils / json / spdlog / Boost）
# 内部头：include/{engine,janus,...}/*.h（PRIVATE include，链接方不可见）
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
        ${CMAKE_CURRENT_SOURCE_DIR}/include/xrtc
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${WEBRTC_INCLUDE}/third_party/abseil-cpp
        ${WEBRTC_INCLUDE}/third_party/libyuv/include
        ${WEBRTC_INCLUDE}
)

target_link_libraries(xrtc
    PUBLIC
        webrtc
    PRIVATE
        utils::utils
        ${_XRTC_SPDLOG_LIB}
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
        # WebRTC 预编译库通常关闭 iterator debugging，Debug/Release 均需匹配
        _ITERATOR_DEBUG_LEVEL=0
        _HAS_ITERATOR_DEBUGGING=0
    PRIVATE
        WEBRTC_WIN
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        $<$<BOOL:${_XRTC_SPDLOG_DEF}>:${_XRTC_SPDLOG_DEF}>
        # Beast/Asio 为 header-only，禁止 MSVC 自动链接 MinGW 版 Boost 二进制
        BOOST_ALL_NO_LIB
)

if(MSVC)
    target_compile_options(xrtc PRIVATE /utf-8 /bigobj)
    target_compile_definitions(xrtc PRIVATE _WIN32_WINNT=0x0A00)
endif()

# ---------- Qt Demo（可选） ----------
option(BUILD_XRTC_DEMO "Build webrtc_test Qt demo" OFF)

if(BUILD_XRTC_DEMO)
# Qt 路径由外部传入：cmake -DQT6_ROOT=... 或 python build.py --demo --qt_lib=...
if(NOT QT6_ROOT)
    message(FATAL_ERROR
        "BUILD_XRTC_DEMO=ON 时必须指定 QT6_ROOT。"
        "示例: cmake -DQT6_ROOT=F:/Qt/6.8.3/msvc2022_64 "
        "或: python build.py --demo --qt_lib=F:/Qt/6.8.3/msvc2022_64")
endif()
list(APPEND CMAKE_PREFIX_PATH "${QT6_ROOT}")
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets)
message(STATUS "Qt6 ${Qt6_VERSION} for demo: ${QT6_ROOT}")

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
        ${_XRTC_SPDLOG_LIB}
        Qt::Core
        Qt::Widgets
)

target_include_directories(webrtc_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(MSVC)
    target_compile_definitions(webrtc_test PRIVATE
        _ITERATOR_DEBUG_LEVEL=0
        _HAS_ITERATOR_DEBUGGING=0)
endif()

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
endif()
