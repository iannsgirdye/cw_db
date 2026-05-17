#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace cw_db {

// String interning: one copy per unique string, records store 32-bit ids.
class StringPool {
public:
    using Id = std::uint32_t;
    static constexpr Id kInvalid = 0;

    static StringPool& instance();

    Id intern(const std::string& s);
    Id intern(std::string&& s);

    const std::string& get(Id id) const;

    std::size_t size() const;

private:
    StringPool();

    mutable std::mutex mu_;
    std::unordered_map<std::string, Id> by_value_;
    std::vector<std::string> by_id_;  // index 0 is reserved sentinel
};

}  // namespace cw_db
