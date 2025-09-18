#include "binancerj/net/WebSocket.hpp"
#include "binancerj/telemetry/PerfTelemetry.hpp"
#include <boost/beast/websocket/ssl.hpp>
#include <boost/system/error_code.hpp>
#include <iostream>

WebSocket::WebSocket(const std::string& host, const std::string& port)
    : host_(host),
    port_(port),
    ssl_context_(boost::asio::ssl::context::tlsv12_client),
    ws_(io_context_, ssl_context_) {
    // Set up SSL verification
    ssl_context_.set_verify_mode(boost::asio::ssl::verify_peer);
    ssl_context_.set_default_verify_paths();
}

WebSocket::~WebSocket() {
    close();
}

void WebSocket::connect() {
    try {
        telemetry::ScopedTimer timer("ws", "connect");
        boost::asio::ip::tcp::resolver resolver(io_context_);
        auto const results = resolver.resolve(host_, port_);

        boost::asio::connect(ws_.next_layer().next_layer(), results.begin(), results.end());
        ws_.next_layer().handshake(boost::asio::ssl::stream_base::client);
        ws_.handshake(host_, "/ws");
        std::cout << "Connected to " << host_ << std::endl;
        telemetry::logEvent("ws", "connected host=" + host_);
    }
    catch (const std::exception& ex) {
        std::cerr << "Connection error: " << ex.what() << std::endl;
        telemetry::logEvent("ws", std::string("connect_error host=") + host_ + " msg=" + ex.what());
        throw;
    }
}

void WebSocket::send(const std::string& message) {
    try {
        telemetry::ScopedTimer timer("ws", "send");
        ws_.write(boost::asio::buffer(message));
        telemetry::logGauge("ws", "send_bytes", static_cast<double>(message.size()));
    }
    catch (const std::exception& ex) {
        std::cerr << "Send error: " << ex.what() << std::endl;
        telemetry::logEvent("ws", std::string("send_error host=") + host_ + " msg=" + ex.what());
    }
}

std::string WebSocket::receive() {
    try {
        telemetry::ScopedTimer timer("ws", "receive");
        boost::beast::flat_buffer buffer;
        ws_.read(buffer);
        auto payload = boost::beast::buffers_to_string(buffer.data());
        telemetry::logGauge("ws", "receive_bytes", static_cast<double>(payload.size()));
        return payload;
    }
    catch (const std::exception& ex) {
        std::cerr << "Receive error: " << ex.what() << std::endl;
        telemetry::logEvent("ws", std::string("receive_error host=") + host_ + " msg=" + ex.what());
        return "";
    }
}

void WebSocket::close() {
    try {
        ws_.close(boost::beast::websocket::close_code::normal);
        std::cout << "WebSocket closed" << std::endl;
        telemetry::logEvent("ws", "closed host=" + host_);
    }
    catch (const std::exception& ex) {
        std::cerr << "Close error: " << ex.what() << std::endl;
        telemetry::logEvent("ws", std::string("close_error host=") + host_ + " msg=" + ex.what());
    }
}
