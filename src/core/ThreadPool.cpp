#include "binancerj/core/ThreadPool.hpp"
#include "binancerj/telemetry/PerfTelemetry.hpp"

#include <exception>
#include <iostream>

namespace binancerj::core {

ThreadPool::ThreadPool(std::size_t threadCount, std::size_t queueCapacity, std::string name)
    : name_(std::move(name)),
      queue_(queueCapacity ? queueCapacity : 1024) {
    if (threadCount == 0) {
        threadCount = 1;
    }
    workers_.reserve(threadCount);
    telemetry::logGauge("thread_pool", name_ + ".configured_threads", static_cast<double>(threadCount));
    for (std::size_t i = 0; i < threadCount; ++i) {
        workers_.emplace_back([this, i]() { workerLoop(i); });
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

bool ThreadPool::schedule(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (shuttingDown_) {
            return false;
        }
    }
    return queue_.waitPush(std::move(task), std::chrono::milliseconds(5));
}

void ThreadPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (shuttingDown_) {
            return;
        }
        shuttingDown_ = true;
    }

    queue_.close();

    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
    telemetry::logEvent("thread_pool", name_ + ".shutdown");
}

std::size_t ThreadPool::pendingTasks() const {
    return queue_.size();
}

void ThreadPool::workerLoop(std::size_t workerIndex) {
    telemetry::logEvent("thread_pool", name_ + ".worker_start index=" + std::to_string(workerIndex));
    while (true) {
        Task task;
        if (!queue_.pop(task)) {
            break;
        }
        try {
            task();
        } catch (const std::exception& ex) {
            telemetry::logEvent("thread_pool", name_ + ".worker_exception index=" + std::to_string(workerIndex) + " msg=" + ex.what());
            std::cerr << "ThreadPool worker exception: " << ex.what() << std::endl;
        } catch (...) {
            telemetry::logEvent("thread_pool", name_ + ".worker_exception index=" + std::to_string(workerIndex) + " msg=unknown");
            std::cerr << "ThreadPool worker exception: unknown" << std::endl;
        }
    }
    telemetry::logEvent("thread_pool", name_ + ".worker_stop index=" + std::to_string(workerIndex));
}

} // namespace binancerj::core