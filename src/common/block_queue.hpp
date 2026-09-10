#pragma once
// 通用线程安全有界队列（M2-3 流水线用）
// 满时丢最旧帧/项(保实时)；pop 支持超时
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstddef>
#include <mutex>
#include <utility>

// 标准库名字逐个引入(头文件不用 using namespace std，避免污染包含者)
using std::chrono;
using std::condition_variable;
using std::deque;
using std::lock_guard;
using std::move;
using std::mutex;
using std::unique_lock;

template <typename T>
class BlockQueue {
public:
    explicit BlockQueue(size_t cap = 4) : cap_(cap) {}

    void push(T v) {
        lock_guard<mutex> lk(m_);
        if (q_.size() >= cap_) q_.pop_front();   // 满则丢最旧
        q_.push_back(move(v));
        cv_.notify_one();
    }

    bool pop(T& out, int timeout_ms = 1000) {
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
    size_t cap_;// 队列容量上限
    deque<T> q_;// 底层容器（双端队列）
    mutable mutex m_;// 互斥锁，mutable 允许在 const 函数中加锁
    condition_variable cv_;// 条件变量，用于阻塞和唤醒线程
};
