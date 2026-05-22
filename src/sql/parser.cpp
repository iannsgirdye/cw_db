#include "sql/parser.h"

#include <sstream>

#include "sql_parser.h"   // bison-generated: ParseContext, YYSTYPE, YYLTYPE, yyparse

// ---------------------------------------------------------------------------
// Forward declarations of the flex-generated reentrant API. Flex does not
// produce a public header for reentrant scanners, and its symbols use C++
// linkage (no extern "C" wrapper), so we just match the signatures verbatim.
// The actual definitions come from src/sql_lexer.cpp.
// ---------------------------------------------------------------------------
typedef void* yyscan_t;
struct yy_buffer_state;
typedef struct yy_buffer_state* YY_BUFFER_STATE;

int  yylex_init_extra(cw_db::ParseContext* user_defined, yyscan_t* scanner);
int  yylex_destroy(yyscan_t scanner);
YY_BUFFER_STATE yy_scan_string(const char* str, yyscan_t scanner);
void yy_delete_buffer(YY_BUFFER_STATE buf, yyscan_t scanner);

namespace cw_db {

Statement Parser::parse_statement() {
    if (done_) {
        throw ParseError("Parse error: no more statements available");
    }
    done_ = true;

    ParseContext ctx;
    yyscan_t scanner = nullptr;
    if (yylex_init_extra(&ctx, &scanner) != 0) {
        throw ParseError("Parse error: failed to initialise scanner");
    }
    YY_BUFFER_STATE buf = yy_scan_string(source_.c_str(), scanner);
    int rc = yyparse(scanner, &ctx);
    yy_delete_buffer(buf, scanner);
    yylex_destroy(scanner);

    if (ctx.lex_error) {
        std::ostringstream os;
        os << "Lex error at " << ctx.error_line << ':' << ctx.error_column
           << ": " << ctx.error_msg;
        throw LexError(os.str());
    }
    if (rc != 0 || !ctx.result) {
        std::ostringstream os;
        os << "Parse error at " << ctx.error_line << ':' << ctx.error_column
           << ": " << (ctx.error_msg.empty() ? "syntax error" : ctx.error_msg);
        throw ParseError(os.str());
    }
    return std::move(*ctx.result);
}

}  // namespace cw_db
