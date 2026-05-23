#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/value.h"
#include "sql/schema.h"
#include "storage/disk_bsp_index.h"
#include "storage/value_less.h"

namespace cw_db {

using RecordId = std::uint64_t;

struct Record {
    std::vector<Value> values;  // one entry per schema column, same order
};

class Table {
public:
    Table() = default;
    explicit Table(std::string name, TableSchema schema);

    const std::string& name() const noexcept { return name_; }
    const TableSchema& schema() const noexcept { return schema_; }

    const std::unordered_map<RecordId, Record>& records() const noexcept { return records_; }

    int require_column(const std::string& name) const;

    RecordId insert(Record record);
    void update(RecordId id, const std::vector<std::pair<int, Value>>& assignments);
    void erase(RecordId id);

    const std::vector<RecordId>& order() const noexcept { return order_; }

    bool column_is_indexed(int col_idx) const;

    std::optional<RecordId> index_find(int col_idx, const Value& key) const;
    void index_collect_range(int col_idx,
                             const std::optional<Value>& lo,
                             bool lo_inclusive,
                             const std::optional<Value>& hi,
                             bool hi_inclusive,
                             std::vector<RecordId>& out) const;

    void save(const std::filesystem::path& dat_path);
    void load(const std::filesystem::path& dat_path);

    void bind_dat_path(const std::filesystem::path& dat_path);

    static std::filesystem::path index_path_for(const std::filesystem::path& dat_path);

    Record materialize(const std::vector<int>& column_positions,
                       std::vector<Value> values) const;

private:
    std::string name_;
    TableSchema schema_;
    std::unordered_map<RecordId, Record> records_;
    std::vector<RecordId> order_;
    std::unordered_map<int, std::unique_ptr<DiskBspIndex>> indexes_;
    std::filesystem::path dat_path_;
    RecordId next_id_ = 1;

    DiskBspIndex& mutable_index(int col_idx);
    const DiskBspIndex* index_at(int col_idx) const;

    void enforce_constraints(const Record& r, std::optional<RecordId> existing_id) const;
    void install_into_indexes(RecordId id, const Record& r);
    void remove_from_indexes(RecordId id, const Record& r);
    void rebuild_indexes_on_disk();
    void open_indexes();
    void flush_indexes();

    void assert_indexed_column_not_null(int col_idx) const;
};

}  // namespace cw_db
