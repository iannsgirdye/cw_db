#include "storage/database.h"

#include <stdexcept>
#include <system_error>

namespace cw_db {

bool Database::has_table(const std::string& name) const {
    return tables_.find(name) != tables_.end();
}

Table* Database::table(const std::string& name) {
    auto it = tables_.find(name);
    return it == tables_.end() ? nullptr : it->second.get();
}

const Table* Database::table(const std::string& name) const {
    auto it = tables_.find(name);
    return it == tables_.end() ? nullptr : it->second.get();
}

Table& Database::require_table(const std::string& name) {
    Table* t = table(name);
    if (!t) throw std::runtime_error("Unknown table '" + name + "' in database '" + name_ + "'");
    return *t;
}

void Database::set_storage_dir(const std::filesystem::path& dir) {
    storage_dir_ = dir;
    for (const auto& [name, table] : tables_) {
        table->bind_dat_path(storage_dir_ / (name + ".dat"));
    }
}

Table& Database::create_table(const std::string& name, TableSchema schema) {
    if (tables_.find(name) != tables_.end()) {
        throw std::runtime_error("Table '" + name + "' already exists in '" + name_ + "'");
    }
    auto ptr = std::make_unique<Table>(name, std::move(schema));
    Table& ref = *ptr;
    tables_.emplace(name, std::move(ptr));
    if (!storage_dir_.empty()) {
        ref.bind_dat_path(storage_dir_ / (name + ".dat"));
    }
    return ref;
}

void Database::drop_table(const std::string& name) {
    auto it = tables_.find(name);
    if (it == tables_.end()) {
        throw std::runtime_error("Unknown table '" + name + "' in database '" + name_ + "'");
    }
    tables_.erase(it);
}

void Database::save_to(const std::filesystem::path& dir) const {
    std::filesystem::create_directories(dir);
    const_cast<Database*>(this)->set_storage_dir(dir);
    const_cast<Database*>(this)->set_storage_dir(dir);
    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        const std::string fname = entry.path().filename().string();
        if (fname.size() < 5) continue;
        std::string stem;
        if (entry.path().extension() == ".dat") {
            stem = entry.path().stem().string();
        } else if (fname.find(".idx") != std::string::npos) {
            stem = fname.substr(0, fname.find(".idx"));
        } else {
            continue;
        }
        if (tables_.find(stem) == tables_.end()) {
            std::filesystem::remove(entry.path(), ec);
        }
    }
    for (const auto& [name, table] : tables_) {
        table->save(dir / (name + ".dat"));
    }
}

void Database::load_from(const std::filesystem::path& dir) {
    tables_.clear();
    storage_dir_ = dir;
    if (!std::filesystem::exists(dir)) return;
    for (auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".dat") continue;
        auto t = std::make_unique<Table>();
        t->load(entry.path());
        std::string name = entry.path().stem().string();
        tables_.emplace(name, std::move(t));
    }
}

}  // namespace cw_db
