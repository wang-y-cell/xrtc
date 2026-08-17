#pragma once

/// SDK 日志统一走 utils::log（Demo 可 set_backend 接到 spdlog）
#include "adapter/log/log.h"

namespace xrtc {
// 勿在全局做 namespace log = ...：会与 <cmath> 的 ::log 冲突（MSVC C2386）
namespace log = utils::log;
}  // namespace xrtc
