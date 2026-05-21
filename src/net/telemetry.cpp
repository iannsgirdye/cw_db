#include "net/telemetry.h"

#include <algorithm>
#include <vector>

namespace cw_db {

using namespace std::chrono_literals;

Telemetry& Telemetry::instance() {
    static Telemetry t;
    return t;
}

void Telemetry::observe(TimePoint started, TimePoint finished, bool ok) {
    std::lock_guard<std::mutex> lock(mu_);
    Entry e{finished,
            std::chrono::duration_cast<std::chrono::microseconds>(finished - started).count(),
            ok};
    window_.push_back(e);
    prune_locked(finished);
    ++total_requests_;
    if (!ok) ++total_errors_;
}

void Telemetry::prune_locked(TimePoint now) {
    // Drop events older than 10 minutes.
    while (!window_.empty() && now - window_.front().when > 10min) {
        window_.pop_front();
    }
}

Telemetry::Snapshot Telemetry::snapshot() {
    std::lock_guard<std::mutex> lock(mu_);
    TimePoint now = Clock::now();
    prune_locked(now);
    Snapshot s;
    s.total_requests = total_requests_.load();
    s.total_errors   = total_errors_.load();

    long long total_latency_10s = 0;
    int count_10s = 0;
    int count_1s  = 0;
    std::uint64_t errors_1m = 0;
    for (const auto& e : window_) {
        if (now - e.when <= 1s)  ++count_1s;
        if (now - e.when <= 10s) {
            total_latency_10s += e.latency_us;
            ++count_10s;
        }
        if (now - e.when <= 60s && !e.ok) ++errors_1m;
    }
    s.current_rps = static_cast<double>(count_1s);
    s.avg_latency_us_10s = count_10s ? static_cast<double>(total_latency_10s) / count_10s : 0.0;
    s.errors_1m = errors_1m;

    // RPS distribution over 10 minutes, bucketed per second.
    if (!window_.empty()) {
        std::vector<std::uint32_t> buckets(601, 0);  // 0..600 inclusive
        for (const auto& e : window_) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(now - e.when).count();
            if (age >= 0 && age < static_cast<long long>(buckets.size())) ++buckets[age];
        }
        std::uint64_t sum = 0;
        std::uint32_t mx = 0;
        for (auto v : buckets) { sum += v; mx = std::max(mx, v); }
        s.avg_rps_10m = static_cast<double>(sum) / buckets.size();
        s.max_rps_10m = static_cast<double>(mx);
    }
    return s;
}

}  // namespace cw_db
