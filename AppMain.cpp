//
// Fixed and working main program

#include "WebSocket.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <iostream>
#include <iomanip>
#include <algorithm>

#include <nlohmann/json.hpp>

// Shared state for simple message display
static std::atomic<bool> bookChanged{false};
static std::atomic<int> messageCount{0};
static std::atomic<int> lastMessageCount{0};
static std::mutex bookMutex;

struct Level { double price; double qty; };
static std::vector<Level> g_bids;
static std::vector<Level> g_asks;

// Worker: connect and receive messages from Binance
void receiveOrderBook(const std::string& host, const std::string& port, int id) {
    try {
        WebSocket ws(host, port);
        ws.connect();

        // Subscribe to BTCUSDT order book updates (20 levels, 100ms)
        ws.send("{\"method\":\"SUBSCRIBE\",\"params\":[\"btcusdt@depth20@100ms\"],\"id\":" + std::to_string(id) + "}");

        for (;;) {
            std::string message = ws.receive();
            if (!message.empty()) {
                try {
                    using nlohmann::json;
                    json j = json::parse(message, nullptr, false);
                    if (j.is_discarded()) goto sleep_short;

                    const json* payload = nullptr;
                    if (j.contains("b") && j.contains("a")) {
                        payload = &j;
                    } else if (j.contains("data") && j["data"].is_object()) {
                        const auto& d = j["data"];
                        if (d.contains("b") && d.contains("a")) payload = &d;
                    }
                    if (!payload) goto sleep_short;

                    std::vector<Level> bids, asks;
                    if ((*payload).contains("b") && (*payload)["b"].is_array()) {
                        for (const auto& v : (*payload)["b"]) {
                            if (!v.is_array() || v.size() < 2) continue;
                            const auto& p = v[0];
                            const auto& q = v[1];
                            double price = p.is_string() ? std::stod(p.get<std::string>()) : p.get<double>();
                            double qty   = q.is_string() ? std::stod(q.get<std::string>()) : q.get<double>();
                            if (qty > 0) bids.push_back(Level{price, qty});
                        }
                    }
                    if ((*payload).contains("a") && (*payload)["a"].is_array()) {
                        for (const auto& v : (*payload)["a"]) {
                            if (!v.is_array() || v.size() < 2) continue;
                            const auto& p = v[0];
                            const auto& q = v[1];
                            double price = p.is_string() ? std::stod(p.get<std::string>()) : p.get<double>();
                            double qty   = q.is_string() ? std::stod(q.get<std::string>()) : q.get<double>();
                            if (qty > 0) asks.push_back(Level{price, qty});
                        }
                    }

                    // Keep only top 10 levels for display
                    auto trim = [](std::vector<Level>& v, size_t n) {
                        if (v.size() > n) v.resize(n);
                    };
                    trim(bids, 10);
                    trim(asks, 10);

                    {
                        std::lock_guard<std::mutex> lock(bookMutex);
                        g_bids = std::move(bids);
                        g_asks = std::move(asks);
                    }
                    bookChanged.store(true, std::memory_order_release);
                    messageCount.fetch_add(1, std::memory_order_relaxed);
                } catch (...) {
                    // ignore parse errors and continue
                }
            } else {
sleep_short:
                // Avoid tight loop on empty reads
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }
    catch (const std::exception& ex) {
        std::cerr << "Error in WebSocket " << id << ": " << ex.what() << std::endl;
    }
}

// UI thread: periodically print last second's message count and last message
void mainThread() {
    auto tickStart = std::chrono::steady_clock::now();
    for (;;) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - tickStart);
        if (elapsed.count() >= 1) {
            tickStart = now;
            lastMessageCount.store(messageCount.exchange(0), std::memory_order_acq_rel);
        }

        if (bookChanged.load(std::memory_order_acquire)) {
            std::vector<Level> bids, asks;
            {
                std::lock_guard<std::mutex> lock(bookMutex);
                bids = g_bids;
                asks = g_asks;
            }
            bookChanged.store(false, std::memory_order_release);

            // Render simple ASCII order book
            system("cls");

            // Determine scaling based on max qty
            double maxQty = 0.0;
            for (const auto& lv : bids) maxQty = std::max(maxQty, lv.qty);
            for (const auto& lv : asks) maxQty = std::max(maxQty, lv.qty);
            if (maxQty <= 0) maxQty = 1.0;

            auto bar = [&](double qty) {
                int width = 40;
                int filled = static_cast<int>(std::round((qty / maxQty) * width));
                std::string s(filled, '#');
                s.resize(width, ' ');
                return s;
            };

            std::cout << "BTCUSDT Depth (Top 10)  | msgs/s: "
                      << lastMessageCount.load(std::memory_order_relaxed) << "\n";
            std::cout << "------------------------+----------------------------------------------\n";
            std::cout << "        Asks (qty→bar)  |  Bids (bar←qty)\n";
            std::cout << "Price         Qty       |       Qty         Price\n";
            std::cout << "------------------------+----------------------------------------------\n";

            // Ensure we have 10 lines each side
            const size_t N = 10;
            bids.resize(std::min(bids.size(), N));
            asks.resize(std::min(asks.size(), N));

            // Print from worst ask to best ask (top→bottom shows improving price)
            // For bids, best bid on top
            std::vector<Level> asksPrint = asks;
            std::reverse(asksPrint.begin(), asksPrint.end());

            for (size_t i = 0; i < N; ++i) {
                // Ask side
                if (i < asksPrint.size()) {
                    const auto& a = asksPrint[i];
                    std::cout << std::fixed << std::setprecision(2)
                              << std::setw(10) << a.price << ' '
                              << std::setw(9) << std::setprecision(6) << a.qty << ' '
                              << bar(a.qty);
                } else {
                    std::cout << std::string(24 + 40, ' ');
                }

                std::cout << " | ";

                // Bid side
                if (i < bids.size()) {
                    const auto& b = bids[i];
                    auto br = bar(b.qty);
                    std::cout << br << ' '
                              << std::setw(9) << std::setprecision(6) << b.qty << ' '
                              << std::fixed << std::setprecision(2)
                              << std::setw(10) << b.price;
                }
                std::cout << "\n";
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
}

int main() {
    try {
        const std::string host = "fstream.binance.com"; // futures
        const std::string port = "443";

        const int numWebSockets = 1; // visualization: single stream is sufficient
        std::vector<std::thread> threads;
        threads.reserve(static_cast<size_t>(numWebSockets) + 1);

        for (int i = 0; i < numWebSockets; ++i) {
            threads.emplace_back(receiveOrderBook, host, port, i + 1);
        }
        threads.emplace_back(mainThread);

        for (auto& t : threads) {
            if (t.joinable()) t.join();
        }
    }
    catch (const std::exception& ex) {
        std::cerr << "Fatal error: " << ex.what() << std::endl;
        return 1;
    }
    return 0;
}
