#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "core/dbms.h"

namespace cw_db {

// HTTP server (Crow): POST /query with SQL body, GET /health.
class Server {
public:
    Server(Dbms& dbms, int port);
    ~Server();

    bool start();
    void stop();

    int port() const noexcept { return port_; }

private:
    Dbms&             dbms_;
    int               port_;
    std::atomic<bool> running_{false};
    std::thread       runner_;

    // Crow app is hidden behind pimpl.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cw_db
