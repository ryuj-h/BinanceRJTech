#include "binancerj/telemetry/PerfTelemetry.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>

namespace {
std::mutex gLogMutex;
std::ofstream gLogFile;
std::atomic<bool> gSessionStarted{false};
std::string gSessionName;

std::string timestampUtc() {
    using clock = std::chrono::system_clock;
    auto now = clock::now();
    auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
    auto fractional = std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count();

    std::time_t tt = clock::to_time_t(seconds);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << fractional << 'Z';
    return oss.str();
}

void ensureLogOpen(const std::string& directory) {
    if (gLogFile.is_open()) {
        return;
    }

    std::filesystem::path dir = directory.empty() ? std::filesystem::path("logs") : std::filesystem::path(directory);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    auto fileName = gSessionName.empty() ? std::string("telemetry.log") : gSessionName + "-telemetry.log";
    auto filePath = dir / fileName;

    gLogFile.open(filePath, std::ios::out | std::ios::app);
}

void writeLine(const std::string& payload) {
    if (!gSessionStarted.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(gLogMutex);
    if (!gLogFile.is_open()) {
        ensureLogOpen("");
    }
    if (!gLogFile.is_open()) {
        return;
    }
    gLogFile << payload << '\n';
}

} // namespace

namespace telemetry {

void startSession(const std::string& sessionName, const std::string& logDirectory) {
    {
        std::lock_guard<std::mutex> lock(gLogMutex);
        gSessionName = sessionName;
        ensureLogOpen(logDirectory);
        gSessionStarted.store(true, std::memory_order_release);
    }
    writeLine("[" + timestampUtc() + "] SESSION start name=" + sessionName);
}

void flush() {
    std::lock_guard<std::mutex> lock(gLogMutex);
    if (gLogFile.is_open()) {
        gLogFile.flush();
    }
}

void logGauge(const std::string& category, const std::string& label, double value) {
    std::ostringstream oss;
    oss << '[' << timestampUtc() << "] GAUGE " << category << '.' << label << " value=" << value << " thread=" << std::this_thread::get_id();
    writeLine(oss.str());
}

void logCounter(const std::string& category, const std::string& label, std::int64_t delta) {
    std::ostringstream oss;
    oss << '[' << timestampUtc() << "] COUNTER " << category << '.' << label << " delta=" << delta << " thread=" << std::this_thread::get_id();
    writeLine(oss.str());
}

void logEvent(const std::string& category, const std::string& message) {
    std::ostringstream oss;
    oss << '[' << timestampUtc() << "] EVENT " << category << " msg=" << message << " thread=" << std::this_thread::get_id();
    writeLine(oss.str());
}

ScopedTimer::ScopedTimer(const std::string& category, const std::string& label)
    : category_(category), label_(label), start_(std::chrono::steady_clock::now()) {
}

ScopedTimer::~ScopedTimer() {
    auto end = std::chrono::steady_clock::now();
    auto durationUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start_).count();
    std::ostringstream oss;
    oss << '[' << timestampUtc() << "] TIMER " << category_ << '.' << label_ << " duration_us=" << durationUs << " thread=" << std::this_thread::get_id();
    writeLine(oss.str());
}

} // namespace telemetry
