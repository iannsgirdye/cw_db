#include "sql/executor.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include "sql/parser.h"

namespace cw_db {

namespace {

// Resolve a TableRef using the current `USE` setting when no database is
// explicitly qualified.
Database& resolve_database(Dbms& dbms, const TableRef& ref) {
    if (!ref.database.empty()) return dbms.require_database(ref.database);
    return dbms.active_or_throw();
}

// ---------- WHERE evaluation ----------

bool to_bool_three_valued(int compare_result, CompareOp op) {
    switch (op) {
        case CompareOp::Eq:    return compare_result == 0;
        case CompareOp::NotEq: return compare_result != 0;
        case CompareOp::Lt:    return compare_result <  0;
        case CompareOp::Gt:    return compare_result >  0;
        case CompareOp::LtEq:  return compare_result <= 0;
        case CompareOp::GtEq:  return compare_result >= 0;
    }
    return false;
}

Value eval_atom(const Expr& e, const Table& table, const Record& row) {
    if (auto* lit = std::get_if<LiteralExpr>(&e.node)) return lit->v;
    if (auto* col = std::get_if<ColumnRefExpr>(&e.node)) {
        int idx = table.require_column(col->column);
        return row.values[idx];
    }
    throw std::runtime_error("invalid expression in atom position");
}

// SQL three-valued logic. UNKNOWN is encoded as `unknown=true`, the boolean
// value is otherwise meaningful.
struct TriBool {
    bool value   = false;
    bool unknown = false;
};

TriBool tri_or(TriBool a, TriBool b) {
    if (a.value || b.value) return {true, false};
    if (a.unknown || b.unknown) return {false, true};
    return {false, false};
}
TriBool tri_and(TriBool a, TriBool b) {
    if (a.unknown && !b.value) return {false, !(b.value)};  // value will only be false here
    if (b.unknown && !a.value) return {false, true};
    if (!a.value || !b.value) {
        if (a.value && b.unknown) return {false, true};
        if (b.value && a.unknown) return {false, true};
        return {false, false};
    }
    return {true, false};
}

TriBool eval_where(const Expr& e, const Table& table, const Record& row) {
    if (auto* cmp = std::get_if<CompareExpr>(&e.node)) {
        Value lhs = eval_atom(*cmp->lhs, table, row);
        Value rhs = eval_atom(*cmp->rhs, table, row);
        bool valid = true;
        int r = lhs.compare(rhs, valid);
        if (!valid) return {false, true};
        return {to_bool_three_valued(r, cmp->op), false};
    }
    if (auto* bt = std::get_if<BetweenExpr>(&e.node)) {
        Value v  = eval_atom(*bt->value, table, row);
        Value lo = eval_atom(*bt->lo, table, row);
        Value hi = eval_atom(*bt->hi, table, row);
        bool valid1 = true, valid2 = true;
        int  c1 = v.compare(lo, valid1);
        int  c2 = v.compare(hi, valid2);
        if (!valid1 || !valid2) return {false, true};
        // Closed interval [lo, hi] per the project spec.
        return {c1 >= 0 && c2 <= 0, false};
    }
    if (auto* lk = std::get_if<LikeExpr>(&e.node)) {
        Value v   = eval_atom(*lk->value,  table, row);
        Value pat = eval_atom(*lk->pattern, table, row);
        if (v.is_null() || pat.is_null()) return {false, true};
        if (!v.is_str() || !pat.is_str()) {
            throw std::runtime_error("LIKE expects string operands");
        }
        try {
            std::regex re(pat.as_str());
            return {std::regex_match(v.as_str(), re), false};
        } catch (const std::regex_error& re) {
            throw std::runtime_error(std::string("Bad regex in LIKE: ") + re.what());
        }
    }
    if (auto* lg = std::get_if<LogicalExpr>(&e.node)) {
        TriBool a = eval_where(*lg->lhs, table, row);
        TriBool b = eval_where(*lg->rhs, table, row);
        return lg->op == LogicalExpr::Op::And ? tri_and(a, b) : tri_or(a, b);
    }
    if (std::get_if<LiteralExpr>(&e.node)) {
        throw std::runtime_error("standalone literal not allowed in WHERE");
    }
    if (std::get_if<ColumnRefExpr>(&e.node)) {
        throw std::runtime_error("standalone column reference not allowed in WHERE");
    }
    return {false, true};
}

// ---------- Index-aware row selection ----------

struct IndexHint {
    bool has = false;
    int  column_idx = -1;
    bool eq = false;
    Value eq_value;
    bool has_lo = false;
    Value lo;
    bool lo_inclusive = false;
    bool has_hi = false;
    Value hi;
    bool hi_inclusive = false;
};

bool is_column_named(const Expr& e, const std::string& col) {
    if (auto* c = std::get_if<ColumnRefExpr>(&e.node)) return c->column == col;
    return false;
}
const LiteralExpr* as_literal(const Expr& e) {
    return std::get_if<LiteralExpr>(&e.node);
}

// Walks the conjunction *only* (we don't recurse into OR for index hints to
// stay correct). Returns a tight hint if we can derive one for any single
// indexed column.
IndexHint derive_index_hint(const Expr* where, const Table& table) {
    IndexHint hint;
    if (!where) return hint;

    // Collect simple comparison clauses from the AND-only top-level chain.
    std::vector<const Expr*> clauses;
    std::function<void(const Expr*)> walk = [&](const Expr* e) {
        if (!e) return;
        if (auto* lg = std::get_if<LogicalExpr>(&e->node);
            lg && lg->op == LogicalExpr::Op::And) {
            walk(lg->lhs.get());
            walk(lg->rhs.get());
            return;
        }
        clauses.push_back(e);
    };
    walk(where);

    // Try each indexed column independently and pick the first that yields
    // useful selectivity.
    for (const auto& col : table.schema().columns) {
        int col_idx = table.schema().index_of(col.name);
        if (!table.column_is_indexed(col_idx)) continue;

        IndexHint cand;
        cand.column_idx = col_idx;

        for (const Expr* c : clauses) {
            if (auto* cmp = std::get_if<CompareExpr>(&c->node)) {
                bool lhs_col = is_column_named(*cmp->lhs, col.name);
                bool rhs_col = is_column_named(*cmp->rhs, col.name);
                const LiteralExpr* lit = nullptr;
                CompareOp op = cmp->op;
                if (lhs_col && (lit = as_literal(*cmp->rhs))) {
                    // col op lit
                } else if (rhs_col && (lit = as_literal(*cmp->lhs))) {
                    // lit op col => swap operator
                    switch (op) {
                        case CompareOp::Lt:    op = CompareOp::Gt;    break;
                        case CompareOp::Gt:    op = CompareOp::Lt;    break;
                        case CompareOp::LtEq:  op = CompareOp::GtEq;  break;
                        case CompareOp::GtEq:  op = CompareOp::LtEq;  break;
                        default: break;
                    }
                } else {
                    continue;
                }
                if (!lit->v.matches(col.type)) continue;
                switch (op) {
                    case CompareOp::Eq:
                        cand.eq = true; cand.eq_value = lit->v; break;
                    case CompareOp::Lt:
                        cand.has_hi = true; cand.hi = lit->v; cand.hi_inclusive = false; break;
                    case CompareOp::LtEq:
                        cand.has_hi = true; cand.hi = lit->v; cand.hi_inclusive = true; break;
                    case CompareOp::Gt:
                        cand.has_lo = true; cand.lo = lit->v; cand.lo_inclusive = false; break;
                    case CompareOp::GtEq:
                        cand.has_lo = true; cand.lo = lit->v; cand.lo_inclusive = true; break;
                    case CompareOp::NotEq:
                        break;  // not selective enough for an index range
                }
            } else if (auto* bt = std::get_if<BetweenExpr>(&c->node)) {
                if (!is_column_named(*bt->value, col.name)) continue;
                auto* llo = as_literal(*bt->lo);
                auto* lhi = as_literal(*bt->hi);
                if (!llo || !lhi) continue;
                cand.has_lo = true; cand.lo = llo->v; cand.lo_inclusive = true;
                cand.has_hi = true; cand.hi = lhi->v; cand.hi_inclusive = true;
            }
        }
        if (cand.eq || cand.has_lo || cand.has_hi) {
            cand.has = true;
            return cand;  // first usable hint wins (single-pass selection)
        }
    }
    return hint;
}

std::vector<RecordId> select_candidates(const Table& table, const Expr* where) {
    IndexHint hint = derive_index_hint(where, table);
    if (!hint.has) return table.order();
    if (!table.column_is_indexed(hint.column_idx)) return table.order();

    std::vector<RecordId> out;
    if (hint.eq) {
        if (auto id = table.index_find(hint.column_idx, hint.eq_value)) {
            out.push_back(*id);
        }
        return out;
    }
    std::optional<Value> lo = hint.has_lo ? std::optional<Value>(hint.lo) : std::nullopt;
    std::optional<Value> hi = hint.has_hi ? std::optional<Value>(hint.hi) : std::nullopt;
    table.index_collect_range(hint.column_idx, lo, hint.lo_inclusive, hi, hint.hi_inclusive, out);
    return out;
}

// ---------- statement implementations ----------

ExecutionResult exec_create_database(Dbms& dbms, const CreateDatabaseStmt& s) {
    dbms.create_database(s.name);
    dbms.save_database(s.name);
    return {true, "Database created: " + s.name, "", 0};
}

ExecutionResult exec_drop_database(Dbms& dbms, const DropDatabaseStmt& s) {
    dbms.drop_database(s.name);
    return {true, "Database dropped: " + s.name, "", 0};
}

ExecutionResult exec_use(Dbms& dbms, const UseStmt& s) {
    dbms.use_database(s.name);
    return {true, "Active database: " + s.name, "", 0};
}

ExecutionResult exec_create_table(Dbms& dbms, const CreateTableStmt& s) {
    Database& db = resolve_database(dbms, s.table);
    // Validate column names are unique and identifiers.
    std::unordered_set<std::string> seen;
    for (const auto& c : s.columns) {
        if (!seen.insert(c.name).second) {
            throw std::runtime_error("Duplicate column name '" + c.name + "'");
        }
    }
    TableSchema schema; schema.columns = s.columns;
    db.create_table(s.table.table, std::move(schema));
    dbms.save_database(db.name());
    return {true, "Table created: " + db.name() + "." + s.table.table, "", 0};
}

ExecutionResult exec_drop_table(Dbms& dbms, const DropTableStmt& s) {
    Database& db = resolve_database(dbms, s.table);
    db.drop_table(s.table.table);
    dbms.save_database(db.name());
    return {true, "Table dropped: " + db.name() + "." + s.table.table, "", 0};
}

ExecutionResult exec_insert(Dbms& dbms, const InsertStmt& s) {
    Database& db = resolve_database(dbms, s.table);
    Table& tbl = db.require_table(s.table.table);

    std::vector<int> positions;
    if (s.columns.empty()) {
        positions.resize(tbl.schema().columns.size());
        for (std::size_t i = 0; i < positions.size(); ++i) positions[i] = static_cast<int>(i);
    } else {
        positions.reserve(s.columns.size());
        for (const auto& cname : s.columns) positions.push_back(tbl.require_column(cname));
    }

    std::size_t inserted = 0;
    for (const auto& row : s.rows) {
        if (row.size() != positions.size()) {
            throw std::runtime_error("INSERT: value count does not match column count");
        }
        Record rec = tbl.materialize(positions, row);
        tbl.insert(std::move(rec));
        ++inserted;
    }
    dbms.save_database(db.name());
    return {true, "Inserted " + std::to_string(inserted) + " row(s)", "", inserted};
}

ExecutionResult exec_update(Dbms& dbms, const UpdateStmt& s) {
    Database& db = resolve_database(dbms, s.table);
    Table& tbl = db.require_table(s.table.table);

    std::vector<std::pair<int, ExprPtr*>> assignments_idx;
    assignments_idx.reserve(s.assignments.size());
    for (const auto& [name, expr] : s.assignments) {
        int idx = tbl.require_column(name);
        assignments_idx.emplace_back(idx, const_cast<ExprPtr*>(&expr));
    }

    auto candidates = select_candidates(tbl, s.where.get());
    std::vector<RecordId> matched;
    for (RecordId id : candidates) {
        const Record& r = tbl.records().at(id);
        if (!s.where) { matched.push_back(id); continue; }
        TriBool t = eval_where(*s.where, tbl, r);
        if (t.value && !t.unknown) matched.push_back(id);
    }
    std::size_t affected = 0;
    for (RecordId id : matched) {
        const Record& r = tbl.records().at(id);
        std::vector<std::pair<int, Value>> assigns;
        assigns.reserve(assignments_idx.size());
        for (auto& [col_idx, expr_ptr] : assignments_idx) {
            // SET expressions may reference other columns of the same row.
            Value v = eval_atom(**expr_ptr, tbl, r);
            assigns.emplace_back(col_idx, std::move(v));
        }
        tbl.update(id, assigns);
        ++affected;
    }
    dbms.save_database(db.name());
    return {true, "Updated " + std::to_string(affected) + " row(s)", "", affected};
}

ExecutionResult exec_delete(Dbms& dbms, const DeleteStmt& s) {
    Database& db = resolve_database(dbms, s.table);
    Table& tbl = db.require_table(s.table.table);

    auto candidates = select_candidates(tbl, s.where.get());
    std::vector<RecordId> matched;
    for (RecordId id : candidates) {
        const Record& r = tbl.records().at(id);
        if (!s.where) { matched.push_back(id); continue; }
        TriBool t = eval_where(*s.where, tbl, r);
        if (t.value && !t.unknown) matched.push_back(id);
    }
    for (RecordId id : matched) tbl.erase(id);
    dbms.save_database(db.name());
    return {true, "Deleted " + std::to_string(matched.size()) + " row(s)", "", matched.size()};
}

ExecutionResult exec_select(Dbms& dbms, const SelectStmt& s) {
    Database& db = resolve_database(dbms, s.table);
    Table& tbl = db.require_table(s.table.table);

    // Decide projection layout.
    enum class ItemKind { Plain, Star, AggStar, Agg };
    struct ResolvedItem {
        ItemKind    kind;
        int         column_idx = -1;
        AggregateFn agg = AggregateFn::None;
        std::string label;
    };
    std::vector<ResolvedItem> items;
    bool any_agg = false, any_non_agg = false;
    if (s.items.size() == 1 && s.items.front().is_star) {
        for (std::size_t i = 0; i < tbl.schema().columns.size(); ++i) {
            ResolvedItem r;
            r.kind = ItemKind::Plain;
            r.column_idx = static_cast<int>(i);
            r.label = tbl.schema().columns[i].name;
            items.push_back(r);
        }
        any_non_agg = true;
    } else {
        for (const auto& it : s.items) {
            ResolvedItem r;
            if (it.agg != AggregateFn::None) {
                r.kind = it.agg_star ? ItemKind::AggStar : ItemKind::Agg;
                r.agg = it.agg;
                if (!it.agg_star) {
                    r.column_idx = tbl.require_column(it.column);
                }
                std::string fn =
                    (it.agg == AggregateFn::Sum) ? "SUM" :
                    (it.agg == AggregateFn::Count) ? "COUNT" : "AVG";
                r.label = it.alias.empty()
                    ? (fn + "(" + (it.agg_star ? std::string("*") : it.column) + ")")
                    : it.alias;
                any_agg = true;
            } else if (it.is_star) {
                throw std::runtime_error("'*' may only appear by itself in SELECT");
            } else {
                r.kind = ItemKind::Plain;
                r.column_idx = tbl.require_column(it.column);
                r.label = it.alias.empty() ? it.column : it.alias;
                any_non_agg = true;
            }
            items.push_back(r);
        }
    }
    if (any_agg && any_non_agg) {
        throw std::runtime_error("Cannot mix aggregates with plain columns without GROUP BY");
    }

    auto candidates = select_candidates(tbl, s.where.get());

    json result = json::array();
    if (any_agg) {
        // Compute aggregates over filtered rows.
        std::vector<std::int64_t> count(items.size(), 0);
        std::vector<long double>  sum(items.size(), 0.0);
        std::vector<bool>         seen_any(items.size(), false);

        for (RecordId id : candidates) {
            const Record& r = tbl.records().at(id);
            if (s.where) {
                TriBool t = eval_where(*s.where, tbl, r);
                if (!t.value || t.unknown) continue;
            }
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (items[i].kind == ItemKind::AggStar) {
                    ++count[i];
                    continue;
                }
                const Value& v = r.values[items[i].column_idx];
                if (v.is_null()) continue;
                if (items[i].agg == AggregateFn::Count) {
                    ++count[i];
                } else {
                    if (!v.is_int()) {
                        throw std::runtime_error("Aggregate requires int column '" +
                                                 tbl.schema().columns[items[i].column_idx].name + "'");
                    }
                    sum[i] += static_cast<long double>(v.as_int());
                    ++count[i];
                    seen_any[i] = true;
                }
            }
        }

        json row = json::object();
        for (std::size_t i = 0; i < items.size(); ++i) {
            const std::string& label = items[i].label;
            if (items[i].agg == AggregateFn::Count) {
                row[label] = count[i];
            } else if (items[i].agg == AggregateFn::Sum) {
                if (!seen_any[i]) row[label] = nullptr;
                else row[label] = static_cast<std::int64_t>(sum[i]);
            } else {  // AVG
                if (!seen_any[i] || count[i] == 0) {
                    row[label] = nullptr;
                } else {
                    long double avg_ld = sum[i] / static_cast<long double>(count[i]);
                    // Preserve historical formatting: integral averages render
                    // as integers (matches ostream's default double formatting).
                    double avg = static_cast<double>(avg_ld);
                    if (std::floor(avg) == avg && std::isfinite(avg) &&
                        std::abs(avg) < 9.0e15) {
                        row[label] = static_cast<std::int64_t>(avg);
                    } else {
                        row[label] = avg;
                    }
                }
            }
        }
        result.push_back(std::move(row));
        return {true, result.dump(), "", 1};
    }

    std::size_t produced = 0;
    for (RecordId id : candidates) {
        const Record& r = tbl.records().at(id);
        if (s.where) {
            TriBool t = eval_where(*s.where, tbl, r);
            if (!t.value || t.unknown) continue;
        }
        json row = json::object();
        for (std::size_t i = 0; i < items.size(); ++i) {
            row[items[i].label] = r.values[items[i].column_idx].to_json();
        }
        result.push_back(std::move(row));
        ++produced;
    }
    return {true, result.dump(), "", produced};
}

}  // namespace

ExecutionResult execute(Dbms& dbms, const Statement& stmt) {
    std::lock_guard<std::mutex> lock(dbms.mutex());
    try {
        if (auto* p = std::get_if<CreateDatabaseStmt>(&stmt)) return exec_create_database(dbms, *p);
        if (auto* p = std::get_if<DropDatabaseStmt>(&stmt))   return exec_drop_database(dbms, *p);
        if (auto* p = std::get_if<UseStmt>(&stmt))            return exec_use(dbms, *p);
        if (auto* p = std::get_if<CreateTableStmt>(&stmt))    return exec_create_table(dbms, *p);
        if (auto* p = std::get_if<DropTableStmt>(&stmt))      return exec_drop_table(dbms, *p);
        if (auto* p = std::get_if<InsertStmt>(&stmt))         return exec_insert(dbms, *p);
        if (auto* p = std::get_if<UpdateStmt>(&stmt))         return exec_update(dbms, *p);
        if (auto* p = std::get_if<DeleteStmt>(&stmt))         return exec_delete(dbms, *p);
        if (auto* p = std::get_if<SelectStmt>(&stmt))         return exec_select(dbms, *p);
        return {false, "", "Unsupported statement", 0};
    } catch (const std::exception& e) {
        return {false, "", e.what(), 0};
    }
}

ExecutionResult run_query(Dbms& dbms, const std::string& sql) {
    try {
        Parser parser(sql);
        if (parser.eof()) return {true, "", "", 0};
        Statement st = parser.parse_statement();
        return execute(dbms, st);
    } catch (const LexError& e) {
        return {false, "", e.what(), 0};
    } catch (const ParseError& e) {
        return {false, "", e.what(), 0};
    } catch (const std::exception& e) {
        return {false, "", e.what(), 0};
    }
}

}  // namespace cw_db
