#include "WebSocket.hpp"

#include <chrono>


int main() {
    try {
        //const std::string host = "stream.binance.com";//���� spot
        const std::string host = "fstream.binance.com";//���� spot


        const std::string port = "443";

        WebSocket ws(host, port);

        ws.connect();
        //ws.send("{\"method\": \"SUBSCRIBE\", \"params\": [\"btcusdt@depth20@100ms\"], \"id\": 1}");//ȣ��â
        ws.send("{\"method\": \"SUBSCRIBE\", \"params\": [\"btcusdt@trade\"], \"id\": 1}");//�ŷ����

        int messageCount = 0; // �޽��� ī��Ʈ
        int lastmessageCount = 0;
        auto startTime = std::chrono::steady_clock::now(); // ���� �ð� ���

        while (true) {

            std::string message = ws.receive();
            if (!message.empty()) {

                //system("cls");

                //std::cout << lastmessageCount << " : " << message << std::endl;
                std::cout <<  message << std::endl;

                messageCount++; // �޽��� �� ����
            }

            auto currentTime = std::chrono::steady_clock::now();
            auto elapsedTime = std::chrono::duration_cast<std::chrono::seconds>(currentTime - startTime);

            if (elapsedTime.count() >= 1) {
                // 1�ʰ� ����� ���

                lastmessageCount = messageCount;
                // �ʱ�ȭ
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
