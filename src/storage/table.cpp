#include "storage/table.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <stdexcept>

namespace cw_db {

namespace {
constexpr std::uint32_t kMagic   = 0x42535444;  // "BSTD"
constexpr std::uint32_t kVersion = 3;
}  // namespace

Table::Table(std::string name, TableSchema schema)
    : name_(std::move(name)), schema_(std::move(schema)) {}

int Table::require_column(const std::string& name) const {
    int idx = schema_.index_of(name);
    if (idx < 0) {
        throw std::runtime_error("Unknown column '" + name + "' in table '" + name_ + "'");
    }
    return idx;
}

bool Table::column_is_indexed(int col_idx) const {
    if (col_idx < 0 || static_cast<std::size_t>(col_idx) >= schema_.columns.size()) return false;
    return schema_.columns[static_cast<std::size_t>(col_idx)].indexed;
}

void Table::bind_dat_path(const std::filesystem::path& dat_path) {
    dat_path_ = dat_path;
    open_indexes();
}

void Table::open_indexes() {
    if (dat_path_.empty()) return;
    const std::filesystem::path idx_base = index_path_for(dat_path_);
    for (std::size_t i = 0; i < schema_.columns.size(); ++i) {
        if (!schema_.columns[i].indexed) continue;
        const int col_idx = static_cast<int>(i);
        if (indexes_.find(col_idx) != indexes_.end()) continue;

        std::filesystem::path col_path = idx_base;
        col_path += "." + std::to_string(col_idx);

        auto disk = std::make_unique<DiskBspIndex>();
        if (std::filesystem::exists(col_path)) {
            disk->open(col_path, ValueLess{});
        } else {
            disk->create_empty(col_path, ValueLess{});
        }
        indexes_.emplace(col_idx, std::move(disk));
    }
}

void Table::flush_indexes() {
    for (const auto& [_, idx] : indexes_) {
        if (idx && idx->is_open()) idx->flush();
    }
}

DiskBspIndex& Table::mutable_index(int col_idx) {
    if (!column_is_indexed(col_idx)) {
        throw std::runtime_error("Column is not indexed");
    }
    if (dat_path_.empty()) {
        throw std::runtime_error("Table storage path is not bound; cannot update on-disk index");
    }
    open_indexes();
    return *indexes_.at(col_idx);
}

const DiskBspIndex* Table::index_at(int col_idx) const {
    if (!column_is_indexed(col_idx)) return nullptr;
    auto it = indexes_.find(col_idx);
    if (it == indexes_.end() || !it->second->is_open()) return nullptr;
    return it->second.get();
}

Record Table::materialize(const std::vector<int>& column_positions,
                          std::vector<Value> values) const {
    if (column_positions.size() != values.size()) {
        throw std::runtime_error("INSERT: column/value count mismatch");
    }
    Record full;
    full.values.resize(schema_.columns.size());
    std::vector<bool> assigned(schema_.columns.size(), false);
    for (std::size_t i = 0; i < column_positions.size(); ++i) {
        int pos = column_positions[i];
        if (assigned[static_cast<std::size_t>(pos)]) {
            throw std::runtime_error("INSERT: column '" + schema_.columns[pos].name + "' specified twice");
        }
        assigned[static_cast<std::size_t>(pos)] = true;
        full.values[static_cast<std::size_t>(pos)] = std::move(values[i]);
    }
    for (std::size_t i = 0; i < schema_.columns.size(); ++i) {
        if (assigned[i]) continue;
        const auto& col = schema_.columns[i];
        if (col.has_default) {
            full.values[i] = col.default_value;
        } else {
            full.values[i] = Value::null();
        }
    }
    return full;
}

void Table::enforce_constraints(const Record& r, std::optional<RecordId> existing_id) const {
    if (r.values.size() != schema_.columns.size()) {
        throw std::runtime_error("Internal: record arity mismatch");
    }
    for (std::size_t i = 0; i < schema_.columns.size(); ++i) {
        const auto& col = schema_.columns[i];
        const auto& v   = r.values[i];
        if (v.is_null()) {
            if (col.not_null || col.indexed) {
                throw std::runtime_error("NULL not allowed in column '" + col.name + "'");
            }
            continue;
        }
        if (!v.matches(col.type)) {
            throw std::runtime_error("Type mismatch for column '" + col.name + "' (expected " +
                                     std::string(type_name(col.type)) + ")");
        }
    }
    for (std::size_t col_idx = 0; col_idx < schema_.columns.size(); ++col_idx) {
        if (!schema_.columns[col_idx].indexed) continue;
        const auto& v = r.values[col_idx];
        if (v.is_null()) continue;
        std::optional<RecordId> found = index_find(static_cast<int>(col_idx), v);
        if (found && (!existing_id || *found != *existing_id)) {
            throw std::runtime_error("Duplicate value in indexed column '" +
                                     schema_.columns[col_idx].name + "'");
        }
    }
}

std::optional<RecordId> Table::index_find(int col_idx, const Value& key) const {
    const DiskBspIndex* idx = index_at(col_idx);
    if (!idx) return std::nullopt;
    auto it = idx->find(key);
    if (it == idx->end()) return std::nullopt;
    return it.value();
}

void Table::index_collect_range(int col_idx,
                                const std::optional<Value>& lo, bool lo_inclusive,
                                const std::optional<Value>& hi, bool hi_inclusive,
                                std::vector<RecordId>& out) const {
    const DiskBspIndex* idx = index_at(col_idx);
    if (!idx) return;
    auto it = lo ? (lo_inclusive ? idx->lower_bound(*lo) : idx->upper_bound(*lo)) : idx->begin();
    auto end = idx->end();
    for (; it != end; ++it) {
        if (hi) {
            bool valid = true;
            int c = it.key().compare(*hi, valid);
            if (!valid) break;
            if (hi_inclusive ? c > 0 : c >= 0) break;
        }
        out.push_back(it.value());
    }
}

std::filesystem::path Table::index_path_for(const std::filesystem::path& dat_path) {
    return dat_path.parent_path() / (dat_path.stem().string() + ".idx");
}

void Table::install_into_indexes(RecordId id, const Record& r) {
    for (std::size_t col_idx = 0; col_idx < schema_.columns.size(); ++col_idx) {
        if (!schema_.columns[col_idx].indexed) continue;
        const auto& v = r.values[col_idx];
        if (v.is_null()) continue;
        mutable_index(static_cast<int>(col_idx)).insert(v, id);
    }
}

void Table::remove_from_indexes(RecordId /*id*/, const Record& r) {
    for (std::size_t col_idx = 0; col_idx < schema_.columns.size(); ++col_idx) {
        if (!schema_.columns[col_idx].indexed) continue;
        const auto& v = r.values[col_idx];
        if (v.is_null()) continue;
        mutable_index(static_cast<int>(col_idx)).erase(v);
    }
}

RecordId Table::insert(Record record) {
    enforce_constraints(record, std::nullopt);
    RecordId id = next_id_++;
    auto [it, _] = records_.emplace(id, std::move(record));
    order_.push_back(id);
    install_into_indexes(id, it->second);
    return id;
}

void Table::update(RecordId id, const std::vector<std::pair<int, Value>>& assignments) {
    auto it = records_.find(id);
    if (it == records_.end()) throw std::runtime_error("Internal: record id not found");
    Record updated = it->second;
    for (auto& [col_idx, val] : assignments) {
        updated.values[static_cast<std::size_t>(col_idx)] = val;
    }
    enforce_constraints(updated, id);
    remove_from_indexes(id, it->second);
    it->second = std::move(updated);
    install_into_indexes(id, it->second);
}

void Table::erase(RecordId id) {
    auto it = records_.find(id);
    if (it == records_.end()) return;
    remove_from_indexes(id, it->second);
    records_.erase(it);
    for (auto oi = order_.begin(); oi != order_.end(); ++oi) {
        if (*oi == id) { order_.erase(oi); break; }
    }
}

namespace {

template <typename T>
void write_pod(std::ofstream& o, const T& v) {
    o.write(reinterpret_cast<const char*>(&v), sizeof(T));
}

template <typename T>
void read_pod(std::ifstream& i, T& v) {
    i.read(reinterpret_cast<char*>(&v), sizeof(T));
}

void write_string(std::ofstream& o, const std::string& s) {
    std::uint32_t n = static_cast<std::uint32_t>(s.size());
    write_pod(o, n);
    o.write(s.data(), n);
}

std::string read_string(std::ifstream& i) {
    std::uint32_t n;
    read_pod(i, n);
    std::string s(n, '\0');
    if (n) i.read(&s[0], n);
    return s;
}

void write_value(std::ofstream& o, const Value& v) {
    std::uint8_t tag = static_cast<std::uint8_t>(v.tag());
    write_pod(o, tag);
    switch (v.tag()) {
        case Value::Tag::Null: break;
        case Value::Tag::Int:  { std::int64_t x = v.as_int(); write_pod(o, x); break; }
        case Value::Tag::Str:  write_string(o, v.as_str()); break;
    }
}

Value read_value(std::ifstream& i) {
    std::uint8_t tag;
    read_pod(i, tag);
    switch (static_cast<Value::Tag>(tag)) {
        case Value::Tag::Null: return Value::null();
        case Value::Tag::Int:  { std::int64_t x; read_pod(i, x); return Value::of_int(x); }
        case Value::Tag::Str:  return Value::of_str(read_string(i));
    }
    throw std::runtime_error("Corrupt table file: unknown value tag");
}

}  // namespace

void Table::save(const std::filesystem::path& path) {
    bind_dat_path(path);
    flush_indexes();

    std::filesystem::create_directories(path.parent_path());
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) throw std::runtime_error("Cannot write " + path.string());
        write_pod(o, kMagic);
        write_pod(o, kVersion);
        write_string(o, name_);
        std::uint32_t cols = static_cast<std::uint32_t>(schema_.columns.size());
        write_pod(o, cols);
        for (const auto& c : schema_.columns) {
            write_string(o, c.name);
            std::uint8_t t = static_cast<std::uint8_t>(c.type);
            write_pod(o, t);
            std::uint8_t flags = static_cast<std::uint8_t>(
                (c.not_null    ? 1 : 0) |
                (c.indexed     ? 2 : 0) |
                (c.has_default ? 4 : 0));
            write_pod(o, flags);
            if (c.has_default) write_value(o, c.default_value);
        }
        write_pod(o, next_id_);
        std::uint64_t n = static_cast<std::uint64_t>(order_.size());
        write_pod(o, n);
        for (RecordId id : order_) {
            write_pod(o, id);
            const Record& r = records_.at(id);
            for (const auto& v : r.values) write_value(o, v);
        }
    }
    std::filesystem::rename(tmp, path);
}

void Table::assert_indexed_column_not_null(int col_idx) const {
    if (!column_is_indexed(col_idx)) return;
    const auto& col = schema_.columns[static_cast<std::size_t>(col_idx)];
    for (RecordId id : order_) {
        const auto& v = records_.at(id).values[static_cast<std::size_t>(col_idx)];
        if (!v.is_null()) continue;
        throw std::runtime_error("Cannot bulk-build index on column '" + col.name +
                                 "': NULL in record id " + std::to_string(id));
    }
}

void Table::rebuild_indexes_on_disk() {
    if (dat_path_.empty()) return;
    indexes_.clear();
    const std::filesystem::path idx_base = index_path_for(dat_path_);
    const ValueLess less;

    for (std::size_t col = 0; col < schema_.columns.size(); ++col) {
        if (!schema_.columns[col].indexed) continue;
        const int col_idx = static_cast<int>(col);
        assert_indexed_column_not_null(col_idx);

        std::vector<std::pair<Value, RecordId>> entries;
        entries.reserve(order_.size());
        for (RecordId id : order_) {
            const auto& v = records_.at(id).values[col];
            entries.emplace_back(v, id);
        }
        std::sort(entries.begin(), entries.end(),
                  [&less](const std::pair<Value, RecordId>& a, const std::pair<Value, RecordId>& b) {
                      return less(a.first, b.first);
                  });

        std::filesystem::path col_path = idx_base;
        col_path += "." + std::to_string(col_idx);
        auto disk = std::make_unique<DiskBspIndex>();
        disk->bulk_build(col_path, ValueLess{}, std::move(entries));
        indexes_.emplace(col_idx, std::move(disk));
    }
}

void Table::load(const std::filesystem::path& path) {
    std::ifstream i(path, std::ios::binary);
    if (!i) throw std::runtime_error("Cannot read " + path.string());
    std::uint32_t magic;
    read_pod(i, magic);
    if (magic != kMagic) throw std::runtime_error("Bad magic in " + path.string());
    std::uint32_t version;
    read_pod(i, version);
    if (version != 1 && version != 2 && version != kVersion) {
        throw std::runtime_error("Unsupported version in " + path.string());
    }
    name_ = read_string(i);
    schema_.columns.clear();
    std::uint32_t cols;
    read_pod(i, cols);
    schema_.columns.reserve(cols);
    for (std::uint32_t k = 0; k < cols; ++k) {
        ColumnDef c;
        c.name = read_string(i);
        std::uint8_t t; read_pod(i, t);
        c.type = static_cast<DataType>(t);
        std::uint8_t flags; read_pod(i, flags);
        c.not_null    = (flags & 1) != 0;
        c.indexed     = (flags & 2) != 0;
        c.has_default = (flags & 4) != 0;
        if (c.has_default) c.default_value = read_value(i);
        schema_.columns.push_back(std::move(c));
    }
    read_pod(i, next_id_);
    std::uint64_t n;
    read_pod(i, n);
    records_.clear();
    order_.clear();
    indexes_.clear();
    order_.reserve(static_cast<std::size_t>(n));
    for (std::uint64_t k = 0; k < n; ++k) {
        RecordId id; read_pod(i, id);
        Record r;
        r.values.resize(schema_.columns.size());
        for (std::size_t c = 0; c < schema_.columns.size(); ++c) {
            r.values[c] = read_value(i);
        }
        records_.emplace(id, std::move(r));
        order_.push_back(id);
    }

    if (version == kVersion) {
        bind_dat_path(path);
        return;
    }

    bind_dat_path(path);

    if (version == 1) {
        rebuild_indexes_on_disk();
        return;
    }

    const std::filesystem::path idx_base = index_path_for(path);
    const ValueLess less;
    std::uint32_t idx_count = 0;
    read_pod(i, idx_count);
    for (std::uint32_t b = 0; b < idx_count; ++b) {
        std::uint32_t col = 0;
        read_pod(i, col);
        std::uint64_t k = 0;
        read_pod(i, k);
        const int col_idx = static_cast<int>(col);
        if (!column_is_indexed(col_idx)) {
            for (std::uint64_t j = 0; j < k; ++j) {
                RecordId throwaway;
                read_pod(i, throwaway);
            }
            continue;
        }

        assert_indexed_column_not_null(col_idx);

        std::vector<std::pair<Value, RecordId>> entries;
        entries.reserve(static_cast<std::size_t>(k));
        for (std::uint64_t j = 0; j < k; ++j) {
            RecordId id = 0;
            read_pod(i, id);
            auto rit = records_.find(id);
            if (rit == records_.end()) continue;
            const auto& v = rit->second.values[col];
            entries.emplace_back(v, id);
        }
        std::sort(entries.begin(), entries.end(),
                  [&less](const std::pair<Value, RecordId>& a, const std::pair<Value, RecordId>& b) {
                      return less(a.first, b.first);
                  });

        indexes_.erase(col_idx);
        std::filesystem::path col_path = idx_base;
        col_path += "." + std::to_string(col_idx);
        auto disk = std::make_unique<DiskBspIndex>();
        disk->bulk_build(col_path, ValueLess{}, std::move(entries));
        indexes_.emplace(col_idx, std::move(disk));
    }
}

}  // namespace cw_db
