#include "WebSocket.hpp"

#include <chrono>


int main() {
    try {
        //const std::string host = "stream.binance.com";//현물 spot
        const std::string host = "fstream.binance.com";//선물 spot


        const std::string port = "443";

        WebSocket ws(host, port);

        ws.connect();
        //ws.send("{\"method\": \"SUBSCRIBE\", \"params\": [\"btcusdt@depth20@100ms\"], \"id\": 1}");//호가창
        ws.send("{\"method\": \"SUBSCRIBE\", \"params\": [\"btcusdt@trade\"], \"id\": 1}");//거래목록

        int messageCount = 0; // 메시지 카운트
        int lastmessageCount = 0;
        auto startTime = std::chrono::steady_clock::now(); // 시작 시간 기록

        while (true) {

            std::string message = ws.receive();
            if (!message.empty()) {

                //system("cls");

                //std::cout << lastmessageCount << " : " << message << std::endl;
                std::cout <<  message << std::endl;

                messageCount++; // 메시지 수 증가
            }

            auto currentTime = std::chrono::steady_clock::now();
            auto elapsedTime = std::chrono::duration_cast<std::chrono::seconds>(currentTime - startTime);

            if (elapsedTime.count() >= 1) {
                // 1초가 경과한 경우

                lastmessageCount = messageCount;
                // 초기화
                messageCount = 0;
                startTime = currentTime;
            }

            //Sleep(100);
        }

        ws.close();
    }
    catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << std::endl;
    }

    return 0;
}
