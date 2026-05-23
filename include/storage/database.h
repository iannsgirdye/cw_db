#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

#include "storage/table.h"

namespace cw_db {

class Database {
public:
    Database() = default;
    explicit Database(std::string name) : name_(std::move(name)) {}

    const std::string& name() const noexcept { return name_; }

    bool has_table(const std::string& name) const;
    Table* table(const std::string& name);
    const Table* table(const std::string& name) const;
    Table& require_table(const std::string& name);

    Table& create_table(const std::string& name, TableSchema schema);
    void drop_table(const std::string& name);

    const std::unordered_map<std::string, std::unique_ptr<Table>>& tables() const noexcept {
        return tables_;
    }

    void save_to(const std::filesystem::path& dir) const;
    void load_from(const std::filesystem::path& dir);

    void set_storage_dir(const std::filesystem::path& dir);

private:
    std::string name_;
    std::unordered_map<std::string, std::unique_ptr<Table>> tables_;
    std::filesystem::path storage_dir_;
};

}  // namespace cw_db
