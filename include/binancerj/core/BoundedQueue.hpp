#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

namespace binancerj::core {

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity)
        : capacity_(capacity ? capacity : 1) {}

    bool tryPush(T value) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (closed_ || queue_.size() >= capacity_) {
            return false;
        }
        queue_.push_back(std::move(value));
        cvPop_.notify_one();
        return true;
    }

    template <typename Rep, typename Period>
    bool waitPush(T value, const std::chrono::duration<Rep, Period>& timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!waitForSpace(lock, timeout)) {
            return false;
        }
        queue_.push_back(std::move(value));
        cvPop_.notify_one();
        return true;
    }

    bool pop(T& out) {
        std::unique_lock<std::mutex> lock(mutex_);
        cvPop_.wait(lock, [&] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) {
            return false;
        }
        out = std::move(queue_.front());
        queue_.pop_front();
        cvPush_.notify_one();
        return true;
    }

    template <typename Rep, typename Period>
    bool waitPop(T& out, const std::chrono::duration<Rep, Period>& timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cvPop_.wait_for(lock, timeout, [&] { return closed_ || !queue_.empty(); })) {
            return false;
        }
        if (queue_.empty()) {
            return false;
        }
        out = std::move(queue_.front());
        queue_.pop_front();
        cvPush_.notify_one();
        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cvPop_.notify_all();
        cvPush_.notify_all();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

private:
    template <typename Rep, typename Period>
    bool waitForSpace(std::unique_lock<std::mutex>& lock, const std::chrono::duration<Rep, Period>& timeout) {
        if (closed_) {
            return false;
        }
        if (queue_.size() < capacity_) {
            return true;
        }
        if (!cvPush_.wait_for(lock, timeout, [&] { return closed_ || queue_.size() < capacity_; })) {
            return false;
        }
        return !closed_;
    }

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable cvPush_;
    std::condition_variable cvPop_;
    std::deque<T> queue_;
    bool closed_{false};
};

} // namespace binancerj::core