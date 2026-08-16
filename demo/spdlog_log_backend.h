#pragma once

#include <memory>

#include <spdlog/spdlog.h>

#include "adapter/log/log.h"

namespace xrtc {

/// 把 utils::log 接到现有 spdlog default logger（console + file）
class SpdlogLogBackend : public utils::log::backend {
public:
    void log(utils::log::level lv, std::string_view message) override {
        auto logger = spdlog::default_logger();
        if (!logger) {
            return;
        }
        switch (lv) {
            case utils::log::level::trace:
                logger->trace("{}", message);
                break;
            case utils::log::level::debug:
                logger->debug("{}", message);
                break;
            case utils::log::level::info:
                logger->info("{}", message);
                break;
            case utils::log::level::warn:
                logger->warn("{}", message);
                break;
            case utils::log::level::error:
                logger->error("{}", message);
                break;
            case utils::log::level::off:
                break;
        }
    }
};

inline void InitXrtcLoggingWithSpdlog() {
    utils::log::set_backend(std::make_shared<SpdlogLogBackend>());
    utils::log::set_level(utils::log::level::trace);
}

}  // namespace xrtc
