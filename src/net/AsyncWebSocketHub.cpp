#include "binancerj/net/AsyncWebSocketHub.hpp"
#include "binancerj/telemetry/PerfTelemetry.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core/bind_handler.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <atomic>
#include <iostream>
#include <sstream>
#include <thread>
#include <utility>

namespace binancerj::net {

namespace {
using tcp = boost::asio::ip::tcp;
using ssl_stream = boost::asio::ssl::stream<tcp::socket>;
using websocket_stream = boost::beast::websocket::stream<ssl_stream>;
} // namespace

struct AsyncWebSocketHub::Impl {
    boost::asio::io_context ioContext;
    boost::asio::ssl::context sslContext{boost::asio::ssl::context::tlsv12_client};
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> workGuard;
    std::vector<std::thread> ioThreads;
    std::atomic<std::uint32_t> subscriptionId{1};

    Impl() {
        sslContext.set_default_verify_paths();
        sslContext.set_verify_mode(boost::asio::ssl::verify_peer);
    }
};

class AsyncWebSocketHub::Session : public std::enable_shared_from_this<AsyncWebSocketHub::Session> {
public:
    Session(AsyncWebSocketHub& parent,
            boost::asio::io_context& io,
            boost::asio::ssl::context& ssl,
            std::string host,
            std::string port,
            std::string stream,
            MessageHandler onMessage,
            ErrorHandler onError,
            std::uint32_t subscriptionId)
        : parent_(parent),
          resolver_(io),
          ws_(io, ssl),
          host_(std::move(host)),
          port_(std::move(port)),
          stream_(std::move(stream)),
          messageHandler_(std::move(onMessage)),
          errorHandler_(std::move(onError)),
          subscriptionId_(subscriptionId) {
        ws_.set_option(boost::beast::websocket::stream_base::timeout::suggested(boost::beast::role_type::client));
        ws_.set_option(boost::beast::websocket::stream_base::decorator([](boost::beast::websocket::request_type& req) {
            req.set(boost::beast::http::field::user_agent, std::string("BinanceRJTech/AsyncClient"));
        }));
    }

    void start() {
        auto self = shared_from_this();
        telemetry::logEvent("ws", "async_session_start stream=" + stream_);
        resolver_.async_resolve(host_, port_,
            [self](const boost::system::error_code& ec, tcp::resolver::results_type results) {
                self->onResolve(ec, results);
            });
    }

    void stop() {
        auto self = shared_from_this();
        boost::asio::dispatch(ws_.get_executor(), [self]() {
            self->stopping_ = true;
            boost::system::error_code ec;
            self->ws_.next_layer().next_layer().cancel(ec);
            self->ws_.async_close(boost::beast::websocket::close_code::normal,
                [self](const boost::system::error_code& closeEc) {
                    if (closeEc) {
                        self->fail("close", closeEc);
                    }
                });
        });
    }

private:
    void onResolve(const boost::system::error_code& ec, tcp::resolver::results_type results) {
        if (ec) {
            fail("resolve", ec);
            return;
        }
        auto self = shared_from_this();
        boost::asio::async_connect(ws_.next_layer().next_layer(), results,
            [self](const boost::system::error_code& connEc, const tcp::endpoint&) {
                self->onConnect(connEc);
            });
    }

    void onConnect(const boost::system::error_code& ec) {
        if (ec) {
            fail("connect", ec);
            return;
        }
        auto self = shared_from_this();
        ws_.next_layer().async_handshake(boost::asio::ssl::stream_base::client,
            [self](const boost::system::error_code& handshakeEc) {
                self->onSslHandshake(handshakeEc);
            });
    }

    void onSslHandshake(const boost::system::error_code& ec) {
        if (ec) {
            fail("ssl_handshake", ec);
            return;
        }
        auto self = shared_from_this();
        ws_.async_handshake(host_, "/ws",
            [self](const boost::system::error_code& hsEc) {
                self->onHandshake(hsEc);
            });
    }

    void onHandshake(const boost::system::error_code& ec) {
        if (ec) {
            fail("handshake", ec);
            return;
        }
        telemetry::logEvent("ws", "async_connected stream=" + stream_);
        sendSubscribe();
    }

    void sendSubscribe() {
        std::ostringstream oss;
        oss << "{\"method\":\"SUBSCRIBE\",\"params\":[\"" << stream_ << "\"],\"id\":" << subscriptionId_ << "}";
        subscribePayload_ = oss.str();
        auto self = shared_from_this();
        ws_.async_write(boost::asio::buffer(subscribePayload_),
            [self](const boost::system::error_code& ec, std::size_t) {
                self->onSubscribe(ec);
            });
    }

    void onSubscribe(const boost::system::error_code& ec) {
        if (ec) {
            fail("subscribe", ec);
            return;
        }
        telemetry::logEvent("ws", "async_subscribed stream=" + stream_);
        doRead();
    }

    void doRead() {
        auto self = shared_from_this();
        ws_.async_read(buffer_,
            [self](const boost::system::error_code& ec, std::size_t bytesTransferred) {
                self->onRead(ec, bytesTransferred);
            });
    }

    void onRead(const boost::system::error_code& ec, std::size_t) {
        if (ec) {
            if (!stopping_ || ec != boost::asio::error::operation_aborted) {
                fail("read", ec);
            }
            return;
        }
        auto payload = boost::beast::buffers_to_string(buffer_.data());
        buffer_.consume(buffer_.size());
        telemetry::logGauge("ws", "async_payload_size", static_cast<double>(payload.size()));
        if (messageHandler_) {
            messageHandler_(std::move(payload));
        }
        doRead();
    }

    void fail(const std::string& stage, const boost::system::error_code& ec) {
        std::ostringstream oss;
        oss << "stream=" << stream_ << " stage=" << stage << " ec=" << ec.message();
        telemetry::logEvent("ws", "async_error " + oss.str());
        if (errorHandler_) {
            errorHandler_(oss.str());
        } else {
            std::cerr << "AsyncWebSocketHub error: " << oss.str() << std::endl;
        }
    }

    AsyncWebSocketHub& parent_;
    tcp::resolver resolver_;
    websocket_stream ws_;
    boost::beast::flat_buffer buffer_;
    std::string host_;
    std::string port_;
    std::string stream_;
    MessageHandler messageHandler_;
    ErrorHandler errorHandler_;
    std::uint32_t subscriptionId_;
    std::string subscribePayload_;
    bool stopping_{false};
};

AsyncWebSocketHub::AsyncWebSocketHub(std::string host, std::string port, std::size_t ioThreads)
    : host_(std::move(host)),
      port_(std::move(port)),
      ioThreadCount_(ioThreads ? ioThreads : 1) {}

AsyncWebSocketHub::~AsyncWebSocketHub() {
    stop();
}

void AsyncWebSocketHub::ensureImpl() {
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
}

void AsyncWebSocketHub::addSubscription(const std::string& stream, MessageHandler onMessage, ErrorHandler onError) {
    ensureImpl();
    auto session = std::make_shared<Session>(*this, impl_->ioContext, impl_->sslContext, host_, port_, stream, std::move(onMessage), std::move(onError), impl_->subscriptionId.fetch_add(1));
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        sessions_.push_back(session);
    }
    if (running()) {
        session->start();
    }
}

void AsyncWebSocketHub::start() {
    ensureImpl();
    if (running_.exchange(true)) {
        return;
    }

    impl_->ioContext.restart();
    impl_->workGuard = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(boost::asio::make_work_guard(impl_->ioContext));

    telemetry::logGauge("ws", "async_io_threads", static_cast<double>(ioThreadCount_));
    impl_->ioThreads.reserve(ioThreadCount_);
    for (std::size_t i = 0; i < ioThreadCount_; ++i) {
        impl_->ioThreads.emplace_back([this, i]() {
            telemetry::logEvent("ws", "io_thread_start index=" + std::to_string(i));
            try {
                impl_->ioContext.run();
            } catch (const std::exception& ex) {
                telemetry::logEvent("ws", "io_thread_exception index=" + std::to_string(i) + " msg=" + ex.what());
            }
            telemetry::logEvent("ws", "io_thread_stop index=" + std::to_string(i));
        });
    }

    std::vector<std::shared_ptr<Session>> toStart;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        toStart = sessions_;
    }
    for (auto& session : toStart) {
        session->start();
    }
}

void AsyncWebSocketHub::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    std::vector<std::shared_ptr<Session>> toStop;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        toStop = sessions_;
    }
    for (auto& session : toStop) {
        session->stop();
    }

    if (impl_) {
        if (impl_->workGuard) {
            impl_->workGuard->reset();
            impl_->workGuard.reset();
        }
        impl_->ioContext.stop();
        for (auto& thread : impl_->ioThreads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        impl_->ioThreads.clear();
    }

    telemetry::logEvent("ws", "async_hub_stopped");
}

bool AsyncWebSocketHub::running() const {
    return running_.load(std::memory_order_acquire);
}

} // namespace binancerj::net