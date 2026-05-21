#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>

namespace cw_db {

// Request metrics: RPS, latency, errors (sliding windows in memory).
class Telemetry {
public:
    using Clock     = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    static Telemetry& instance();

    void observe(TimePoint started, TimePoint finished, bool ok);

    struct Snapshot {
        double current_rps   = 0;   // requests in the last 1s
        double avg_rps_10m   = 0;
        double max_rps_10m   = 0;
        double avg_latency_us_10s = 0;
        std::uint64_t errors_1m = 0;
        std::uint64_t total_requests = 0;
        std::uint64_t total_errors   = 0;
    };
    Snapshot snapshot();

private:
    Telemetry() = default;

    struct Entry {
        TimePoint when;
        long long latency_us;
        bool      ok;
    };
    std::mutex mu_;
    std::deque<Entry> window_;
    std::atomic<std::uint64_t> total_requests_{0};
    std::atomic<std::uint64_t> total_errors_{0};

    void prune_locked(TimePoint now);
};

}  // namespace cw_db
