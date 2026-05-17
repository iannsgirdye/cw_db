#include "core/dbms.h"

#include <stdexcept>
#include <system_error>

namespace cw_db {

Dbms::Dbms(std::filesystem::path root) : root_(std::move(root)) {
    std::filesystem::create_directories(root_);
}

bool Dbms::has_database(const std::string& name) const {
    return databases_.find(name) != databases_.end();
}

Database& Dbms::require_database(const std::string& name) {
    auto it = databases_.find(name);
    if (it == databases_.end()) throw std::runtime_error("Unknown database '" + name + "'");
    return *it->second;
}

Database& Dbms::create_database(const std::string& name) {
    if (databases_.find(name) != databases_.end()) {
        throw std::runtime_error("Database '" + name + "' already exists");
    }
    auto db = std::make_unique<Database>(name);
    Database& ref = *db;
    databases_.emplace(name, std::move(db));
    const auto db_dir = root_ / name;
    std::filesystem::create_directories(db_dir);
    ref.set_storage_dir(db_dir);
    return ref;
}

void Dbms::drop_database(const std::string& name) {
    auto it = databases_.find(name);
    if (it == databases_.end()) throw std::runtime_error("Unknown database '" + name + "'");
    databases_.erase(it);
    if (active_ == name) active_.clear();
    std::error_code ec;
    std::filesystem::remove_all(root_ / name, ec);
}

void Dbms::use_database(const std::string& name) {
    require_database(name);
    active_ = name;
}

Database& Dbms::active_or_throw() {
    if (active_.empty()) throw std::runtime_error("No database selected (use USE first)");
    return require_database(active_);
}

void Dbms::save_database(const std::string& name) const {
    auto it = databases_.find(name);
    if (it == databases_.end()) return;
    it->second->save_to(root_ / name);
}

void Dbms::save_all() const {
    for (const auto& [name, db] : databases_) {
        db->save_to(root_ / name);
    }
}

void Dbms::load_all() {
    databases_.clear();
    if (!std::filesystem::exists(root_)) return;
    for (auto& entry : std::filesystem::directory_iterator(root_)) {
        if (!entry.is_directory()) continue;
        std::string name = entry.path().filename().string();
        auto db = std::make_unique<Database>(name);
        db->load_from(entry.path());
        databases_.emplace(name, std::move(db));
    }
}

}  // namespace cw_db
