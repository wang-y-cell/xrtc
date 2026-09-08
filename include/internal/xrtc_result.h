#pragma once

#include <string>
#include <string_view>
#include <utility>

#include "reliability/expected.h"
#include <xrtc/xrtc_defines.h>

namespace xrtc {

/// 统一可失败返回：utils::result<T, XRtcError>；无值操作用 Rest<> / Rest<void>
template <class T = void>
using Rest = utils::result<T, XRtcError>;

template <class T>
using XRtcResult = Rest<T>;

using XRtcStatus = Rest<>;

inline Rest<> xrtc_ok() { return {}; }

template <class T>
inline Rest<T> xrtc_ok(T&& value) {
    return Rest<T>(std::forward<T>(value));
}

/// 失败：用 unexpected(E)，勿用 utils::err(枚举)（会变成 error_info<E>）
inline Rest<> xrtc_err(XRtcError e) {
    return Rest<>(utils::unexpect, e);
}

template <class T>
inline Rest<T> xrtc_err_t(XRtcError e) {
    return Rest<T>(utils::unexpect, e);
}

inline std::string_view XRtcErrorToString(XRtcError error) {
    switch (error) {
        case XRtcError::kNOERROR:
            return "ok";
        case XRtcError::kVideoSourceNotInit:
            return "video_source_not_init";
        case XRtcError::kVideoSourceStartFailed:
            return "video_source_start_failed";
        case XRtcError::kAlreadyInCall:
            return "already_in_call";
        case XRtcError::kNotInCall:
            return "not_in_call";
        case XRtcError::kInvalidParam:
            return "invalid_param";
        case XRtcError::kSignalingFailed:
            return "signaling_failed";
        case XRtcError::kPeerConnectionFailed:
            return "peer_connection_failed";
        case XRtcError::kMediaStartFailed:
            return "media_start_failed";
    }
    return "unknown";
}

}  // namespace xrtc
