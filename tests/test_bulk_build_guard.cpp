#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/value.h"
#include "storage/bulk_build_guard.h"
#include "storage/value_io.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

template <typename Fn>
void expect_throw(Fn&& fn, const char* needle) {
    try {
        fn();
        check(false, "expected exception");
    } catch (const std::runtime_error& e) {
        const std::string msg = e.what();
        if (msg.find(needle) == std::string::npos) {
            std::cerr << "FAIL: message missing " << needle << ": " << msg << '\n';
            ++failures;
        }
    }
}

void test_null_key() {
    std::vector<std::pair<cw_db::Value, std::uint64_t>> entries;
    entries.emplace_back(cw_db::Value::null(), 42);
    expect_throw([&] { cw_db::validate_bulk_build_input(entries); }, "NULL index key for record id 42");
}

void test_oversized_key() {
    std::vector<std::pair<cw_db::Value, std::uint64_t>> entries;
    entries.emplace_back(cw_db::Value::of_str(std::string(600, 'x')), 7);
    expect_throw([&] { cw_db::validate_bulk_build_input(entries); }, "index key too large for record id 7");
}

void test_memory_budget() {
#if defined(_WIN32)
    _putenv_s("CW_DB_BULK_MEM_BUDGET", "1");
#else
    setenv("CW_DB_BULK_MEM_BUDGET", "1", 1);
#endif
    std::vector<std::pair<cw_db::Value, std::uint64_t>> entries;
    entries.emplace_back(cw_db::Value::of_int(1), 1);
    expect_throw([&] { cw_db::validate_bulk_build_input(entries); }, "exceeds budget");
#if defined(_WIN32)
    _putenv_s("CW_DB_BULK_MEM_BUDGET", "");
#else
    unsetenv("CW_DB_BULK_MEM_BUDGET");
#endif
}

void test_ok_entries() {
    std::vector<std::pair<cw_db::Value, std::uint64_t>> entries;
    entries.emplace_back(cw_db::Value::of_int(1), 1);
    entries.emplace_back(cw_db::Value::of_int(2), 2);
    try {
        cw_db::validate_bulk_build_input(entries);
    } catch (const std::exception& e) {
        std::cerr << "FAIL: valid entries threw: " << e.what() << '\n';
        ++failures;
    }
}

void test_estimate() {
    const std::size_t est = cw_db::estimate_bulk_build_bytes(100, 900);
    check(est > 900, "estimate_bulk_build_bytes should include overhead");
}

}  // namespace

int main() {
    test_null_key();
    test_oversized_key();
    test_memory_budget();
    test_ok_entries();
    test_estimate();

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "bulk_build_guard unit tests ok\n";
    return 0;
}
