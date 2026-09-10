#pragma once
// 帧结构 + 线程安全有界队列（生产者-消费者，满时丢最旧帧）
#include <cstdint>
#include <vector>
#include <deque>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <chrono>

// 标准库名字逐个引入(头文件不用 using namespace std，避免污染包含者)
using std::chrono;
using std::condition_variable;
using std::deque;
using std::lock_guard;
using std::move;
using std::mutex;
using std::shared_ptr;
using std::unique_lock;
using std::vector;

enum class PixelFormat { NV12, RGB888, BGR888, GRAY };

// 一帧图像（流水线中只传 shared_ptr，避免深拷贝）
struct Frame {
    uint32_t width = 0;
    uint32_t height = 0;
    PixelFormat fmt = PixelFormat::BGR888;
    uint64_t pts_us = 0;   // 采集时刻（微秒）
    vector<uint8_t> data;
};
using FramePtr = shared_ptr<Frame>;

class FrameQueue {
public:
    explicit FrameQueue(size_t capacity = 4) : cap_(capacity) {}

    bool push(FramePtr f) {
        lock_guard<mutex> lk(m_);
        if (q_.size() >= cap_) q_.pop_front();   // 保实时：丢最旧
        q_.push_back(move(f));
        cv_.notify_one();
        return true;
    }

    bool pop(FramePtr& out, int timeout_ms = 1000) {
        unique_lock<mutex> lk(m_);
        if (!cv_.wait_for(lk, chrono::milliseconds(timeout_ms),
                          [this] { return !q_.empty(); }))
            return false;
        out = move(q_.front());
        q_.pop_front();
        return true;
    }

    size_t size() const { lock_guard<mutex> lk(m_); return q_.size(); }
    void clear() { lock_guard<mutex> lk(m_); q_.clear(); }

private:
    size_t cap_;
    deque<FramePtr> q_;
    mutable mutex m_;
    condition_variable cv_;
};
