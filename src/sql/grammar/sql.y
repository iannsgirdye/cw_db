/* Bison grammar for the course DBMS SQL dialect. Generated into
 * build/generated/sql_parser.cpp, sql_parser.h.
 *
 * The grammar parses exactly one statement terminated by ';'. The host code
 * (Parser class in src/parser.cpp) is responsible for splitting batches into
 * individual statements before invoking the parser.
 *
 * Pure reentrant interface: every call to yyparse uses its own scanner state
 * (passed as void* yyscanner) and writes results into the supplied
 * cw_db::ParseContext. This is required because the HTTP server runs
 * multiple parser instances concurrently from different threads.
 */

%define api.pure full
%define parse.error verbose
%locations

%parse-param { void* yyscanner }
%parse-param { cw_db::ParseContext* ctx }
%lex-param   { void* yyscanner }

%code requires {
    #include <cstdint>
    #include <memory>
    #include <string>
    #include <utility>
    #include <vector>

    #include "sql/ast.h"
    #include "sql/schema.h"
    #include "core/value.h"

    namespace cw_db {

    // Filled by yyparse: at most one error message, or the successfully built
    // statement. The host wrapper consumes whichever is set.
    struct ParseContext {
        std::unique_ptr<Statement> result;
        std::string                error_msg;
        int                        error_line   = 0;
        int                        error_column = 0;
        bool                       lex_error    = false;
    };

    }  // namespace cw_db
}

%code provides {
    int  yylex(YYSTYPE* yylvalp, YYLTYPE* yyllocp, void* yyscanner);
    void yyerror(YYLTYPE* yyllocp, void* yyscanner,
                 cw_db::ParseContext* ctx, const char* msg);
}

%union {
    std::int64_t                                            ival;
    std::string*                                            sval;
    cw_db::Value*                                        valp;
    cw_db::Statement*                                    stmtp;
    cw_db::TableRef*                                     trefp;
    cw_db::ColumnDef*                                    coldefp;
    std::vector<cw_db::ColumnDef>*                       coldef_listp;
    std::vector<std::string>*                               strlistp;
    std::vector<cw_db::Value>*                           vallistp;
    std::vector<std::vector<cw_db::Value>>*              valmatp;
    cw_db::Expr*                                         exprp;
    std::vector<std::pair<std::string, cw_db::ExprPtr>>* assignlistp;
    std::pair<std::string, cw_db::ExprPtr>*              assignp;
    cw_db::SelectItem*                                   selitemp;
    std::vector<cw_db::SelectItem>*                      selitem_listp;
    cw_db::AggregateFn                                   aggfn;
    cw_db::CompareOp                                     cmpop;
    cw_db::DataType                                      dtype;
}

%token <ival> INT_LIT
%token <sval> STRING_LIT IDENT

/* Keywords carry the original lexeme in <sval> so they can also appear as
 * entity names (database / table / column / alias) when the grammar allows. */
%token <sval> CREATE       "CREATE"
%token <sval> DATABASE_KW  "DATABASE"
%token <sval> DROP         "DROP"
%token <sval> USE          "USE"
%token <sval> TABLE        "TABLE"
%token <sval> INSERT       "INSERT"
%token <sval> INTO         "INTO"
%token <sval> VALUE_KW     "VALUE"
%token <sval> UPDATE       "UPDATE"
%token <sval> SET          "SET"
%token <sval> WHERE        "WHERE"
%token <sval> DELETE       "DELETE"
%token <sval> FROM         "FROM"
%token <sval> SELECT       "SELECT"
%token <sval> AS           "AS"
%token <sval> NOT_NULL     "NOT_NULL"
%token <sval> INDEXED      "INDEXED"
%token <sval> DEFAULT      "DEFAULT"
%token <sval> AND_KW       "AND"
%token <sval> OR_KW        "OR"
%token <sval> BETWEEN      "BETWEEN"
%token <sval> LIKE         "LIKE"
%token <sval> INT_KW       "int"
%token <sval> STRING_KW    "string"
%token <sval> SUM          "SUM"
%token <sval> COUNT        "COUNT"
%token <sval> AVG          "AVG"
%token <sval> NULL_KW      "NULL"

%token LPAREN  "("
%token RPAREN  ")"
%token COMMA   ","
%token SEMI    ";"
%token DOT     "."
%token STAR    "*"

%token EQEQ    "=="
%token EQ      "="
%token NEQ     "!="
%token LT      "<"
%token GT      ">"
%token LEQ     "<="
%token GEQ     ">="

/* Surfaced by the lexer for situations the lexer must reject (currently only
 * "mixed case in a keyword"). The text is communicated through ctx. */
%token LEX_ERROR

%type <stmtp>          stmt create_stmt drop_stmt use_stmt insert_stmt update_stmt delete_stmt select_stmt
%type <trefp>          table_ref
%type <sval>           identifier
%type <coldefp>        column_def
%type <coldef_listp>   column_def_list
%type <valp>           literal
%type <dtype>          col_type
%type <strlistp>       insert_col_list opt_insert_cols
%type <vallistp>       insert_row literal_list
%type <valmatp>        insert_row_list
%type <exprp>          opt_where expr or_expr and_expr predicate atom
%type <assignlistp>    assignment_list
%type <assignp>        assignment
%type <selitem_listp>  select_items_or_star select_items
%type <selitemp>       select_item
%type <aggfn>          aggregate_fn
%type <cmpop>          compare_op

%destructor { delete $$; }  <sval> <valp> <stmtp> <trefp> <coldefp> <coldef_listp>
                            <strlistp> <vallistp> <valmatp> <exprp>
                            <assignlistp> <assignp> <selitemp> <selitem_listp>

%start program

%%

program
    : stmt SEMI {
        ctx->result.reset($1);
      }
    ;

stmt
    : create_stmt    { $$ = $1; }
    | drop_stmt      { $$ = $1; }
    | use_stmt       { $$ = $1; }
    | insert_stmt    { $$ = $1; }
    | update_stmt    { $$ = $1; }
    | delete_stmt    { $$ = $1; }
    | select_stmt    { $$ = $1; }
    ;

/* ---------- identifiers / table refs ---------- */

identifier
    : IDENT        { $$ = $1; }
    | CREATE       { $$ = $1; }
    | DATABASE_KW  { $$ = $1; }
    | DROP         { $$ = $1; }
    | USE          { $$ = $1; }
    | TABLE        { $$ = $1; }
    | INSERT       { $$ = $1; }
    | INTO         { $$ = $1; }
    | VALUE_KW     { $$ = $1; }
    | UPDATE       { $$ = $1; }
    | SET          { $$ = $1; }
    | WHERE        { $$ = $1; }
    | DELETE       { $$ = $1; }
    | FROM         { $$ = $1; }
    | SELECT       { $$ = $1; }
    | AS           { $$ = $1; }
    | NOT_NULL     { $$ = $1; }
    | INDEXED      { $$ = $1; }
    | DEFAULT      { $$ = $1; }
    | AND_KW       { $$ = $1; }
    | OR_KW        { $$ = $1; }
    | BETWEEN      { $$ = $1; }
    | LIKE         { $$ = $1; }
    | INT_KW       { $$ = $1; }
    | STRING_KW    { $$ = $1; }
    | SUM          { $$ = $1; }
    | COUNT        { $$ = $1; }
    | AVG          { $$ = $1; }
    | NULL_KW      { $$ = $1; }
    ;

table_ref
    : identifier {
        auto* r = new cw_db::TableRef;
        r->table = std::move(*$1);
        delete $1;
        $$ = r;
      }
    | identifier DOT identifier {
        auto* r = new cw_db::TableRef;
        r->database = std::move(*$1); delete $1;
        r->table    = std::move(*$3); delete $3;
        $$ = r;
      }
    ;

/* ---------- CREATE ---------- */

create_stmt
    : CREATE DATABASE_KW identifier {
        cw_db::CreateDatabaseStmt s;
        s.name = std::move(*$3); delete $3;
        $$ = new cw_db::Statement(std::move(s));
      }
    | CREATE TABLE table_ref LPAREN column_def_list RPAREN {
        cw_db::CreateTableStmt s;
        s.table   = std::move(*$3); delete $3;
        s.columns = std::move(*$5); delete $5;
        $$ = new cw_db::Statement(std::move(s));
      }
    ;

column_def_list
    : column_def {
        $$ = new std::vector<cw_db::ColumnDef>;
        $$->push_back(std::move(*$1)); delete $1;
      }
    | column_def_list COMMA column_def {
        $$ = $1;
        $$->push_back(std::move(*$3)); delete $3;
      }
    ;

col_type
    : INT_KW    { $$ = cw_db::DataType::Int; }
    | STRING_KW { $$ = cw_db::DataType::Str; }
    ;

column_def
    : identifier col_type {
        auto* c = new cw_db::ColumnDef;
        c->name = std::move(*$1); delete $1;
        c->type = $2;
        $$ = c;
      }
    | column_def NOT_NULL {
        $$ = $1;
        $$->not_null = true;
      }
    | column_def INDEXED {
        $$ = $1;
        $$->indexed  = true;
        $$->not_null = true;  /* INDEXED implies NOT_NULL per spec */
      }
    | column_def DEFAULT literal {
        $$ = $1;
        $$->has_default = true;
        $$->default_value = std::move(*$3); delete $3;
        if (!$$->default_value.is_null() && !$$->default_value.matches($$->type)) {
            ctx->error_msg = "DEFAULT literal type does not match column '" + $$->name + "'";
            YYERROR;
        }
      }
    ;

literal
    : INT_LIT      { $$ = new cw_db::Value(cw_db::Value::of_int($1)); }
    | STRING_LIT   { $$ = new cw_db::Value(cw_db::Value::of_str(std::move(*$1))); delete $1; }
    | NULL_KW      { $$ = new cw_db::Value(cw_db::Value::null()); }
    ;

/* ---------- DROP ---------- */

drop_stmt
    : DROP DATABASE_KW identifier {
        cw_db::DropDatabaseStmt s;
        s.name = std::move(*$3); delete $3;
        $$ = new cw_db::Statement(std::move(s));
      }
    | DROP TABLE table_ref {
        cw_db::DropTableStmt s;
        s.table = std::move(*$3); delete $3;
        $$ = new cw_db::Statement(std::move(s));
      }
    ;

/* ---------- USE ---------- */

use_stmt
    : USE identifier {
        cw_db::UseStmt s;
        s.name = std::move(*$2); delete $2;
        $$ = new cw_db::Statement(std::move(s));
      }
    ;

/* ---------- INSERT ---------- */

insert_stmt
    : INSERT INTO table_ref opt_insert_cols VALUE_KW insert_row_list {
        cw_db::InsertStmt s;
        s.table   = std::move(*$3); delete $3;
        if ($4) { s.columns = std::move(*$4); delete $4; }
        s.rows    = std::move(*$6); delete $6;
        $$ = new cw_db::Statement(std::move(s));
      }
    ;

opt_insert_cols
    : /* empty */                       { $$ = nullptr; }
    | LPAREN insert_col_list RPAREN     { $$ = $2; }
    ;

insert_col_list
    : identifier {
        $$ = new std::vector<std::string>;
        $$->push_back(std::move(*$1)); delete $1;
      }
    | insert_col_list COMMA identifier {
        $$ = $1;
        $$->push_back(std::move(*$3)); delete $3;
      }
    ;

insert_row_list
    : insert_row {
        $$ = new std::vector<std::vector<cw_db::Value>>;
        $$->push_back(std::move(*$1)); delete $1;
      }
    | insert_row_list COMMA insert_row {
        $$ = $1;
        $$->push_back(std::move(*$3)); delete $3;
      }
    ;

insert_row
    : LPAREN literal_list RPAREN { $$ = $2; }
    ;

literal_list
    : literal {
        $$ = new std::vector<cw_db::Value>;
        $$->push_back(std::move(*$1)); delete $1;
      }
    | literal_list COMMA literal {
        $$ = $1;
        $$->push_back(std::move(*$3)); delete $3;
      }
    ;

/* ---------- UPDATE ---------- */

update_stmt
    : UPDATE table_ref SET assignment_list opt_where {
        cw_db::UpdateStmt s;
        s.table = std::move(*$2); delete $2;
        for (auto& a : *$4) {
            s.assignments.emplace_back(std::move(a.first), std::move(a.second));
        }
        delete $4;
        if ($5) s.where.reset($5);
        $$ = new cw_db::Statement(std::move(s));
      }
    ;

assignment_list
    : assignment {
        $$ = new std::vector<std::pair<std::string, cw_db::ExprPtr>>;
        $$->emplace_back(std::move($1->first), std::move($1->second));
        delete $1;
      }
    | assignment_list COMMA assignment {
        $$ = $1;
        $$->emplace_back(std::move($3->first), std::move($3->second));
        delete $3;
      }
    ;

assignment
    : identifier EQ atom {
        $$ = new std::pair<std::string, cw_db::ExprPtr>(std::move(*$1), cw_db::ExprPtr($3));
        delete $1;
      }
    | identifier EQEQ atom {
        $$ = new std::pair<std::string, cw_db::ExprPtr>(std::move(*$1), cw_db::ExprPtr($3));
        delete $1;
      }
    ;

/* ---------- DELETE ---------- */

delete_stmt
    : DELETE FROM table_ref opt_where {
        cw_db::DeleteStmt s;
        s.table = std::move(*$3); delete $3;
        if ($4) s.where.reset($4);
        $$ = new cw_db::Statement(std::move(s));
      }
    ;

/* ---------- SELECT ---------- */

select_stmt
    : SELECT select_items_or_star FROM table_ref opt_where {
        cw_db::SelectStmt s;
        s.items = std::move(*$2); delete $2;
        s.table = std::move(*$4); delete $4;
        if ($5) s.where.reset($5);
        $$ = new cw_db::Statement(std::move(s));
      }
    ;

select_items_or_star
    : STAR {
        cw_db::SelectItem it; it.is_star = true;
        $$ = new std::vector<cw_db::SelectItem>{};
        $$->push_back(std::move(it));
      }
    | select_items { $$ = $1; }
    ;

select_items
    : select_item {
        $$ = new std::vector<cw_db::SelectItem>;
        $$->push_back(std::move(*$1)); delete $1;
      }
    | select_items COMMA select_item {
        $$ = $1;
        $$->push_back(std::move(*$3)); delete $3;
      }
    ;

select_item
    : identifier {
        cw_db::SelectItem it;
        it.column = std::move(*$1); delete $1;
        $$ = new cw_db::SelectItem(std::move(it));
      }
    | identifier AS identifier {
        cw_db::SelectItem it;
        it.column = std::move(*$1); delete $1;
        it.alias  = std::move(*$3); delete $3;
        $$ = new cw_db::SelectItem(std::move(it));
      }
    | aggregate_fn LPAREN STAR RPAREN {
        if ($1 != cw_db::AggregateFn::Count) {
            ctx->error_msg = "only COUNT supports '*'";
            YYERROR;
        }
        cw_db::SelectItem it;
        it.agg = $1; it.agg_star = true;
        $$ = new cw_db::SelectItem(std::move(it));
      }
    | aggregate_fn LPAREN STAR RPAREN AS identifier {
        if ($1 != cw_db::AggregateFn::Count) {
            ctx->error_msg = "only COUNT supports '*'";
            YYERROR;
        }
        cw_db::SelectItem it;
        it.agg = $1; it.agg_star = true;
        it.alias = std::move(*$6); delete $6;
        $$ = new cw_db::SelectItem(std::move(it));
      }
    | aggregate_fn LPAREN identifier RPAREN {
        cw_db::SelectItem it;
        it.agg = $1;
        it.column = std::move(*$3); delete $3;
        $$ = new cw_db::SelectItem(std::move(it));
      }
    | aggregate_fn LPAREN identifier RPAREN AS identifier {
        cw_db::SelectItem it;
        it.agg = $1;
        it.column = std::move(*$3); delete $3;
        it.alias  = std::move(*$6); delete $6;
        $$ = new cw_db::SelectItem(std::move(it));
      }
    ;

aggregate_fn
    : SUM    { $$ = cw_db::AggregateFn::Sum; }
    | COUNT  { $$ = cw_db::AggregateFn::Count; }
    | AVG    { $$ = cw_db::AggregateFn::Avg; }
    ;

/* ---------- WHERE clause ---------- */

opt_where
    : /* empty */     { $$ = nullptr; }
    | WHERE expr      { $$ = $2; }
    ;

expr
    : or_expr         { $$ = $1; }
    ;

or_expr
    : and_expr        { $$ = $1; }
    | or_expr OR_KW and_expr {
        cw_db::LogicalExpr lg;
        lg.op  = cw_db::LogicalExpr::Op::Or;
        lg.lhs = cw_db::ExprPtr($1);
        lg.rhs = cw_db::ExprPtr($3);
        $$ = new cw_db::Expr(std::move(lg));
      }
    ;

and_expr
    : predicate       { $$ = $1; }
    | and_expr AND_KW predicate {
        cw_db::LogicalExpr lg;
        lg.op  = cw_db::LogicalExpr::Op::And;
        lg.lhs = cw_db::ExprPtr($1);
        lg.rhs = cw_db::ExprPtr($3);
        $$ = new cw_db::Expr(std::move(lg));
      }
    ;

predicate
    : LPAREN expr RPAREN { $$ = $2; }
    | atom compare_op atom {
        cw_db::CompareExpr ce;
        ce.op  = $2;
        ce.lhs = cw_db::ExprPtr($1);
        ce.rhs = cw_db::ExprPtr($3);
        $$ = new cw_db::Expr(std::move(ce));
      }
    | atom BETWEEN atom AND_KW atom {
        cw_db::BetweenExpr be;
        be.value = cw_db::ExprPtr($1);
        be.lo    = cw_db::ExprPtr($3);
        be.hi    = cw_db::ExprPtr($5);
        $$ = new cw_db::Expr(std::move(be));
      }
    | atom LIKE atom {
        cw_db::LikeExpr le;
        le.value   = cw_db::ExprPtr($1);
        le.pattern = cw_db::ExprPtr($3);
        $$ = new cw_db::Expr(std::move(le));
      }
    ;

compare_op
    : EQEQ  { $$ = cw_db::CompareOp::Eq; }
    | EQ    { $$ = cw_db::CompareOp::Eq;    /* lenient: '=' also accepted */ }
    | NEQ   { $$ = cw_db::CompareOp::NotEq; }
    | LT    { $$ = cw_db::CompareOp::Lt; }
    | GT    { $$ = cw_db::CompareOp::Gt; }
    | LEQ   { $$ = cw_db::CompareOp::LtEq; }
    | GEQ   { $$ = cw_db::CompareOp::GtEq; }
    ;

atom
    : literal {
        cw_db::LiteralExpr l;
        l.v = std::move(*$1); delete $1;
        $$ = new cw_db::Expr(std::move(l));
      }
    | identifier {
        cw_db::ColumnRefExpr c;
        c.column = std::move(*$1); delete $1;
        $$ = new cw_db::Expr(std::move(c));
      }
    ;

%%

void yyerror(YYLTYPE* yyllocp, void* /*yyscanner*/,
             cw_db::ParseContext* ctx, const char* msg) {
    if (!ctx->lex_error && ctx->error_msg.empty()) {
        ctx->error_msg   = msg ? msg : "parse error";
        ctx->error_line  = yyllocp ? yyllocp->first_line   : 0;
        ctx->error_column = yyllocp ? yyllocp->first_column : 0;
    }
}
