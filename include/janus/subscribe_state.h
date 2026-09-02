#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace xrtc {

/// Janus 订阅去重状态（可单测）：pending attach / feed→handle 映射
class SubscribeState {
public:
    /// 开始订阅：已订阅或 attach 中返回 false
    bool TryBeginSubscribe(uint64_t feed_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (feed_to_handle_.count(feed_id) || pending_.count(feed_id)) {
            return false;
        }
        pending_.insert(feed_id);
        return true;
    }

    void OnAttachSuccess(uint64_t feed_id, uint64_t handle_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(feed_id);
        if (feed_id != 0 && handle_id != 0) {
            feed_to_handle_[feed_id] = handle_id;
        }
    }

    /// attach 失败 / timeout：允许后续重试
    void OnAttachFailure(uint64_t feed_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(feed_id);
    }

    /// 远端离开：返回被移除的 handle（若有）
    std::optional<uint64_t> OnFeedLeft(uint64_t feed_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(feed_id);
        auto it = feed_to_handle_.find(feed_id);
        if (it == feed_to_handle_.end()) {
            return std::nullopt;
        }
        const uint64_t handle = it->second;
        feed_to_handle_.erase(it);
        return handle;
    }

    std::optional<uint64_t> HandleForFeed(uint64_t feed_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = feed_to_handle_.find(feed_id);
        if (it == feed_to_handle_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    std::optional<uint64_t> FeedForHandle(uint64_t handle_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& kv : feed_to_handle_) {
            if (kv.second == handle_id) {
                return kv.first;
            }
        }
        return std::nullopt;
    }

    bool IsPending(uint64_t feed_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_.count(feed_id) > 0;
    }

    bool IsSubscribed(uint64_t feed_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return feed_to_handle_.count(feed_id) > 0;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.clear();
        feed_to_handle_.clear();
    }

    size_t pending_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_.size();
    }

    size_t subscribed_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return feed_to_handle_.size();
    }

private:
    mutable std::mutex mutex_;
    std::unordered_set<uint64_t> pending_;
    std::unordered_map<uint64_t, uint64_t> feed_to_handle_;
};

}  // namespace xrtc
