#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace telemetry {

// Initializes the telemetry sink. When logDirectory is empty a default logs/ folder is used.
void startSession(const std::string& sessionName, const std::string& logDirectory = {});

// Flushes any buffered telemetry data to disk.
void flush();

// Records a numeric gauge value (e.g., FPS, outstanding jobs).
void logGauge(const std::string& category, const std::string& label, double value);

// Records a counter delta (e.g., messages processed).
void logCounter(const std::string& category, const std::string& label, std::int64_t delta);

// Emits a text event for informational messages.
void logEvent(const std::string& category, const std::string& message);

// Captures the duration of a scoped block.
class ScopedTimer {
public:
    ScopedTimer(const std::string& category, const std::string& label);
    ~ScopedTimer();

private:
    std::string category_;
    std::string label_;
    std::chrono::steady_clock::time_point start_;
};

// Executes a callable and records its duration (helper for asynchronous code).
template <typename Func>
inline auto measure(const std::string& category, const std::string& label, Func&& fn) -> decltype(fn()) {
    auto begin = std::chrono::steady_clock::now();
    auto result = fn();
    auto end = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
    logGauge(category, label + ".duration_us", static_cast<double>(elapsed));
    return result;
}

} // namespace telemetry
