#pragma once

#include "core/value.h"

namespace cw_db {

struct ValueLess {
    bool operator()(const Value& a, const Value& b) const noexcept {
        bool valid = true;
        return a.compare(b, valid) < 0;
    }
};

}  // namespace cw_db
