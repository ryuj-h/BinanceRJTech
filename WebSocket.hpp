#ifndef WEBSOCKETCLIENT_H
#define WEBSOCKETCLIENT_H

#define BOOST_DISABLE_CURRENT_LOCATION


#include <boost/beast.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <string>
#include <iostream>

class WebSocket {
public:
    WebSocket(const std::string& host, const std::string& port);
    ~WebSocket();

    void connect();
    void send(const std::string& message);
    std::string receive();
    void close();

private:
    std::string host_;
    std::string port_;
    boost::asio::io_context io_context_;
    boost::asio::ssl::context ssl_context_; 
    boost::beast::websocket::stream<boost::beast::ssl_stream<boost::asio::ip::tcp::socket>> ws_;

};

#endif // WEBSOCKET_HPP