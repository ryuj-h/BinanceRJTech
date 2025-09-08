// Dear ImGui visualization entry point

#include "WebSocket.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <iostream>
#include <algorithm>
#include <unordered_map>
#include <map>
#include <cmath>
#include <limits>
#include <deque>
#include <tuple>
#include <utility>
#include <cstdlib>
#include <future>
#include <condition_variable>
#include <set>
#include <ctime>
#include <climits>
#include <climits>

#include <nlohmann/json.hpp>

// Win32 + D3D11 + Dear ImGui
#include <windows.h>
#include <tchar.h>
#include <d3d11.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

#include "third_party/imgui/imgui.h"
#include "third_party/imgui/backends/imgui_impl_win32.h"
#include "third_party/imgui/backends/imgui_impl_dx11.h"
#include "BinanceRest.hpp"

// Shared state for visualization
static std::atomic<int> messageCount{0};
static std::atomic<int> lastMessageCount{0};
static std::mutex bookMutex;

struct Level { double price; double qty; };
static std::vector<Level> g_bids;
static std::vector<Level> g_asks;
// Full book aggregates (apply diffs here, render top-N in UI)
static std::map<double, double, std::greater<double>> g_bookBids; // highest price first
static std::map<double, double, std::less<double>>    g_bookAsks; // lowest price first

// Public trades buffer (moved to file scope so both receiver and UI can access)
struct PubTrade { double price; double qty; long long ts; bool isBuy; };
static std::vector<PubTrade> g_trades;
static std::mutex tradesMutex;

// ==== Chart state (candlesticks) ====
struct Candle { long long t0; long long t1; double o; double h; double l; double c; double v; };
static std::vector<Candle> g_candles;
static std::mutex g_candlesMutex;
static std::string g_chartSymbol = "BTCUSDT";
static std::string g_chartInterval = "1m";
static bool g_chartLoading = false;
static bool g_chartLive = true;
static bool g_showChartWin = true;
static std::atomic<bool> g_chartStreamRunning{false};
static std::atomic<double> g_lastTradePrice{0.0};
// Global fee rates for cross-feature usage (updated by account poller)
static std::atomic<double> g_takerRate{0.0005};
static std::atomic<double> g_makerRate{0.0002};

// Positions overlay for chart (symbol, amount, entry)
static std::vector<std::tuple<std::string,double,double>> g_posOverlay;
static std::mutex g_posOverlayMutex;

// Forward decl
static void RenderChartWindow();
static void StartOrRestartKlineStream(const std::string& symbolLower, const std::string& interval);
static void StartOrRestartAggTradeStream(const std::string& symbolLower);

// Worker: connect and receive messages from Binance
static void receiveOrderBook(const std::string& host, const std::string& port, int id) {
    try {
        WebSocket ws(host, port);
        ws.connect();
        // Subscribe to full diff depth (not limited to 20 levels)
        ws.send("{\"method\":\"SUBSCRIBE\",\"params\":[\"btcusdt@depth@100ms\"],\"id\":" + std::to_string(id) + "}");

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
                            bids.push_back(Level{price, qty});
                        }
                    }
                    if ((*payload).contains("a") && (*payload)["a"].is_array()) {
                        for (const auto& v : (*payload)["a"]) {
                            if (!v.is_array() || v.size() < 2) continue;
                            const auto& p = v[0];
                            const auto& q = v[1];
                            double price = p.is_string() ? std::stod(p.get<std::string>()) : p.get<double>();
                            double qty   = q.is_string() ? std::stod(q.get<std::string>()) : q.get<double>();
                            asks.push_back(Level{price, qty});
                        }
                    }
                    // Apply diffs to aggregate book (qty == 0 removes the level)
                    {
                        std::lock_guard<std::mutex> lock(bookMutex);
                        const double priceTick = 0.1;  // quantize price to 0.1 tick
                        auto pquant = [&](double p){ return std::round(p/priceTick)*priceTick; };

                        for (const auto& b : bids) {
                            double qp = pquant(b.price);
                            double qq = b.qty; // keep true size, including < 0.1
                            if (qq <= 0.0) g_bookBids.erase(qp); else g_bookBids[qp] = qq;
                        }
                        for (const auto& a : asks) {
                            double qp = pquant(a.price);
                            double qq = a.qty; // keep true size, including < 0.1
                            if (qq <= 0.0) g_bookAsks.erase(qp); else g_bookAsks[qp] = qq;
                        }
                        // Optional: cap extreme map sizes to avoid unbounded growth
                        const size_t cap = 1000;
                        if (g_bookBids.size() > cap) { auto it = std::next(g_bookBids.begin(), (long)cap); g_bookBids.erase(it, g_bookBids.end()); }
                        if (g_bookAsks.size() > cap) { auto it = std::next(g_bookAsks.begin(), (long)cap); g_bookAsks.erase(it, g_bookAsks.end()); }
                    }
                    messageCount.fetch_add(1, std::memory_order_relaxed);
                } catch (...) {
                    // ignore parse errors
                }
            } else {
sleep_short:
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    } catch (const std::exception& ex) {
        std::cerr << "Worker error: " << ex.what() << std::endl;
    }
}

// Receive public trades and keep small ring buffer
static void receivePublicTrades(const std::string& host, const std::string& port, const std::string& symbolLower)
{
    try {
        WebSocket ws(host, port);
        ws.connect();
        std::string sub = std::string("{\"method\":\"SUBSCRIBE\",\"params\":[\"") + symbolLower + "@trade\"],\"id\":99}";
        ws.send(sub);
        for (;;) {
            std::string msg = ws.receive();
            if (msg.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
            try {
                using nlohmann::json; json j = json::parse(msg, nullptr, false);
                if (j.is_discarded()) continue;
                const json* d = nullptr;
                if (j.contains("data")) d = &j["data"]; else d = &j;
                if (!d->is_object()) continue;
                double price=0, qty=0; long long ts=0; bool isBuy=true;
                if (d->contains("p")) price = std::stod((*d)["p"].get<std::string>());
                if (d->contains("q")) qty   = std::stod((*d)["q"].get<std::string>());
                if (d->contains("T")) ts    = (*d)["T"].get<long long>();
                if (d->contains("m")) { bool m = (*d)["m"].get<bool>(); isBuy = !m; }
                if (price>0 && qty>0) {
                    std::lock_guard<std::mutex> lk(tradesMutex);
                    g_trades.push_back(PubTrade{price, qty, ts, isBuy});
                    // Time-based retention to avoid dropping trades within current candle
                    long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    const long long keep_ms = 10LL * 60LL * 1000LL; // keep last 10 minutes
                    long long cutoff = now_ms - keep_ms;
                    size_t cutIdx = 0;
                    while (cutIdx < g_trades.size() && g_trades[cutIdx].ts < cutoff) ++cutIdx;
                    if (cutIdx > 0) g_trades.erase(g_trades.begin(), g_trades.begin() + (std::ptrdiff_t)cutIdx);
                }
            } catch (...) {}
        }
    } catch (...) { /* ignore */ }
}

// ===== ImGui + D3D11 integration =====
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

static void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer = nullptr;
    if (SUCCEEDED(g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer))) && pBackBuffer)
    {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

static void CleanupRenderTarget()
{
    if (g_mainRenderTargetView)
    {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}


static bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    
    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
    {
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
            featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    }
    if (res != S_OK)
        return false;
    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    switch (msg)
    {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

static void RenderOrderBookUI()
{
    // State and UI rendering

    ImGui::Begin("Order Book - BTCUSDT");
    ImGui::Text("Updates/sec: %d", lastMessageCount.load());
    ImGui::Separator();
    static bool s_showBookSettings = false;
    static bool s_showTradingWin = true;
    static bool s_showPositionsWin = true;
    ImGui::Checkbox("Show Settings", &s_showBookSettings);
    ImGui::SameLine();
    ImGui::Checkbox("Show Trading", &s_showTradingWin);
    ImGui::SameLine();
    ImGui::Checkbox("Show Positions", &s_showPositionsWin);
    ImGui::SameLine();
    ImGui::Checkbox("Show Chart", &g_showChartWin);
    ImGui::Separator();

    // Base quantity (bar unit). Default 20, Auto ON by default
    static float baseQty = 20.0f; // default unit
    static bool autoBase = true;
    if (baseQty < 0.000001f) baseQty = 0.000001f;

    // No animation: render directly at target positions and sizes
    ImGuiIO& io = ImGui::GetIO();
    const float dt = std::max(0.0f, io.DeltaTime);

    // Layout: vertical stack (Asks on top, controls, Bids below)
    const float full_w = ImGui::GetContentRegionAvail().x;
    const float row_h = ImGui::GetTextLineHeightWithSpacing() * 1.2f;
    static int displayLevels = 20;
    displayLevels = std::max(5, std::min(displayLevels, 200));
    const float half_h = std::max(10, std::min(displayLevels, 60)) * row_h + 10.0f;

    // No per-row or per-price animation state retained
    // Controls for N-seconds-ago average visual (shared across panes)
    static float s_lagSec = 5.0f;   // seconds ago
    static float s_winSec = 0.5f;   // averaging window width

    // Shared trading UI state (accessible from multiple windows)
    static char t_sym[32] = "BTCUSDT";
    static float t_orderQty = 0.001f;
    static float t_limitPrice = 0.0f;
    static float t_stopPrice = 0.0f;
    static int t_orderTypeIdx = 0; // 0=MARKET,1=LIMIT,2=STOP_MARKET,3=TAKE_PROFIT_MARKET
    static int t_tifIdx = 0; // 0=GTC,1=IOC,2=FOK
    static bool t_reduceOnly = false;
    static bool t_dualSide = false; // hedge mode
    static int t_leverage = 20;
    static int t_marginTypeIdx = 0; // 0=CROSS 1=ISOLATED
    static std::string t_lastOrderResp;

    // Filters and sizing helpers
    static double s_qtyStep = 0.001;   // LOT_SIZE/MARKET_LOT_SIZE.stepSize
    static double s_priceTick = 0.1;   // PRICE_FILTER.tickSize
    static double s_minQty = 0.0;
    static double s_availableUSDT = 0.0;   // availableBalance
    static double s_marginBalanceUSDT = 0.0; // marginBalance
    static double s_takerRate = 0.0005;    // taker commission rate (default)
    static double s_makerRate = 0.0002;    // maker commission rate (default)
    static float  s_sizePct = 10.0f;       // percent of margin balance to use
    static bool   s_useLeverageForSize = true;
    static bool   s_sizeForLong = true;    // reference side for auto sizing
    static std::string s_filtersMsg;
    // Hotkey settings
    static bool s_hkQuickLimit = true;   // enable Ctrl+J/K
    static bool s_hkEmergency = true;    // enable Ctrl+X flatten
    static std::vector<std::tuple<std::string,double,double,int,double,std::string,std::string,double>> s_positions; // symbol, amt, entry, lev, upnl, marginType, side, mark
    static std::mutex s_positionsMutex;

    static std::unique_ptr<BinanceRest> s_rest(new BinanceRest("fapi.binance.com"));
    if (s_rest) s_rest->setInsecureTLS(false);

    // Background poller to refresh positions/balances (faster cadence)
    static bool s_posPollerStarted = false;
    if (!s_posPollerStarted) {
        s_posPollerStarted = true;
        std::thread([&]{
            try {
                BinanceRest rest("fapi.binance.com");
                rest.setInsecureTLS(false);
                for (;;) {
                    auto r = rest.getAccountInfo(5000);
                    if (r.ok) {
                        try {
                            using nlohmann::json; auto j = json::parse(r.body, nullptr, false);
                            auto getd = [](const nlohmann::json& v)->double { if (v.is_string()) return std::stod(v.get<std::string>()); else return v.get<double>(); };
                            double avail = s_availableUSDT, margin = s_marginBalanceUSDT;
                            double taker = s_takerRate, maker = s_makerRate;
                            std::vector<std::tuple<std::string,double,double,int,double,std::string,std::string,double>> pos;
                            if (j.is_object()) {
                                if (j.contains("assets") && j["assets"].is_array()) {
                                    for (auto& a : j["assets"]) {
                                        if (!a.is_object() || !a.contains("asset")) continue;
                                        if (a["asset"].get<std::string>() == "USDT") {
                                            if (a.contains("availableBalance")) avail = getd(a["availableBalance"]);
                                            if (a.contains("marginBalance"))    margin = getd(a["marginBalance"]);
                                        }
                                    }
                                }
                                if (j.contains("takerCommissionRate")) taker = getd(j["takerCommissionRate"]);
                                if (j.contains("makerCommissionRate")) maker = getd(j["makerCommissionRate"]);
                                if (j.contains("positions") && j["positions"].is_array()) {
                                    for (auto& p : j["positions"]) {
                                        if (!p.is_object()) continue;
                                        std::string sym = p.contains("symbol") ? p["symbol"].get<std::string>() : std::string();
                                        double amt   = p.contains("positionAmt") ? getd(p["positionAmt"]) : 0.0;
                                        if (std::abs(amt) < 1e-12) continue;
                                        double entry = p.contains("entryPrice") ? getd(p["entryPrice"]) : 0.0;
                                        int lev      = p.contains("leverage") ? (p["leverage"].is_string()? std::stoi(p["leverage"].get<std::string>()) : p["leverage"].get<int>()) : 0;
                                        double upnl  = p.contains("unrealizedProfit") ? getd(p["unrealizedProfit"]) : 0.0;
                                        std::string mtype = p.contains("marginType") ? p["marginType"].get<std::string>() : std::string();
                                        std::string pside = p.contains("positionSide") ? p["positionSide"].get<std::string>() : std::string();
                                        double mark  = p.contains("markPrice") ? getd(p["markPrice"]) : 0.0;
                                        pos.emplace_back(sym, amt, entry, lev, upnl, mtype, pside, mark);
                                    }
                                }
                            }
                            {
                                std::lock_guard<std::mutex> lk(s_positionsMutex);
                                s_availableUSDT = avail;
                                s_marginBalanceUSDT = margin;
                                s_takerRate = taker;
                                s_makerRate = maker;
                                g_takerRate.store(taker, std::memory_order_relaxed);
                                g_makerRate.store(maker, std::memory_order_relaxed);
                                s_positions.swap(pos);
                            }
                            // Publish lightweight overlay for chart
                            std::vector<std::tuple<std::string,double,double>> ov;
                            for (auto &t : s_positions) {
                                const std::string& sym = std::get<0>(t);
                                double amt = std::get<1>(t);
                                double entry = std::get<2>(t);
                                if (std::abs(amt) > 1e-12 && entry > 0.0)
                                    ov.emplace_back(sym, amt, entry);
                            }
                            {
                                std::lock_guard<std::mutex> gk(g_posOverlayMutex);
                                g_posOverlay.swap(ov);
                            }
                        } catch (...) {}
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            } catch (...) {}
        }).detach();
    }

    // Public Trades window toggle
    static bool s_showTradesWin = true;

    auto draw_side = [&](const char*, const std::vector<Level>&, bool, ImU32, ImU32){};

    // Colors for bars
    const ImU32 colBid = IM_COL32(80, 200, 120, 255);
    const ImU32 colBidBg = IM_COL32(30, 80, 50, 140);
    const ImU32 colAsk = IM_COL32(220, 90, 90, 255);
    const ImU32 colAskBg = IM_COL32(90, 35, 35, 140);

    // Build contiguous price ladders at 0.1 tick, filling missing with qty=0
    // Use overscan: render more rows than visible for smooth global scroll
    std::vector<Level> asks, bids;
    {
        std::lock_guard<std::mutex> lock(bookMutex);
        const double priceTick = 0.1;
        auto pquant = [&](double p){ return std::round(p/priceTick)*priceTick; };

        static double lastBestAsk = std::numeric_limits<double>::quiet_NaN();
        static double lastBestBid = std::numeric_limits<double>::quiet_NaN();

        double bestAsk = std::numeric_limits<double>::quiet_NaN();
        double bestBid = std::numeric_limits<double>::quiet_NaN();
        if (!g_bookAsks.empty()) bestAsk = g_bookAsks.begin()->first;
        if (!g_bookBids.empty()) bestBid = g_bookBids.begin()->first;

        if (!std::isnan(bestAsk)) lastBestAsk = bestAsk;
        if (!std::isnan(bestBid)) lastBestBid = bestBid;

        if (std::isnan(bestAsk)) {
            if (!std::isnan(lastBestAsk)) bestAsk = lastBestAsk;
            else if (!std::isnan(lastBestBid)) bestAsk = lastBestBid + priceTick;
            else bestAsk = 0.0;
        }
        if (std::isnan(bestBid)) {
            if (!std::isnan(lastBestBid)) bestBid = lastBestBid;
            else if (!std::isnan(bestAsk)) bestBid = bestAsk - priceTick;
            else bestBid = 0.0;
        }

        bestAsk = pquant(bestAsk);
        bestBid = pquant(bestBid);

        const int overscan = std::max(10, displayLevels); // extra rows above/below
        const int renderLevels = displayLevels + overscan; // per side

        // Asks: from best ask upwards
        for (int i = 0; i < renderLevels; ++i) {
            double price = pquant(bestAsk + i * priceTick);
            double qty = 0.0;
            auto it = g_bookAsks.find(price);
            if (it != g_bookAsks.end()) qty = it->second;
            asks.push_back({price, qty});
        }
        // Bids: from best bid downwards
        for (int i = 0; i < renderLevels; ++i) {
            double price = pquant(bestBid - i * priceTick);
            double qty = 0.0;
            auto it = g_bookBids.find(price);
            if (it != g_bookBids.end()) qty = it->second;
            bids.push_back({price, qty});
        }
    }

    // Global scroll offset: move the whole ladder smoothly when mid price tick changes
    static bool s_midInit = false;
    static int  s_prevMidTick = 0;
    static float s_scrollOffset = 0.0f; // current pixels, added to all row y positions
    static float s_scrollTarget = 0.0f; // desired offset
    // Mid-price history for visualizing "n seconds ago average"
    static std::deque<std::pair<double,double>> s_midHist; // {timeSec, midPrice}
    const double priceTick = 0.1;
    // Derive current mid tick from current bests (fallback to last seen values in build block)
    double curBestAsk = asks.empty() ? 0.0 : asks[0].price; // nearest ask above center is roughly best ask
    double curBestBid = bids.empty() ? 0.0 : bids[0].price; // nearest bid below center is roughly best bid
    // Record mid price sample
    double nowSec = ImGui::GetTime();
    if (curBestAsk > 0.0 && curBestBid > 0.0) {
        double mid = 0.5 * (curBestAsk + curBestBid);
        if (s_midHist.empty() || s_midHist.back().first < nowSec) s_midHist.emplace_back(nowSec, mid);
        // keep last 10 minutes
        const double keepSec = 600.0;
        while (!s_midHist.empty() && (nowSec - s_midHist.front().first) > keepSec) s_midHist.pop_front();
    }
    int curMidTick = (int)std::llround(((curBestAsk + curBestBid) * 0.5) / priceTick);
    if (!s_midInit) {
        s_prevMidTick = curMidTick;
        s_midInit = true;
    } else {
        int d = curMidTick - s_prevMidTick;
        if (d != 0) {
            // Positive d (mid up) -> scroll content up; small per-tick shift (fraction of row)
            const float stepFrac = 0.25f; // move quarter-row per tick change
            s_scrollTarget += (float)(-d) * (row_h * stepFrac);
            // keep target bounded so motion stays subtle
            const float maxAbs = row_h * 0.9f;
            if (s_scrollTarget >  maxAbs) s_scrollTarget =  maxAbs;
            if (s_scrollTarget < -maxAbs) s_scrollTarget = -maxAbs;
            s_prevMidTick = curMidTick;
        }
    }
    // Move current offset toward target quickly for snappy feel
    const float catchupSpeed = row_h * 60.0f; // fast response (pixels/sec)
    if (s_scrollOffset < s_scrollTarget) {
        s_scrollOffset = std::min(s_scrollTarget, s_scrollOffset + catchupSpeed * dt);
    } else if (s_scrollOffset > s_scrollTarget) {
        s_scrollOffset = std::max(s_scrollTarget, s_scrollOffset - catchupSpeed * dt);
    }

    // Order book width uses full content; settings live in separate window
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float book_w = std::max(200.0f, avail.x);

    // Left: book (fixed visible height; we render with overscan inside)
    const float panelH = (displayLevels * 2) * row_h + 8.0f;
    ImGui::BeginChild("BookPanel", ImVec2(book_w, panelH), true);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        const int kSegments = 10;
        const float margin = 10.0f;
        const float segW = (book_w - margin) / (float)kSegments;

        float centerY = p0.y + (float)displayLevels * row_h + 4.0f;
        // center divider
        dl->AddLine(ImVec2(p0.x, centerY - 2.0f), ImVec2(p0.x + book_w - 4.0f, centerY - 2.0f), IM_COL32(140,140,140,120), 1.0f);

        // Render helper for one side (no animation). Allow click-to-set limit price
        auto render_rows = [&](const std::vector<Level>& levels, int thisSide, ImU32 colBar, ImU32 colBarBg)
        {
            int count = (int)levels.size();
            for (int i = 0; i < count; ++i)
            {
                const Level& lv = levels[i];
                float y = (thisSide==0)
                    ? (centerY - (i + 1) * row_h)
                    : (centerY + i * row_h);
                y += s_scrollOffset;

                double ratio = std::max(0.0, lv.qty / (double)baseQty);
                ratio = std::min(ratio, (double)kSegments);
                int segsFull = (int)std::floor(ratio);
                float segFrac = (float)(ratio - segsFull);
                segsFull = std::max(0, std::min(segsFull, kSegments));

                ImVec2 rmin = ImVec2(p0.x, y);
                ImVec2 rmax = ImVec2(p0.x + book_w - 6.0f, y + row_h - 2.0f);
                dl->AddRectFilled(rmin, rmax, colBarBg, 3.0f);
                // Click to set Trading limit price
                if (ImGui::IsMouseHoveringRect(rmin, rmax) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    t_orderTypeIdx = 1; // LIMIT
                    t_limitPrice = (float)lv.price;
                }
                for (int s = 0; s < segsFull; ++s) {
                    ImVec2 smin = ImVec2(p0.x + s * segW, y);
                    ImVec2 smax = ImVec2(p0.x + (s + 1) * segW - 2.0f, y + row_h - 2.0f);
                    dl->AddRectFilled(smin, smax, colBar, 3.0f);
                }
                if (segsFull < kSegments && segFrac > 0.0f) {
                    ImVec2 pmin2 = ImVec2(p0.x + segsFull * segW, y);
                    ImVec2 pmax2 = ImVec2(p0.x + segsFull * segW + segFrac * segW - 2.0f, y + row_h - 2.0f);
                    dl->AddRectFilled(pmin2, pmax2, colBar, 3.0f);
                }

                char buf[160];
                snprintf(buf, sizeof(buf), "%.2f  @ %.2f  (x%.2f)", lv.price, lv.qty, (float)ratio);
                dl->AddText(ImVec2(p0.x + 6.0f, y + 2.0f), IM_COL32(230,230,230,255), buf);
                if (ImGui::IsMouseHoveringRect(rmin, rmax)) {
                    ImGui::SetTooltip("Click to set limit price: %.2f", lv.price);
                }
            }
        };

        // draw asks (0) then bids (1)
        render_rows(asks, 0, colAsk, colAskBg);
        render_rows(bids, 1, colBid, colBidBg);

        // Visual: mid-price position from N seconds ago (exact, with linear interpolation)
        double targetT = nowSec - (double)s_lagSec;
        bool havePos = false; double posPrice = 0.0;
        if (!s_midHist.empty()) {
            if (targetT <= s_midHist.front().first) {
                posPrice = s_midHist.front().second; havePos = true;
            } else if (targetT >= s_midHist.back().first) {
                posPrice = s_midHist.back().second; havePos = true;
            } else {
                for (size_t i = 1; i < s_midHist.size(); ++i) {
                    if (s_midHist[i].first >= targetT) {
                        double t0 = s_midHist[i-1].first, t1 = s_midHist[i].first;
                        double v0 = s_midHist[i-1].second, v1 = s_midHist[i].second;
                        double u = (t1 > t0) ? (targetT - t0) / (t1 - t0) : 0.0;
                        posPrice = v0 + (v1 - v0) * u;
                        havePos = true;
                        break;
                    }
                }
            }
        }
        if (havePos && curBestAsk > 0.0 && curBestBid > 0.0) {
            // Map position price to y within current ladder
            float yPos = centerY;
            const float yTop = p0.y;
            const float yBot = p0.y + panelH;
            if (posPrice >= curBestAsk) {
                double ticks = (posPrice - curBestAsk) / priceTick;
                int it = (int)std::floor(ticks);
                double frac = ticks - it;
                yPos = centerY - (it + 1) * row_h - (float)frac * row_h + s_scrollOffset;
            } else if (posPrice <= curBestBid) {
                double ticks = (curBestBid - posPrice) / priceTick;
                int it = (int)std::floor(ticks);
                double frac = ticks - it;
                yPos = centerY + it * row_h + (float)frac * row_h + s_scrollOffset;
            } else {
                yPos = centerY + s_scrollOffset; // inside spread
            }
            if (yPos > yTop && yPos < yBot) {
                ImU32 col = IM_COL32(255, 200, 60, 210);
                dl->AddLine(ImVec2(p0.x, yPos), ImVec2(p0.x + book_w - 6.0f, yPos), col, 2.0f);
                char lab[64];
                snprintf(lab, sizeof(lab), "T-%.1fs: %.2f", (double)s_lagSec, posPrice);
                ImVec2 ts = ImGui::CalcTextSize(lab);
                ImVec2 bx0 = ImVec2(p0.x + book_w - ts.x - 10.0f, yPos - ts.y - 2.0f);
                ImVec2 bx1 = ImVec2(p0.x + book_w - 6.0f, yPos + 2.0f);
                dl->AddRectFilled(bx0, bx1, IM_COL32(55, 40, 20, 220), 3.0f);
                dl->AddText(ImVec2(bx1.x - ts.x - 2.0f, yPos - ts.y), IM_COL32(240,240,240,255), lab);
            }
        }

        // Push dummy height to keep child tall
        ImGui::Dummy(ImVec2(0, centerY - p0.y + (float)bids.size()*row_h + 8.0f));
    }
    ImGui::EndChild();
    ImGui::End();

    // Separate: Order Book Settings window
    if (s_showBookSettings) {
        ImGui::SetNextWindowSize(ImVec2(420, 260), ImGuiCond_FirstUseEver);
        ImGui::Begin("Order Book Settings", &s_showBookSettings);
        if (ImGui::BeginTable("BookSettingsTable", 2, ImGuiTableFlags_Resizable|ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 150.0f);
            ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch);

            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("Updates/sec");
            ImGui::TableSetColumnIndex(1); ImGui::Text("%d", lastMessageCount.load());

            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("Auto Base");
            ImGui::TableSetColumnIndex(1); ImGui::Checkbox("##AutoBase", &autoBase);

            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("Base Qty");
            ImGui::TableSetColumnIndex(1); if (autoBase) ImGui::BeginDisabled();
            ImGui::SetNextItemWidth(-FLT_MIN); ImGui::SliderFloat("##BaseQty", &baseQty, 0.0001f, 100000.0f, "%.6f", ImGuiSliderFlags_Logarithmic);
            if (autoBase) ImGui::EndDisabled();

            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("Levels");
            ImGui::TableSetColumnIndex(1); ImGui::SetNextItemWidth(-FLT_MIN); ImGui::SliderInt("##Levels", &displayLevels, 5, 200);

            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("Lag (s)");
            ImGui::TableSetColumnIndex(1); ImGui::SetNextItemWidth(-FLT_MIN); ImGui::SliderFloat("##Lag", &s_lagSec, 0.1f, 60.0f, "%.1f");

            ImGui::EndTable();
        }
        ImGui::End();
    }

    // Auto adjust independent of settings visibility
    if (autoBase) {
        double maxQty = 0.0;
        for (const auto& v : bids) maxQty = std::max(maxQty, v.qty);
        for (const auto& v : asks) maxQty = std::max(maxQty, v.qty);
        const int kSegments = 10; const double targetFill = 0.90 * kSegments;
        if (maxQty > 0.0) {
            double targetBase = std::max(0.0001, maxQty / targetFill);
            float a = std::min(1.0f, dt * 2.5f);
            baseQty = baseQty + (float(targetBase) - baseQty) * a;
        }
    }

    // Separate: Public Trades window
    if (s_showTradesWin) {
        ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_FirstUseEver);
        ImGui::Begin("Public Trades", &s_showTradesWin);
        std::vector<PubTrade> local;
        {
            std::lock_guard<std::mutex> lk(tradesMutex);
            local = g_trades;
        }
        if (ImGui::BeginTable("TradesTable", 4, ImGuiTableFlags_RowBg|ImGuiTableFlags_Borders|ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 200.0f);
            ImGui::TableSetupColumn("Side", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("Price", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Qty", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (int i = (int)local.size()-1; i >= 0; --i) {
                const auto& t = local[(size_t)i];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char tb[64];
                time_t sec = (time_t)(t.ts / 1000);
                int ms = (int)(t.ts % 1000);
                struct tm tmv{};
#if defined(_WIN32)
                localtime_s(&tmv, &sec);
#else
                tmv = *std::localtime(&sec);
#endif
                char dtb[48]; strftime(dtb, sizeof(dtb), "%Y-%m-%d %H:%M:%S", &tmv);
                snprintf(tb, sizeof(tb), "%s.%03d", dtb, ms);
                ImGui::TextUnformatted(tb);
                ImGui::TableSetColumnIndex(1);
                ImVec4 col = t.isBuy?ImVec4(0.2f,1.0f,0.4f,1.0f):ImVec4(1.0f,0.3f,0.3f,1.0f);
                ImGui::TextColored(col, t.isBuy?"BUY":"SELL");
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.2f", t.price);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%.6f", t.qty);
            }
            ImGui::EndTable();
        }
        ImGui::End();
    }

    // Separate: Trading window
    // Separate: Trading window (modernized order panel)
    if (s_showTradingWin) {
        ImGui::SetNextWindowSize(ImVec2(460, 640), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(360, 420), ImVec2(900, 1100));
        ImGui::Begin("Trade Panel", &s_showTradingWin, ImGuiWindowFlags_NoCollapse);

        // Shared trading state aliases
        char* sym = t_sym;
        float& orderQty = t_orderQty;
        float& limitPrice = t_limitPrice;
        float& stopPrice = t_stopPrice;
        int& orderTypeIdx = t_orderTypeIdx; // 0=MARKET,1=LIMIT,2=STOP_MARKET,3=TAKE_PROFIT_MARKET
        int& tifIdx = t_tifIdx;             // 0=GTC,1=IOC,2=FOK
        bool& reduceOnly = t_reduceOnly;
        bool& dualSide = t_dualSide;
        int& leverage = t_leverage;
        int& marginTypeIdx = t_marginTypeIdx; // 0=CROSS 1=ISOLATED
        std::string& lastOrderResp = t_lastOrderResp;
        const char* types[] = {"MARKET","LIMIT","STOP_MARKET","TAKE_PROFIT_MARKET"};
        const char* tifs[] = {"GTC","IOC","FOK"};
        const char* margins[] = {"CROSS","ISOLATED"};
        const char* workingTypes[] = {"MARK_PRICE","CONTRACT_PRICE"};
        static int workingTypeIdx = 0; // for stop/TP

        // New UX state
        static bool amtInQuote = true;        // USDT-based sizing
        static float quoteNotional = 100.0f;  // USDT amount when amtInQuote
        static float sizePct = 10.0f;         // quick percent presets/slider
        static bool attachTP = false, attachSL = false;
        static float tpOffsetPct = 0.5f;      // % away from ref price
        static float slOffsetPct = 0.5f;
        static bool confirmBeforeSend = false;
        static bool s_sizeRefLong = true;     // sizing reference side (Long->Ask, Short->Bid)

        // Helpers
        auto floor_step = [](double v, double step)->double { if (step <= 0) return v; double n = std::floor((v + 1e-12) / step); return n * step; };
        auto best_prices = []()->std::pair<double,double> {
            std::lock_guard<std::mutex> lk(bookMutex);
            double ask = 0.0, bid = 0.0;
            if (!g_bookAsks.empty()) ask = g_bookAsks.begin()->first;
            if (!g_bookBids.empty()) bid = g_bookBids.begin()->first;
            return {ask,bid};
        };

        // Header: symbol, account, quick toggles
        static int s_lastHttpStatus = 0; static std::string s_lastHttpBody;
        ImGui::Text("Symbol");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140); ImGui::InputText("##sym", sym, sizeof(t_sym));
        ImGui::SameLine();
        if (ImGui::Button("Refresh Filters/Bal")) {
            if (s_rest) {
                // Exchange filters
                auto r1 = s_rest->getExchangeInfo(sym);
                s_lastHttpStatus = r1.status; s_lastHttpBody = r1.body;
                if (r1.ok) {
                    try {
                        using nlohmann::json; auto j = json::parse(r1.body, nullptr, false);
                        if (j.is_object() && j.contains("symbols") && j["symbols"].is_array() && !j["symbols"].empty()) {
                            auto s = j["symbols"][0];
                            if (s.contains("filters") && s["filters"].is_array()) {
                                double tick = s_priceTick, step = s_qtyStep, minq = s_minQty;
                                for (auto& f : s["filters"]) {
                                    if (!f.is_object() || !f.contains("filterType")) continue;
                                    std::string ft = f["filterType"].get<std::string>();
                                    auto getd = [](const nlohmann::json& v)->double { if (v.is_string()) return std::stod(v.get<std::string>()); else return v.get<double>(); };
                                    if (ft == "PRICE_FILTER") {
                                        if (f.contains("tickSize")) tick = getd(f["tickSize"]);
                                    } else if (ft == "LOT_SIZE") {
                                        if (f.contains("stepSize")) step = getd(f["stepSize"]);
                                        if (f.contains("minQty"))  minq = getd(f["minQty"]);
                                    }
                                }
                                s_priceTick = tick; s_qtyStep = step; s_minQty = minq;
                                s_filtersMsg = "Loaded filters: tick=" + std::to_string(s_priceTick) + ", step=" + std::to_string(s_qtyStep) + ", minQty=" + std::to_string(s_minQty);
                            }
                        }
                    } catch (...) {}
                } else {
                    s_filtersMsg = std::string("exchangeInfo ERR ") + std::to_string(r1.status);
                }
                // Account
                auto r2 = s_rest->getAccountInfo(5000);
                s_lastHttpStatus = r2.status; s_lastHttpBody = r2.body;
                if (r2.ok) {
                    try {
                        using nlohmann::json; auto j = json::parse(r2.body, nullptr, false);
                        auto getd = [](const nlohmann::json& v)->double { if (v.is_string()) return std::stod(v.get<std::string>()); else return v.get<double>(); };
                        if (j.is_object()) {
                            if (j.contains("assets") && j["assets"].is_array()) {
                                for (auto& a : j["assets"]) {
                                    if (!a.is_object() || !a.contains("asset")) continue;
                                    if (a["asset"].get<std::string>() == "USDT") {
                                        if (a.contains("availableBalance")) s_availableUSDT = getd(a["availableBalance"]);
                                        if (a.contains("marginBalance"))    s_marginBalanceUSDT = getd(a["marginBalance"]);
                                        break;
                                    }
                                }
                            }
                            if (j.contains("takerCommissionRate")) s_takerRate = getd(j["takerCommissionRate"]);
                            if (j.contains("makerCommissionRate")) s_makerRate = getd(j["makerCommissionRate"]);
                        }
                    } catch (...) {}
                }
            }
        }
        if (!s_filtersMsg.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", s_filtersMsg.c_str()); }
        // API diagnostics (last REST result)
        if (s_lastHttpStatus != 0) {
            ImGui::TextDisabled("API %s %d", (s_lastHttpStatus>=200&&s_lastHttpStatus<300)?"OK":"ERR", s_lastHttpStatus);
            if (s_lastHttpStatus < 200 || s_lastHttpStatus >= 300) {
                ImGui::BeginChild("apidiag", ImVec2(0, 80), true);
                std::string body = s_lastHttpBody; if (body.size()>400) body = body.substr(0,400) + "...";
                ImGui::TextWrapped("%s", body.c_str());
                ImGui::EndChild();
            }
        }

        // Account summary row
        ImGui::Separator();
        ImGui::Text("Avail %.2f USDT | Margin %.2f USDT", s_availableUSDT, s_marginBalanceUSDT);
        ImGui::SameLine(); ImGui::TextDisabled("Maker %.4f%% / Taker %.4f%%", s_makerRate*100.0, s_takerRate*100.0);

        // Mode row (compact): Hedge toggle only
        if (ImGui::Checkbox("Hedge", &dualSide)) { if (s_rest) (void)s_rest->setDualPosition(dualSide); }
        // Dedicated leverage slider row (1..125)
        ImGui::Text("Leverage");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderInt("##levSlider", &leverage, 1, 125, "%d x")) {
            leverage = std::max(1, std::min(leverage, 125));
            if (s_rest) (void)s_rest->setLeverage(sym, leverage);
        }

        ImGui::Separator();

        // Type tabs
        if (ImGui::BeginTabBar("OrderTypeTabs", ImGuiTabBarFlags_None)) {
            if (ImGui::BeginTabItem("Market")) { orderTypeIdx = 0; ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Limit"))  { orderTypeIdx = 1; ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Stop"))   { orderTypeIdx = 2; ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Take Profit")) { orderTypeIdx = 3; ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }

        // Price helpers
        auto [ask,bid] = best_prices();
        double mid = (ask>0 && bid>0) ? 0.5 * (ask + bid) : 0.0;
        static float prevLimitObserved = 0.0f;
        bool externalLimitChanged = (orderTypeIdx==1 && fabsf(prevLimitObserved - limitPrice) > 1e-6f);
        if (externalLimitChanged) { prevLimitObserved = limitPrice; }

        // Amount section
        ImGui::Text("Amount"); ImGui::SameLine();
        if (ImGui::RadioButton("USDT", amtInQuote)) amtInQuote = true;
        ImGui::SameLine();
        if (ImGui::RadioButton("Base", !amtInQuote)) amtInQuote = false;

        if (amtInQuote) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputFloat("##notional", &quoteNotional, 0.0f, 0.0f, "%.2f");
        } else {
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputFloat("##qty", &orderQty, 0.0f, 0.0f, "%.6f");
        }

        // Percent chips + slider (auto size by Available * pct * leverage)
        auto compute_qty_from_pct = [&](){
            double refPrice = 0.0;
            if (orderTypeIdx==1 && limitPrice>0) {
                refPrice = limitPrice; // explicit limit price wins
            } else {
                // Use side-specific reference: Long->Ask, Short->Bid
                if (s_sizeRefLong) refPrice = (ask>0? ask : (bid>0? bid:0.0));
                else               refPrice = (bid>0? bid : (ask>0? ask:0.0));
            }
            if (refPrice > 0.0) {
                double notional = s_availableUSDT * (sizePct/100.0f) * (s_useLeverageForSize? (double)leverage : 1.0);
                if (notional < 0) notional = 0;
                double q = notional / refPrice; q = floor_step(q, s_qtyStep); if (q < s_minQty) q = s_minQty;
                orderQty = (float)q;
            }
        };

        ImGui::TextDisabled("Quick % of margin");
        ImGui::SameLine(); if (ImGui::RadioButton("Ref: Long(Ask)", s_sizeRefLong)) { s_sizeRefLong = true; compute_qty_from_pct(); }
        ImGui::SameLine(); if (ImGui::RadioButton("Short(Bid)", !s_sizeRefLong)) { s_sizeRefLong = false; compute_qty_from_pct(); }
        for (float p : {10.f,25.f,50.f,75.f,100.f}) {
            ImGui::SameLine();
            bool sel = fabsf(sizePct - p) < 0.01f;
            if (sel) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(60,140,230,255));
            if (ImGui::SmallButton((std::to_string((int)p) + "%").c_str())) { sizePct = p; compute_qty_from_pct(); }
            if (sel) ImGui::PopStyleColor();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        bool pctChangedUI = ImGui::SliderFloat("##pctslider", &sizePct, 1.0f, 100.0f, "%.0f%%");
        if (pctChangedUI) compute_qty_from_pct();

        // Auto update qty when using USDT mode
        if (amtInQuote) {
            double refP;
            if (orderTypeIdx==1 && limitPrice>0) refP = limitPrice; else refP = (s_sizeRefLong ? (ask>0?ask:mid) : (bid>0?bid:mid));
            if (refP > 0 && quoteNotional > 0) {
                double q = (double)quoteNotional / refP; q = floor_step(q, s_qtyStep); if (q < s_minQty) q = s_minQty;
                orderQty = (float)q;
            }
        }
        // If limit price was changed externally (e.g., from order book click), refresh size from %
        if (externalLimitChanged) compute_qty_from_pct();
        // Re-compute qty when leverage slider changed
        static int lastLevApplied = leverage;
        if (lastLevApplied != leverage) { lastLevApplied = leverage; compute_qty_from_pct(); }

        // Price section for Limit/Stops
        if (orderTypeIdx==1) {
            ImGui::Separator(); ImGui::Text("Limit Price");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputFloat("##limitPx", &limitPrice, 0.0f, 0.0f, "%.2f")) compute_qty_from_pct();
            // quick picks
            if (ImGui::SmallButton("-tick")) { limitPrice = (float)floor_step(limitPrice - (float)s_priceTick, s_priceTick); compute_qty_from_pct(); }
            ImGui::SameLine(); if (ImGui::SmallButton("Bid")) { limitPrice = (float)floor_step((float)bid, s_priceTick); compute_qty_from_pct(); }
            ImGui::SameLine(); if (ImGui::SmallButton("Mid")) { limitPrice = (float)floor_step((float)mid, s_priceTick); compute_qty_from_pct(); }
            ImGui::SameLine(); if (ImGui::SmallButton("Ask")) { limitPrice = (float)floor_step((float)ask, s_priceTick); compute_qty_from_pct(); }
            ImGui::SameLine(); if (ImGui::SmallButton("+tick")) { limitPrice = (float)floor_step(limitPrice + (float)s_priceTick, s_priceTick); compute_qty_from_pct(); }
            ImGui::SameLine(); ImGui::TextDisabled("TIF"); ImGui::SameLine(); ImGui::SetNextItemWidth(100); ImGui::Combo("##tif", &tifIdx, tifs, IM_ARRAYSIZE(tifs));
        }
        if (orderTypeIdx==2 || orderTypeIdx==3) {
            ImGui::Separator(); ImGui::Text("Trigger (Stop) Price");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputFloat("##stopPx", &stopPrice, 0.0f, 0.0f, "%.2f");
            ImGui::TextDisabled("Working Type"); ImGui::SameLine(); ImGui::SetNextItemWidth(160); ImGui::Combo("##worktp", &workingTypeIdx, workingTypes, IM_ARRAYSIZE(workingTypes));
        }

        // Advanced options (moved Margin Type here to save space)
        if (ImGui::CollapsingHeader("Advanced", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Margin Type"); ImGui::SameLine();
            ImGui::SetNextItemWidth(140);
            if (ImGui::Combo("##mtype_adv", &marginTypeIdx, margins, IM_ARRAYSIZE(margins))) {
                if (s_rest) (void)s_rest->setMarginType(sym, margins[marginTypeIdx]);
            }
            ImGui::Checkbox("Reduce Only", &reduceOnly);
            ImGui::SameLine(); ImGui::Checkbox("Use Leverage in sizing", &s_useLeverageForSize);
            ImGui::Separator();
            ImGui::Checkbox("Attach Take Profit", &attachTP); ImGui::SameLine(); ImGui::SetNextItemWidth(120); ImGui::DragFloat("TP %", &tpOffsetPct, 0.05f, 0.1f, 10.0f, "%.2f%%");
            ImGui::Checkbox("Attach Stop Loss", &attachSL); ImGui::SameLine(); ImGui::SetNextItemWidth(120); ImGui::DragFloat("SL %", &slOffsetPct, 0.05f, 0.1f, 10.0f, "%.2f%%");
            ImGui::Checkbox("Confirm before send", &confirmBeforeSend);
        }

        // Live summary (yellow) + validation
        ImGui::Separator();
        double refPxDisp = 0.0; if (orderTypeIdx==0) refPxDisp = (ask>0 && bid>0)? (orderQty>0? (ask*0.5+bid*0.5) : mid) : mid; else if (orderTypeIdx==1) refPxDisp = limitPrice; else refPxDisp = stopPrice;
        if (!(refPxDisp>0)) refPxDisp = (ask>0 && bid>0)? (ask+bid)*0.5 : (ask>0?ask:bid);
        double notionalEst = (orderQty>0 && refPxDisp>0)? orderQty * refPxDisp : 0.0;
        double feeRate = (orderTypeIdx==1)? s_makerRate : s_takerRate;
        double feeEst = notionalEst * feeRate;
        ImGui::TextColored(ImVec4(1.0f,0.85f,0.2f,1.0f), "%s %s  qty=%.6f  @ %.2f  ~%.2f USDT (fee ~ %.2f)", (orderTypeIdx==0?"MARKET":(orderTypeIdx==1?"LIMIT":(orderTypeIdx==2?"STOP":"TP"))), sym, (double)orderQty, refPxDisp, notionalEst, feeEst);

        bool valid = true;
        if (std::string(sym).empty()) { ImGui::TextColored(ImVec4(1,0.7f,0,1), "Enter symbol."); valid = false; }
        if (orderQty <= 0.0f || orderQty < (float)s_minQty) { ImGui::TextColored(ImVec4(1,0.7f,0,1), "Quantity too small. Min=%.6f", s_minQty); valid = false; }
        if (orderTypeIdx==1 && limitPrice <= 0.0f) { ImGui::TextColored(ImVec4(1,0.7f,0,1), "Enter limit price."); valid = false; }
        if ((orderTypeIdx==2 || orderTypeIdx==3) && stopPrice <= 0.0f) { ImGui::TextColored(ImVec4(1,0.7f,0,1), "Enter trigger price."); valid = false; }

        // Actions
        auto send_order = [&](bool isLong){
            if (!s_rest) return;
            std::string side = isLong?"BUY":"SELL";
            std::string positionSide; if (dualSide) positionSide = isLong?"LONG":"SHORT";
            double qQty = floor_step(orderQty, s_qtyStep); if (qQty < s_minQty) qQty = s_minQty;
            double qPrice = (orderTypeIdx==1)? floor_step(limitPrice, s_priceTick) : 0.0;
            double qStop  = (orderTypeIdx==2 || orderTypeIdx==3)? floor_step(stopPrice, s_priceTick) : 0.0;
            const char* ot = types[orderTypeIdx];
            if (confirmBeforeSend) { ImGui::OpenPopup("ConfirmOrder"); }
            bool doSend = !confirmBeforeSend;
            if (ImGui::BeginPopupModal("ConfirmOrder", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("%s %s %.6f @ %.2f", side.c_str(), sym, qQty, (orderTypeIdx==1? qPrice : (orderTypeIdx==0? (float)mid : qStop)));
                if (ImGui::Button("확인", ImVec2(120,0))) { doSend = true; ImGui::CloseCurrentPopup(); }
                ImGui::SameLine(); if (ImGui::Button("취소", ImVec2(120,0))) { doSend = false; ImGui::CloseCurrentPopup(); }
                ImGui::EndPopup();
            }
            if (doSend) {
                auto r = s_rest->placeOrder(sym, side, ot, qQty, qPrice, tifs[tifIdx], reduceOnly, false, 5000, positionSide, qStop, (orderTypeIdx>=2? workingTypes[workingTypeIdx]:"MARK_PRICE"));
                char hdr[96]; snprintf(hdr, sizeof(hdr), "%s %s %s: ", side.c_str(), ot, sym);
                lastOrderResp = std::string(hdr) + (r.ok?"OK ":"ERR ") + std::to_string(r.status) + "\n" + r.body;
                // Attached TP/SL as reduce-only market stops
                double ref = (orderTypeIdx==1 && qPrice>0)? qPrice : (mid>0? mid : (isLong? ask:bid));
                if (attachTP && ref>0) {
                    double tpp = ref * (isLong? (1.0 + tpOffsetPct/100.0) : (1.0 - tpOffsetPct/100.0));
                    tpp = floor_step(tpp, s_priceTick);
                    (void)s_rest->placeOrder(sym, isLong?"SELL":"BUY", "TAKE_PROFIT_MARKET", qQty, 0.0, "GTC", true, false, 5000, positionSide, tpp, workingTypes[workingTypeIdx]);
                }
                if (attachSL && ref>0) {
                    double slp = ref * (isLong? (1.0 - slOffsetPct/100.0) : (1.0 + slOffsetPct/100.0));
                    slp = floor_step(slp, s_priceTick);
                    (void)s_rest->placeOrder(sym, isLong?"SELL":"BUY", "STOP_MARKET", qQty, 0.0, "GTC", true, false, 5000, positionSide, slp, workingTypes[workingTypeIdx]);
                }
                // Quick async refresh for positions overlay
                std::thread([s=std::string(sym)]{
                    try {
                        BinanceRest rest("fapi.binance.com"); rest.setInsecureTLS(false);
                        auto rr = rest.getAccountInfo(3000);
                        if (!rr.ok) return;
                        using nlohmann::json; auto j = json::parse(rr.body, nullptr, false);
                        if (!j.is_object() || !j.contains("positions")) return;
                        std::vector<std::tuple<std::string,double,double>> ov;
                        auto getd = [](const nlohmann::json& v)->double { if (v.is_string()) return std::stod(v.get<std::string>()); else return v.get<double>(); };
                        for (auto &p : j["positions"]) {
                            if (!p.is_object()) continue;
                            std::string sy = p.contains("symbol") ? p["symbol"].get<std::string>() : std::string();
                            if (!s.empty() && sy != s) continue;
                            double amt = p.contains("positionAmt") ? getd(p["positionAmt"]) : 0.0;
                            double entry = p.contains("entryPrice") ? getd(p["entryPrice"]) : 0.0;
                            if (std::abs(amt) > 1e-12 && entry > 0.0) ov.emplace_back(sy, amt, entry);
                        }
                        if (!ov.empty()) { std::lock_guard<std::mutex> lk(g_posOverlayMutex); g_posOverlay.swap(ov); }
                    } catch (...) {}
                }).detach();
            }
        };

        // Keyboard shortcuts
        {
            ImGuiIO& ioHK = ImGui::GetIO();
            if (ioHK.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_B)) { orderTypeIdx = 0; if (valid) send_order(true); }
            if (ioHK.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) { orderTypeIdx = 0; if (valid) send_order(false); }
            if (s_hkQuickLimit && ioHK.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_J)) {
                orderTypeIdx = 1; // LIMIT
                double refP = (ask>0? ask : (mid>0?mid:0.0));
                if (refP>0) {
                    double notional = s_availableUSDT * 1.0 * (s_useLeverageForSize? (double)leverage : 1.0);
                    double q = notional / refP; q = floor_step(q, s_qtyStep); if (q < s_minQty) q = s_minQty; orderQty = (float)q;
                    limitPrice = (float)floor_step(refP, s_priceTick);
                    if (valid) send_order(true);
                }
            }
            if (s_hkQuickLimit && ioHK.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K)) {
                orderTypeIdx = 1; // LIMIT
                double refP = (bid>0? bid : (mid>0?mid:0.0));
                if (refP>0) {
                    double notional = s_availableUSDT * 1.0 * (s_useLeverageForSize? (double)leverage : 1.0);
                    double q = notional / refP; q = floor_step(q, s_qtyStep); if (q < s_minQty) q = s_minQty; orderQty = (float)q;
                    limitPrice = (float)floor_step(refP, s_priceTick);
                    if (valid) send_order(false);
                }
            }
            // Emergency flatten (Ctrl+X): limit IOC across all positions
            if (s_hkEmergency && ioHK.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X)) {
                std::vector<std::tuple<std::string,double,double,int,double,std::string,std::string,double>> pos_copy;
                {
                    std::lock_guard<std::mutex> lk(s_positionsMutex);
                    pos_copy = s_positions;
                }
                std::string log;
                for (auto &pt : pos_copy) {
                    const std::string& psym = std::get<0>(pt);
                    double amt = std::get<1>(pt);
                    if (std::abs(amt) < 1e-12) continue;
                    double mark = std::get<7>(pt);
                    // determine side and price
                    std::string side = (amt>0)? "SELL" : "BUY";
                    double px = 0.0;
                    if (psym == "BTCUSDT") {
                        std::lock_guard<std::mutex> lk(bookMutex);
                        if (amt>0 && !g_bookBids.empty()) px = g_bookBids.begin()->first; // close long at bid
                        if (amt<0 && !g_bookAsks.empty()) px = g_bookAsks.begin()->first; // close short at ask
                    }
                    if (px <= 0.0 && mark > 0.0) px = mark;
                    double q = std::abs(amt);
                    // quantize with current s_qtyStep as fallback
                    auto floor_step2 = [&](double v, double step)->double { if (step <= 0) return v; double n = std::floor((v + 1e-12) / step); return n * step; };
                    q = floor_step2(q, s_qtyStep); if (q < s_minQty) q = s_minQty;
                    double qPrice = floor_step2(px, s_priceTick);
                    std::string positionSide; if (dualSide) positionSide = (amt>0?"LONG":"SHORT");
                    if (s_rest && q>0 && qPrice>0) {
                        auto r = s_rest->placeOrder(psym.c_str(), side, "LIMIT", q, qPrice, "IOC", true, false, 5000, positionSide, 0.0, "MARK_PRICE");
                        log += psym + " FLAT "+ side + " q=" + std::to_string(q) + " @" + std::to_string(qPrice) + " -> " + (r.ok?"OK ":"ERR ") + std::to_string(r.status) + "\n";
                    }
                }
                if (!log.empty()) lastOrderResp = log + lastOrderResp;
            }
        }

        // Action buttons
        ImVec2 btnSize = ImVec2((ImGui::GetContentRegionAvail().x - 6.0f) * 0.5f, 36.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(40,150,90,255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(60,180,110,255));
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button("BUY / LONG", btnSize)) send_order(true);
        ImGui::SameLine();
        ImGui::PopStyleColor(2);
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(160,60,60,255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(190,80,80,255));
        if (ImGui::Button("SELL / SHORT", btnSize)) send_order(false);
        ImGui::PopStyleColor(2);
        ImGui::EndDisabled();

        ImGui::TextDisabled("Hotkeys: Ctrl+B BUY MKT, Ctrl+S SELL MKT, Ctrl+J/K Quick LIMIT 100%%");

        // Response panel
        if (!lastOrderResp.empty()) {
            ImGui::Separator();
            ImGui::Text("Response");
            ImGui::BeginChild("orderresp2", ImVec2(0, 260), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(lastOrderResp.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndChild();
        }

        ImGui::End();
    }

    // Separate: Positions window
    if (s_showPositionsWin) {
        ImGui::SetNextWindowSize(ImVec2(620, 520), ImGuiCond_FirstUseEver);
        ImGui::Begin("Positions / Orders", &s_showPositionsWin);
        // Snapshot positions under lock for rendering
        std::vector<std::tuple<std::string,double,double,int,double,std::string,std::string,double>> pos_local;
        {
            std::lock_guard<std::mutex> lk(s_positionsMutex);
            pos_local = s_positions;
        }
        if (ImGui::BeginTabBar("PosTabs")) {
            if (ImGui::BeginTabItem("Positions")) {
                // Summary profit/loss split
                double totalP=0.0, totalL=0.0;
                for (auto& t : pos_local) {
                    double amt = std::get<1>(t);
                    double entry = std::get<2>(t);
                    double upnl = std::get<4>(t);
                    double refMark = std::get<7>(t);
                    if (refMark <= 0.0) {
                        std::lock_guard<std::mutex> lk(bookMutex);
                        double ask = (!g_bookAsks.empty()) ? g_bookAsks.begin()->first : 0.0;
                        double bid = (!g_bookBids.empty()) ? g_bookBids.begin()->first : 0.0;
                        refMark = (ask>0 && bid>0) ? (ask+bid)/2.0 : (ask>0?ask:bid);
                    }
                    double openFee = std::abs(amt) * entry * s_takerRate;
                    double closeFee = std::abs(amt) * refMark * s_takerRate;
                    double pnl = upnl - openFee - closeFee;
                    if (pnl >= 0) totalP += pnl; else totalL += pnl; // totalL negative sum
                }
                double net = totalP + totalL;
                ImGui::TextColored(ImVec4(0.2f,0.9f,0.5f,1.0f), "Profit:  %.2f USDT", totalP);
                ImGui::SameLine(); ImGui::TextColored(ImVec4(1.0f,0.4f,0.4f,1.0f), "Loss:  %.2f USDT", totalL);
                ImGui::SameLine(); ImGui::TextColored(net>=0?ImVec4(0.2f,0.9f,0.5f,1.0f):ImVec4(1.0f,0.4f,0.4f,1.0f), "Net:  %.2f USDT", net);

                if (ImGui::BeginTable("PositionsTable", 9, ImGuiTableFlags_RowBg|ImGuiTableFlags_Borders|ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("Symbol", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                    ImGui::TableSetupColumn("Side", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                    ImGui::TableSetupColumn("Qty", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Entry", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Lev", ImGuiTableColumnFlags_WidthFixed, 50.0f);
                    ImGui::TableSetupColumn("PnL(fee)", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("PnL%", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                    ImGui::TableSetupColumn("Margin", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGui::TableSetupColumn("Rate", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGui::TableHeadersRow();
            for (auto& t : pos_local) {
                const std::string& psymbol = std::get<0>(t);
                double amt = std::get<1>(t);
                double entry = std::get<2>(t);
                int lev = std::get<3>(t);
                double upnl = std::get<4>(t);
                const std::string& mtype = std::get<5>(t);
                const std::string& pside = std::get<6>(t);
                double mark = std::get<7>(t);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(psymbol.c_str());
                ImVec4 sideCol = amt>0? ImVec4(0.2f,0.9f,0.5f,1.0f) : (amt<0? ImVec4(1.0f,0.4f,0.4f,1.0f) : ImVec4(0.7f,0.7f,0.8f,1.0f));
                ImGui::TableSetColumnIndex(1); ImGui::TextColored(sideCol, amt>0?"LONG":(amt<0?"SHORT":pside.c_str()));
                ImGui::TableSetColumnIndex(2); ImGui::Text("%.6f", amt);
                ImGui::TableSetColumnIndex(3); ImGui::Text("%.2f", entry);
                ImGui::TableSetColumnIndex(4); ImGui::Text("%d", lev);
                // Fee-adjusted PnL: assume taker open + taker close
                double refMark = mark;
                if (refMark <= 0.0) {
                    std::lock_guard<std::mutex> lk(bookMutex);
                    double ask = (!g_bookAsks.empty()) ? g_bookAsks.begin()->first : 0.0;
                    double bid = (!g_bookBids.empty()) ? g_bookBids.begin()->first : 0.0;
                    refMark = (ask>0 && bid>0) ? (ask+bid)/2.0 : (ask>0?ask:bid);
                }
                double openFee = std::abs(amt) * entry * s_takerRate;
                double closeFee = std::abs(amt) * refMark * s_takerRate;
                double pnlFee = upnl - openFee - closeFee;
                double notion = std::abs(amt) * entry;
                double pnlPct = (notion > 1e-12) ? (pnlFee / notion) * 100.0 : 0.0;
                ImVec4 pnlCol = pnlFee >= 0 ? ImVec4(0.2f,0.9f,0.5f,1.0f) : ImVec4(1.0f,0.4f,0.4f,1.0f);
                ImGui::TableSetColumnIndex(5); ImGui::TextColored(pnlCol, "%.2f", pnlFee);
                ImGui::TableSetColumnIndex(6); ImGui::TextColored(pnlCol, "%.2f%%", pnlPct);
                ImGui::TableSetColumnIndex(7); ImGui::TextUnformatted(mtype.c_str());
                ImGui::TableSetColumnIndex(8); ImGui::Text("T%.4f%%", s_takerRate*100.0);
            }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Orders")) {
                static std::string openOrdersBody, userTradesBody; static int lastStatusOO=0, lastStatusUT=0;
                ImGui::TextDisabled("Open orders and recent fills for %s", g_chartSymbol.c_str());
                if (ImGui::Button("Refresh Open Orders")) { if (s_rest) { auto r = s_rest->getOpenOrders(g_chartSymbol, 5000); lastStatusOO=r.status; openOrdersBody=r.body; } }
                ImGui::SameLine(); if (ImGui::Button("Refresh Recent Fills")) { if (s_rest) { auto r = s_rest->getUserTrades(g_chartSymbol, 50, 5000); lastStatusUT=r.status; userTradesBody=r.body; } }

                // Parse open orders
                struct OO { long long id; std::string side,type,status,pside; double price,origQty,executedQty; long long time; bool reduceOnly; };
                std::vector<OO> oos;
                try {
                    using nlohmann::json; auto j = json::parse(openOrdersBody, nullptr, false);
                    if (j.is_array()) {
                        for (auto &e : j) {
                            OO x{}; x.id = e.contains("orderId")? e["orderId"].get<long long>() : 0;
                            x.side = e.value("side", ""); x.type = e.value("type", ""); x.status = e.value("status", "");
                            x.pside = e.value("positionSide", ""); x.reduceOnly = e.value("reduceOnly", false);
                            auto getd=[&](const nlohmann::json& v)->double{ if (v.is_string()) return std::stod(v.get<std::string>()); else if(v.is_number()) return v.get<double>(); else return 0.0; };
                            x.price = e.contains("price")? getd(e["price"]) : 0.0;
                            x.origQty = e.contains("origQty")? getd(e["origQty"]) : 0.0;
                            x.executedQty = e.contains("executedQty")? getd(e["executedQty"]) : 0.0;
                            x.time = e.contains("time")? e["time"].get<long long>() : 0;
                            oos.push_back(x);
                        }
                    }
                } catch (...) {}

                ImGui::Text("OpenOrders: %s %d  (rows=%d)", (lastStatusOO>=200&&lastStatusOO<300)?"OK":"ERR", lastStatusOO, (int)oos.size());
                if (ImGui::BeginTable("OOTable", 9, ImGuiTableFlags_RowBg|ImGuiTableFlags_Borders|ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                    ImGui::TableSetupColumn("Side", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGui::TableSetupColumn("Price", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Qty", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Exec", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                    ImGui::TableSetupColumn("PosSide", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGui::TableSetupColumn("Flags", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGui::TableHeadersRow();
                    for (size_t i=0;i<oos.size();++i) {
                        auto &x = oos[i];
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::Text("%lld", x.id);
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored((x.side=="BUY"?ImVec4(0.2f,0.9f,0.5f,1.0f):ImVec4(1.0f,0.4f,0.4f,1.0f)), "%s", x.side.c_str());
                        ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(x.type.c_str());
                        ImGui::TableSetColumnIndex(3); ImGui::Text("%.4f", x.price);
                        ImGui::TableSetColumnIndex(4); ImGui::Text("%.6f", x.origQty);
                        ImGui::TableSetColumnIndex(5); ImGui::Text("%.6f", x.executedQty);
                        ImGui::TableSetColumnIndex(6); ImGui::TextUnformatted(x.status.c_str());
                        ImGui::TableSetColumnIndex(7); ImGui::TextUnformatted(x.pside.c_str());
                        ImGui::TableSetColumnIndex(8); ImGui::TextUnformatted(x.reduceOnly?"RO":"");
                        // Context menu per row
                        ImGui::PushID((int)i);
                        if (ImGui::BeginPopupContextItem("oo_ctx")) {
                            if (ImGui::MenuItem("Cancel")) {
                                if (s_rest) { auto r = s_rest->cancelOrder(g_chartSymbol, x.id, "", 5000); t_lastOrderResp = std::string("Cancel ")+std::to_string(x.id)+": "+(r.ok?"OK ":"ERR ")+std::to_string(r.status)+"\n"+r.body; auto r2=s_rest->getOpenOrders(g_chartSymbol,5000); lastStatusOO=r2.status; openOrdersBody=r2.body; }
                            }
                            if (ImGui::MenuItem("Cancel ALL (symbol)")) {
                                if (s_rest) { auto r = s_rest->cancelAllOpenOrders(g_chartSymbol, 5000); t_lastOrderResp = std::string("CancelAll ")+g_chartSymbol+": "+(r.ok?"OK ":"ERR ")+std::to_string(r.status)+"\n"+r.body; auto r2=s_rest->getOpenOrders(g_chartSymbol,5000); lastStatusOO=r2.status; openOrdersBody=r2.body; }
                            }
                            if (ImGui::MenuItem("Duplicate as LIMIT")) {
                                snprintf(t_sym, sizeof(t_sym), "%s", g_chartSymbol.c_str());
                                t_orderTypeIdx = 1; t_limitPrice = (float)x.price; t_orderQty = (float)(x.origQty - x.executedQty); if (t_orderQty<0) t_orderQty=0; ImGui::CloseCurrentPopup();
                            }
                            if (ImGui::MenuItem("Duplicate as MARKET")) {
                                snprintf(t_sym, sizeof(t_sym), "%s", g_chartSymbol.c_str());
                                t_orderTypeIdx = 0; t_orderQty = (float)(x.origQty - x.executedQty); if (t_orderQty<0) t_orderQty=0; ImGui::CloseCurrentPopup();
                            }
                            ImGui::EndPopup();
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }

                // Parse recent fills (userTrades)
                struct FT { long long id; bool isBuyer; double price, qty; long long time; double commission; std::string commissionAsset; };
                std::vector<FT> fills;
                try {
                    using nlohmann::json; auto j = json::parse(userTradesBody, nullptr, false);
                    if (j.is_array()) {
                        for (auto &e : j) {
                            FT f{}; f.id = e.value("id", 0LL); f.isBuyer = e.value("isBuyer", false);
                            auto getd=[&](const nlohmann::json& v)->double{ if (v.is_string()) return std::stod(v.get<std::string>()); else if(v.is_number()) return v.get<double>(); else return 0.0; };
                            f.price = e.contains("price")? getd(e["price"]) : 0.0; f.qty = e.contains("qty")? getd(e["qty"]) : 0.0;
                            f.time = e.value("time", 0LL); f.commission = e.contains("commission")? getd(e["commission"]) : 0.0; f.commissionAsset = e.value("commissionAsset", "");
                            fills.push_back(f);
                        }
                    }
                } catch (...) {}

                ImGui::Separator();
                ImGui::Text("Recent Fills: %s %d (rows=%d)", (lastStatusUT>=200&&lastStatusUT<300)?"OK":"ERR", lastStatusUT, (int)fills.size());
                if (ImGui::BeginTable("FillsTable", 6, ImGuiTableFlags_RowBg|ImGuiTableFlags_Borders|ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                    ImGui::TableSetupColumn("Side", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                    ImGui::TableSetupColumn("Price", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Qty", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 180.0f);
                    ImGui::TableSetupColumn("Fee", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                    ImGui::TableHeadersRow();
                    for (size_t i=0;i<fills.size();++i) {
                        auto &f = fills[i]; ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::Text("%lld", f.id);
                        ImGui::TableSetColumnIndex(1); ImGui::TextColored(f.isBuyer?ImVec4(0.2f,0.9f,0.5f,1.0f):ImVec4(1.0f,0.4f,0.4f,1.0f), "%s", f.isBuyer?"BUY":"SELL");
                        ImGui::TableSetColumnIndex(2); ImGui::Text("%.4f", f.price);
                        ImGui::TableSetColumnIndex(3); ImGui::Text("%.6f", f.qty);
                        ImGui::TableSetColumnIndex(4);
                        time_t sec = (time_t)(f.time / 1000); struct tm tmv{}; 
#if defined(_WIN32)
                        localtime_s(&tmv, &sec);
#else
                        tmv = *std::localtime(&sec);
#endif
                        char tb[64]; strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", &tmv); ImGui::TextUnformatted(tb);
                        ImGui::TableSetColumnIndex(5); ImGui::Text("%.6f %s", f.commission, f.commissionAsset.c_str());
                        // Context menu: set limit price/qty from fill
                        ImGui::PushID((int)i);
                        if (ImGui::BeginPopupContextItem("fill_ctx")) {
                            if (ImGui::MenuItem("Use as LIMIT")) { snprintf(t_sym, sizeof(t_sym), "%s", g_chartSymbol.c_str()); t_orderTypeIdx = 1; t_limitPrice = (float)f.price; t_orderQty = (float)f.qty; ImGui::CloseCurrentPopup(); }
                            if (ImGui::MenuItem("Use as MARKET")) { snprintf(t_sym, sizeof(t_sym), "%s", g_chartSymbol.c_str()); t_orderTypeIdx = 0; t_orderQty = (float)f.qty; ImGui::CloseCurrentPopup(); }
                            ImGui::EndPopup();
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }

                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Hotkeys")) {
                ImGui::Checkbox("Enable Quick LIMIT (Ctrl+J/K)", &s_hkQuickLimit);
                ImGui::Checkbox("Enable Emergency Flatten (Ctrl+X)", &s_hkEmergency);
                if (ImGui::Button("Flatten Now (LIMIT IOC)")) {
                    ImGuiIO& ioHK = ImGui::GetIO(); (void)ioHK; // reuse same routine via key path
                    // trigger same as Ctrl+X handler
                    std::vector<std::tuple<std::string,double,double,int,double,std::string,std::string,double>> pos_copy;
                    { std::lock_guard<std::mutex> lk(s_positionsMutex); pos_copy = s_positions; }
                    std::string log;
                    for (auto &pt : pos_copy) {
                        const std::string& psym = std::get<0>(pt);
                        double amt = std::get<1>(pt); if (std::abs(amt) < 1e-12) continue;
                        double mark = std::get<7>(pt); std::string side = (amt>0)?"SELL":"BUY"; double px=0.0;
                        if (psym == "BTCUSDT") { std::lock_guard<std::mutex> lk(bookMutex); if (amt>0 && !g_bookBids.empty()) px=g_bookBids.begin()->first; if (amt<0 && !g_bookAsks.empty()) px=g_bookAsks.begin()->first; }
                        if (px<=0.0 && mark>0.0) px=mark; double q=fabs(amt);
                        auto fstep=[&](double v,double s){ if (s<=0) return v; double n=floor((v+1e-12)/s); return n*s; };
                        q=fstep(q,s_qtyStep); if (q<s_minQty) q=s_minQty; double qPrice=fstep(px,s_priceTick);
                        std::string positionSide; if (t_dualSide) positionSide=(amt>0?"LONG":"SHORT");
                        if (s_rest && q>0 && qPrice>0) { auto r = s_rest->placeOrder(psym.c_str(), side, "LIMIT", q, qPrice, "IOC", true, false, 5000, positionSide, 0.0, "MARK_PRICE"); log += psym+" FLAT "+side+" q="+std::to_string(q)+" @"+std::to_string(qPrice)+" -> "+(r.ok?"OK ":"ERR ")+std::to_string(r.status)+"\n"; }
                    }
                    if (!log.empty()) t_lastOrderResp = log + t_lastOrderResp;
                }
                ImGui::TextDisabled("Ctrl+B: MARKET BUY, Ctrl+S: MARKET SELL, Ctrl+J/K: Quick LIMIT 100%%, Ctrl+X: Flatten LIMIT IOC");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::End();
    }

    // No animation state to maintain
}

// ====== Chart renderer and data management ======
static long long interval_to_ms(const std::string& iv) {
    struct Item { const char* k; long long v; };
    static const Item map[] = {
        {"1m", 60LL*1000}, {"3m", 3LL*60*1000}, {"5m", 5LL*60*1000}, {"15m", 15LL*60*1000}, {"30m", 30LL*60*1000},
        {"1h", 60LL*60*1000}, {"2h", 2LL*60*60*1000}, {"4h", 4LL*60*60*1000}, {"6h", 6LL*60*60*1000}, {"12h", 12LL*60*60*1000},
        {"1d", 24LL*60*60*1000}
    };
    for (auto& it : map) if (iv == it.k) return it.v; return 60LL*1000;
}

static void merge_and_sort_candles(std::vector<Candle>& base, std::vector<Candle>& add) {
    std::unordered_map<long long, Candle> m;
    m.reserve(base.size() + add.size());
    for (auto& c : base) m[c.t0] = c;
    for (auto& c : add) m[c.t0] = c;
    base.clear(); base.reserve(m.size());
    for (auto& kv : m) base.push_back(kv.second);
    std::sort(base.begin(), base.end(), [](const Candle& a, const Candle& b){ return a.t0 < b.t0; });
}

static std::vector<Candle> parse_klines_body(const std::string& body) {
    std::vector<Candle> out;
    try {
        using nlohmann::json; auto j = json::parse(body, nullptr, false);
        if (!j.is_array()) return out;
        out.reserve(j.size());
        for (auto& e : j) {
            if (!e.is_array() || e.size() < 7) continue;
            long long t0 = e[0].get<long long>();
            double o = std::stod(e[1].get<std::string>());
            double h = std::stod(e[2].get<std::string>());
            double l = std::stod(e[3].get<std::string>());
            double c = std::stod(e[4].get<std::string>());
            double v = std::stod(e[5].get<std::string>());
            long long t1 = e[6].get<long long>();
            out.push_back(Candle{t0, t1, o, h, l, c, v});
        }
    } catch (...) {}
    return out;
}

static void fetch_klines_parallel(const std::string& symbol, const std::string& iv, int candles) {
    if (candles <= 0) return;
    g_chartLoading = true;
    std::thread([symbol, iv, candles]{
        BinanceRest rest("fapi.binance.com");
        rest.setInsecureTLS(false);
        const long long ms_per = interval_to_ms(iv);
        const int max_per_req = 1500;
        // Determine overall time range
        long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        long long total_span = (long long)candles * ms_per;
        long long start = now_ms - total_span;
        if (start < 0) start = 0;
        // Build chunks of <= max_per_req candles (in chronological order)
        struct Seg { long long a; long long b; };
        std::vector<Seg> segs;
        int remaining = candles;
        long long seg_end = now_ms;
        while (remaining > 0) {
            int take = std::min(remaining, max_per_req);
            long long seg_start = seg_end - (long long)take * ms_per;
            if (seg_start < start) seg_start = start;
            segs.push_back(Seg{seg_start, seg_end});
            seg_end = seg_start;
            remaining -= take;
        }
        // Fetch concurrently (limit parallelism)
        std::vector<std::future<std::vector<Candle>>> futs;
        futs.reserve(segs.size());
        for (size_t i = 0; i < segs.size(); ++i) {
            auto seg = segs[i];
            futs.push_back(std::async(std::launch::async, [symbol, iv, seg](){
                BinanceRest lr("fapi.binance.com");
                lr.setInsecureTLS(false);
                auto r = lr.getKlines(symbol, iv, seg.a, seg.b, 1500);
                if (!r.ok) return std::vector<Candle>{};
                return parse_klines_body(r.body);
            }));
        }
        std::vector<Candle> merged;
        for (auto& f : futs) {
            try {
                auto part = f.get();
                if (!part.empty()) merge_and_sort_candles(merged, part);
            } catch (...) {}
        }
        if (!merged.empty()) {
            std::lock_guard<std::mutex> lk(g_candlesMutex);
            g_candles.swap(merged);
        }
        g_chartLoading = false;
    }).detach();
}

static void StartOrRestartKlineStream(const std::string& symbolLower, const std::string& interval) {
    static std::string lastKey;
    std::string key = symbolLower + "@kline_" + interval;
    if (g_chartStreamRunning.load() && key == lastKey) return;
    lastKey = key;
    g_chartStreamRunning.store(true);
    std::thread([key]{
        try {
            WebSocket ws("fstream.binance.com", "443");
            ws.connect();
            std::string sub = std::string("{\"method\":\"SUBSCRIBE\",\"params\":[\"") + key + "\"],\"id\":1234}";
            ws.send(sub);
            for (;;) {
                std::string msg = ws.receive();
                if (msg.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
                try {
                    using nlohmann::json; auto j = json::parse(msg, nullptr, false);
                    const json* d = nullptr;
                    if (j.contains("data")) d = &j["data"]; else d = &j;
                    if (!d || !d->is_object() || !d->contains("k")) continue;
                    auto k = (*d)["k"];
                    long long t0 = k["t"].get<long long>();
                    long long t1 = k["T"].get<long long>();
                    double o = std::stod(k["o"].get<std::string>());
                    double h = std::stod(k["h"].get<std::string>());
                    double l = std::stod(k["l"].get<std::string>());
                    double c = std::stod(k["c"].get<std::string>());
                    double v = std::stod(k["v"].get<std::string>());
                    bool kx = k["x"].get<bool>(); // closed
                    Candle nc{t0,t1,o,h,l,c,v};
                    {
                        std::lock_guard<std::mutex> lk(g_candlesMutex);
                        if (!g_candles.empty() && g_candles.back().t0 == t0) g_candles.back() = nc;
                        else if (g_candles.empty() || g_candles.back().t0 < t0) g_candles.push_back(nc);
                    }
                } catch (...) {}
            }
        } catch (...) {
            g_chartStreamRunning.store(false);
        }
    }).detach();
    // Also subscribe to aggTrade for faster-than-100ms last price updates
    StartOrRestartAggTradeStream(key.substr(0, key.find("@kline_")));
}

static void StartOrRestartAggTradeStream(const std::string& symbolLower)
{
    static std::string lastSym;
    if (lastSym == symbolLower) return; // simple guard; multiple subs are cheap but we avoid duplicates
    lastSym = symbolLower;
    std::thread([symbolLower]{
        try {
            WebSocket ws("fstream.binance.com", "443");
            ws.connect();
            std::string sub = std::string("{\"method\":\"SUBSCRIBE\",\"params\":[\"") + symbolLower + "@aggTrade\"],\"id\":2233}";
            ws.send(sub);
            for (;;) {
                std::string msg = ws.receive();
                if (msg.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
                try {
                    using nlohmann::json; auto j = json::parse(msg, nullptr, false);
                    const json* d = nullptr; if (j.contains("data")) d = &j["data"]; else d = &j; if (!d||!d->is_object()) continue;
                    double price = 0.0, qty=0.0; long long ts=0;
                    if (d->contains("p")) price = std::stod((*d)["p"].get<std::string>());
                    if (d->contains("q")) qty   = std::stod((*d)["q"].get<std::string>());
                    if (d->contains("T")) ts    = (*d)["T"].get<long long>();
                    if (price>0) {
                        g_lastTradePrice.store(price, std::memory_order_relaxed);
                        // update latest candle close immediately when live
                        if (g_chartLive) {
                            std::lock_guard<std::mutex> lk(g_candlesMutex);
                            if (!g_candles.empty()) {
                                Candle &back = g_candles.back();
                                // only if trade falls into current candle time window, adjust close
                                if (ts >= back.t0 && ts <= back.t1) {
                                    back.c = price;
                                }
                            }
                        }
                    }
                } catch (...) {}
            }
        } catch (...) {}
    }).detach();
}

static void RenderChartWindow()
{
    if (!g_showChartWin) return;
    ImGui::Begin("Chart - Klines");
    // Chart local persistent states
    static bool s_priceManual = false; // false=auto, true=manual
    static double s_viewPmin = 0.0, s_viewPmax = 0.0;
    // Controls
    static char symBuf[32] = "BTCUSDT";
    static const char* intervals[] = {"1m","3m","5m","15m","30m","1h","2h","4h","6h","12h","1d"};
    static int ivIdx = 0; // 1m
    static int histCandles = 10000;
    static bool showSMA = true; static int sma1=7,sma2=25,sma3=99; static bool showBB=false; static int bbLen=20; static float bbK=2.0f;
    static bool showVol = true; static bool showCross = true; static bool showRSI=false; static int rsiLen=14; static bool showMACD=false; static int macdFast=12, macdSlow=26, macdSig=9;
    ImGui::SetNextItemWidth(120);
    ImGui::InputText("Symbol", symBuf, sizeof(symBuf));
    ImGui::SameLine(); ImGui::SetNextItemWidth(100);
    ImGui::Combo("Interval", &ivIdx, intervals, IM_ARRAYSIZE(intervals));
    ImGui::SameLine(); ImGui::SetNextItemWidth(110); ImGui::InputInt("History", &histCandles); ImGui::SameLine(); ImGui::TextUnformatted("candles");
    if (ImGui::Button(g_chartLoading?"Loading...":"Load", ImVec2(120,0))) {
        if (!g_chartLoading) {
            g_chartSymbol = symBuf; g_chartInterval = intervals[ivIdx];
            fetch_klines_parallel(g_chartSymbol, g_chartInterval, std::max(100, histCandles));
            std::string symLower = g_chartSymbol; std::transform(symLower.begin(), symLower.end(), symLower.begin(), ::tolower);
            StartOrRestartKlineStream(symLower, g_chartInterval);
        }
    }
    ImGui::SameLine(); ImGui::Checkbox("AutoUpdate", &g_chartLive); ImGui::SameLine(); ImGui::Checkbox("Crosshair", &showCross);
    ImGui::SameLine(); ImGui::Checkbox("Volume", &showVol); ImGui::SameLine(); ImGui::Checkbox("RSI", &showRSI); ImGui::SameLine(); ImGui::Checkbox("MACD", &showMACD); ImGui::SameLine(); ImGui::Checkbox("SMA", &showSMA);
    if (showSMA) {
        ImGui::SameLine(); ImGui::SetNextItemWidth(90); ImGui::InputInt("S1", &sma1);
        ImGui::SameLine(); ImGui::SetNextItemWidth(90); ImGui::InputInt("S2", &sma2);
        ImGui::SameLine(); ImGui::SetNextItemWidth(90); ImGui::InputInt("S3", &sma3);
    }
    ImGui::SameLine(); ImGui::Checkbox("BollBands", &showBB);
    if (showBB) {
        ImGui::SameLine(); ImGui::SetNextItemWidth(60); ImGui::InputInt("BBn", &bbLen);
        ImGui::SameLine(); ImGui::SetNextItemWidth(60); ImGui::InputFloat("BBk", &bbK);
    }
    // Big trade threshold (base asset qty)
    static double uiBigTradeQty = 1.0; ImGui::SameLine(); ImGui::SetNextItemWidth(80); ImGui::InputDouble("BigQty", &uiBigTradeQty);
    // (A) button moved to chart overlay bottom-right

    // Chart area
    ImVec2 avail = ImGui::GetContentRegionAvail();
    int subPanels = (showVol?1:0) + (showRSI?1:0) + (showMACD?1:0);
    float subTotalH = subPanels>0 ? std::max(90.0f, avail.y * 0.30f) : 0.0f;
    float chartH = std::max(160.0f, avail.y - subTotalH - 8.0f);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 p1 = ImVec2(p0.x + avail.x, p0.y + chartH);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(18,18,22,255));

    // Snapshot candles
    std::vector<Candle> cs;
    {
        std::lock_guard<std::mutex> lk(g_candlesMutex);
        cs = g_candles;
    }
    if (cs.size() >= 2) {
        static long long viewT0 = 0, viewT1 = 0;
        const long long ms_per = interval_to_ms(intervals[ivIdx]);
        if (viewT0 == 0 || viewT1 == 0) {
            // default to last 200 bars
            size_t n = cs.size();
            viewT0 = cs[n>200?n-200:0].t0;
            viewT1 = cs.back().t1;
        }
        auto clamp_view = [&]{ if (viewT0 >= viewT1) { viewT1 = viewT0 + ms_per; } };
        clamp_view();
        auto t_to_x = [&](long long t){ double a = double(t - viewT0) / double(viewT1 - viewT0); return p0.x + (float)(a * (p1.x - p0.x)); };
        auto price_minmax = [&](long long a, long long b){ double mn=1e300,mx=-1e300; for (auto& k:cs){ if (k.t1<a||k.t0>b) continue; mn=std::min(mn,k.l); mx=std::max(mx,k.h);} if (mx<mn){mn=0;mx=1;} return std::pair<double,double>(mn,mx); };
        auto [pmin,pmax] = price_minmax(viewT0, viewT1);
        double pad = (pmax - pmin) * 0.05; if (pad<=0) pad=1.0; pmin-=pad; pmax+=pad;
        if (!s_priceManual || s_viewPmax <= s_viewPmin) { s_viewPmin = pmin; s_viewPmax = pmax; }
        auto p_to_y = [&](double p){ double a = (p - s_viewPmin) / (s_viewPmax - s_viewPmin); return p1.y - (float)(a * (p1.y - p0.y)); };
        auto y_to_p = [&](float y){ double a = (p1.y - y) / std::max(1.0f, (p1.y - p0.y)); return s_viewPmin + a * (s_viewPmax - s_viewPmin); };

        // A toggle button FIRST so it gets input priority over the chart-area listener
        {
            bool autoMode = !s_priceManual;
            ImVec2 btnPos = ImVec2(p1.x - 34.0f, p0.y + 6.0f);
            ImGui::SetCursorScreenPos(btnPos);
            ImVec4 onCol(0.15f,0.45f,0.25f,1.0f), offCol(0.25f,0.25f,0.25f,1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, autoMode?onCol:offCol);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, autoMode?ImVec4(0.18f,0.52f,0.3f,1.0f):ImVec4(0.35f,0.35f,0.35f,1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, autoMode?ImVec4(0.12f,0.38f,0.22f,1.0f):ImVec4(0.20f,0.20f,0.20f,1.0f));
            if (ImGui::Button("A", ImVec2(26,18))) { s_priceManual = autoMode; }
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Auto Price Scale");
        }

        // Interaction: zoom/pan (placed AFTER A button so it doesn't steal clicks there)
        ImGui::InvisibleButton("chart_area", ImVec2(avail.x, chartH));
        bool hovered = ImGui::IsItemHovered(); bool active = ImGui::IsItemActive();
        if (hovered) {
            float wheel = ImGui::GetIO().MouseWheel;
            // Avoid time zoom when mouse is over the right price axis
            float axisLeft = p1.x - 60.0f;
            float mx = ImGui::GetIO().MousePos.x;
            bool inAxis = (mx >= axisLeft && mx <= p1.x);
            if (wheel != 0.0f && !inAxis) {
                double factor = (wheel > 0) ? 0.9 : 1.1;
                long long t_mouse = (long long)(viewT0 + (viewT1 - viewT0) * ((mx - p0.x) / std::max(1.0f,(p1.x - p0.x))));
                long long span = (long long)((viewT1 - viewT0) * factor);
                viewT0 = t_mouse - (long long)((double)(t_mouse - viewT0) * factor);
                viewT1 = viewT0 + span;
                clamp_view();
            }
        }
        static ImVec2 dragStart; static long long v0_start=0, v1_start=0; static double prMinStart=0.0, prMaxStart=0.0; static bool dragOnAxis=false;
        if (active && ImGui::IsMouseClicked(0)) { 
            dragStart = ImGui::GetIO().MousePos; v0_start=viewT0; v1_start=viewT1; prMinStart = s_viewPmin; prMaxStart = s_viewPmax; 
            float axisLeft = p1.x - 60.0f; dragOnAxis = (dragStart.x >= axisLeft && dragStart.x <= p1.x);
            if (dragOnAxis && !s_priceManual) { s_priceManual = true; /* engage manual only on price-axis drag */ }
        }
        if (active && ImGui::IsMouseDragging(0)) {
            ImVec2 cur = ImGui::GetIO().MousePos;
            ImVec2 d = ImVec2(cur.x - dragStart.x, cur.y - dragStart.y);
            if (!dragOnAxis) {
                long long dt = (long long)((double)(v1_start - v0_start) * (d.x / std::max(1.0f,(p1.x - p0.x))));
                viewT0 = v0_start - dt; viewT1 = v1_start - dt; clamp_view();
            }
            // vertical pan maps pixel delta to price delta (allow on chart too when manual)
            if (s_priceManual && fabsf(d.y) > 0.0f) {
                double span = (prMaxStart - prMinStart);
                double dyRatio = (double)d.y / std::max(1.0f, (p1.y - p0.y));
                double dPrice = dyRatio * span; // drag up (negative y) -> move window down (decrease price)
                s_viewPmin = prMinStart + dPrice;
                s_viewPmax = prMaxStart + dPrice;
            }
        }

        // Background grid removed (price grid still drawn by labels below)

        // Axis labels (right side, price)
        auto nice_step = [&](double range){
            double exp10 = pow(10.0, floor(log10(range)));
            double f = range / exp10;
            double step;
            if (f < 2) step = 2; else if (f < 5) step = 5; else step = 10;
            return step * exp10 / 5.0; // aim ~5-7 ticks
        };
        auto fmt_price = [&](double v){ char buf[64]; int prec = (s_viewPmax < 1.0) ? 6 : (s_viewPmax < 100.0 ? 3 : 2); snprintf(buf, sizeof(buf), "%.*f", prec, v); return std::string(buf); };
        double step = nice_step(s_viewPmax - s_viewPmin);
        double t0 = ceil(s_viewPmin / step) * step;
        for (double yv = t0; yv <= s_viewPmax + 1e-9; yv += step) {
            float y = p_to_y(yv);
            // price grid line
            dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), IM_COL32(64,64,80,80));
            std::string s = fmt_price(yv);
            ImVec2 ts = ImGui::CalcTextSize(s.c_str());
            dl->AddRectFilled(ImVec2(p1.x - ts.x - 6, y - ts.y*0.5f - 1), ImVec2(p1.x - 2, y + ts.y*0.5f + 1), IM_COL32(20,20,24,200));
            dl->AddText(ImVec2(p1.x - ts.x - 4, y - ts.y*0.5f), IM_COL32(190,190,210,255), s.c_str());
        }

        // Right axis zoom (mouse wheel) and reset (double-click)
        ImVec2 axisMin = ImVec2(p1.x - 60.0f, p0.y);
        ImVec2 axisMax = ImVec2(p1.x, p1.y);
        ImVec2 mpos = ImGui::GetIO().MousePos;
        bool overAxis = (mpos.x >= axisMin.x && mpos.x <= axisMax.x && mpos.y >= axisMin.y && mpos.y <= axisMax.y && ImGui::IsWindowHovered());
        if (overAxis) {
            float wheel = ImGui::GetIO().MouseWheel;
            // On any wheel interaction over price axis, switch to manual mode (toggle off A)
            if (wheel != 0.0f && !s_priceManual) { s_priceManual = true; }
            if (wheel != 0.0f && s_priceManual) {
                double oldSpan = (s_viewPmax - s_viewPmin);
                double factor = (wheel > 0) ? 0.9 : 1.1; if (factor < 0.05) factor = 0.05;
                double newSpan = std::max(1e-9, oldSpan * factor);
                double anchor = y_to_p(mpos.y);
                double ratio = (anchor - s_viewPmin) / std::max(1e-12, oldSpan);
                s_viewPmin = anchor - ratio * newSpan;
                s_viewPmax = s_viewPmin + newSpan;
            }
            if (ImGui::IsMouseDoubleClicked(0)) { s_priceManual = false; }
        }

        // Last price line + label
        double lastC = cs.back().c;
        if (g_lastTradePrice.load() > 0.0) lastC = g_lastTradePrice.load();
        float yLast = p_to_y(lastC);
        dl->AddLine(ImVec2(p0.x, yLast), ImVec2(p1.x, yLast), IM_COL32(255,215,0,160));
        std::string lp = fmt_price(lastC);
        ImVec2 lps = ImGui::CalcTextSize(lp.c_str());
        dl->AddRectFilled(ImVec2(p1.x - lps.x - 10, yLast - lps.y*0.5f - 2), ImVec2(p1.x - 2, yLast + lps.y*0.5f + 2), IM_COL32(40,40,10,230));
        dl->AddText(ImVec2(p1.x - lps.x - 6, yLast - lps.y*0.5f), IM_COL32(255,235,120,255), lp.c_str());

        // Positions overlay: draw entry lines for current symbol, colored by live PnL
        {
            std::vector<std::tuple<std::string,double,double>> pos;
            {
                std::lock_guard<std::mutex> lk(g_posOverlayMutex);
                pos = g_posOverlay;
            }
            for (auto &t : pos) {
                const std::string &ps = std::get<0>(t);
                if (ps != g_chartSymbol) continue;
                double amt = std::get<1>(t);
                double entry = std::get<2>(t);
                if (entry <= 0.0 || std::abs(amt) < 1e-12) continue;
                bool isLong = amt > 0;
                // Fee-adjusted PnL and percent
                double q = std::abs(amt);
                double rfee = g_takerRate.load(std::memory_order_relaxed);
                double raw = amt * (lastC - entry);
                double fees = q * entry * rfee + q * lastC * rfee; // taker open + taker close
                double pnlFee = raw - fees;
                double notion = q * entry;
                double pnlPct = (notion > 1e-12) ? (pnlFee / notion) * 100.0 : 0.0;
                ImU32 col = pnlFee >= 0 ? IM_COL32(60,200,120,220) : IM_COL32(220,80,80,220);
                float y = p_to_y(entry);
                dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), col, 2.0f);
                char lab[160]; snprintf(lab, sizeof(lab), "%s  %.6f @ %.2f  (%.2f%%)", isLong?"LONG":"SHORT", fabs(amt), entry, pnlPct);
                ImVec2 ts = ImGui::CalcTextSize(lab);
                float lx = p0.x + 8.0f; float ly = y - ts.y - 2.0f; if (ly < p0.y) ly = p0.y + 2.0f; if (ly + ts.y + 4 > p1.y) ly = p1.y - (ts.y + 4);
                dl->AddRectFilled(ImVec2(lx-3, ly-2), ImVec2(lx + ts.x + 4, ly + ts.y + 2), IM_COL32(18,18,20,210));
                dl->AddText(ImVec2(lx, ly), col, lab);

                // Break-even line (dotted), fee-inclusive
                double bePrice = 0.0;
                if (isLong) {
                    double den = (1.0 - rfee); if (den > 1e-9) bePrice = entry * (1.0 + rfee) / den;
                } else {
                    double den = (1.0 + rfee); if (den > 1e-9) bePrice = entry * (1.0 - rfee) / den;
                }
                if (bePrice > 0.0) {
                    float ybe = p_to_y(bePrice);
                    // dashed line across chart
                    float dash = 8.0f, gap = 6.0f; float x = p0.x;
                    while (x < p1.x) {
                        float x2 = std::min(p1.x, x + dash);
                        dl->AddLine(ImVec2(x, ybe), ImVec2(x2, ybe), IM_COL32(200,200,200,140), 1.0f);
                        x += dash + gap;
                    }
                    // label
                    char bl[64]; snprintf(bl, sizeof(bl), "BE %.2f", bePrice);
                    ImVec2 bsz = ImGui::CalcTextSize(bl);
                    float bx = p1.x - bsz.x - 8.0f; float by = ybe - bsz.y - 2.0f; if (by < p0.y) by = p0.y + 2.0f; if (by + bsz.y + 4 > p1.y) by = p1.y - (bsz.y + 4);
                    dl->AddRectFilled(ImVec2(bx-3, by-2), ImVec2(bx + bsz.x + 4, by + bsz.y + 2), IM_COL32(18,18,20,190));
                    dl->AddText(ImVec2(bx, by), IM_COL32(220,220,230,220), bl);
                }
            }
        }

        // Per-candle cumulative BUY/SELL overlay near most recent candle (right side)
        {
            std::vector<PubTrade> tr2; { std::lock_guard<std::mutex> lk(tradesMutex); tr2 = g_trades; }
            long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            double buySum=0.0, sellSum=0.0;
            const Candle &lc = cs.back();
            long long cStart = lc.t0;
            long long cEnd   = std::min<long long>(now_ms, lc.t1);
            for (auto &t : tr2) { if (t.ts >= cStart && t.ts <= cEnd) { if (t.isBuy) buySum += t.qty; else sellSum += t.qty; } }
            float xLast = t_to_x((long long)((lc.t0 + lc.t1)/2));
            float yMid  = p_to_y((lc.h + lc.l) * 0.5);
            // compute overlay width using two lines
            char lineBuy[128], lineSell[128];
            snprintf(lineBuy, sizeof(lineBuy),   "BUY   : %.3f", buySum);
            snprintf(lineSell, sizeof(lineSell), "SELL  : %.3f", sellSum);
            ImVec2 szBuy = ImGui::CalcTextSize(lineBuy);
            ImVec2 szSell = ImGui::CalcTextSize(lineSell);
            float wTxt = std::max(szBuy.x, szSell.x);
            float hTxt = szBuy.y + 2.0f + szSell.y;
            // position to the right of last candle considering candle width
            float px_per_ms_tmp = (p1.x - p0.x) / (float)(viewT1 - viewT0);
            float cw = std::max(6.0f, (float)(ms_per * px_per_ms_tmp * 0.6f));
            float px = std::min(p1.x - wTxt - 12.0f, xLast + cw * 0.5f + 8.0f);
            float py = std::max(p0.y + 4.0f, std::min(p1.y - hTxt - 4.0f, yMid - hTxt*0.5f));
            // background
            dl->AddRectFilled(ImVec2(px-4, py-2), ImVec2(px + wTxt + 8, py + hTxt + 4), IM_COL32(18,18,20,200));
            // colored texts
            ImU32 colBuy = IM_COL32(60,200,140,255);
            ImU32 colSell= IM_COL32(220,90,90,255);
            dl->AddText(ImVec2(px, py), colBuy, lineBuy);
            dl->AddText(ImVec2(px, py + szBuy.y + 2.0f), colSell, lineSell);
            // mini bars to the right of text block with adaptive baseline that only increases during the candle
            static long long s_barCandleT0 = 0; static double s_barScale = 1.0;
            if (s_barCandleT0 != lc.t0) { s_barCandleT0 = lc.t0; s_barScale = std::max(1.0, std::max(buySum, sellSum)); }
            double curMax = std::max(buySum, sellSum);
            if (curMax > s_barScale) s_barScale = curMax; // only increase during candle
            float oBarW = 70.0f; float barH2 = 6.0f; float sp2 = 3.0f; float bx = px + wTxt + 10.0f; float by = py + 1.0f;
            double mx = std::max(1.0, s_barScale);
            float bw = (float)(oBarW * (buySum / mx));
            float sw = (float)(oBarW * (sellSum / mx));
            if (bx + oBarW < p1.x - 4.0f) {
                dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + oBarW, by + barH2), IM_COL32(40,50,40,180));
                dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + barH2), colBuy);
                dl->AddRectFilled(ImVec2(bx, by+barH2+sp2), ImVec2(bx + oBarW, by+barH2+sp2 + barH2), IM_COL32(60,40,40,180));
                dl->AddRectFilled(ImVec2(bx, by+barH2+sp2), ImVec2(bx + sw, by+barH2+sp2 + barH2), colSell);
            }
        }

        // Prepare SMA arrays
        auto compute_sma = [&](int n){ std::vector<float> out(cs.size(), NAN); if (n<=1) return out; double s=0; int k=0; for (size_t i=0;i<cs.size();++i){ s += cs[i].c; if (++k>=n){ out[i] = (float)(s/n); s -= cs[i-n+1].c; } } return out; };
        std::vector<float> smaA, smaB, smaC; if (showSMA){ smaA=compute_sma(std::max(1,sma1)); smaB=compute_sma(std::max(1,sma2)); smaC=compute_sma(std::max(1,sma3)); }
        auto fmt_units = [&](double v){
            char b[64]; double av=fabs(v);
            if (av>=1e9) { snprintf(b,sizeof(b),"%.2fB", v/1e9); }
            else if (av>=1e6) { snprintf(b,sizeof(b),"%.2fM", v/1e6); }
            else if (av>=1e3) { snprintf(b,sizeof(b),"%.2fK", v/1e3); }
            else { snprintf(b,sizeof(b),"%.2f", v); }
            return std::string(b);
        };

        // Limit drawing to chart area to avoid overlapping top controls
        dl->PushClipRect(p0, p1, true);
        // Candle width
        float px_per_ms = (p1.x - p0.x) / (float)(viewT1 - viewT0);
        float barW = std::max(1.0f, (float)(ms_per * px_per_ms * 0.6f));
        ImU32 colUp = IM_COL32(40,200,140,255), colDn = IM_COL32(220,80,80,255);
        // Draw candles in view
        for (auto& k: cs) {
            if (k.t1 < viewT0 || k.t0 > viewT1) continue;
            float x = t_to_x((long long)((k.t0 + k.t1)/2));
            float x0 = x - barW*0.5f, x1 = x + barW*0.5f;
            float yO = p_to_y(k.o), yC = p_to_y(k.c), yH = p_to_y(k.h), yL = p_to_y(k.l);
            ImU32 col = (k.c >= k.o) ? colUp : colDn;
            // wick
            dl->AddLine(ImVec2(x, yH), ImVec2(x, yL), col, 1.0f);
            // body
            if (fabsf(yC - yO) < 1.0f) {
                dl->AddLine(ImVec2(x0, (yO+yC)*0.5f), ImVec2(x1, (yO+yC)*0.5f), col, 3.0f);
            } else {
                dl->AddRectFilled(ImVec2(x0, yO), ImVec2(x1, yC), col);
            }
        }
        // Candle hover info (OHLCV) when mouse is over a bar
        {
            ImVec2 m = ImGui::GetIO().MousePos;
            if (m.x>=p0.x && m.x<=p1.x && m.y>=p0.y && m.y<=p1.y && ImGui::IsWindowHovered()) {
                // nearest candle by x
                size_t best = 0; float bestd = 1e9f;
                for (size_t i=0;i<cs.size();++i){ float x = t_to_x((long long)((cs[i].t0+cs[i].t1)/2)); float d = fabsf(m.x - x); if (d<bestd){bestd=d;best=i;} }
                if (best < cs.size()) {
                    auto &k = cs[best];
                    float x = t_to_x((long long)((k.t0+k.t1)/2)); float x0 = x - barW*0.5f, x1 = x + barW*0.5f;
                    if (m.x >= x0 && m.x <= x1) {
                        time_t sec = (time_t)(k.t0/1000);
                        struct tm tmv{}; char tb[64];
#if defined(_WIN32)
                        localtime_s(&tmv, &sec);
#else
                        tmv = *std::localtime(&sec);
#endif
                        strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", &tmv);
                        ImGui::BeginTooltip();
                        ImGui::Text("%s", tb);
                        ImGui::Text("O %.2f  H %.2f  L %.2f  C %.2f", k.o,k.h,k.l,k.c);
                        ImGui::Text("V %.6f", k.v);
                        ImGui::EndTooltip();
                        // highlight rect
                        float yO = p_to_y(k.o), yC = p_to_y(k.c);
                        dl->AddRect(ImVec2(x0-1, std::min(yO,yC)-1), ImVec2(x1+1, std::max(yO,yC)+1), IM_COL32(200,200,220,180));
                    }
                }
            }
        }
        // Big trade pulses (flash & qty text) and impact fireworks at trade price
        static long long lastSeenTradeTs = 0;
        struct Pulse { long long ts; double qty; bool isBuy; };
        static std::vector<Pulse> pulses;
        struct Impact { long long ts; double price; double qty; bool isBuy; };
        static std::vector<Impact> impacts;
        struct MarkerLine { double price; bool isBuy; long long ts; double qty; };
        static std::vector<MarkerLine> markerLines;
        static float pulseMs = 1500.0f;
        static float pulseMaxThick = 6.0f;
        static float pulseMinThick = 2.0f;
        static float pulseTextLift = 14.0f;
        // Snapshot trades
        std::vector<PubTrade> tr;
        {
            std::lock_guard<std::mutex> lk(tradesMutex);
            tr = g_trades;
        }
        long long maxTs = lastSeenTradeTs;
        for (auto& t : tr) {
            if (t.ts <= lastSeenTradeTs) continue;
            if (t.qty >= uiBigTradeQty) {
                pulses.push_back(Pulse{t.ts, t.qty, t.isBuy});
            }
            // Always spawn impact
            impacts.push_back(Impact{t.ts, t.price, t.qty, t.isBuy});
            // Huge trades: persistent marker line
            if (t.qty >= 10.0) {
                markerLines.push_back(MarkerLine{t.price, t.isBuy, t.ts, t.qty});
                if (markerLines.size() > 256) markerLines.erase(markerLines.begin(), markerLines.begin() + (markerLines.size() - 256));
            }
            if (t.ts > maxTs) maxTs = t.ts;
        }
        if (maxTs > lastSeenTradeTs) lastSeenTradeTs = maxTs;
        // Draw marker lines (persistent)
        for (auto &m : markerLines) {
            float y = p_to_y(m.price);
            if (y < p0.y || y > p1.y) continue;
            ImU32 col = m.isBuy ? IM_COL32(80,220,160,180) : IM_COL32(240,120,120,180);
            dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), col, 1.5f);
            char lb[64]; snprintf(lb, sizeof(lb), "%s %.2f", m.isBuy?"BUY":"SELL", m.qty);
            ImVec2 tsz = ImGui::CalcTextSize(lb);
            dl->AddRectFilled(ImVec2(p1.x - tsz.x - 8, y - tsz.y*0.5f - 2), ImVec2(p1.x - 2, y + tsz.y*0.5f + 2), IM_COL32(24,24,28,210));
            dl->AddText(ImVec2(p1.x - tsz.x - 6, y - tsz.y*0.5f), IM_COL32(230,230,240,255), lb);
        }

        // Draw pulses (prune only; visual handled by impacts below)
        long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::vector<Pulse> kept;
        kept.reserve(pulses.size());
        for (auto &p : pulses) {
            float age = (float)(now_ms - p.ts);
            if (age < 0) age = 0;
            if (age > pulseMs) continue; // drop
            kept.push_back(p);
        }
        pulses.swap(kept);

        // Draw impact fireworks at exact trade price position and prune
        std::vector<Impact> keptImp; keptImp.reserve(impacts.size());
        dl->PushClipRect(p0, p1, true);
        for (auto &im : impacts) {
            float age = (float)(now_ms - im.ts);
            float dur = (im.qty >= uiBigTradeQty) ? 900.0f : 400.0f;
            if (age < 0) age = 0; if (age > dur) continue;
            float u = age / dur; float a = 1.0f - u;
            float cx = t_to_x(im.ts);
            float cy = p_to_y(im.price);
            if (cx < p0.x || cx > p1.x || cy < p0.y || cy > p1.y) { keptImp.push_back(im); continue; }
            ImU32 col = im.isBuy ? IM_COL32(80,240,180,(int)(220*a)) : IM_COL32(255,120,120,(int)(220*a));
            // expanding ring
            float r = 2.0f + 20.0f * u;
            dl->AddCircle(ImVec2(cx, cy), r, col, 16, 1.5f);
            // starburst rays
            int rays = 10;
            for (int i=0;i<rays;i++){
                const float TWO_PI = 6.28318530717958647692f;
                float ang = (float)(i * (TWO_PI / rays));
                float dx = cosf(ang), dy = sinf(ang);
                float scale = 1.0f + (float)std::min(3.0, std::sqrt(std::max(0.0, im.qty)));
                float r1 = (3.0f + 10.0f * u) * scale;
                float r2 = r1 + 8.0f * (1.0f - u) * scale * 0.6f;
                dl->AddLine(ImVec2(cx + dx*r1, cy + dy*r1), ImVec2(cx + dx*r2, cy + dy*r2), col, 1.5f);
            }
            // partial body highlight inside candle: bottom for buy, top for sell
            // find containing candle
            size_t idx = (size_t)-1;
            for (size_t i=0;i<cs.size();++i) { if (im.ts >= cs[i].t0 && im.ts < cs[i].t1) { idx = i; break; } }
            if (idx == (size_t)-1) {
                long long bestd = LLONG_MAX; for (size_t i=0;i<cs.size();++i){ long long mid = (cs[i].t0+cs[i].t1)/2; long long d = llabs(mid - im.ts); if (d<bestd){bestd=d; idx=i;}}
            }
            if (idx != (size_t)-1) {
                auto &k = cs[idx];
                float x = t_to_x(k.t0 + ms_per/2);
                float x0 = x - barW*0.5f, x1 = x + barW*0.5f;
                float yBodyTop = p_to_y(std::max(k.o, k.c));
                float yBodyBot = p_to_y(std::min(k.o, k.c));
                float yTrade = p_to_y(im.price);
                float y0, y1;
                if (im.isBuy) { // highlight bottom portion
                    float yClamp = std::max(std::min(yTrade, yBodyBot), yBodyTop);
                    y0 = yClamp; y1 = yBodyBot;
                } else {        // highlight top portion
                    float yClamp = std::max(std::min(yBodyTop, yTrade), yBodyTop); // clamp into body
                    y0 = yBodyTop; y1 = std::min(std::max(yTrade, yBodyTop), yBodyBot);
                }
                if (y1 - y0 > 1.0f) {
                    ImU32 hcol = im.isBuy ? IM_COL32(60,220,160,(int)(100*a)) : IM_COL32(230,90,90,(int)(100*a));
                    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), hcol);
                }
            }
            // big trades: show qty text briefly
            if (im.qty >= uiBigTradeQty) {
                char tb[32]; snprintf(tb, sizeof(tb), "%c%.2f", im.isBuy?'+':'-', im.qty);
                ImVec2 tsz = ImGui::CalcTextSize(tb);
                dl->AddRectFilled(ImVec2(cx - tsz.x*0.5f - 3, cy - r - tsz.y - 6), ImVec2(cx + tsz.x*0.5f + 3, cy - r - 2), IM_COL32(22,22,26,(int)(200*a)));
                dl->AddText(ImVec2(cx - tsz.x*0.5f, cy - r - tsz.y - 4), col, tb);
            }
            keptImp.push_back(im);
        }
        dl->PopClipRect();
        impacts.swap(keptImp);
        // Draw SMAs
        auto draw_line_series = [&](const std::vector<float>& arr, ImU32 col){
            ImVec2 prev{}; bool has=false; for (size_t i=0;i<cs.size();++i){ if (std::isnan(arr[i])) continue; long long t = cs[i].t0 + ms_per/2; if (t<viewT0||t>viewT1) continue; ImVec2 p = ImVec2(t_to_x(t), p_to_y(arr[i])); if (has) dl->AddLine(prev,p,col,1.5f); prev=p; has=true; }
        };
        if (showSMA) {
            draw_line_series(smaA, IM_COL32(255, 215, 0, 255));
            draw_line_series(smaB, IM_COL32(0, 180, 255, 255));
            draw_line_series(smaC, IM_COL32(200, 120, 255, 255));
        }
        // Bollinger Bands
        if (showBB && bbLen > 1) {
            std::vector<float> mean = compute_sma(bbLen);
            std::vector<float> upper(mean.size(), NAN), lower(mean.size(), NAN);
            for (size_t i=0;i<cs.size();++i){ if (i+1<(size_t)bbLen) continue; double mu=mean[i]; double s=0; for (int k=0;k<bbLen;k++){ double d = cs[i-k].c - mu; s += d*d; } double st = sqrt(s/bbLen); upper[i]= (float)(mu + bbK*st); lower[i]=(float)(mu - bbK*st);} 
            draw_line_series(upper, IM_COL32(180,180,180,200));
            draw_line_series(lower, IM_COL32(180,180,180,200));
        }

        // Crosshair with date+price labels
        if (showCross && hovered) {
            ImVec2 m = ImGui::GetIO().MousePos;
            if (m.x>=p0.x && m.x<=p1.x && m.y>=p0.y && m.y<=p1.y) {
                dl->AddLine(ImVec2(m.x, p0.y), ImVec2(m.x, p1.y), IM_COL32(200,200,200,80));
                dl->AddLine(ImVec2(p0.x, m.y), ImVec2(p1.x, m.y), IM_COL32(200,200,200,80));
                long long t_mouse = (long long)(viewT0 + (viewT1 - viewT0) * ((m.x - p0.x) / std::max(1.0f,(p1.x - p0.x))));
                // nearest candle
                size_t best = 0; long long bestd = LLONG_MAX; for (size_t i=0;i<cs.size();++i){ long long ct = cs[i].t0 + ms_per/2; long long d = llabs(ct - t_mouse); if (d<bestd){bestd=d;best=i;}}
                // Price label at crosshair (right axis)
                double priceAtMouse = y_to_p(m.y);
                std::string ps = fmt_price(priceAtMouse);
                ImVec2 pts = ImGui::CalcTextSize(ps.c_str());
                dl->AddRectFilled(ImVec2(p1.x - pts.x - 10, m.y - pts.y*0.5f - 2), ImVec2(p1.x - 2, m.y + pts.y*0.5f + 2), IM_COL32(10,60,70,230));
                dl->AddText(ImVec2(p1.x - pts.x - 6, m.y - pts.y*0.5f), IM_COL32(220,255,255,255), ps.c_str());
                // Time label at bottom
                time_t sec = (time_t)(t_mouse / 1000);
                struct tm tmv{};
#if defined(_WIN32)
                localtime_s(&tmv, &sec);
#else
                tmv = *std::localtime(&sec);
#endif
                char tb[64]; strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", &tmv);
                std::string ts(tb);
                ImVec2 tts = ImGui::CalcTextSize(ts.c_str());
                float tx0 = m.x - tts.x*0.5f; if (tx0 < p0.x+2) tx0 = p0.x+2; if (tx0 + tts.x + 6 > p1.x) tx0 = p1.x - (tts.x + 6);
                dl->AddRectFilled(ImVec2(tx0, p1.y - tts.y - 6), ImVec2(tx0 + tts.x + 6, p1.y - 2), IM_COL32(20,20,24,230));
                dl->AddText(ImVec2(tx0 + 3, p1.y - tts.y - 5), IM_COL32(220,220,230,255), ts.c_str());
            }
        }

        dl->PopClipRect();
        // Reserve a dedicated time-axis strip between chart and volume
        const float axisH = 18.0f;
        // Sub-panels stacked (Vol/RSI/MACD)
        float yBase = p1.y + axisH + 4.0f;
        float eachH = subPanels>0 ? (subTotalH / subPanels) : 0.0f;
        // Time vertical grid across chart + sub-panels, and labels in the axis strip
        {
            float gridBottom = (subPanels>0) ? (yBase + eachH * subPanels - 2.0f) : p1.y;
            // grid lines across chart + subpanels
            dl->PushClipRect(p0, ImVec2(p1.x, gridBottom), true);
            long long span = (viewT1 - viewT0);
            const long long steps[] = {
                1000LL, 2000LL, 5000LL, 10000LL, 15000LL, 30000LL,
                60000LL, 120000LL, 300000LL, 600000LL, 900000LL, 1800000LL,
                3600000LL, 7200000LL, 14400000LL, 21600000LL, 43200000LL, 86400000LL
            };
            long long step = steps[0];
            for (long long s : steps) { if (span / s <= 8) { step = s; break; } else step = s; }
            long long first = ((viewT0 + step - 1) / step) * step;
            for (long long t = first; t < viewT1; t += step) {
                float x = t_to_x(t);
                dl->AddLine(ImVec2(x, p0.y), ImVec2(x, gridBottom), IM_COL32(70,70,90,90));
            }
            dl->PopClipRect();
            // labels in dedicated strip [p1.y .. p1.y+axisH]
            ImVec2 a0 = ImVec2(p0.x, p1.y);
            ImVec2 a1 = ImVec2(p1.x, p1.y + axisH);
            dl->AddRectFilled(a0, a1, IM_COL32(20,20,24,240));
            long long first2 = first;
            for (long long t = first2; t < viewT1; t += step) {
                float x = t_to_x(t);
                time_t sec = (time_t)(t / 1000);
                struct tm tmv{};
#if defined(_WIN32)
                localtime_s(&tmv, &sec);
#else
                tmv = *std::localtime(&sec);
#endif
                char tb[32];
                if (step >= 3600000LL) strftime(tb, sizeof(tb), "%H:%M", &tmv);
                else if (step >= 60000LL) strftime(tb, sizeof(tb), "%H:%M", &tmv);
                else strftime(tb, sizeof(tb), "%H:%M:%S", &tmv);
                ImVec2 tsz = ImGui::CalcTextSize(tb);
                float tx = x - tsz.x * 0.5f; if (tx < a0.x+2) tx = a0.x+2; if (tx + tsz.x + 4 > a1.x) tx = a1.x - (tsz.x + 4);
                float ty = a0.y + (axisH - tsz.y) * 0.5f;
                dl->AddText(ImVec2(tx, ty), IM_COL32(200,200,210,255), tb);
            }
        }
        auto draw_panel_frame = [&](const char* title, ImVec2 a, ImVec2 b){ dl->AddRectFilled(a,b,IM_COL32(16,16,18,255)); dl->AddText(ImVec2(a.x+6,a.y+4), IM_COL32(180,180,180,255), title); };
        int paneIdx = 0;
        if (showVol) {
            ImVec2 v0 = ImVec2(p0.x, yBase + eachH * paneIdx);
            ImVec2 v1 = ImVec2(p1.x, yBase + eachH * (paneIdx+1) - 2.0f);
            draw_panel_frame("Volume", v0, v1);
            double vmax=1.0; for (auto& k:cs){ if (k.t1<viewT0||k.t0>viewT1) continue; vmax=std::max(vmax,k.v);} 
            for (auto& k: cs) {
                if (k.t1<viewT0||k.t0>viewT1) continue; float x = t_to_x((k.t0+k.t1)/2); float x0=x-barW*0.5f, x1=x+barW*0.5f; float vh = (float)((k.v / vmax) * (v1.y - v0.y - 16.0f)); float y1 = v1.y-6.0f; float y0 = y1 - vh; ImU32 col = (k.c>=k.o)?IM_COL32(60,180,120,200):IM_COL32(200,80,80,200); dl->AddRectFilled(ImVec2(x0,y0),ImVec2(x1,y1),col);
            }
            // Right-side axis labels: 0, vmax/2, vmax
            std::string s0 = "0"; auto s1 = fmt_units(vmax*0.5); auto s2 = fmt_units(vmax);
            ImVec2 ts0 = ImGui::CalcTextSize(s0.c_str()); ImVec2 ts1 = ImGui::CalcTextSize(s1.c_str()); ImVec2 ts2 = ImGui::CalcTextSize(s2.c_str());
            dl->AddText(ImVec2(v1.x - ts0.x - 4, v1.y - ts0.y - 2), IM_COL32(170,170,180,220), s0.c_str());
            dl->AddText(ImVec2(v1.x - ts1.x - 4, v0.y + (v1.y - v0.y)*0.5f - ts1.y*0.5f), IM_COL32(170,170,180,220), s1.c_str());
            dl->AddText(ImVec2(v1.x - ts2.x - 4, v0.y + 2), IM_COL32(170,170,180,220), s2.c_str());
            // Current value label
            if (!cs.empty()) {
                double lastV = cs.back().v;
                std::string cur = std::string("Vol: ") + fmt_units(lastV);
                dl->AddText(ImVec2(v0.x + 60, v0.y + 4), IM_COL32(220,220,220,255), cur.c_str());
                // Baseline at current volume level across the pane
                float y1 = v1.y - 6.0f;
                float yBase = y1 - (float)((lastV / std::max(1.0, vmax)) * (v1.y - v0.y - 16.0f));
                dl->AddLine(ImVec2(v0.x+2, yBase), ImVec2(v1.x-2, yBase), IM_COL32(180,180,200,120), 1.0f);
                // Label at right
                auto sCur = fmt_units(lastV);
                ImVec2 tsc = ImGui::CalcTextSize(sCur.c_str());
                dl->AddRectFilled(ImVec2(v1.x - tsc.x - 8, yBase - tsc.y*0.5f - 1), ImVec2(v1.x - 2, yBase + tsc.y*0.5f + 1), IM_COL32(22,22,26,210));
                dl->AddText(ImVec2(v1.x - tsc.x - 6, yBase - tsc.y*0.5f), IM_COL32(210,210,230,255), sCur.c_str());
            }
            // Hover on volume bar -> show info
            ImVec2 m = ImGui::GetIO().MousePos;
            if (m.x>=v0.x && m.x<=v1.x && m.y>=v0.y && m.y<=v1.y && ImGui::IsWindowHovered()) {
                // nearest candle
                size_t best=0; float bestd=1e9f; float bestx=0; for (size_t i=0;i<cs.size();++i){ float x = t_to_x((long long)((cs[i].t0+cs[i].t1)/2)); float d=fabsf(m.x-x); if (d<bestd){bestd=d; best=i; bestx=x;} }
                auto &k = cs[best];
                ImGui::BeginTooltip(); ImGui::Text("Vol: %.6f", k.v); ImGui::EndTooltip();
                // outline
                float x0 = bestx - barW*0.5f, x1 = bestx + barW*0.5f; float vh = (float)((k.v / std::max(1.0, vmax)) * (v1.y - v0.y - 16.0f)); float yy1=v1.y-6.0f; float yy0=yy1-vh;
                dl->AddRect(ImVec2(x0, yy0), ImVec2(x1, yy1), IM_COL32(200,200,220,160));
            }
            paneIdx++;
        }
        if (showRSI) {
            ImVec2 r0 = ImVec2(p0.x, yBase + eachH * paneIdx);
            ImVec2 r1 = ImVec2(p1.x, yBase + eachH * (paneIdx+1) - 2.0f);
            draw_panel_frame("RSI", r0, r1);
            // compute RSI
            std::vector<float> rsi(cs.size(), NAN);
            int n = std::max(2, rsiLen); double avgU=0, avgD=0; int k=0; for (size_t i=1;i<cs.size();++i){ double ch = cs[i].c - cs[i-1].c; double u = ch>0?ch:0; double d = ch<0?-ch:0; if (k < n){ avgU += u; avgD += d; k++; if (k==n){ avgU/=n; avgD/=n; rsi[i]= (float)(100.0 * (avgU+avgD>0? (avgU/(avgU+avgD)) : 0.5)); } } else { avgU = (avgU*(n-1)+u)/n; avgD = (avgD*(n-1)+d)/n; rsi[i]=(float)(100.0*(avgU+avgD>0? (avgU/(avgU+avgD)) : 0.5)); } }
            auto y_of = [&](float v){ return r1.y - (float)((v/100.0f) * (r1.y - r0.y - 10.0f)) - 6.0f; };
            // guide lines
            float y30=y_of(30), y70=y_of(70); dl->AddLine(ImVec2(r0.x,y30),ImVec2(r1.x,y30),IM_COL32(140,140,140,100)); dl->AddLine(ImVec2(r0.x,y70),ImVec2(r1.x,y70),IM_COL32(140,140,140,100));
            ImVec2 prev{}; bool has=false; for (size_t i=0;i<cs.size();++i){ if (std::isnan(rsi[i])) continue; long long t = cs[i].t0 + (long long)(interval_to_ms(intervals[ivIdx])/2); if (t<viewT0||t>viewT1) continue; ImVec2 p = ImVec2(t_to_x(t), y_of(rsi[i])); if (has) dl->AddLine(prev,p,IM_COL32(0,200,255,220),1.5f); prev=p; has=true; }
            // Axis labels 0/30/50/70/100 on right
            auto place_label = [&](float val){ std::string s = std::to_string((int)val); ImVec2 ts = ImGui::CalcTextSize(s.c_str()); dl->AddText(ImVec2(r1.x - ts.x - 4, y_of(val) - ts.y*0.5f), IM_COL32(170,170,180,220), s.c_str()); };
            place_label(0); place_label(30); place_label(50); place_label(70); place_label(100);
            // Current RSI value
            float lastR = NAN; for (int i=(int)rsi.size()-1;i>=0;--i){ if (!std::isnan(rsi[i])){ lastR = rsi[i]; break; } }
            if (!std::isnan(lastR)) { char b[64]; snprintf(b,sizeof(b),"RSI: %.1f", lastR); dl->AddText(ImVec2(r0.x + 50, r0.y + 4), IM_COL32(220,220,220,255), b); }
            // Hover on RSI line -> show value nearest to mouse
            ImVec2 m = ImGui::GetIO().MousePos;
            if (m.x>=r0.x && m.x<=r1.x && m.y>=r0.y && m.y<=r1.y && ImGui::IsWindowHovered()) {
                size_t best=0; float bestd=1e9f; for (size_t i=0;i<cs.size();++i){ long long t = cs[i].t0 + ms_per/2; float x=t_to_x(t); float d=fabsf(m.x-x); if (d<bestd){bestd=d; best=i;} }
                if (best<rsi.size() && !std::isnan(rsi[best])) {
                    float y = y_of(rsi[best]); float x = t_to_x(cs[best].t0 + ms_per/2);
                    dl->AddCircleFilled(ImVec2(x,y), 3.0f, IM_COL32(0,200,255,220));
                    ImGui::BeginTooltip(); ImGui::Text("RSI: %.2f", rsi[best]); ImGui::EndTooltip();
                }
            }
            paneIdx++;
        }
        if (showMACD) {
            ImVec2 m0 = ImVec2(p0.x, yBase + eachH * paneIdx);
            ImVec2 m1 = ImVec2(p1.x, yBase + eachH * (paneIdx+1) - 2.0f);
            draw_panel_frame("MACD", m0, m1);
            // EMA helper
            auto ema = [&](int n){ std::vector<double> out(cs.size(), NAN); double k = 2.0/(n+1.0); double v = cs[0].c; out[0]=v; for (size_t i=1;i<cs.size();++i){ v = k*cs[i].c + (1.0-k)*v; out[i]=v; } return out; };
            int f=std::max(2,macdFast), s=std::max(3,macdSlow), sig=std::max(2,macdSig);
            auto emF = ema(f), emS = ema(s);
            std::vector<double> macd(cs.size(), NAN); for (size_t i=0;i<cs.size();++i){ if (!std::isnan(emF[i]) && !std::isnan(emS[i])) macd[i] = emF[i]-emS[i]; }
            // signal EMA over macd
            std::vector<double> sigv(cs.size(), NAN); if (cs.size()>1){ double k=2.0/(sig+1.0); double v=macd[1]; sigv[1]=v; for (size_t i=2;i<cs.size();++i){ double m=std::isnan(macd[i])?v:macd[i]; v = k*m + (1.0-k)*v; sigv[i]=v; }}
            // scale
            double mn=1e300,mx=-1e300; for (size_t i=0;i<cs.size();++i){ if (!std::isnan(macd[i])){ mn=std::min<double>(mn, macd[i]); mx=std::max<double>(mx, macd[i]); } if (!std::isnan(sigv[i])){ mn=std::min<double>(mn, sigv[i]); mx=std::max<double>(mx, sigv[i]); } }
            if (mx<=mn){ mn=-1; mx=1; }
            auto y_of = [&](double v){ double a=(v-mn)/(mx-mn); return m1.y - (float)(a*(m1.y-m0.y-10.0f)) - 6.0f; };
            ImVec2 prevM{}, prevS{}; bool hm=false, hs=false; for (size_t i=0;i<cs.size();++i){ long long t = cs[i].t0 + (long long)(interval_to_ms(intervals[ivIdx])/2); if (t<viewT0||t>viewT1) continue; if (!std::isnan(macd[i])){ ImVec2 p = ImVec2(t_to_x(t), y_of(macd[i])); if (hm) dl->AddLine(prevM,p,IM_COL32(255,180,0,220),1.5f); prevM=p; hm=true; } if (!std::isnan(sigv[i])){ ImVec2 p = ImVec2(t_to_x(t), y_of(sigv[i])); if (hs) dl->AddLine(prevS,p,IM_COL32(0,200,120,220),1.5f); prevS=p; hs=true; } }
            // Zero line and axis labels (min/0/max) on right
            float yZero = y_of(0.0); dl->AddLine(ImVec2(m0.x, yZero), ImVec2(m1.x, yZero), IM_COL32(130,130,140,100));
            auto put_lbl = [&](double val){ char b[64]; snprintf(b,sizeof(b),"%.4f", val); ImVec2 ts=ImGui::CalcTextSize(b); dl->AddText(ImVec2(m1.x - ts.x - 4, y_of(val) - ts.y*0.5f), IM_COL32(170,170,180,220), b); };
            put_lbl(mx); put_lbl(0.0); put_lbl(mn);
            // Current values MACD/Signal
            double mlast=NAN, slast=NAN; for (int i=(int)macd.size()-1;i>=0;--i){ if (std::isnan(mlast) && !std::isnan(macd[i])) mlast=macd[i]; if (std::isnan(slast) && !std::isnan(sigv[i])) slast=sigv[i]; if (!std::isnan(mlast) && !std::isnan(slast)) break; }
            if (!std::isnan(mlast) && !std::isnan(slast)) { char b[128]; snprintf(b,sizeof(b),"MACD: %.4f  Sig: %.4f", mlast, slast); dl->AddText(ImVec2(m0.x + 60, m0.y + 4), IM_COL32(220,220,220,255), b); }
            paneIdx++;
        }
    } else {
        ImVec2 center = ImVec2((p0.x+p1.x)*0.5f, (p0.y+p1.y)*0.5f);
        dl->AddText(center, IM_COL32(180,180,180,255), g_chartLoading?"Loading...":"No data. Click Load.");
        ImGui::Dummy(ImVec2(avail.x, chartH));
    }

    ImGui::End();
}

static void GuiMain()
{
    // Register and create window
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, _T("BinanceRJTechClass"), NULL };
    RegisterClassEx(&wc);
    // Create window sized to screen
    int scrW = GetSystemMetrics(SM_CXSCREEN);
    int scrH = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindow(wc.lpszClassName, _T("Binance Order Book (ImGui)"), WS_OVERLAPPEDWINDOW, 0, 0, scrW, scrH, NULL, NULL, wc.hInstance, NULL);

    // Initialize Direct3D
    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        UnregisterClass(wc.lpszClassName, wc.hInstance);
        return;
    }

    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(hwnd);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.IniFilename = "imgui_layout.ini"; // remember window positions/sizes
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // Main loop
    MSG msg;
    ZeroMemory(&msg, sizeof(msg));
    auto tickStart = std::chrono::steady_clock::now();
    using clock = std::chrono::steady_clock;
    auto frameStart = clock::now();
    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }

        // Update 1-second counter
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - tickStart);
        if (elapsed.count() >= 1) {
            tickStart = now;
            lastMessageCount.store(messageCount.exchange(0), std::memory_order_acq_rel);
        }

        // Start frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        RenderOrderBookUI();
        RenderChartWindow();

        // Render
        ImGui::Render();
        const float clear_color_with_alpha[4] = { 0.06f, 0.06f, 0.07f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        // Present immediately (no vsync) and cap to ~100 FPS
        g_pSwapChain->Present(0, 0);

        /*auto frameEnd = clock::now();
        auto frameDur = std::chrono::duration_cast<std::chrono::microseconds>(frameEnd - frameStart);
        const auto target = std::chrono::milliseconds(10); // ~100 FPS
        if (frameDur < target)
            std::this_thread::sleep_for(target - frameDur);
        frameStart = clock::now();
    */
    }

    // Cleanup
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    // Save UI layout/state
    if (io.IniFilename && *io.IniFilename)
        ImGui::SaveIniSettingsToDisk(io.IniFilename);
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClass(wc.lpszClassName, wc.hInstance);
}

int main() {
    try {
        const std::string host = "fstream.binance.com"; // futures
        const std::string port = "443";

        // Hide console window (we keep Console subsystem for simpler linking)
        ::ShowWindow(::GetConsoleWindow(), SW_HIDE);

        // Start 10 worker threads with small stagger to smooth updates
        const int kWorkers = 20;
        for (int i = 0; i < kWorkers; ++i) {
            std::thread(receiveOrderBook, host, port, i + 1).detach();
            std::this_thread::sleep_for(std::chrono::milliseconds(5)); // ~50 ms total spread
        }
        // Start public trades receiver for BTCUSDT
        std::thread(receivePublicTrades, host, port, std::string("btcusdt")).detach();
        GuiMain();
    }
    catch (const std::exception& ex) {
        std::cerr << "Fatal error: " << ex.what() << std::endl;
        return 1;
    }
    return 0;
}
