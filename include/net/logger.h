#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

namespace cw_db {

// Access log: one JSON line per request (id, client, timing, status).
class AccessLog {
public:
    static AccessLog& instance();

    // Empty path disables logging (tests, local prog).
    void open(const std::string& path);
    void close();
    bool is_open() const { return out_.is_open(); }

    using Clock      = std::chrono::system_clock;
    using TimePoint  = Clock::time_point;
    using RequestId  = std::uint64_t;

    RequestId next_request_id() { return ++next_id_; }

    void log(RequestId req_id,
             const std::string& client_id,
             const std::string& handler_id,
             const std::string& request_body,
             TimePoint start,
             TimePoint end,
             int status_code,
             const std::string& status_text);

private:
    AccessLog() = default;
    std::mutex mu_;
    std::ofstream out_;
    std::atomic<RequestId> next_id_{0};
};

}  // namespace cw_db
