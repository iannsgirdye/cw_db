#pragma once

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "core/value.h"
#include "sql/schema.h"

namespace cw_db {

struct TableRef {
    std::string database;  // empty -> use active database
    std::string table;
};

// ---------------------------------------------------------------------------
// WHERE expressions
// ---------------------------------------------------------------------------

enum class CompareOp { Eq, NotEq, Lt, Gt, LtEq, GtEq };

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct ColumnRefExpr {
    std::string column;          // bare identifier (no qualification supported in expressions)
};

struct LiteralExpr {
    Value v;
};

struct CompareExpr {
    CompareOp op;
    ExprPtr   lhs;
    ExprPtr   rhs;
};

struct BetweenExpr {
    ExprPtr value;
    ExprPtr lo;
    ExprPtr hi;
};

struct LikeExpr {
    ExprPtr value;
    ExprPtr pattern;
};

struct LogicalExpr {
    enum class Op { And, Or } op;
    ExprPtr lhs;
    ExprPtr rhs;
};

struct Expr {
    std::variant<ColumnRefExpr, LiteralExpr, CompareExpr, BetweenExpr, LikeExpr, LogicalExpr> node;

    template <typename T> Expr(T v) : node(std::move(v)) {}
};

// ---------------------------------------------------------------------------
// SELECT items
// ---------------------------------------------------------------------------

enum class AggregateFn { None, Sum, Count, Avg };

struct SelectItem {
    bool        is_star = false;
    AggregateFn agg     = AggregateFn::None;
    bool        agg_star = false;       // COUNT(*)
    std::string column;                 // empty when is_star or agg_star
    std::string alias;
};

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

struct CreateDatabaseStmt { std::string name; };
struct DropDatabaseStmt   { std::string name; };
struct UseStmt            { std::string name; };

struct CreateTableStmt {
    TableRef    table;
    std::vector<ColumnDef> columns;
};

struct DropTableStmt { TableRef table; };

struct InsertStmt {
    TableRef   table;
    std::vector<std::string>          columns;   // may be empty -> implicit all columns
    std::vector<std::vector<Value>>   rows;
};

struct UpdateStmt {
    TableRef                                   table;
    std::vector<std::pair<std::string, ExprPtr>> assignments;
    ExprPtr                                    where;   // may be null
};

struct DeleteStmt {
    TableRef table;
    ExprPtr  where;   // may be null
};

struct SelectStmt {
    TableRef                table;
    std::vector<SelectItem> items;
    ExprPtr                 where;   // may be null
};

using Statement = std::variant<
    CreateDatabaseStmt, DropDatabaseStmt, UseStmt,
    CreateTableStmt,    DropTableStmt,
    InsertStmt,         UpdateStmt,        DeleteStmt, SelectStmt>;

}  // namespace cw_db
