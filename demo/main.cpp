#include "widget.h"

#include <cstdio>
#include <memory>

#include <QApplication>
#include <QSurfaceFormat>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace {

void InitLogging() {
    try {
        auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console->set_level(spdlog::level::info);
        auto file = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            "xrtc_debug.log", true);
        file->set_level(spdlog::level::trace);
        auto logger = std::make_shared<spdlog::logger>(
            "xrtc", spdlog::sinks_init_list{console, file});
        logger->set_level(spdlog::level::trace);
        logger->flush_on(spdlog::level::info);
        logger->set_pattern("[%H:%M:%S.%e] [%l] %v");
        spdlog::set_default_logger(logger);
        spdlog::info("logging ready (console + xrtc_debug.log)");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "spdlog init failed: %s\n", e.what());
    }
}

void InitOpenGLFormat() {
    QSurfaceFormat fmt;
    fmt.setDepthBufferSize(16);
    fmt.setStencilBufferSize(8);
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    QSurfaceFormat::setDefaultFormat(fmt);
}

}  // namespace

int main(int argc, char* argv[]) {
    InitLogging();
    InitOpenGLFormat();

    QApplication a(argc, argv);
    Widget w;
    w.show();
    return QCoreApplication::exec();
}
