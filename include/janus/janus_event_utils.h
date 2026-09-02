#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace xrtc {

/// 解析 VideoRoom event 中的 unpublished / leaving。
/// - 本端 unpublish 成功：`"unpublished":"ok"` → nullopt（应忽略）
/// - 他人停推/离开：数字 feed id → 该 id
inline std::optional<uint64_t> ParsePublisherLeftFeed(
    const nlohmann::json& data) {
    auto parse_field = [](const nlohmann::json& v) -> std::optional<uint64_t> {
        if (v.is_string()) {
            const auto& s = v.get_ref<const std::string&>();
            if (s == "ok" || s.empty()) {
                return std::nullopt;
            }
            try {
                return static_cast<uint64_t>(std::stoull(s));
            } catch (...) {
                return std::nullopt;
            }
        }
        if (v.is_number_unsigned() || v.is_number_integer()) {
            const auto id = v.get<uint64_t>();
            return id == 0 ? std::nullopt : std::optional<uint64_t>(id);
        }
        return std::nullopt;
    };

    if (data.contains("unpublished")) {
        return parse_field(data.at("unpublished"));
    }
    if (data.contains("leaving")) {
        return parse_field(data.at("leaving"));
    }
    return std::nullopt;
}

}  // namespace xrtc
