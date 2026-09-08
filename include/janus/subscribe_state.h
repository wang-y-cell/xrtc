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
    /// 开始订阅：已订阅或 attach 中返回 false,检查是否存在重复订阅
    /// 没有重复订阅就返回true,否则返回false
    bool TryBeginSubscribe(uint64_t feed_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        // 如果已经订阅或正在attach中，则返回false
        if (feed_to_handle_.count(feed_id) || pending_.count(feed_id)) {
            return false;
        }
        // 没有重复订阅,这是第一次订阅, 将feed_id加入到等待attach的集合中
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

    /// 通过房间插件的handle_id获得对应feed_id,这个feed可能是我们作为发布者加入房间的,也可能是我们作为订阅者加入房间的对端feed
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
    std::unordered_set<uint64_t> pending_; // 等待attach的feed_id集合
    std::unordered_map<uint64_t, uint64_t> feed_to_handle_; // feed_id到handle_id的映射
};

}  // namespace xrtc
