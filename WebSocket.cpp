#include "WebSocket.hpp"
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
        boost::asio::ip::tcp::resolver resolver(io_context_);
        auto const results = resolver.resolve(host_, port_);

        boost::asio::connect(ws_.next_layer().next_layer(), results.begin(), results.end());
        ws_.next_layer().handshake(boost::asio::ssl::stream_base::client);
        ws_.handshake(host_, "/ws");
        std::cout << "Connected to " << host_ << std::endl;
    }
    catch (const std::exception& ex) {
        std::cerr << "Connection error: " << ex.what() << std::endl;
        throw;
    }
}

void WebSocket::send(const std::string& message) {
    try {
        ws_.write(boost::asio::buffer(message));
    }
    catch (const std::exception& ex) {
        std::cerr << "Send error: " << ex.what() << std::endl;
    }
}

std::string WebSocket::receive() {
    try {
        boost::beast::flat_buffer buffer;
        ws_.read(buffer);
        return boost::beast::buffers_to_string(buffer.data());
    }
    catch (const std::exception& ex) {
        std::cerr << "Receive error: " << ex.what() << std::endl;
        return "";
    }
}

void WebSocket::close() {
    try {
        ws_.close(boost::beast::websocket::close_code::normal);
        std::cout << "WebSocket closed" << std::endl;
    }
    catch (const std::exception& ex) {
        std::cerr << "Close error: " << ex.what() << std::endl;
    }
}
