#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/value.h"

namespace cw_db {

struct ColumnDef {
    std::string name;
    DataType    type     = DataType::Int;
    bool        not_null = false;   // NOT_NULL constraint
    bool        indexed  = false;   // INDEXED constraint (implies unique + not null)
    bool        has_default = false;
    Value       default_value;      // valid when has_default is true
};

struct TableSchema {
    std::vector<ColumnDef> columns;

    int index_of(const std::string& name) const noexcept {
        for (std::size_t i = 0; i < columns.size(); ++i) {
            if (columns[i].name == name) return static_cast<int>(i);
        }
        return -1;
    }
};

}  // namespace cw_db
