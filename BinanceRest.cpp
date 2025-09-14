#include "BinanceRest.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <openssl/hmac.h>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <cctype>
#include <filesystem>

using tcp = boost::asio::ip::tcp;
namespace ssl = boost::asio::ssl;
namespace beast = boost::beast;
namespace http = beast::http;

static std::string url_encode(const std::string& s) {
    std::ostringstream oss;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c=='-'||c=='_'||c=='.'||c=='~') oss << c;
        else {
            oss << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << (int)c << std::nouppercase << std::dec;
        }
    }
    return oss.str();
}

static std::string trim_quotes_ws(std::string v) {
    auto is_ws = [](unsigned char c){ return std::isspace(c) != 0; };
    while (!v.empty() && is_ws(static_cast<unsigned char>(v.front()))) v.erase(v.begin());
    while (!v.empty() && is_ws(static_cast<unsigned char>(v.back()))) v.pop_back();
    if (v.size() >= 2) {
        char a = v.front(), b = v.back();
        if ((a=='"' && b=='"') || (a=='\'' && b=='\'')) {
            v = v.substr(1, v.size()-2);
        }
    }
    return v;
}

struct BinanceRest::Impl {
    std::string host;
    std::string apiKey;
    std::string apiSecret;
    long long timeOffsetMs{0};
    bool insecureTLS{false};

    Impl(const std::string& h) : host(h) {
        auto get_env = [](const char* name)->std::string {
#ifdef _WIN32
            size_t len = 0; char* buf = nullptr;
            if (_dupenv_s(&buf, &len, name) == 0 && buf) { std::string v(buf); free(buf); return v; }
            return {};
#else
            const char* v = std::getenv(name);
            return v ? std::string(v) : std::string();
#endif
        };
        std::string k = trim_quotes_ws(get_env("BINANCE_API_KEY"));
        std::string s = trim_quotes_ws(get_env("BINANCE_API_SECRET"));
        if (!k.empty()) apiKey = std::move(k);
        if (!s.empty()) apiSecret = std::move(s);
    }

    std::string hmac_sha256_hex(const std::string& data) {
        unsigned char md[EVP_MAX_MD_SIZE]; unsigned int md_len = 0;
        HMAC(EVP_sha256(), apiSecret.data(), (int)apiSecret.size(), (const unsigned char*)data.data(), data.size(), md, &md_len);
        std::ostringstream oss;
        for (unsigned int i=0;i<md_len;++i) oss << std::hex << std::setw(2) << std::setfill('0') << (int)md[i];
        return oss.str();
    }

    void try_load_ca(ssl::context& ctx) {
        // Try environment overrides first
        auto get_env = [](const char* name)->std::string {
#ifdef _WIN32
            size_t len = 0; char* buf = nullptr;
            if (_dupenv_s(&buf, &len, name) == 0 && buf) { std::string v(buf); free(buf); return v; }
            return {};
#else
            const char* v = std::getenv(name);
            return v ? std::string(v) : std::string();
#endif
        };
        std::string cafile = get_env("SSL_CERT_FILE");
        if (cafile.empty()) cafile = get_env("CURL_CA_BUNDLE");
        if (!cafile.empty()) {
            SSL_CTX_load_verify_locations(ctx.native_handle(), cafile.c_str(), nullptr);
            return;
        }
        // Try common local files
        namespace fs = std::filesystem;
        const char* guesses[] = {
            "cacert.pem",
            "cert\\cacert.pem",
            "certs\\cacert.pem",
            "C:\\Program Files\\OpenSSL-Win64\\certs\\cacert.pem",
            "C:\\Program Files\\Git\\mingw64\\ssl\\certs\\ca-bundle.crt",
        };
        for (auto p : guesses) {
            if (fs::exists(p)) {
                SSL_CTX_load_verify_locations(ctx.native_handle(), p, nullptr);
                break;
            }
        }
    }

    Result https_request(const std::string& method, const std::string& target, const std::string& bodyOrQuery, bool isPost, const std::string& apiKeyHdr) {
        Result r;
        try {
            boost::asio::io_context ioc;
            ssl::context ctx(ssl::context::tlsv12_client);
            ctx.set_default_verify_paths();
            if (insecureTLS) ctx.set_verify_mode(ssl::verify_none);
            else {
                ctx.set_verify_mode(ssl::verify_peer);
                try_load_ca(ctx);
            }

            tcp::resolver resolver(ioc);
            auto const results = resolver.resolve(host, "443");

            beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
            if(! SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()))
            {
                beast::error_code ec(static_cast<int>(::ERR_get_error()), boost::asio::error::get_ssl_category());
                throw beast::system_error(ec, "Failed to set SNI");
            }

            beast::get_lowest_layer(stream).connect(results);
            stream.handshake(ssl::stream_base::client);

            http::request<http::string_body> req;
            // Select HTTP verb based on 'method' argument
            http::verb verb = http::verb::get;
            if (method == "GET") verb = http::verb::get;
            else if (method == "POST") verb = http::verb::post;
            else if (method == "DELETE") verb = http::verb::delete_;
            else if (method == "PUT") verb = http::verb::put;
            req.method(verb);
            req.target(target);
            req.version(11);
            req.set(http::field::host, host);
            req.set(http::field::user_agent, "BinanceRJTech/1.0");
            req.set(http::field::accept, "application/json");
            if (!apiKeyHdr.empty()) req.set("X-MBX-APIKEY", apiKeyHdr);
            if (isPost) {
                req.set(http::field::content_type, "application/x-www-form-urlencoded");
                req.body() = bodyOrQuery;
                req.content_length(req.body().size());
            }

            http::write(stream, req);

            beast::flat_buffer buffer;
            http::response<http::string_body> res;
            http::read(stream, buffer, res);

            beast::error_code ec;
            stream.shutdown(ec);

            r.ok = (res.result_int() >= 200 && res.result_int() < 300);
            r.status = res.result_int();
            r.body = std::move(res.body());
        } catch (const std::exception& ex) {
            r.ok = false;
            r.status = -1;
            r.body = std::string("HTTPS error: ") + ex.what();
        }
        return r;
    }
};

BinanceRest::BinanceRest(const std::string& baseHost) : impl_(new Impl(baseHost)) {}
BinanceRest::~BinanceRest() = default;

void BinanceRest::setCredentials(const std::string& apiKey, const std::string& apiSecret) {
    impl_->apiKey = apiKey;
    impl_->apiSecret = apiSecret;
}

BinanceRest::Result BinanceRest::getServerTime() {
    std::string target = "/fapi/v1/time";
    return impl_->https_request("GET", target, {}, false, {});
}

void BinanceRest::setInsecureTLS(bool v) {
    impl_->insecureTLS = v;
}

BinanceRest::Result BinanceRest::getAccountInfo(int recvWindowMs) {
    using namespace std::chrono;
    long long ts = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count() + impl_->timeOffsetMs;
    std::ostringstream q;
    q << "recvWindow=" << recvWindowMs << "&timestamp=" << ts;
    std::string qs = q.str();
    qs += "&signature=" + impl_->hmac_sha256_hex(qs);
    return impl_->https_request("GET", "/fapi/v2/account?" + qs, {}, false, impl_->apiKey);
}

BinanceRest::Result BinanceRest::getExchangeInfo(const std::string& symbol) {
    std::string target = "/fapi/v1/exchangeInfo?symbol=" + symbol;
    return impl_->https_request("GET", target, {}, false, {});
}

BinanceRest::Result BinanceRest::getKlines(const std::string& symbol, const std::string& interval, long long startTime, long long endTime, int limit) {
    std::ostringstream q;
    q << "/fapi/v1/klines?symbol=" << symbol << "&interval=" << interval;
    if (startTime > 0) q << "&startTime=" << startTime;
    if (endTime > 0)   q << "&endTime=" << endTime;
    if (limit > 0)     q << "&limit=" << limit;
    return impl_->https_request("GET", q.str(), {}, false, {});
}

BinanceRest::Result BinanceRest::getOpenOrders(const std::string& symbol, int recvWindowMs) {
    using namespace std::chrono;
    long long ts = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count() + impl_->timeOffsetMs;
    std::ostringstream q; q << "symbol=" << symbol << "&recvWindow=" << recvWindowMs << "&timestamp=" << ts;
    std::string qs = q.str(); qs += "&signature=" + impl_->hmac_sha256_hex(qs);
    return impl_->https_request("GET", "/fapi/v1/openOrders?" + qs, {}, false, impl_->apiKey);
}

BinanceRest::Result BinanceRest::getUserTrades(const std::string& symbol, int limit, int recvWindowMs) {
    using namespace std::chrono;
    long long ts = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count() + impl_->timeOffsetMs;
    std::ostringstream q; q << "symbol=" << symbol;
    if (limit > 0) q << "&limit=" << limit;
    q << "&recvWindow=" << recvWindowMs << "&timestamp=" << ts;
    std::string qs = q.str(); qs += "&signature=" + impl_->hmac_sha256_hex(qs);
    return impl_->https_request("GET", "/fapi/v1/userTrades?" + qs, {}, false, impl_->apiKey);
}

BinanceRest::Result BinanceRest::cancelOrder(const std::string& symbol, long long orderId, const std::string& origClientOrderId, int recvWindowMs) {
    using namespace std::chrono;
    long long ts = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count() + impl_->timeOffsetMs;
    std::ostringstream q; q << "symbol=" << symbol;
    if (orderId > 0) q << "&orderId=" << orderId;
    if (!origClientOrderId.empty()) q << "&origClientOrderId=" << url_encode(origClientOrderId);
    q << "&recvWindow=" << recvWindowMs << "&timestamp=" << ts;
    std::string qs = q.str(); qs += "&signature=" + impl_->hmac_sha256_hex(qs);
    return impl_->https_request("DELETE", "/fapi/v1/order?" + qs, {}, false, impl_->apiKey);
}

BinanceRest::Result BinanceRest::cancelAllOpenOrders(const std::string& symbol, int recvWindowMs) {
    using namespace std::chrono;
    long long ts = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count() + impl_->timeOffsetMs;
    std::ostringstream q; q << "symbol=" << symbol << "&recvWindow=" << recvWindowMs << "&timestamp=" << ts;
    std::string qs = q.str(); qs += "&signature=" + impl_->hmac_sha256_hex(qs);
    return impl_->https_request("DELETE", "/fapi/v1/allOpenOrders?" + qs, {}, false, impl_->apiKey);
}

BinanceRest::Result BinanceRest::getDepth(const std::string& symbol, int limit) {
    std::ostringstream q;
    q << "/fapi/v1/depth?symbol=" << symbol;
    if (limit > 0) q << "&limit=" << limit;
    return impl_->https_request("GET", q.str(), {}, false, {});
}

BinanceRest::Result BinanceRest::getTickerPrice(const std::string& symbol) {
    std::ostringstream q;
    q << "/fapi/v1/ticker/price?symbol=" << symbol;
    return impl_->https_request("GET", q.str(), {}, false, {});
}

BinanceRest::Result BinanceRest::placeOrder(const std::string& symbol, const std::string& side, const std::string& type, double quantity, double price, const std::string& tif, bool reduceOnly, bool testOnly, int recvWindowMs, const std::string& positionSide, double stopPrice, const std::string& workingType) {
    // Build query
    using namespace std::chrono;
    long long ts = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count() + impl_->timeOffsetMs;

    std::ostringstream q;
    q << "symbol=" << symbol;
    q << "&side=" << side;
    q << "&type=" << type;
    q << "&quantity=" << std::fixed << std::setprecision(8) << quantity;
    if (type == "LIMIT") {
        q << "&price=" << std::fixed << std::setprecision(8) << price;
        q << "&timeInForce=" << tif;
    }
    if (type == "STOP_MARKET" || type == "TAKE_PROFIT_MARKET") {
        if (stopPrice > 0) q << "&stopPrice=" << std::fixed << std::setprecision(8) << stopPrice;
        if (!workingType.empty()) q << "&workingType=" << workingType;
    }
    if (!positionSide.empty()) q << "&positionSide=" << positionSide;
    if (reduceOnly) q << "&reduceOnly=true";
    q << "&recvWindow=" << recvWindowMs;
    q << "&timestamp=" << ts;

    std::string qs = q.str();
    std::string sig = impl_->hmac_sha256_hex(qs);
    qs += "&signature=" + sig;

    std::string endpoint = testOnly ? "/fapi/v1/order/test" : "/fapi/v1/order";
    // Send parameters in query string only (avoid duplicating in body to prevent signature mismatches)
    return impl_->https_request("POST", endpoint + "?" + qs, "", true, impl_->apiKey);
}

BinanceRest::Result BinanceRest::setLeverage(const std::string& symbol, int leverage) {
    using namespace std::chrono;
    long long ts = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count() + impl_->timeOffsetMs;
    std::ostringstream q;
    q << "symbol=" << symbol << "&leverage=" << leverage << "&timestamp=" << ts;
    std::string qs = q.str();
    qs += "&signature=" + impl_->hmac_sha256_hex(qs);
    return impl_->https_request("POST", "/fapi/v1/leverage?" + qs, "", true, impl_->apiKey);
}

BinanceRest::Result BinanceRest::setMarginType(const std::string& symbol, const std::string& marginType) {
    using namespace std::chrono;
    long long ts = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count() + impl_->timeOffsetMs;
    std::ostringstream q;
    q << "symbol=" << symbol << "&marginType=" << marginType << "&timestamp=" << ts;
    std::string qs = q.str();
    qs += "&signature=" + impl_->hmac_sha256_hex(qs);
    return impl_->https_request("POST", "/fapi/v1/marginType?" + qs, "", true, impl_->apiKey);
}

BinanceRest::Result BinanceRest::setDualPosition(bool enable) {
    using namespace std::chrono;
    long long ts = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count() + impl_->timeOffsetMs;
    std::ostringstream q;
    q << "dualSidePosition=" << (enable?"true":"false") << "&timestamp=" << ts;
    std::string qs = q.str();
    qs += "&signature=" + impl_->hmac_sha256_hex(qs);
    return impl_->https_request("POST", "/fapi/v1/positionSide/dual?" + qs, "", true, impl_->apiKey);
}

BinanceRest::Result BinanceRest::cancelReplaceOrder(
    const std::string& symbol,
    long long cancelOrderId,
    const std::string& side,
    const std::string& type,
    double quantity,
    double price,
    const std::string& timeInForce,
    bool reduceOnly,
    const std::string& positionSide,
    const std::string& cancelReplaceMode,
    int recvWindowMs)
{
    using namespace std::chrono;
    long long ts = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count() + impl_->timeOffsetMs;
    std::ostringstream q;
    q << "symbol=" << symbol;
    q << "&cancelOrderId=" << cancelOrderId;
    q << "&side=" << side;
    q << "&type=" << type;
    q << std::fixed << std::setprecision(8);
    q << "&quantity=" << quantity;
    if (type == "LIMIT") {
        q << "&price=" << price;
        q << "&timeInForce=" << timeInForce;
    }
    if (!positionSide.empty()) q << "&positionSide=" << positionSide;
    if (reduceOnly) q << "&reduceOnly=true";
    q << "&cancelReplaceMode=" << cancelReplaceMode;
    q << "&recvWindow=" << recvWindowMs;
    q << "&timestamp=" << ts;
    std::string qs = q.str();
    qs += "&signature=" + impl_->hmac_sha256_hex(qs);
    return impl_->https_request("POST", "/fapi/v1/order/cancelReplace?" + qs, "", true, impl_->apiKey);
}
