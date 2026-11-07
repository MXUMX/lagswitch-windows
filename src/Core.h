#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace lag {
using Millis = std::uint64_t;
enum class Scope { Genshin, System };
struct Binding {
    bool ipv6{};
    std::uint16_t port{}; // Host byte order, matched at the transport layer.
    bool operator<(const Binding& b) const { return ipv6 != b.ipv6 ? ipv6 < b.ipv6 : port < b.port; }
};
struct Rule {
    bool ipv6{};
    bool inbound{};
    bool system{};
    std::uint16_t port{};
    bool includeLoopback{}; // Only used by isolated loopback integration tests.
};
inline std::vector<Rule> plan(Scope scope, const std::set<Binding>& ports) {
    std::vector<Rule> result;
    if (scope == Scope::System) {
        for (bool v6 : {false, true}) for (bool in : {false, true}) result.push_back({v6, in, true, 0, false});
    } else {
        if (ports.empty()) throw std::runtime_error("No Genshin UDP ports are ready. Start the game and wait for READY.");
        for (const auto& b : ports) {
            if (!b.port) throw std::runtime_error("Invalid UDP port.");
            for (bool in : {false, true}) result.push_back({b.ipv6, in, false, b.port, false});
        }
    }
    return result;
}
inline bool validMinutes(double value) { return std::isfinite(value) && value > 0 && value <= 1440; }
inline double parseMinutes(const std::wstring& text) {
    std::size_t end{};
    double value{};
    try { value = std::stod(text, &end); } catch (...) { throw std::runtime_error("Enter minutes between 0 and 1440, for example 0.5."); }
    while (end < text.size() && (text[end] == L' ' || text[end] == L'\t')) ++end;
    if (end != text.size() || !validMinutes(value)) throw std::runtime_error("Enter minutes greater than 0 and at most 1440, for example 0.5.");
    return value;
}
struct Backend {
    virtual ~Backend() = default;
    virtual void block(const std::vector<Rule>& rules) = 0;
    virtual void restore() = 0;
};
struct Status {
    Scope scope{Scope::Genshin};
    bool blocked{};
    bool ready{};
    bool timed{};
    double minutes{0.5};
    Millis remaining{};
    std::size_t portCount{};
    std::string detectionError;
};

// Discovery never holds this lock while querying processes or socket tables.
// A watchdog calls tick independently of the UI message loop and settings dialog.
class Controller {
    Backend& backend_;
    mutable std::mutex mutex_;
    Scope scope_{Scope::Genshin};
    bool blocked_{}, timed_{}, uiTimedOut_{};
    double minutes_{0.5};
    Millis began_{}, observed_{}, heartbeat_{};
    std::set<Binding> ports_;
    std::string detectionError_;
    static constexpr Millis cacheLifetime = 5000;
    bool fresh(Millis now) const { return !ports_.empty() && now >= observed_ && now - observed_ <= cacheLifetime; }
    void restoreLocked() { backend_.restore(); blocked_ = false; began_ = 0; }
    void due(Millis now) {
        if (blocked_ && timed_ && now - began_ >= static_cast<Millis>(std::ceil(minutes_ * 60000))) restoreLocked();
    }
public:
    explicit Controller(Backend& backend) : backend_(backend) {}
    void updateDiscovery(std::set<Binding> ports, Millis observed, std::string error = {}) {
        std::lock_guard<std::mutex> guard(mutex_);
        ports_ = error.empty() ? std::move(ports) : std::set<Binding>{};
        observed_ = observed;
        detectionError_ = std::move(error);
    }
    void heartbeat(Millis now) {
        std::lock_guard<std::mutex> guard(mutex_);
        heartbeat_ = now; uiTimedOut_ = false;
    }
    void tick(Millis now) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (now >= heartbeat_ && now - heartbeat_ > 6000) { uiTimedOut_ = true; if (blocked_) restoreLocked(); }
        due(now);
    }
    void configure(Scope scope, bool timed, double minutes, Millis now) {
        if (!validMinutes(minutes)) throw std::runtime_error("Invalid restore duration.");
        std::lock_guard<std::mutex> guard(mutex_);
        if (scope != scope_) restoreLocked();
        scope_ = scope; timed_ = timed; minutes_ = minutes;
        due(now);
    }
    void toggle(Millis now) {
        std::lock_guard<std::mutex> guard(mutex_);
        // A delayed UI must not re-block immediately after watchdog recovery.
        if (uiTimedOut_) throw std::runtime_error("Traffic was restored because the app stopped responding. Try the shortcut again.");
        if (blocked_) { restoreLocked(); return; }
        if (scope_ == Scope::Genshin && !fresh(now)) throw std::runtime_error(detectionError_.empty() ? "No recent Genshin UDP ports are ready. Start the game and wait for READY." : detectionError_);
        backend_.block(plan(scope_, ports_));
        blocked_ = true; began_ = now;
    }
    void restore() { std::lock_guard<std::mutex> guard(mutex_); restoreLocked(); }
    Status status(Millis now) const {
        std::lock_guard<std::mutex> guard(mutex_);
        Millis remaining{};
        if (blocked_ && timed_) {
            const auto duration = static_cast<Millis>(std::ceil(minutes_ * 60000));
            remaining = now - began_ >= duration ? 0 : duration - (now - began_);
        }
        std::set<std::uint16_t> uniquePorts;
        for (const auto& binding : ports_) uniquePorts.insert(binding.port);
        return {scope_, blocked_, scope_ == Scope::System || fresh(now), timed_, minutes_, remaining, uniquePorts.size(), detectionError_};
    }
};
} // namespace lag
