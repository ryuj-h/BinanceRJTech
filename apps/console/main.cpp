#include "binancerj/core/ThreadPool.hpp"
#include "binancerj/net/AsyncWebSocketHub.hpp"
#include "binancerj/telemetry/PerfTelemetry.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> shutdownRequested{false};
std::atomic<int> messageCounter{0};
std::atomic<int> droppedMessages{0};
std::atomic<int> lastMessagesPerSecond{0};

struct MessageState {
    std::mutex mutex;
    std::string lastPayload;
    std::string lastStream;
};

MessageState messageState;

void handleSignal(int) {
    shutdownRequested.store(true, std::memory_order_release);
    telemetry::logEvent("app", "shutdown signal received");
}

void statsLoop(binancerj::core::ThreadPool& processingPool) {
    using namespace std::chrono;
    using namespace std::chrono_literals;
    auto lastTick = steady_clock::now();
    int droppedSnapshot = 0;

    while (!shutdownRequested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(1s);
        auto now = steady_clock::now();
        auto elapsed = duration_cast<seconds>(now - lastTick).count();
        if (elapsed <= 0) {
            continue;
        }
        lastTick = now;

        int total = messageCounter.exchange(0, std::memory_order_acq_rel);
        int perSecond = static_cast<int>(total / elapsed);
        lastMessagesPerSecond.store(perSecond, std::memory_order_release);
        telemetry::logGauge("ws", "messages_per_second", perSecond);

        int droppedTotal = droppedMessages.load(std::memory_order_acquire);
        int droppedDelta = droppedTotal - droppedSnapshot;
        droppedSnapshot = droppedTotal;
        telemetry::logGauge("ws", "messages_dropped_total", static_cast<double>(droppedTotal));
        telemetry::logGauge("app", "processing_queue_depth", static_cast<double>(processingPool.pendingTasks()));

        std::string payloadCopy;
        std::string streamCopy;
        {
            std::lock_guard<std::mutex> lock(messageState.mutex);
            payloadCopy = messageState.lastPayload;
            streamCopy = messageState.lastStream;
        }
        if (!payloadCopy.empty()) {
            telemetry::logGauge("ws", "last_payload_size", static_cast<double>(payloadCopy.size()));
        }

        std::cout << "msgs/s=" << perSecond
                  << " dropped/s=" << droppedDelta
                  << " total_dropped=" << droppedTotal
                  << " last_stream=" << streamCopy
                  << '\r';
    }
}

} // namespace

int runConsoleApp() {
    telemetry::startSession("console_app");
    telemetry::logEvent("app", "console entry");

    shutdownRequested.store(false, std::memory_order_release);
    messageCounter.store(0, std::memory_order_release);
    droppedMessages.store(0, std::memory_order_release);
    lastMessagesPerSecond.store(0, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(messageState.mutex);
        messageState.lastPayload.clear();
        messageState.lastStream.clear();
    }

    std::signal(SIGINT, handleSignal);

    const std::string host = "fstream.binance.com";
    const std::string port = "443";
    const int workerCount = 10;

    telemetry::logGauge("app", "hardware_threads", static_cast<double>(std::thread::hardware_concurrency()));
    telemetry::logGauge("app", "configured_workers", workerCount);

    binancerj::net::AsyncWebSocketHub hub(host, port, 2);
    binancerj::core::ThreadPool processingPool(std::max(2u, std::thread::hardware_concurrency() / 2u), 2048, "processing");

    auto errorHandler = [](const std::string& error) {
        telemetry::logEvent("ws", "subscription_error " + error);
    };

    for (int i = 0; i < workerCount; ++i) {
        std::string stream = "btcusdt@depth20@100ms";
        hub.addSubscription(stream,
            [stream, &processingPool](std::string&& payload) {
                messageCounter.fetch_add(1, std::memory_order_acq_rel);
                telemetry::logGauge("ws", "receive_bytes", static_cast<double>(payload.size()));
                auto payloadPtr = std::make_shared<std::string>(std::move(payload));
                bool accepted = processingPool.schedule([payloadPtr, stream]() {
                    std::lock_guard<std::mutex> lock(messageState.mutex);
                    messageState.lastPayload = *payloadPtr;
                    messageState.lastStream = stream;
                });
                if (!accepted) {
                    droppedMessages.fetch_add(1, std::memory_order_acq_rel);
                    telemetry::logEvent("ws", "processing_queue_overflow stream=" + stream);
                }
            },
            errorHandler);
    }

    hub.start();

    std::thread statsThread([&processingPool]() {
        statsLoop(processingPool);
    });

    while (!shutdownRequested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    hub.stop();
    processingPool.shutdown();
    if (statsThread.joinable()) {
        statsThread.join();
    }

    telemetry::logEvent("app", "console exit");
    telemetry::flush();
    return 0;
}

#if defined(BINANCE_RJ_ENABLE_CONSOLE_ENTRY)
int main() {
    return runConsoleApp();
}
#endif
