#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/value.h"

namespace cw_db {

inline constexpr std::size_t kMaxIndexKeyBytes = 512;

inline std::size_t encoded_value_size(const Value& v) {
    switch (v.tag()) {
        case Value::Tag::Null: return 1;
        case Value::Tag::Int:  return 1 + sizeof(std::int64_t);
        case Value::Tag::Str:  return 1 + sizeof(std::uint32_t) + v.as_str().size();
    }
    return 1;
}

inline void encode_value(std::vector<std::uint8_t>& out, const Value& v) {
    const std::size_t need = encoded_value_size(v);
    if (out.size() + need > kMaxIndexKeyBytes) {
        throw std::runtime_error("Index key too large for disk page");
    }
    switch (v.tag()) {
        case Value::Tag::Null:
            out.push_back(static_cast<std::uint8_t>(Value::Tag::Null));
            break;
        case Value::Tag::Int: {
            out.push_back(static_cast<std::uint8_t>(Value::Tag::Int));
            std::int64_t x = v.as_int();
            const auto* p = reinterpret_cast<const std::uint8_t*>(&x);
            out.insert(out.end(), p, p + sizeof(x));
            break;
        }
        case Value::Tag::Str: {
            out.push_back(static_cast<std::uint8_t>(Value::Tag::Str));
            const std::string& s = v.as_str();
            std::uint32_t n = static_cast<std::uint32_t>(s.size());
            const auto* p = reinterpret_cast<const std::uint8_t*>(&n);
            out.insert(out.end(), p, p + sizeof(n));
            out.insert(out.end(), s.begin(), s.end());
            break;
        }
    }
}

inline Value decode_value(const std::uint8_t* data, std::size_t len, std::size_t& consumed) {
    if (len < 1) throw std::runtime_error("Corrupt encoded value");
    consumed = 0;
    auto tag = static_cast<Value::Tag>(data[0]);
    ++consumed;
    switch (tag) {
        case Value::Tag::Null:
            return Value::null();
        case Value::Tag::Int: {
            if (len < 1 + sizeof(std::int64_t)) throw std::runtime_error("Corrupt encoded int");
            std::int64_t x = 0;
            std::memcpy(&x, data + 1, sizeof(x));
            consumed += sizeof(x);
            return Value::of_int(x);
        }
        case Value::Tag::Str: {
            if (len < 1 + sizeof(std::uint32_t)) throw std::runtime_error("Corrupt encoded string");
            std::uint32_t n = 0;
            std::memcpy(&n, data + 1, sizeof(n));
            consumed += sizeof(n);
            if (len < consumed + n) throw std::runtime_error("Corrupt encoded string payload");
            std::string s(reinterpret_cast<const char*>(data + consumed), n);
            consumed += n;
            return Value::of_str(std::move(s));
        }
    }
    throw std::runtime_error("Unknown value tag on disk");
}

inline int compare_encoded_keys(const std::uint8_t* a, std::size_t alen,
                                const std::uint8_t* b, std::size_t blen) {
    std::size_t ca = 0, cb = 0;
    Value va = decode_value(a, alen, ca);
    Value vb = decode_value(b, blen, cb);
    bool valid = true;
    return va.compare(vb, valid);
}

}  // namespace cw_db
