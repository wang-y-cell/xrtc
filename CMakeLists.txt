cmake_minimum_required(VERSION 3.19)
project(xtrc LANGUAGES C CXX)

if(POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW)
endif()

# webrtc.lib / Qt msvc2022_64 都是 MSVC ABI，不能用 MinGW
if(WIN32 AND NOT MSVC)
    message(FATAL_ERROR
        "本工程必须用 MSVC x64（cl.exe），当前编译器是 "
        "${CMAKE_CXX_COMPILER} (${CMAKE_CXX_COMPILER_ID}).\n"
        "请在 x64 Native Tools Command Prompt 或先执行 vcvars64.bat，"
        "删掉构建目录后重新配置，并指定:\n"
        "  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl\n"
        "不要使用 MinGW（PATH 里的 F:/mingw/.../c++.exe 会被 CMake 误选）。")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# 勿强制 Debug：作为子工程时须沿用父工程的 CMAKE_BUILD_TYPE（Release/Debug）
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    endif()
endif()

set(XRTC_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# 配置阶段自动 git submodule update（spdlog / google_test）
function(xrtc_init_git_submodules)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.gitmodules")
        return()
    endif()
    find_package(Git QUIET)
    if(NOT GIT_FOUND)
        message(WARNING
            "未找到 Git，无法自动拉取 submodule。请先安装 Git 后重新配置，"
            "或手动执行: git submodule update --init --recursive")
        return()
    endif()
    message(STATUS "Updating git submodules...")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" submodule update --init --recursive
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE _git_sub_rc)
    if(NOT _git_sub_rc EQUAL 0)
        message(FATAL_ERROR
            "git submodule update failed (code ${_git_sub_rc}). "
            "请确认 Git 能访问 GitHub（必要时为 git 配置代理），或手动执行:\n"
            "  git submodule update --init --recursive")
    endif()
endfunction()
xrtc_init_git_submodules()

# 下载 GitHub Release 压缩包并解压到 dest（marker 已存在则跳过）
function(xrtc_fetch_archive)
    # URL: 下载地址
    # ARCHIVE: 压缩包名
    # DEST: 解压目录
    # MARKER: 标记文件,表示已经解过了
    set(oneValueArgs URL ARCHIVE DEST MARKER) # 参数名
    # 把实参拆成 FETCH_URL、FETCH_ARCHIVE、FETCH_DEST、FETCH_MARKER。中间两个 "" 表示没有开关参数、没有多值参数,${ARGN}表示传入的参数列表
    cmake_parse_arguments(FETCH "" "${oneValueArgs}" "" ${ARGN})
    if(FETCH_MARKER AND EXISTS "${FETCH_MARKER}") # 如果标记文件存在,则跳过
        return()
    endif()

    message(STATUS "Downloading ${FETCH_URL}")
    set(_archive "${CMAKE_BINARY_DIR}/${FETCH_ARCHIVE}")
    file(DOWNLOAD "${FETCH_URL}" "${_archive}"
        SHOW_PROGRESS # 显示进度
        STATUS _dl_status # 结果放入列表
        TLS_VERIFY ON # 验证证书
        TIMEOUT 1800 # 超时时间,秒
        INACTIVITY_TIMEOUT 120) # 连续 120 秒没数据算失败
    list(GET _dl_status 0 _dl_code) # 取出失败码
    if(NOT _dl_code EQUAL 0)
        list(GET _dl_status 1 _dl_msg)
        message(FATAL_ERROR
            "Failed to download ${FETCH_URL}: ${_dl_msg}")
    endif()

    file(MAKE_DIRECTORY "${FETCH_DEST}") # 解压目录如果不存在就创建
    file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${FETCH_DEST}") # 解压
    file(REMOVE "${_archive}") # 删除压缩包

    if(FETCH_MARKER AND NOT EXISTS "${FETCH_MARKER}")
        message(FATAL_ERROR
            "Archive extracted to ${FETCH_DEST} but missing ${FETCH_MARKER}")
    endif()
endfunction()

# WebRTC 预编译 SDK：include.7z + debug/release .lib 三个独立压缩包
set(WEBRTC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/webrtc" CACHE PATH
    "WebRTC SDK root (expect include/ + lib_debug/ and/or lib_release/)")
set(WEBRTC_LIB_BASE_URL
    "https://github.com/wang-y-cell/xrtc/releases/download/v1.0.0"
    CACHE STRING "GitHub Release base URL for WebRTC archives")
set(WEBRTC_INCLUDE_URL
    "${WEBRTC_LIB_BASE_URL}/webrtc_include.7z"
    CACHE STRING "WebRTC headers 7z")
set(WEBRTC_LIB_DEBUG_URL
    "${WEBRTC_LIB_BASE_URL}/webrtc_win_msvc2022_x64_debug.7z"
    CACHE STRING "Debug webrtc.lib 7z")
set(WEBRTC_LIB_RELEASE_URL
    "${WEBRTC_LIB_BASE_URL}/webrtc_win_msvc2022_x64_release.7z"
    CACHE STRING "Release webrtc.lib 7z")
set(WEBRTC_INCLUDE "${WEBRTC_ROOT}/include")
set(WEBRTC_LIB_DEBUG_DIR "${WEBRTC_ROOT}/lib_debug")
set(WEBRTC_LIB_RELEASE_DIR "${WEBRTC_ROOT}/lib_release")

function(xrtc_find_webrtc_header out_var search_root)
    foreach(_hdr
        "${search_root}/api/peer_connection_interface.h"
        "${search_root}/include/api/peer_connection_interface.h"
        "${search_root}/webrtc/include/api/peer_connection_interface.h")
        if(EXISTS "${_hdr}")
            set(${out_var} "${_hdr}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    file(GLOB_RECURSE _hits
        "${search_root}/*/api/peer_connection_interface.h")
    if(_hits)
        list(GET _hits 0 _first)
        set(${out_var} "${_first}" PARENT_SCOPE)
        return()
    endif()
    set(${out_var} "" PARENT_SCOPE)
endfunction()

function(xrtc_ensure_webrtc_include)
    set(_marker "${WEBRTC_INCLUDE}/api/peer_connection_interface.h")
    if(EXISTS "${_marker}")
        return()
    endif()

    set(_extract "${CMAKE_BINARY_DIR}/_xrtc_webrtc_extract/webrtc_include.7z")
    file(REMOVE_RECURSE "${_extract}") # 如果存在这个目录就删除
    xrtc_fetch_archive(
        URL "${WEBRTC_INCLUDE_URL}"
        ARCHIVE "webrtc_include.7z"
        DEST "${_extract}")

    xrtc_find_webrtc_header(_hdr "${_extract}")
    if(NOT _hdr)
        message(FATAL_ERROR
            "webrtc_include.7z 中没有 api/peer_connection_interface.h")
    endif()

    # _hdr = .../include/api/peer_connection_interface.h → 拷贝 include 目录
    get_filename_component(_api_dir "${_hdr}" DIRECTORY)
    get_filename_component(_include_dir "${_api_dir}" DIRECTORY)
    file(MAKE_DIRECTORY "${WEBRTC_ROOT}")
    if(NOT _include_dir STREQUAL "${WEBRTC_INCLUDE}")
        file(COPY "${_include_dir}/" DESTINATION "${WEBRTC_INCLUDE}")
    endif()
    file(REMOVE_RECURSE "${_extract}")
    message(STATUS "WebRTC include ready: ${WEBRTC_INCLUDE}")
endfunction()

# 把压缩包里的 webrtc.lib 放到 dest_dir
function(xrtc_ensure_webrtc_lib url archive dest_dir)
    set(_lib "${dest_dir}/webrtc.lib")
    if(EXISTS "${_lib}")
        return()
    endif()

    set(_extract "${CMAKE_BINARY_DIR}/_xrtc_webrtc_extract/${archive}")
    file(REMOVE_RECURSE "${_extract}")
    xrtc_fetch_archive(
        URL "${url}"
        ARCHIVE "${archive}"
        DEST "${_extract}")

    set(_picked "")
    if(EXISTS "${_extract}/webrtc.lib")
        set(_picked "${_extract}/webrtc.lib")
    else()
        file(GLOB_RECURSE _libs "${_extract}/*.lib")
        foreach(_f IN LISTS _libs)
            get_filename_component(_n "${_f}" NAME)
            if(_n STREQUAL "webrtc.lib")
                set(_picked "${_f}")
                break()
            endif()
        endforeach()
    endif()
    if(NOT _picked)
        message(FATAL_ERROR
            "${archive} 中没有 webrtc.lib（来源: ${url}）")
    endif()

    file(MAKE_DIRECTORY "${dest_dir}")
    file(COPY "${_picked}" DESTINATION "${dest_dir}")
    file(REMOVE_RECURSE "${_extract}")
    message(STATUS "WebRTC lib ready: ${_lib}")
endfunction()

function(xrtc_ensure_webrtc_sdk)
    xrtc_ensure_webrtc_include()

    set(_need_debug OFF)
    set(_need_release OFF)
    if(CMAKE_CONFIGURATION_TYPES)
        set(_need_debug ON)
        set(_need_release ON)
    elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(_need_debug ON)
    else()
        set(_need_release ON)
    endif()

    if(_need_debug)
        xrtc_ensure_webrtc_lib(
            "${WEBRTC_LIB_DEBUG_URL}"
            "webrtc_win_msvc2022_x64_debug.7z"
            "${WEBRTC_LIB_DEBUG_DIR}")
    endif()
    if(_need_release)
        xrtc_ensure_webrtc_lib(
            "${WEBRTC_LIB_RELEASE_URL}"
            "webrtc_win_msvc2022_x64_release.7z"
            "${WEBRTC_LIB_RELEASE_DIR}")
    endif()

    if(NOT EXISTS "${WEBRTC_INCLUDE}/api/peer_connection_interface.h")
        message(FATAL_ERROR
            "WebRTC 头文件缺失: ${WEBRTC_INCLUDE}/api/peer_connection_interface.h")
    endif()
    if(_need_debug AND NOT EXISTS "${WEBRTC_LIB_DEBUG_DIR}/webrtc.lib")
        message(FATAL_ERROR "缺少 ${WEBRTC_LIB_DEBUG_DIR}/webrtc.lib")
    endif()
    if(_need_release AND NOT EXISTS "${WEBRTC_LIB_RELEASE_DIR}/webrtc.lib")
        message(FATAL_ERROR "缺少 ${WEBRTC_LIB_RELEASE_DIR}/webrtc.lib")
    endif()
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

# ---------- nlohmann/json（Release include.zip，header-only）----------
set(NLOHMANN_JSON_URL
    "https://github.com/nlohmann/json/releases/download/v3.12.0/include.zip"
    CACHE STRING "GitHub Release URL for nlohmann/json include.zip")
set(NLOHMANN_JSON_ROOT "${XRTC_THIRD_PARTY}/json" CACHE PATH
    "nlohmann/json extract root")

function(xrtc_ensure_nlohmann_json)
    set(_markers
        "${NLOHMANN_JSON_ROOT}/single_include/nlohmann/json.hpp"
        "${NLOHMANN_JSON_ROOT}/include/nlohmann/json.hpp"
        "${NLOHMANN_JSON_ROOT}/nlohmann/json.hpp")
    foreach(_m IN LISTS _markers)
        if(EXISTS "${_m}")
            return()
        endif()
    endforeach()

    xrtc_fetch_archive(
        URL "${NLOHMANN_JSON_URL}" # 下载地址
        ARCHIVE "nlohmann_json_include.zip" # 压缩包名
        DEST "${NLOHMANN_JSON_ROOT}" # 解压目录
        MARKER "${NLOHMANN_JSON_ROOT}/include/nlohmann/json.hpp") # 标记文件,表示已经解过了
endfunction()

xrtc_ensure_nlohmann_json()

set(_json_inc "")
if(EXISTS "${NLOHMANN_JSON_ROOT}/single_include/nlohmann/json.hpp")
    set(_json_inc "${NLOHMANN_JSON_ROOT}/single_include")
elseif(EXISTS "${NLOHMANN_JSON_ROOT}/include/nlohmann/json.hpp")
    set(_json_inc "${NLOHMANN_JSON_ROOT}/include")
elseif(EXISTS "${NLOHMANN_JSON_ROOT}/nlohmann/json.hpp")
    set(_json_inc "${NLOHMANN_JSON_ROOT}")
else()
    message(FATAL_ERROR
        "nlohmann/json.hpp not found under ${NLOHMANN_JSON_ROOT}")
endif()

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE "${_json_inc}")
message(STATUS "nlohmann/json include: ${_json_inc}")

# ---------- spdlog（git submodule）----------
if(NOT EXISTS "${XRTC_THIRD_PARTY}/spdlog/include/spdlog/spdlog.h")
    message(FATAL_ERROR
        "spdlog submodule missing: ${XRTC_THIRD_PARTY}/spdlog\n"
        "Run: git submodule update --init --recursive")
endif()
if(TARGET spdlog::spdlog)
    set(_XRTC_SPDLOG_LIB spdlog::spdlog)
    set(_XRTC_SPDLOG_DEF SPDLOG_COMPILED_LIB)
else()
    add_library(spdlog_header INTERFACE)
    target_include_directories(spdlog_header INTERFACE
        ${XRTC_THIRD_PARTY}/spdlog/include)
    set(_XRTC_SPDLOG_LIB spdlog_header)
endif()

# ---------- utils framework（安装式 header-only 包：utils/include + utils/lib/cmake）----------
find_package(Threads REQUIRED)
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/utils")
find_package(utils REQUIRED CONFIG)
message(STATUS "utils found: ${utils_DIR}")

# ---------- Boost（Release 源码包，Beast/Asio 仅需头文件，不跑 b2）----------
set(BOOST_RELEASE_URL
    "https://github.com/boostorg/boost/releases/download/boost-1.92.0/boost-1.92.0-b2-nodocs.7z"
    CACHE STRING "GitHub Release URL for Boost source archive")

function(xrtc_locate_boost_inc out_var)
    set(_roots)
    if(Boost_ROOT)
        list(APPEND _roots "${Boost_ROOT}" "${Boost_ROOT}/include")
    endif()
    list(APPEND _roots
        "${XRTC_THIRD_PARTY}/boost-1.92.0"
        "${XRTC_THIRD_PARTY}/boost_1_92_0"
        "${XRTC_THIRD_PARTY}/boost")
    file(GLOB _boost_ver_files
        "${XRTC_THIRD_PARTY}/*/boost/version.hpp")
    foreach(_f IN LISTS _boost_ver_files)
        get_filename_component(_boost_dir "${_f}" DIRECTORY) # .../boost
        get_filename_component(_root "${_boost_dir}" DIRECTORY)
        list(APPEND _roots "${_root}")
    endforeach()

    foreach(_r IN LISTS _roots)
        if(EXISTS "${_r}/boost/version.hpp")
            set(${out_var} "${_r}" PARENT_SCOPE)
            return()
        endif()
        if(EXISTS "${_r}/include/boost/version.hpp")
            set(${out_var} "${_r}/include" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_var} "" PARENT_SCOPE)
endfunction()

xrtc_locate_boost_inc(_boost_inc)
if(NOT _boost_inc)
    message(STATUS "Boost headers missing, downloading ${BOOST_RELEASE_URL}")
    # 顶层目录可能是 boost-1.92.0 或 boost_1_92_0，解压后再定位
    xrtc_fetch_archive(
        URL "${BOOST_RELEASE_URL}"
        ARCHIVE "boost-1.92.0-b2-nodocs.7z"
        DEST "${XRTC_THIRD_PARTY}")
    xrtc_locate_boost_inc(_boost_inc)
endif()
if(NOT _boost_inc)
    message(FATAL_ERROR
        "Boost headers not found after extract under ${XRTC_THIRD_PARTY}. "
        "Need a directory containing boost/version.hpp. "
        "You may pass -DBoost_ROOT=<path>.")
endif()

if(NOT TARGET Boost::headers)
    add_library(Boost_headers INTERFACE)
    add_library(Boost::headers ALIAS Boost_headers)
    target_include_directories(Boost_headers INTERFACE "${_boost_inc}")
endif()
message(STATUS "Boost headers: ${_boost_inc}")

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
    src/media/audio_capture.cpp
    src/media/xrtc_audio_device_module.cpp
    src/media/video_capability_selector.cpp
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

target_link_libraries(xrtc
    PUBLIC
        webrtc
        utils::utils
    PRIVATE
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
        # Beast/Asio 为 header-only，禁止 MSVC 自动链接 Boost 二进制
        BOOST_ALL_NO_LIB
)

if(MSVC)
    message(STATUS "MSVC ${CMAKE_CXX_COMPILER_VERSION}: ${CMAKE_CXX_COMPILER}")
    target_compile_options(xrtc PRIVATE /utf-8 /bigobj /wd4068)
    target_compile_definitions(xrtc PRIVATE _WIN32_WINNT=0x0A00)
endif()

# ---------- GoogleTest（submodule: third_party/google_test）----------
option(BUILD_XRTC_TESTS "Build xrtc unit tests" ON)
if(BUILD_XRTC_TESTS)
    if(NOT EXISTS "${XRTC_THIRD_PARTY}/google_test/CMakeLists.txt")
        message(FATAL_ERROR
            "GoogleTest submodule missing: ${XRTC_THIRD_PARTY}/google_test\n"
            "Run: git submodule update --init --recursive")
    endif()
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    # googletest 1.8 仍是 cmake_minimum_required(2.8.8)，CMake 4 已拒绝 < 3.5
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING
        "Allow googletest 1.8 cmake_minimum_required < 3.5")
    set(CMAKE_POLICY_DEFAULT_CMP0148 OLD)
    if(POLICY CMP0148)
        cmake_policy(SET CMP0148 OLD)
    endif()
    set(_xrtc_warn_deprecated "${CMAKE_WARN_DEPRECATED}")
    set(CMAKE_WARN_DEPRECATED OFF)
    add_subdirectory(${XRTC_THIRD_PARTY}/google_test EXCLUDE_FROM_ALL)
    set(CMAKE_WARN_DEPRECATED "${_xrtc_warn_deprecated}")
    # googletest 1.8.x 只有 gtest / gtest_main，没有 GTest:: 命名空间
    if(TARGET gtest_main AND NOT TARGET GTest::gtest_main)
        add_library(GTest::gtest_main ALIAS gtest_main)
    endif()
    if(TARGET gtest AND NOT TARGET GTest::gtest)
        add_library(GTest::gtest ALIAS gtest)
    endif()
    # 与 webrtc/xrtc 一致：关闭 iterator debugging，避免 LNK2038
    if(TARGET gtest)
        target_compile_definitions(gtest PUBLIC
            _ITERATOR_DEBUG_LEVEL=0
            _HAS_ITERATOR_DEBUGGING=0)
    endif()
    if(TARGET gtest_main)
        target_compile_definitions(gtest_main PUBLIC
            _ITERATOR_DEBUG_LEVEL=0
            _HAS_ITERATOR_DEBUGGING=0)
    endif()

    enable_testing()
    include(GoogleTest)

    add_executable(xrtc_tests
        tests/janus_event_utils_test.cpp
        tests/websocket_url_test.cpp
        tests/video_select_strategy_test.cpp
        tests/subscribe_state_test.cpp
        tests/session_lifecycle_test.cpp
    )
    target_link_libraries(xrtc_tests
        PRIVATE
            xrtc
            GTest::gtest_main
            nlohmann_json
            ${_XRTC_SPDLOG_LIB}
    )
    target_include_directories(xrtc_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${WEBRTC_INCLUDE}/third_party/abseil-cpp
        ${WEBRTC_INCLUDE}/third_party/libyuv/include
        ${WEBRTC_INCLUDE}
    )
    target_compile_definitions(xrtc_tests PRIVATE
        WEBRTC_WIN
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _ITERATOR_DEBUG_LEVEL=0
        _HAS_ITERATOR_DEBUGGING=0
        BOOST_ALL_NO_LIB
    )
    if(MSVC)
        target_compile_options(xrtc_tests PRIVATE /utf-8 /bigobj /wd4068)
        target_compile_definitions(xrtc_tests PRIVATE _WIN32_WINNT=0x0A00)
    endif()
    gtest_discover_tests(xrtc_tests DISCOVERY_MODE PRE_TEST)
endif()

# ---------- Qt Demo（默认关闭；-DBUILD_XRTC_DEMO=ON 才编译） ----------
option(BUILD_XRTC_DEMO "Build xrtc Qt demo" OFF)
if(BUILD_XRTC_DEMO)
    add_subdirectory(demo)
endif()
