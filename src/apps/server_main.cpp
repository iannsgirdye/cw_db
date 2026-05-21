// prog-server entry: HTTP server, optional access log, periodic telemetry.

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

#include "core/dbms.h"
#include "net/logger.h"
#include "net/server.h"
#include "net/telemetry.h"

namespace {
std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }
}  // namespace

int main(int argc, char** argv) {
    std::string data_dir = "./data";
    std::string log_path = "./access.log";
    int port = 5432;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--port" && i + 1 < argc)      port = std::atoi(argv[++i]);
        else if (a == "--data" && i + 1 < argc) data_dir = argv[++i];
        else if (a == "--log"  && i + 1 < argc) log_path = argv[++i];
        else if (a == "--no-log")               log_path.clear();
    }
    cw_db::AccessLog::instance().open(log_path);

    cw_db::Dbms dbms(data_dir);
    dbms.load_all();

    cw_db::Server server(dbms, port);
    if (!server.start()) {
        std::cerr << "Failed to bind on port " << port << "\n";
        return 1;
    }
    std::cerr << "Server listening on 127.0.0.1:" << port
              << " (data dir " << data_dir << ", log " << (log_path.empty() ? "<disabled>" : log_path) << ")\n";

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    using namespace std::chrono_literals;
    while (!g_stop.load()) {
        std::this_thread::sleep_for(5s);
        auto s = cw_db::Telemetry::instance().snapshot();
        std::cerr << "[telemetry] rps=" << s.current_rps
                  << " avg10m=" << s.avg_rps_10m
                  << " max10m=" << s.max_rps_10m
                  << " lat10s_us=" << s.avg_latency_us_10s
                  << " err1m=" << s.errors_1m
                  << " total=" << s.total_requests
                  << " (errors=" << s.total_errors << ")\n";
    }
    server.stop();
    cw_db::AccessLog::instance().close();
    return 0;
}
