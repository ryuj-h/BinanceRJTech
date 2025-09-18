#pragma once

#include <string>
#include <memory>

// Minimal Binance Futures REST client (sync) for placing orders
// Uses HTTPS (Boost.Beast + OpenSSL). API key/secret read from env:
//   BINANCE_API_KEY, BINANCE_API_SECRET

class BinanceRest {
public:
    struct Result {
        bool ok{false};
        int status{0};
        std::string body;  // JSON or error text
    };

    // baseHost: e.g., "fapi.binance.com" or "testnet.binancefuture.com"
    explicit BinanceRest(const std::string& baseHost);
    ~BinanceRest();

    // Simple order (MARKET/LIMIT/STOP_MARKET/TAKE_PROFIT_MARKET)
    Result placeOrder(
        const std::string& symbol,           // e.g., "BTCUSDT"
        const std::string& side,             // "BUY" or "SELL"
        const std::string& type,             // "MARKET","LIMIT","STOP_MARKET","TAKE_PROFIT_MARKET"
        double quantity,                     // base asset qty
        double price = 0.0,                  // for LIMIT
        const std::string& timeInForce = "GTC",
        bool reduceOnly = false,
        bool testOnly = true,                // POST /order/test when true
        int recvWindowMs = 5000,
        const std::string& positionSide = "", // "LONG"/"SHORT" when dual-side enabled
        double stopPrice = 0.0,               // for STOP_MARKET/TP_MARKET
        const std::string& workingType = ""   // "MARK_PRICE" or "CONTRACT_PRICE"
    );

    // Optional: ping server time to compute diff
    Result getServerTime();
    Result getAccountInfo(int recvWindowMs = 5000);
    Result getExchangeInfo(const std::string& symbol); // GET /fapi/v1/exchangeInfo?symbol=BTCUSDT
    // GET /fapi/v1/klines?symbol=BTCUSDT&interval=1m&startTime=...&endTime=...&limit=1500
    Result getKlines(
        const std::string& symbol,
        const std::string& interval,
        long long startTime = 0,
        long long endTime = 0,
        int limit = 500);

    // Orders/Fills
    Result getOpenOrders(const std::string& symbol, int recvWindowMs = 5000);
    Result getUserTrades(const std::string& symbol, int limit = 50, int recvWindowMs = 5000);
    // Public ticker price (e.g., "BNBUSDT")
    Result getTickerPrice(const std::string& symbol);

    // Cancel orders
    Result cancelOrder(
        const std::string& symbol,
        long long orderId = 0,
        const std::string& origClientOrderId = "",
        int recvWindowMs = 5000);
    Result cancelAllOpenOrders(const std::string& symbol, int recvWindowMs = 5000);
    // Order book snapshot (unsiged)
    Result getDepth(const std::string& symbol, int limit = 500);
    // Atomic cancel+replace for editing an existing order (Futures)
    // POST /fapi/v1/order/cancelReplace
    Result cancelReplaceOrder(
        const std::string& symbol,
        long long cancelOrderId,
        const std::string& side,
        const std::string& type,
        double quantity,
        double price,
        const std::string& timeInForce = "GTC",
        bool reduceOnly = false,
        const std::string& positionSide = "",
        const std::string& cancelReplaceMode = "STOP_ON_FAILURE",
        int recvWindowMs = 5000);

    // Configure API key/secret (fallback to env)
    void setCredentials(const std::string& apiKey, const std::string& apiSecret);

    // Insecure mode (skip TLS peer verification). Default false.
    void setInsecureTLS(bool v);

    // Futures account configuration
    Result setLeverage(const std::string& symbol, int leverage);            // POST /fapi/v1/leverage
    Result setMarginType(const std::string& symbol, const std::string& marginType); // CROSS/ISOLATED
    Result setDualPosition(bool enable);                                     // POST /fapi/v1/positionSide/dual

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

