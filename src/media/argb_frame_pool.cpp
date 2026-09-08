#include <media/argb_frame_pool.h>

namespace xrtc {
namespace {

/// 常见预览档（升序）；更大分辨率走 heap fallback
constexpr std::size_t kBucketSizes[] = {
    640u * 480u * 4u,    // ~1.2 MiB
    1280u * 720u * 4u,   // ~3.5 MiB
    1920u * 1080u * 4u,  // ~8 MiB
};

}  // namespace

ArgbFramePool& ArgbFramePool::Instance() {
    static ArgbFramePool pool;
    return pool;
}

ArgbFramePool::ArgbFramePool() {
    buckets_.reserve(sizeof(kBucketSizes) / sizeof(kBucketSizes[0]));
    for (std::size_t sz : kBucketSizes) {
        Bucket bucket;
        bucket.block_size = sz;
        // 帧块很大：每 chunk 少放几块，避免一次申请过多
        bucket.pool = std::make_unique<utils::memory_pool>(sz, 2);
        buckets_.push_back(std::move(bucket));
    }
}

std::shared_ptr<uint8_t> ArgbFramePool::Acquire(std::size_t bytes) {
    if (bytes == 0) {
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (Bucket& bucket : buckets_) {
            if (bucket.block_size < bytes || !bucket.pool) {
                continue;
            }
            void* raw = bucket.pool->allocate();
            const std::size_t block_size = bucket.block_size;
            return std::shared_ptr<uint8_t>(
                static_cast<uint8_t*>(raw),
                [this, block_size](uint8_t* p) {
                    if (!p) {
                        return;
                    }
                    std::lock_guard<std::mutex> lock(mutex_);
                    for (Bucket& bucket : buckets_) {
                        if (bucket.block_size == block_size && bucket.pool) {
                            bucket.pool->deallocate(p);
                            return;
                        }
                    }
                });
        }
    }

    // 超过最大档：普通堆，仍统一成 shared_ptr<uint8_t>
    return std::shared_ptr<uint8_t>(new uint8_t[bytes],
                                    [](uint8_t* p) { delete[] p; });
}

}  // namespace xrtc
