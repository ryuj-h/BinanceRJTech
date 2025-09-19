#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace binancerj::net {

class AsyncWebSocketHub {
public:
    using MessageHandler = std::function<void(std::string&&)>;
    using ErrorHandler = std::function<void(const std::string&)>;

    AsyncWebSocketHub(std::string host, std::string port, std::size_t ioThreads = 2);
    ~AsyncWebSocketHub();

    AsyncWebSocketHub(const AsyncWebSocketHub&) = delete;
    AsyncWebSocketHub& operator=(const AsyncWebSocketHub&) = delete;

    void addSubscription(const std::string& stream, MessageHandler onMessage, ErrorHandler onError = {});

    void start();
    void stop();
    bool running() const;

private:
    class Session;
    struct Impl;

    void ensureImpl();

    std::string host_;
    std::string port_;
    std::size_t ioThreadCount_;
    std::atomic<bool> running_{false};

    std::mutex sessionMutex_;
    std::vector<std::shared_ptr<Session>> sessions_;
    std::unique_ptr<Impl> impl_;
};

} // namespace binancerj::net