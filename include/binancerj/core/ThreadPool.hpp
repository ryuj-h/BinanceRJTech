#pragma once

#include "binancerj/core/BoundedQueue.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace binancerj::core {

class ThreadPool {
public:
    ThreadPool(std::size_t threadCount, std::size_t queueCapacity = 1024, std::string name = "pool");
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    bool schedule(std::function<void()> task);

    template <typename Func>
    auto submit(Func&& func) -> std::future<typename std::invoke_result_t<Func>> {
        using ResultT = typename std::invoke_result_t<Func>;
        auto packaged = std::make_shared<std::packaged_task<ResultT()>>(std::forward<Func>(func));
        auto future = packaged->get_future();
        if (!schedule([packaged]() { (*packaged)(); })) {
            throw std::runtime_error("thread pool rejected task");
        }
        return future;
    }

    void shutdown();

    std::size_t pendingTasks() const;

private:
    using Task = std::function<void()>;

    void workerLoop(std::size_t workerIndex);

    const std::string name_;
    mutable std::mutex stateMutex_;
    bool shuttingDown_{false};
    BoundedQueue<Task> queue_;
    std::vector<std::thread> workers_;
};

} // namespace binancerj::core