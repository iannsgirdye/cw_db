#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "core/value.h"

namespace cw_db {

std::size_t estimate_bulk_build_bytes(std::size_t entry_count, std::size_t total_encoded_key_bytes);

std::optional<std::size_t> available_memory_bytes() noexcept;

void validate_bulk_build_input(const std::vector<std::pair<Value, std::uint64_t>>& entries);

}  // namespace cw_db
