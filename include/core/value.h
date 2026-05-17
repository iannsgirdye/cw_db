#pragma once

#include <cstdint>
#include <iosfwd>
#include <nlohmann/json.hpp>
#include <string>

#include "core/string_pool.h"

namespace cw_db {

// All JSON output preserves insertion order so the user-visible field order
// matches the SELECT (or schema) ordering. nlohmann::json by default uses
// std::map which would sort keys alphabetically and break tests / spec.
using json = nlohmann::ordered_json;

enum class DataType : std::uint8_t {
    Int = 1,
    Str = 2,
};

const char* type_name(DataType t);
bool parse_type_name(const std::string& word, DataType& out);

// A single value living in a record cell or appearing as a literal in a
// query. Internally the storage is tagged: NULL / int64 / interned string id.
class Value {
public:
    enum class Tag : std::uint8_t {
        Null = 0,
        Int = 1,
        Str = 2,
    };

    Value() noexcept : tag_(Tag::Null), int_(0) {}

    static Value null() noexcept { return {}; }
    static Value of_int(std::int64_t v) noexcept;
    static Value of_str(const std::string& s);
    static Value of_str(std::string&& s);
    static Value of_str_id(StringPool::Id id) noexcept;

    Tag tag() const noexcept { return tag_; }
    bool is_null() const noexcept { return tag_ == Tag::Null; }
    bool is_int() const noexcept { return tag_ == Tag::Int; }
    bool is_str() const noexcept { return tag_ == Tag::Str; }

    std::int64_t as_int() const noexcept { return int_; }
    StringPool::Id as_str_id() const noexcept { return str_id_; }
    const std::string& as_str() const;

    // Whether this value has the data-type of a typed column. NULL matches
    // any column type.
    bool matches(DataType column_type) const noexcept;

    // JSON view of the value. NULL → json::null, Int → number, Str → string.
    json to_json() const;

    // Three-way ordering used by SQL comparisons. Returns negative / 0 /
    // positive. If either side is NULL the result is reported via *valid set
    // to false (SQL semantics: NULL comparisons yield unknown).
    int compare(const Value& other, bool& valid) const noexcept;

    bool operator==(const Value& other) const noexcept;
    bool operator!=(const Value& other) const noexcept { return !(*this == other); }

private:
    Tag tag_;
    union {
        std::int64_t int_;
        StringPool::Id str_id_;
    };
};

std::ostream& operator<<(std::ostream& os, const Value& v);

}  // namespace cw_db
