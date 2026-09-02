#pragma once

#include <cstdint>
#include <string>

#include <xrtc/ixrtc_engine.h>

namespace xrtc {

/// CallSession 生命周期决策（纯逻辑，可供单测）
struct SessionLifecycle {
    bool active = false;
    bool join_notified = false;
    bool tearing_down = false;
    uint64_t generation = 0;

    enum class Event {
        kStartOk,
        kUserStop,
        kFailJoin,
        kSignalingError,
        kQuietDestroy,  // WS 安静关闭 / destroyed
        kIceFailed,
    };

    struct Action {
        bool bump_generation = false;
        bool set_inactive = false;
        bool disconnect = false;
        bool cleanup_media = false;
        bool notify_join_fail = false;
        bool notify_leave = false;
        bool ignore = false;  // 重复/重入应忽略
        XRtcError leave_or_join_error = XRtcError::kNOERROR;
        std::string message;
    };

    Action On(Event event, const std::string& detail = {}) {
        Action a;
        switch (event) {
            case Event::kStartOk:
                if (active) {
                    a.ignore = true;
                    a.notify_join_fail = true;
                    a.leave_or_join_error = XRtcError::kAlreadyInCall;
                    a.message = "already in call";
                    return a;
                }
                join_notified = false;
                ++generation;
                active = true;
                a.bump_generation = true;
                return a;

            case Event::kUserStop: {
                if (tearing_down) {
                    a.ignore = true;
                    return a;
                }
                const bool was = active;
                tearing_down = true;
                active = false;
                ++generation;
                a.bump_generation = true;
                a.set_inactive = true;
                a.disconnect = true;
                a.cleanup_media = true;
                a.notify_leave = was;
                a.leave_or_join_error = XRtcError::kNOERROR;
                a.message = detail.empty() ? "left" : detail;
                return a;
            }

            case Event::kFailJoin: {
                if (tearing_down) {
                    a.ignore = true;
                    return a;
                }
                tearing_down = true;
                active = false;
                ++generation;
                a.bump_generation = true;
                a.set_inactive = true;
                a.disconnect = true;
                a.cleanup_media = true;
                a.notify_join_fail = !join_notified;
                a.leave_or_join_error =
                    detail.empty() ? XRtcError::kMediaStartFailed
                                   : XRtcError::kPeerConnectionFailed;
                a.message = detail.empty() ? "fail join" : detail;
                return a;
            }

            case Event::kSignalingError: {
                if (tearing_down) {
                    a.ignore = true;
                    return a;
                }
                const bool was_joined = join_notified;
                const bool was_active = active;
                tearing_down = true;
                active = false;
                ++generation;
                a.bump_generation = true;
                a.set_inactive = true;
                a.disconnect = true;
                a.cleanup_media = true;
                if (was_joined) {
                    a.notify_leave = was_active || was_joined;
                    a.leave_or_join_error = XRtcError::kSignalingFailed;
                } else {
                    a.notify_join_fail = true;
                    a.leave_or_join_error = XRtcError::kSignalingFailed;
                }
                a.message = detail.empty() ? "signaling failed" : detail;
                return a;
            }

            case Event::kQuietDestroy: {
                // 本端 Stop/failJoin 触发的 Disconnect 重入
                if (tearing_down) {
                    a.ignore = true;
                    return a;
                }
                if (!active) {
                    // 已 inactive 但仍可能残留媒体：补清理，不重复通知
                    a.cleanup_media = true;
                    a.ignore = true;
                    return a;
                }
                const bool was_joined = join_notified;
                tearing_down = true;
                active = false;
                ++generation;
                a.bump_generation = true;
                a.set_inactive = true;
                a.cleanup_media = true;
                // Disconnect 已发生，不必再 disconnect
                if (was_joined) {
                    a.notify_leave = true;
                    a.leave_or_join_error = XRtcError::kSignalingFailed;
                    a.message = detail.empty() ? "connection closed" : detail;
                } else {
                    a.notify_join_fail = true;
                    a.leave_or_join_error = XRtcError::kSignalingFailed;
                    a.message =
                        detail.empty() ? "connection closed before join" : detail;
                }
                return a;
            }

            case Event::kIceFailed: {
                if (tearing_down || !active) {
                    a.ignore = true;
                    return a;
                }
                if (!join_notified) {
                    // 尚未对外宣称进房成功：按 failJoin
                    return On(Event::kFailJoin,
                              detail.empty() ? "ice/pc failed" : detail);
                }
                tearing_down = true;
                active = false;
                ++generation;
                a.bump_generation = true;
                a.set_inactive = true;
                a.disconnect = true;
                a.cleanup_media = true;
                a.notify_leave = true;
                a.leave_or_join_error = XRtcError::kPeerConnectionFailed;
                a.message = detail.empty() ? "ice/pc failed" : detail;
                return a;
            }
        }
        a.ignore = true;
        return a;
    }

    void FinishTeardown() { tearing_down = false; }

    void MarkJoinNotified() { join_notified = true; }
};

}  // namespace xrtc
