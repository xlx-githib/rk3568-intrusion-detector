#pragma once
// 帧结构 + 线程安全有界队列（生产者-消费者，满时丢最旧帧）
#include <cstdint>
#include <vector>
#include <deque>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <chrono>

enum class PixelFormat { NV12, RGB888, BGR888, GRAY };

// 一帧图像（流水线中只传 shared_ptr，避免深拷贝）
struct Frame {
    uint32_t width = 0;
    uint32_t height = 0;
    PixelFormat fmt = PixelFormat::BGR888;
    uint64_t pts_us = 0;   // 采集时刻（微秒）
    std::vector<uint8_t> data;
};
using FramePtr = std::shared_ptr<Frame>;

class FrameQueue {
public:
    explicit FrameQueue(size_t capacity = 4) : cap_(capacity) {}

    bool push(FramePtr f) {
        std::lock_guard<std::mutex> lk(m_);
        if (q_.size() >= cap_) q_.pop_front();   // 保实时：丢最旧
        q_.push_back(std::move(f));
        cv_.notify_one();
        return true;
    }

    bool pop(FramePtr& out, int timeout_ms = 1000) {
        std::unique_lock<std::mutex> lk(m_);
        if (!cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                          [this] { return !q_.empty(); }))
            return false;
        out = std::move(q_.front());
        q_.pop_front();
        return true;
    }

    size_t size() const { std::lock_guard<std::mutex> lk(m_); return q_.size(); }
    void clear() { std::lock_guard<std::mutex> lk(m_); q_.clear(); }

private:
    size_t cap_;
    std::deque<FramePtr> q_;
    mutable std::mutex m_;
    std::condition_variable cv_;
};
