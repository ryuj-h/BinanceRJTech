#include "WebSocket.hpp"

#include <chrono>
#include <vector>
#include <thread>

/* 스레드 생성 부분 */
int main() {
    try {
        const std::string host = "fstream.binance.com";//선물 spot
        const std::string port = "443";

        auto startTime = std::chrono::steady_clock::now(); // 시작 시간 기록

        const int numWebSockets = 10;
        std::vector<std::thread> threads;

        for (int i = 0; i < numWebSockets; ++i) {
            threads.emplace_back(receiveOrderBook, host, port, i + 1);
        }
        threads.emplace_back(mainThread);

        for (auto& thread : threads) {
            while (true) {
                auto currentTime = std::chrono::steady_clock::now();
                auto elapsedTime = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - startTime);
                if (elapsedTime.count() >= 8) {
                    startTime = currentTime;
                    break;
                }
            }

            thread.join();
        }


        //서브스레드   
        void receiveOrderBook(const std::string & host, const std::string & port, int id) {
            try {
                WebSocket ws(host, port);
                ws.connect();
                //ws.send("{\"method\": \"SUBSCRIBE\", \"params\": [\"btcusdt@depth20@100ms\"], \"id\": 1}");//호가창
                //ws.send("{\"method\": \"SUBSCRIBE\", \"params\": [\"btcusdt@trade\"], \"id\": 1}");//거래목록
                ws.send("{\"method\": \"SUBSCRIBE\", \"params\": [\"btcusdt@depth20@100ms\"], \"id\": " + std::to_string(id) + "}");

                while (true) {
                    std::string message = ws.receive();
                    if (!message.empty()) {
                        messageChanged = true;
                        orderMessage = message;
                        messageCount = messageCount + 1;
                    }
                }

                ws.close();
            }
            catch (const std::exception& ex) {
                std::cerr << "Error in WebSocket " << id << ": " << ex.what() << std::endl;
            }
        }

        //메인스레드
        void mainThread() {
            auto startTime = std::chrono::steady_clock::now(); // 시작 시간 기록
            while (true) {
                auto currentTime = std::chrono::steady_clock::now();
                auto elapsedTime = std::chrono::duration_cast<std::chrono::seconds>(currentTime - startTime);
                if (elapsedTime.count() >= 1) {
                    startTime = currentTime;
                    lastMessageCount = messageCount;
                    messageCount = 0;
                }

                if (messageChanged) {
                    system("cls");
                    std::cout << lastMessageCount << " : " << orderMessage << std::endl;
                    messageChanged = false;
                }

            }
        }
    }
}
