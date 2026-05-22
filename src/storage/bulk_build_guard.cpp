#include "storage/bulk_build_guard.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

#include "storage/pager.h"
#include "storage/value_io.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <fstream>
#include <string>
#endif

namespace cw_db {

namespace {

constexpr double kMaxAvailFraction = 0.25;
constexpr std::size_t kPerEntryOverhead = 96;

std::optional<std::size_t> bulk_build_memory_budget_override() {
    const char* p = std::getenv("CW_DB_BULK_MEM_BUDGET");
    if (p == nullptr || *p == '\0') return std::nullopt;
    char*             end = nullptr;
    unsigned long long v  = std::strtoull(p, &end, 10);
    if (end == p) return std::nullopt;
    return static_cast<std::size_t>(v);
}

}  // namespace

std::size_t estimate_bulk_build_bytes(std::size_t entry_count, std::size_t total_encoded_key_bytes) {
    std::size_t bytes = entry_count * kPerEntryOverhead;
    bytes += total_encoded_key_bytes * 2;
    bytes += entry_count * sizeof(std::pair<Value, std::uint64_t>);
    bytes += kDefaultPageCacheCapacity * kPageSize;
    return bytes;
}

std::optional<std::size_t> available_memory_bytes() noexcept {
#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(status.ullAvailPhys);
#elif defined(__linux__)
    std::ifstream meminfo("/proc/meminfo");
    if (!meminfo) return std::nullopt;
    std::string line;
    while (std::getline(meminfo, line)) {
        if (line.compare(0, 13, "MemAvailable:") != 0) continue;
        unsigned long long kb = 0;
        if (std::sscanf(line.c_str(), "MemAvailable: %llu kB", &kb) != 1) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(kb) * 1024;
    }
    return std::nullopt;
#else
    return std::nullopt;
#endif
}

void validate_bulk_build_input(const std::vector<std::pair<Value, std::uint64_t>>& entries) {
    std::size_t total_key_bytes = 0;
    for (const auto& [key, rid] : entries) {
        if (key.is_null()) {
            std::ostringstream os;
            os << "bulk_build: NULL index key for record id " << rid;
            throw std::runtime_error(os.str());
        }
        const std::size_t enc = encoded_value_size(key);
        if (enc > kMaxIndexKeyBytes) {
            throw std::runtime_error("bulk_build: index key too large for record id " +
                                     std::to_string(rid));
        }
        total_key_bytes += enc;
    }

    const std::size_t need = estimate_bulk_build_bytes(entries.size(), total_key_bytes);

    std::size_t budget = 0;
    if (const auto test_budget = bulk_build_memory_budget_override()) {
        budget = *test_budget;
    } else {
        const auto avail = available_memory_bytes();
        if (!avail || *avail == 0) {
            return;
        }
        budget = static_cast<std::size_t>(static_cast<double>(*avail) * kMaxAvailFraction);
    }

    if (need > budget) {
        std::ostringstream os;
        os << "bulk_build: estimated memory " << need << " bytes exceeds budget " << budget
           << " bytes";
        if (!bulk_build_memory_budget_override()) {
            const auto avail = available_memory_bytes();
            if (avail && *avail != 0) {
                os << " (" << static_cast<int>(kMaxAvailFraction * 100) << "% of available "
                   << *avail << " bytes)";
            }
        }
        throw std::runtime_error(os.str());
    }
}

}  // namespace cw_db
