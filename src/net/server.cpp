#include "net/server.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <thread>

// Crow header included only here.
#include "crow.h"

#include "sql/executor.h"
#include "net/logger.h"
#include "net/telemetry.h"

namespace cw_db {

using json = nlohmann::ordered_json;

struct Server::Impl {
    crow::SimpleApp app;
    std::atomic<std::uint64_t> next_client_id{0};
};

Server::Server(Dbms& dbms, int port)
    : dbms_(dbms), port_(port), impl_(std::make_unique<Impl>()) {}

Server::~Server() { stop(); }

namespace {

std::string handler_id_for_this_thread() {
    std::ostringstream os;
    os << "thread-" << std::this_thread::get_id();
    return os.str();
}

// Trim whitespace; ensure trailing ';' for the executor.
std::string normalise_sql(std::string sql) {
    while (!sql.empty() && std::isspace(static_cast<unsigned char>(sql.back()))) {
        sql.pop_back();
    }
    if (!sql.empty() && sql.back() != ';') sql.push_back(';');
    return sql;
}

}  // namespace

bool Server::start() {
    if (running_.exchange(true)) return false;  // already started

    CROW_ROUTE(impl_->app, "/health")
    ([] {
        json j;
        j["ok"] = true;
        return crow::response{200, j.dump()};
    });

    CROW_ROUTE(impl_->app, "/query").methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req) {
        std::string sql = normalise_sql(req.body);

        const auto client_id  = std::string("client-") + req.remote_ip_address;
        const auto handler_id = handler_id_for_this_thread();

        auto rid = AccessLog::instance().next_request_id();
        auto t0  = std::chrono::system_clock::now();
        auto s0  = Telemetry::Clock::now();
        ExecutionResult result = run_query(dbms_, sql);
        auto t1  = std::chrono::system_clock::now();
        auto s1  = Telemetry::Clock::now();

        const int status_code = result.ok ? 200 : 400;
        const std::string status_text = result.ok ? "OK" : result.error;

        AccessLog::instance().log(rid, client_id, handler_id, sql,
                                  t0, t1, status_code, status_text);
        Telemetry::instance().observe(s0, s1, result.ok);

        json body;
        body["ok"] = result.ok;
        if (result.ok) {
            // If the executor returned a JSON array (SELECT), expose it under
            // `data` -- the client can then render it without re-parsing.
            // Otherwise `text` carries the human-readable status.
            if (!result.output.empty() && result.output.front() == '[') {
                try {
                    body["data"] = json::parse(result.output);
                } catch (const std::exception& e) {
                    // Unparseable JSON would be a bug; surface it as an error.
                    body["ok"]    = false;
                    body["error"] = std::string("internal: bad SELECT JSON: ") + e.what();
                    return crow::response{500, body.dump()};
                }
            } else {
                body["text"] = result.output;
            }
            body["affected_rows"] = result.affected_rows;
        } else {
            body["error"] = result.error;
        }

        crow::response resp{status_code, body.dump()};
        resp.set_header("Content-Type", "application/json");
        return resp;
    });

    runner_ = std::thread([this] {
        impl_->app.loglevel(crow::LogLevel::Warning);
        impl_->app.port(static_cast<std::uint16_t>(port_)).multithreaded().run();
    });

    // Block until Crow signals that the accept loop is live. This is the
    // intended way to make start() synchronous and avoid races where a test
    // tries to connect before the listener exists.
    try {
        impl_->app.wait_for_server_start();
    } catch (const std::exception& e) {
        running_ = false;
        if (runner_.joinable()) runner_.join();
        std::cerr << "server: failed to start: " << e.what() << "\n";
        return false;
    }
    return true;
}

void Server::stop() {
    if (!running_.exchange(false)) return;
    impl_->app.stop();
    if (runner_.joinable()) runner_.join();
}

}  // namespace cw_db
