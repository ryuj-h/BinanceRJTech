#include "binancerj/telemetry/PerfTelemetry.hpp"
#include "binancerj/net/WebSocket.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
std::atomic<bool> shutdownRequested{false};
std::atomic<int> messageCounter{0};
std::atomic<int> lastMessagesPerSecond{0};
std::mutex lastMessageMutex;
std::string lastPayload;

void handleSignal(int) {
    shutdownRequested.store(true, std::memory_order_release);
    telemetry::logEvent("app", "shutdown signal received");
}

void receiveOrderBook(const std::string& host, const std::string& port, const std::string& stream, int id) {
    telemetry::logEvent("ws", "thread start id=" + std::to_string(id) + " stream=" + stream);
    try {
        telemetry::ScopedTimer connectTimer("ws", "connect" + std::to_string(id));
        WebSocket ws(host, port);
        ws.connect();
        telemetry::logEvent("ws", "connected id=" + std::to_string(id));

        std::string subscribe = "{\"method\":\"SUBSCRIBE\",\"params\":[\"" + stream + "\"],\"id\":" + std::to_string(id) + "}";
        ws.send(subscribe);
        telemetry::logEvent("ws", "subscribed id=" + std::to_string(id));

        while (!shutdownRequested.load(std::memory_order_acquire)) {
            telemetry::ScopedTimer recvTimer("ws", "receive_latency" + std::to_string(id));
            std::string message = ws.receive();
            if (!message.empty()) {
                messageCounter.fetch_add(1, std::memory_order_acq_rel);
                {
                    std::lock_guard<std::mutex> lock(lastMessageMutex);
                    lastPayload = message;
                }
            }
        }

        ws.close();
    } catch (const std::exception& ex) {
        telemetry::logEvent("ws", std::string("worker error id=") + std::to_string(id) + " msg=" + ex.what());
        std::cerr << "Error in WebSocket worker " << id << ": " << ex.what() << std::endl;
    }
    telemetry::logEvent("ws", "thread exit id=" + std::to_string(id));
}

void statsLoop() {
    using namespace std::chrono_literals;
    auto lastTick = std::chrono::steady_clock::now();
    while (!shutdownRequested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(1s);
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastTick).count();
        if (elapsed <= 0) {
            continue;
        }
        lastTick = now;
        int total = messageCounter.exchange(0, std::memory_order_acq_rel);
        int perSecond = static_cast<int>(total / elapsed);
        lastMessagesPerSecond.store(perSecond, std::memory_order_release);
        telemetry::logGauge("ws", "messages_per_second", perSecond);

        std::string payloadCopy;
        {
            std::lock_guard<std::mutex> lock(lastMessageMutex);
            payloadCopy = lastPayload;
        }
        if (!payloadCopy.empty()) {
            telemetry::logGauge("ws", "last_payload_size", static_cast<double>(payloadCopy.size()));
        }
        std::cout << "msgs/s=" << perSecond << '\r';
    }
}

} // namespace

int runConsoleApp() {
    telemetry::startSession("console_app");
    telemetry::logEvent("app", "console entry");

    std::signal(SIGINT, handleSignal);

    const std::string host = "fstream.binance.com";
    const std::string port = "443";
    std::vector<std::thread> workers;

    telemetry::logGauge("app", "hardware_threads", static_cast<double>(std::thread::hardware_concurrency()));

    const int workerCount = 10;
    workers.reserve(workerCount + 1);
    telemetry::logGauge("app", "configured_workers", workerCount);

    for (int i = 0; i < workerCount; ++i) {
        std::string stream = "btcusdt@depth20@100ms";
        workers.emplace_back(receiveOrderBook, host, port, stream, i + 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    workers.emplace_back(statsLoop);

    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
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
