# Qt Demo：仅在父工程 -DBUILD_XRTC_DEMO=ON 时 add_subdirectory。
if(NOT QT6_ROOT)
    message(FATAL_ERROR
        "BUILD_XRTC_DEMO=ON 时必须指定 QT6_ROOT。"
        "示例: cmake -DBUILD_XRTC_DEMO=ON -DQT6_ROOT=F:/Qt/6.8.3/msvc2022_64")
endif()

if(POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW)
endif()
# find_package(Qt6) 认 Qt6_ROOT，不是 QT6_ROOT
set(Qt6_ROOT "${QT6_ROOT}")
list(APPEND CMAKE_PREFIX_PATH "${QT6_ROOT}")
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets OpenGL OpenGLWidgets)
message(STATUS "Qt6 ${Qt6_VERSION} for demo: ${QT6_ROOT}")

qt_standard_project_setup()

qt_add_executable(webrtc_test
    #WIN32
    MACOSX_BUNDLE
    main.cpp
    widget.cpp
    widget.h
    widget.ui
    yuv_gl_widget.cpp
    yuv_gl_widget.h
)

target_link_libraries(webrtc_test
    PRIVATE
        xrtc
        ${_XRTC_SPDLOG_LIB}
        Qt::Core
        Qt::Widgets
        Qt::OpenGL
        Qt::OpenGLWidgets
)

target_include_directories(webrtc_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
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
