#pragma once

#include <string>

#include "core/dbms.h"
#include "sql/ast.h"

namespace cw_db {

struct ExecutionResult {
    bool        ok = true;
    std::string output;    // JSON array for SELECT, status text otherwise
    std::string error;     // populated when ok == false
    std::size_t affected_rows = 0;
};

// Execute one parsed statement against the DBMS. The function holds the
// DBMS-level mutex internally and persists changes that mutate state.
ExecutionResult execute(Dbms& dbms, const Statement& stmt);

// One-shot convenience: parse + execute. Catches lex/parse/runtime errors
// and turns them into a non-ok ExecutionResult. Used by the interactive,
// batch and (future) server paths.
ExecutionResult run_query(Dbms& dbms, const std::string& sql);

}  // namespace cw_db
