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
                    if (g_trades.size() > 200) g_trades.erase(g_trades.begin(), g_trades.begin() + (g_trades.size()-200));
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
    static std::vector<std::tuple<std::string,double,double,int,double,std::string,std::string,double>> s_positions; // symbol, amt, entry, lev, upnl, marginType, side, mark
    static std::mutex s_positionsMutex;

    static std::unique_ptr<BinanceRest> s_rest(new BinanceRest("fapi.binance.com"));
    if (s_rest) s_rest->setInsecureTLS(false);

    // Background poller to refresh positions/balances every 100ms
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
                                s_positions.swap(pos);
                            }
                        } catch (...) {}
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 160.0f);
            ImGui::TableSetupColumn("Side", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("Price", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Qty", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (int i = (int)local.size()-1; i >= 0; --i) {
                const auto& t = local[(size_t)i];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char tb[64];
                double sec = t.ts / 1000.0; snprintf(tb, sizeof(tb), "%.3f", sec);
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
    if (s_showTradingWin) {
        ImGui::SetNextWindowSize(ImVec2(560, 680), ImGuiCond_FirstUseEver);
        ImGui::Begin("Trading - Binance Futures (Mainnet)", &s_showTradingWin);
        // Use shared trading state
        char* sym = t_sym;
        float& orderQty = t_orderQty;
        float& limitPrice = t_limitPrice;
        float& stopPrice = t_stopPrice;
        int& orderTypeIdx = t_orderTypeIdx;
        int& tifIdx = t_tifIdx;
        bool& reduceOnly = t_reduceOnly;
        bool& dualSide = t_dualSide;
        int& leverage = t_leverage;
        int& marginTypeIdx = t_marginTypeIdx;
        std::string& lastOrderResp = t_lastOrderResp;
        const char* types[] = {"MARKET","LIMIT","STOP_MARKET","TAKE_PROFIT_MARKET"};
        const char* tifs[] = {"GTC","IOC","FOK"};
        const char* margins[] = {"CROSS","ISOLATED"};

        if (ImGui::BeginTable("TradeForm", 2, ImGuiTableFlags_Resizable|ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 170.0f);
            ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch);

            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("Symbol");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN); ImGui::InputText("##sym", sym, sizeof(sym));
            if (std::string(sym) == "BTCUSDT") s_qtyStep = 0.001; // enforce BTC minimum lot size

            // Load symbol filters and account balance
            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("Filters/Bal");
            ImGui::TableSetColumnIndex(1);
            if (ImGui::Button("Refresh (exchangeInfo + account)", ImVec2(-FLT_MIN, 0))) {
                if (s_rest) {
                    // exchangeInfo
                    auto r1 = s_rest->getExchangeInfo(sym);
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
                                        auto getd = [](const nlohmann::json& v)->double {
                                            if (v.is_string()) return std::stod(v.get<std::string>()); else return v.get<double>(); };
                                        if (ft == "PRICE_FILTER") {
                                            if (f.contains("tickSize")) tick = getd(f["tickSize"]);
                                        } else if (ft == "LOT_SIZE") {
                                            if (f.contains("stepSize")) step = getd(f["stepSize"]);
                                            if (f.contains("minQty"))  minq = getd(f["minQty"]);
                                        } else if (ft == "MARKET_LOT_SIZE") {
                                            if (f.contains("stepSize")) step = getd(f["stepSize"]); // prefer market step
                                            if (f.contains("minQty"))  minq = getd(f["minQty"]);
                                        }
                                    }
                                    s_priceTick = tick; s_qtyStep = step; s_minQty = minq;
                                }
                            }
                        } catch (...) {}
                    }
                    // account info
                    auto r2 = s_rest->getAccountInfo(5000);
                    if (r2.ok) {
                        try {
                            using nlohmann::json; auto j = json::parse(r2.body, nullptr, false);
                            // commission rates at account root
                            if (j.is_object()) {
                                auto getd = [](const nlohmann::json& v)->double { if (v.is_string()) return std::stod(v.get<std::string>()); else return v.get<double>(); };
                                if (j.contains("takerCommissionRate")) s_takerRate = getd(j["takerCommissionRate"]);
                                if (j.contains("makerCommissionRate")) s_makerRate = getd(j["makerCommissionRate"]);
                            }
                            if (j.is_object() && j.contains("assets") && j["assets"].is_array()) {
                                for (auto& a : j["assets"]) {
                                    if (!a.is_object() || !a.contains("asset")) continue;
                                    auto asset = a["asset"].get<std::string>();
                                    if (asset == "USDT") {
                                        auto getd = [](const nlohmann::json& v)->double { if (v.is_string()) return std::stod(v.get<std::string>()); else return v.get<double>(); };
                                        double avail = s_availableUSDT, margin = s_marginBalanceUSDT;
                                        if (a.contains("availableBalance")) avail = getd(a["availableBalance"]);
                                        if (a.contains("marginBalance"))    margin = getd(a["marginBalance"]);
                                        {
                                            std::lock_guard<std::mutex> lk(s_positionsMutex);
                                            s_availableUSDT = avail;
                                            s_marginBalanceUSDT = margin;
                                        }
                                        break;
                                    }
                                }
                            }
                            // positions
                            std::vector<std::tuple<std::string,double,double,int,double,std::string,std::string,double>> pos;
                            if (j.is_object() && j.contains("positions") && j["positions"].is_array()) {
                                for (auto& p : j["positions"]) {
                                    if (!p.is_object()) continue;
                                    auto getd = [](const nlohmann::json& v)->double { if (v.is_string()) return std::stod(v.get<std::string>()); else return v.get<double>(); };
                                    std::string psymbol = p.contains("symbol") ? p["symbol"].get<std::string>() : std::string();
                                    double amt = p.contains("positionAmt") ? getd(p["positionAmt"]) : 0.0;
                                    double entry = p.contains("entryPrice") ? getd(p["entryPrice"]) : 0.0;
                                    int lev = p.contains("leverage") ? (p["leverage"].is_string()? std::stoi(p["leverage"].get<std::string>()) : p["leverage"].get<int>()) : 0;
                                    double upnl = p.contains("unrealizedProfit") ? getd(p["unrealizedProfit"]) : 0.0;
                                    std::string mtype = p.contains("marginType") ? p["marginType"].get<std::string>() : std::string();
                                    std::string pside = p.contains("positionSide") ? p["positionSide"].get<std::string>() : std::string();
                                    double mark = p.contains("markPrice") ? getd(p["markPrice"]) : 0.0;
                                    if (std::abs(amt) > 1e-12) pos.emplace_back(psymbol, amt, entry, lev, upnl, mtype, pside, mark);
                                }
                            }
                            {
                                std::lock_guard<std::mutex> lk(s_positionsMutex);
                                s_positions.swap(pos);
                            }
                            // keep BTC min step explicitly
                            if (std::string(sym) == "BTCUSDT") s_qtyStep = 0.001;
                        } catch (...) {}
                    }
                    char buf[256]; snprintf(buf, sizeof(buf), "tick=%.8g step=%.8g minQty=%.8g  avail=%.2f  margin=%.2f", s_priceTick, s_qtyStep, s_minQty, s_availableUSDT, s_marginBalanceUSDT);
                    s_filtersMsg = buf;
                }
            }
            if (!s_filtersMsg.empty()) { ImGui::TextWrapped("%s", s_filtersMsg.c_str()); }
            // Explicit balance rows
            ImGui::Text("Available: %.2f USDT", s_availableUSDT);
            ImGui::SameLine();
            ImGui::Text("Margin Balance: %.2f USDT", s_marginBalanceUSDT);

            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("Leverage");
            ImGui::TableSetColumnIndex(1); ImGui::SetNextItemWidth(-FLT_MIN); ImGui::SliderInt("##lev", &leverage, 1, 125);
            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("");
            ImGui::TableSetColumnIndex(1); if (ImGui::Button("Set Leverage", ImVec2(-FLT_MIN, 0))) { std::unique_ptr<BinanceRest> tmp(new BinanceRest("fapi.binance.com")); auto r = tmp->setLeverage(sym, leverage); lastOrderResp = std::string("Set Leverage: ") + (r.ok?"OK ":"ERR ") + std::to_string(r.status) + "\n" + r.body; }

            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("Margin Type");
            ImGui::TableSetColumnIndex(1); ImGui::SetNextItemWidth(-FLT_MIN); ImGui::Combo("##margin", &marginTypeIdx, margins, IM_ARRAYSIZE(margins));
            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("");
            ImGui::TableSetColumnIndex(1); if (ImGui::Button("Apply Margin", ImVec2(-FLT_MIN, 0))) { std::unique_ptr<BinanceRest> tmp(new BinanceRest("fapi.binance.com")); auto r = tmp->setMarginType(sym, margins[marginTypeIdx]); lastOrderResp = std::string("MarginType: ") + (r.ok?"OK ":"ERR ") + std::to_string(r.status) + "\n" + r.body; }

            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("Hedge Mode");
            ImGui::TableSetColumnIndex(1); ImGui::Checkbox("##dual", &dualSide);
            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("");
            ImGui::TableSetColumnIndex(1); if (ImGui::Button("Apply Mode", ImVec2(-FLT_MIN, 0))) { std::unique_ptr<BinanceRest> tmp(new BinanceRest("fapi.binance.com")); auto r = tmp->setDualPosition(dualSide); lastOrderResp = std::string("DualSide: ") + (r.ok?"OK ":"ERR ") + std::to_string(r.status) + "\n" + r.body; }

            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("Type");
            ImGui::TableSetColumnIndex(1); ImGui::SetNextItemWidth(-FLT_MIN); ImGui::Combo("##type", &orderTypeIdx, types, IM_ARRAYSIZE(types));

            // Sizing helpers
            auto floor_step = [](double v, double step)->double { if (step <= 0) return v; double n = std::floor((v + 1e-12) / step); return n * step; };
            auto best_prices = []()->std::pair<double,double> {
                std::lock_guard<std::mutex> lk(bookMutex);
                double ask = 0.0, bid = 0.0;
                if (!g_bookAsks.empty()) ask = g_bookAsks.begin()->first; 
                if (!g_bookBids.empty()) bid = g_bookBids.begin()->first; 
                return {ask,bid}; };

            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("Quantity");
            ImGui::TableSetColumnIndex(1); ImGui::SetNextItemWidth(-FLT_MIN); ImGui::InputFloat("##qty", &orderQty, 0.0f, 0.0f, "%.6f");

            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("Size %% of Margin");
            ImGui::TableSetColumnIndex(1);
            bool pctChanged = false;
            ImGui::SetNextItemWidth(-FLT_MIN); pctChanged = ImGui::SliderFloat("##szpct", &s_sizePct, 1.0f, 100.0f, "%.0f%%");
            ImGui::Checkbox("Use Leverage in sizing", &s_useLeverageForSize);
            ImGui::SameLine();
            if (ImGui::RadioButton("Long", s_sizeForLong==true)) s_sizeForLong = true;
            ImGui::SameLine();
            if (ImGui::RadioButton("Short", s_sizeForLong==false)) s_sizeForLong = false;
            ImGui::SameLine();
            if (ImGui::Button("Apply to Qty (LONG)", ImVec2(0,0))) {
                double refPrice = (orderTypeIdx==1 && limitPrice>0)? limitPrice : best_prices().first; // ask
                if (refPrice > 0) {
                    double notional = s_marginBalanceUSDT * (s_sizePct/100.0f) * (s_useLeverageForSize? (double)leverage : 1.0);
                    double q = notional / refPrice; q = floor_step(q, s_qtyStep); if (q < s_minQty) q = s_minQty;
                    orderQty = (float)q;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Apply to Qty (SHORT)", ImVec2(0,0))) {
                double refPrice = (orderTypeIdx==1 && limitPrice>0)? limitPrice : best_prices().second; // bid
                if (refPrice > 0) {
                    double notional = s_marginBalanceUSDT * (s_sizePct/100.0f) * (s_useLeverageForSize? (double)leverage : 1.0);
                    double q = notional / refPrice; q = floor_step(q, s_qtyStep); if (q < s_minQty) q = s_minQty;
                    orderQty = (float)q;
                }
            }
            // Real-time update when percent changes
            if (pctChanged) {
                auto [ask,bid] = best_prices();
                double refPrice = (orderTypeIdx==1 && limitPrice>0)? limitPrice : (s_sizeForLong? ask : bid);
                if (refPrice > 0) {
                    double notional = s_marginBalanceUSDT * (s_sizePct/100.0f) * (s_useLeverageForSize? (double)leverage : 1.0);
                    double q = notional / refPrice; q = floor_step(q, s_qtyStep); if (q < s_minQty) q = s_minQty;
                    orderQty = (float)q;
                }
            }

            if (orderTypeIdx==1) {
                ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("Limit Price");
                ImGui::TableSetColumnIndex(1); ImGui::SetNextItemWidth(-FLT_MIN); ImGui::InputFloat("##limit", &limitPrice, 0.0f, 0.0f, "%.2f");
            }
            if (orderTypeIdx==2 || orderTypeIdx==3) {
                ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("Stop Price");
                ImGui::TableSetColumnIndex(1); ImGui::SetNextItemWidth(-FLT_MIN); ImGui::InputFloat("##stop", &stopPrice, 0.0f, 0.0f, "%.2f");
            }

            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("TimeInForce");
            ImGui::TableSetColumnIndex(1); ImGui::SetNextItemWidth(-FLT_MIN); ImGui::Combo("##tif", &tifIdx, tifs, IM_ARRAYSIZE(tifs));

            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("Reduce Only");
            ImGui::TableSetColumnIndex(1); ImGui::Checkbox("##reduce", &reduceOnly);

            // Action buttons
            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("");
            ImGui::TableSetColumnIndex(1);
            auto do_order = [&](bool isLong){
                if (!s_rest) return;
                std::string side = isLong?"BUY":"SELL";
                std::string positionSide;
                if (dualSide) positionSide = isLong?"LONG":"SHORT";
                // Quantize quantity/price/stop to exchange filters
                double qQty = floor_step(orderQty, s_qtyStep); if (qQty < s_minQty) qQty = s_minQty;
                double qPrice = (orderTypeIdx==1 && limitPrice>0)? floor_step(limitPrice, s_priceTick) : 0.0;
                double qStop = (orderTypeIdx==2 || orderTypeIdx==3) ? floor_step(stopPrice, s_priceTick) : 0.0;
                auto r = s_rest->placeOrder(sym, side, types[orderTypeIdx], qQty, qPrice, tifs[tifIdx], reduceOnly, false, 5000, positionSide, qStop, "MARK_PRICE");
                char hdr[64]; snprintf(hdr, sizeof(hdr), "%s %s: ", isLong?"LONG":"SHORT", types[orderTypeIdx]);
                lastOrderResp = std::string(hdr) + (r.ok?"OK ":"ERR ") + std::to_string(r.status) + "\n" + r.body;
            };
            // Hotkey: Ctrl+J (Limit Long 100%), Ctrl+K (Limit Short 100%)
            auto do_quick_limit = [&](bool isLong){
                if (!s_rest) return;
                auto [ask,bid] = best_prices();
                double refPrice = isLong ? ask : bid;
                if (refPrice <= 0.0) return;
                double notional = s_marginBalanceUSDT * 1.0 * (s_useLeverageForSize? (double)leverage : 1.0);
                double qty = notional / refPrice;
                // enforce BTC step when BTCUSDT
                if (std::string(sym) == "BTCUSDT" && s_qtyStep < 0.001) s_qtyStep = 0.001;
                double qQty = floor_step(qty, s_qtyStep); if (qQty < s_minQty) qQty = s_minQty;
                double qPrice = floor_step(refPrice, s_priceTick);
                std::string side = isLong?"BUY":"SELL";
                std::string positionSide;
                if (dualSide) positionSide = isLong?"LONG":"SHORT";
                auto r = s_rest->placeOrder(sym, side, "LIMIT", qQty, qPrice, tifs[tifIdx], false, false, 5000, positionSide, 0.0, "MARK_PRICE");
                char hdr[64]; snprintf(hdr, sizeof(hdr), "HK %s LIMIT: ", isLong?"LONG":"SHORT");
                lastOrderResp = std::string(hdr) + (r.ok?"OK ":"ERR ") + std::to_string(r.status) + "\n" + r.body;
            };
            {
                ImGuiIO& ioHK = ImGui::GetIO();
                if (ioHK.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_J)) do_quick_limit(true);
                if (ioHK.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K)) do_quick_limit(false);
            }
            if (ImGui::Button("LONG (BUY)", ImVec2(-FLT_MIN, 0))) do_order(true);
            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("");
            ImGui::TableSetColumnIndex(1); if (ImGui::Button("SHORT (SELL)", ImVec2(-FLT_MIN, 0))) do_order(false);

            // Response box spans control column
            if (!lastOrderResp.empty()) {
                ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("Response");
                ImGui::TableSetColumnIndex(1);
                ImGui::BeginChild("orderresp", ImVec2(0, 220), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(lastOrderResp.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndChild();
            }

            ImGui::EndTable();
        }

        ImGui::End();
    }

    // Separate: Positions window
    if (s_showPositionsWin) {
        ImGui::SetNextWindowSize(ImVec2(520, 360), ImGuiCond_FirstUseEver);
        ImGui::Begin("Positions", &s_showPositionsWin);
        // uses s_positions shared vector
        if (ImGui::Button("Refresh Positions", ImVec2(-FLT_MIN, 0))) {
            if (s_rest) {
                auto r = s_rest->getAccountInfo(5000);
                if (r.ok) {
                    try {
                        using nlohmann::json; auto j = json::parse(r.body, nullptr, false);
                        s_positions.clear();
                        if (j.is_object() && j.contains("positions") && j["positions"].is_array()) {
                            for (auto& p : j["positions"]) {
                                if (!p.is_object()) continue;
                                auto getd = [](const nlohmann::json& v)->double { if (v.is_string()) return std::stod(v.get<std::string>()); else return v.get<double>(); };
                                std::string psymbol = p.contains("symbol") ? p["symbol"].get<std::string>() : std::string();
                                double amt = p.contains("positionAmt") ? getd(p["positionAmt"]) : 0.0;
                                double entry = p.contains("entryPrice") ? getd(p["entryPrice"]) : 0.0;
                                int lev = p.contains("leverage") ? (p["leverage"].is_string()? std::stoi(p["leverage"].get<std::string>()) : p["leverage"].get<int>()) : 0;
                                    double upnl = p.contains("unrealizedProfit") ? getd(p["unrealizedProfit"]) : 0.0;
                                    std::string mtype = p.contains("marginType") ? p["marginType"].get<std::string>() : std::string();
                                    std::string pside = p.contains("positionSide") ? p["positionSide"].get<std::string>() : std::string();
                                    double mark = p.contains("markPrice") ? getd(p["markPrice"]) : 0.0;
                                    if (std::abs(amt) > 1e-12) s_positions.emplace_back(psymbol, amt, entry, lev, upnl, mtype, pside, mark);
                            }
                        }
                        // also update balances
                        if (j.is_object() && j.contains("assets") && j["assets"].is_array()) {
                            for (auto& a : j["assets"]) {
                                if (!a.is_object() || !a.contains("asset")) continue;
                                auto asset = a["asset"].get<std::string>();
                                if (asset == "USDT") {
                                    auto getd = [](const nlohmann::json& v)->double { if (v.is_string()) return std::stod(v.get<std::string>()); else return v.get<double>(); };
                                    if (a.contains("availableBalance")) s_availableUSDT = getd(a["availableBalance"]);
                                    if (a.contains("marginBalance"))    s_marginBalanceUSDT = getd(a["marginBalance"]);
                                    break;
                                }
                            }
                        }
                    } catch (...) {}
                }
            }
        }
        // Snapshot positions under lock for rendering
        std::vector<std::tuple<std::string,double,double,int,double,std::string,std::string,double>> pos_local;
        {
            std::lock_guard<std::mutex> lk(s_positionsMutex);
            pos_local = s_positions;
        }
        if (ImGui::BeginTable("PositionsTable", 8, ImGuiTableFlags_RowBg|ImGuiTableFlags_Borders|ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Symbol", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("Side", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("Qty", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Entry", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Lev", ImGuiTableColumnFlags_WidthFixed, 50.0f);
            ImGui::TableSetupColumn("uPnL(fee)", ImGuiTableColumnFlags_WidthStretch);
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
                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(amt>0?"LONG":(amt<0?"SHORT":pside.c_str()));
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
                ImGui::TableSetColumnIndex(5); ImGui::Text("%.2f", pnlFee);
                ImGui::TableSetColumnIndex(6); ImGui::TextUnformatted(mtype.c_str());
                ImGui::TableSetColumnIndex(7); ImGui::Text("T%.4f%%", s_takerRate*100.0);
            }
            ImGui::EndTable();
        }
        ImGui::End();
    }

    // No animation state to maintain
}

static void GuiMain()
{
    // Register and create window
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, _T("BinanceRJTechClass"), NULL };
    RegisterClassEx(&wc);
    HWND hwnd = CreateWindow(wc.lpszClassName, _T("Binance Order Book (ImGui)"), WS_OVERLAPPEDWINDOW, 100, 100, 960, 720, NULL, NULL, wc.hInstance, NULL);

    // Initialize Direct3D
    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        UnregisterClass(wc.lpszClassName, wc.hInstance);
        return;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
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
