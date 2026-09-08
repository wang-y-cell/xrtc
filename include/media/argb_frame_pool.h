#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#define ARGB_FRAME_POOL_ENABLED 0
#include "memory/memory_pool.h"

namespace xrtc {

/// 预览 ARGB 帧缓冲池：按常见分辨率分档，底层用 utils::memory_pool。
/// 采集/渲染线程交叉持有 shared_ptr，故分配/归还加互斥（池本身无锁）。
class ArgbFramePool {
public:
    static ArgbFramePool& Instance();

    /// 分配至少 bytes 字节；失败抛 bad_alloc。归还时走自定义 deleter。
    [[nodiscard]] std::shared_ptr<uint8_t> Acquire(std::size_t bytes);

    ArgbFramePool(const ArgbFramePool&) = delete;
    ArgbFramePool& operator=(const ArgbFramePool&) = delete;

private:
    ArgbFramePool();

    struct Bucket {
        std::size_t block_size = 0;
        /// memory_pool 不可移动，用 unique_ptr 放入 vector
        std::unique_ptr<utils::memory_pool> pool;
    };

    std::mutex mutex_;
    std::vector<Bucket> buckets_;
};

/// 分配 width*height*4 的 ARGB 缓冲（供 XRTCVideoFrame::argb）
[[nodiscard]] inline std::shared_ptr<uint8_t> AcquireArgbBuffer(int width,
                                                                int height) {
    if (width <= 0 || height <= 0) {
        return nullptr;
    }
    const std::size_t bytes =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    return ArgbFramePool::Instance().Acquire(bytes);
}

}  // namespace xrtc
