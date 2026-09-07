#pragma once
// 通用线程安全有界队列（M2-3 流水线用）
// 满时丢最旧帧/项(保实时)；pop 支持超时
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstddef>
#include <mutex>
#include <utility>

template <typename T>
class BlockQueue {
public:
    explicit BlockQueue(size_t cap = 4) : cap_(cap) {}

    void push(T v) {
        std::lock_guard<std::mutex> lk(m_);
        if (q_.size() >= cap_) q_.pop_front();   // 满则丢最旧
        q_.push_back(std::move(v));
        cv_.notify_one();
    }

    bool pop(T& out, int timeout_ms = 1000) {
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
    std::deque<T> q_;
    mutable std::mutex m_;
    std::condition_variable cv_;
};
