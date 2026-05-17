#include "core/value.h"

#include <algorithm>
#include <ostream>

namespace cw_db {

const char* type_name(DataType t) {
    switch (t) {
        case DataType::Int: return "int";
        case DataType::Str: return "string";
    }
    return "?";
}

bool parse_type_name(const std::string& word, DataType& out) {
    std::string low;
    low.reserve(word.size());
    for (char c : word) {
        low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (low == "int" || low == "integer") {
        out = DataType::Int;
        return true;
    }
    if (low == "string" || low == "str" || low == "text") {
        out = DataType::Str;
        return true;
    }
    return false;
}

Value Value::of_int(std::int64_t v) noexcept {
    Value out;
    out.tag_ = Tag::Int;
    out.int_ = v;
    return out;
}

Value Value::of_str(const std::string& s) {
    Value out;
    out.tag_ = Tag::Str;
    out.str_id_ = StringPool::instance().intern(s);
    return out;
}

Value Value::of_str(std::string&& s) {
    Value out;
    out.tag_ = Tag::Str;
    out.str_id_ = StringPool::instance().intern(std::move(s));
    return out;
}

Value Value::of_str_id(StringPool::Id id) noexcept {
    Value out;
    out.tag_ = Tag::Str;
    out.str_id_ = id;
    return out;
}

const std::string& Value::as_str() const {
    return StringPool::instance().get(str_id_);
}

bool Value::matches(DataType column_type) const noexcept {
    if (is_null()) return true;
    if (column_type == DataType::Int) return is_int();
    if (column_type == DataType::Str) return is_str();
    return false;
}

json Value::to_json() const {
    switch (tag_) {
        case Tag::Null: return json(nullptr);
        case Tag::Int:  return json(int_);
        case Tag::Str:  return json(as_str());
    }
    return json(nullptr);
}

int Value::compare(const Value& other, bool& valid) const noexcept {
    if (is_null() || other.is_null()) {
        valid = false;
        return 0;
    }
    valid = true;
    if (tag_ != other.tag_) {
        // Mixed types: declare them incomparable (SQL: undefined).
        valid = false;
        return 0;
    }
    if (tag_ == Tag::Int) {
        if (int_ < other.int_) return -1;
        if (int_ > other.int_) return 1;
        return 0;
    }
    // String comparison: lexicographic.
    if (str_id_ == other.str_id_) return 0;  // interned: same id -> same content
    const auto& a = as_str();
    const auto& b = other.as_str();
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

bool Value::operator==(const Value& other) const noexcept {
    if (tag_ != other.tag_) return false;
    switch (tag_) {
        case Tag::Null: return true;
        case Tag::Int:  return int_ == other.int_;
        case Tag::Str:  return str_id_ == other.str_id_;
    }
    return false;
}

std::ostream& operator<<(std::ostream& os, const Value& v) {
    return os << v.to_json().dump();
}

}  // namespace cw_db
