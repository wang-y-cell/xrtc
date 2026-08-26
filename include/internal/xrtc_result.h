#pragma once

#include <string>
#include <string_view>

#include "reliability/result/expected.h"
#include <xrtc/ixrtc_engine.h>

namespace xrtc {

template <class T>
using XRtcResult = utils::expected<T, XRtcError>;

using XRtcStatus = utils::expected<void, XRtcError>;

inline XRtcStatus xrtc_ok() { return utils::ok<XRtcError>(); }

inline auto xrtc_err(XRtcError e) { return utils::err(e); }

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
