#include "net/logger.h"

#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>

namespace cw_db {

using json = nlohmann::ordered_json;

AccessLog& AccessLog::instance() {
    static AccessLog l;
    return l;
}

namespace {
std::string fmt_time(AccessLog::TimePoint tp) {
    auto t = AccessLog::Clock::to_time_t(tp);
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()) % 1'000'000;
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%FT%T") << "." << std::setw(6) << std::setfill('0') << us.count() << "Z";
    return os.str();
}
}  // namespace

void AccessLog::open(const std::string& path) {
    std::lock_guard<std::mutex> lock(mu_);
    if (out_.is_open()) out_.close();
    if (path.empty()) return;
    out_.open(path, std::ios::app);
}

void AccessLog::close() {
    std::lock_guard<std::mutex> lock(mu_);
    if (out_.is_open()) out_.close();
}

void AccessLog::log(RequestId req_id,
                    const std::string& client_id,
                    const std::string& handler_id,
                    const std::string& request_body,
                    TimePoint start,
                    TimePoint end,
                    int status_code,
                    const std::string& status_text) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!out_.is_open()) return;
    auto dur_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    json entry = {
        {"request_id",  req_id},
        {"client_id",   client_id},
        {"handler_id",  handler_id},
        {"started_at",  fmt_time(start)},
        {"ended_at",    fmt_time(end)},
        {"duration_us", dur_us},
        {"status",      status_code},
        {"status_text", status_text},
        {"request",     request_body},
    };
    out_ << entry.dump() << '\n';
    out_.flush();
}

}  // namespace cw_db
