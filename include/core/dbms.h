#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "storage/database.h"

namespace cw_db {

// Top-level DBMS holds all databases and is responsible for persisting them
// under a single root directory.
//
// Each database lives in <root>/<db_name>/, and inside that directory every
// table is one binary file <table_name>.dat.
//
// The class is safe to call concurrently as long as one mutates and only
// short-lived const references to internal tables are held. For the
// single-process interactive/batch use case this is sufficient. The
// client-server addition (task 3) takes the lock around every executed
// statement.
class Dbms {
public:
    explicit Dbms(std::filesystem::path root);

    // Persistence root (where the on-disk state of every database lives).
    const std::filesystem::path& root() const noexcept { return root_; }

    bool has_database(const std::string& name) const;
    Database& require_database(const std::string& name);

    Database& create_database(const std::string& name);
    void      drop_database(const std::string& name);

    void use_database(const std::string& name);
    const std::string& active_database() const noexcept { return active_; }

    // Returns the database that should be the implicit target of a statement
    // when no explicit "db.table" prefix is given. Throws if none was
    // selected with USE.
    Database& active_or_throw();

    const std::unordered_map<std::string, std::unique_ptr<Database>>& databases() const noexcept {
        return databases_;
    }

    void save_all() const;
    void save_database(const std::string& name) const;
    void load_all();

    // Returned by the executor; allows interactive/batch loop to manage I/O.
    std::mutex& mutex() noexcept { return mu_; }

private:
    std::filesystem::path root_;
    std::unordered_map<std::string, std::unique_ptr<Database>> databases_;
    std::string active_;
    mutable std::mutex mu_;
};

}  // namespace cw_db
