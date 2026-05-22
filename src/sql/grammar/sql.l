/* Flex source for the course DBMS SQL dialect. Generated into
 * src/sql_lexer.cpp. Works in lockstep with src/grammar/sql.y -- see that
 * file for the parser.
 *
 * Reentrant (one yyscan_t per call). The scanner's yyextra carries a pointer
 * to the surrounding cw_db::ParseContext so individual rules can deposit
 * lex-level diagnostics (e.g. mixed-case keywords).
 */

%{
    #include <cctype>
    #include <cstdint>
    #include <cstdlib>
    #include <cstring>
    #include <string>
    #include <unordered_map>

    #include "core/value.h"
    #include "sql/ast.h"
    #include "sql_parser.h"   /* bison-generated: tokens, YYSTYPE, YYLTYPE */

    /* yyextra is used to thread the ParseContext through the lexer so that
     * actions can attach informational messages to the surrounding parse. */
    #define YY_EXTRA_TYPE cw_db::ParseContext*

    /* Update the location after every match. Newlines are processed by a
     * separate rule that also rewinds the column counter. */
    #define YY_USER_ACTION                                              \
        do {                                                            \
            yylloc->first_line   = yyextra_col_line(yyscanner);         \
            yylloc->first_column = yyextra_col_col(yyscanner);          \
            yylloc->last_line    = yylloc->first_line;                  \
            yyextra_col_advance(yyscanner, yyleng);                     \
            yylloc->last_column  = yyextra_col_col(yyscanner);          \
        } while (0);

    /* Forward declarations of the per-scanner column tracking helpers.
     * They live in the user-code section below. */
    static int  yyextra_col_line   (yyscan_t s);
    static int  yyextra_col_col    (yyscan_t s);
    static void yyextra_col_advance(yyscan_t s, int n);
    static void yyextra_col_newline(yyscan_t s);

    /* Decoded string literal (unescaped), populated by the STR_BODY rule. */
    static std::string  g_dummy_unused;

    /* Maps the lowercased lexeme to a bison token id, or 0 if not a keyword.
     * Also: keywords must have uniform case (project spec). */
    static int keyword_kind(const std::string& lower);

    /* Decodes the string literal text (between the surrounding quotes) into
     * an std::string, handling \\, \", \n, \t, \r escapes. Other escaped
     * characters are passed through (matches the legacy hand-written
     * behaviour). */
    static std::string decode_string(const char* text, int len);
%}

%option reentrant bison-bridge bison-locations
%option noyywrap nounput batch never-interactive
%option extra-type="cw_db::ParseContext*"

ID_START [A-Za-z_]
ID_CONT  [A-Za-z0-9_]

%%

[ \t]+                          { /* whitespace */ }

\r?\n                           { yyextra_col_newline(yyscanner); }

"--"[^\n]*                      { /* line comment */ }

"/*"([^*]|\*+[^/*])*\*+"/"      {
                                    /* block comment: scan the matched text
                                     * to maintain line/column tracking. */
                                    for (int i = 0; i < yyleng; ++i) {
                                        if (yytext[i] == '\n') yyextra_col_newline(yyscanner);
                                    }
                                }

\"([^"\\\n]|\\[^\n])*\"         {
                                    auto* s = new std::string(decode_string(yytext, yyleng));
                                    yylval->sval = s;
                                    return STRING_LIT;
                                }

\"([^"\\\n]|\\[^\n])*           {
                                    yyextra->error_msg   = "Unterminated string literal";
                                    yyextra->lex_error   = true;
                                    yyextra->error_line  = yylloc->first_line;
                                    yyextra->error_column = yylloc->first_column;
                                    return LEX_ERROR;
                                }

-?[0-9]+                        {
                                    yylval->ival = std::strtoll(yytext, nullptr, 10);
                                    return INT_LIT;
                                }

{ID_START}{ID_CONT}*            {
                                    std::string low;
                                    low.reserve(yyleng);
                                    bool has_upper = false, has_lower = false;
                                    for (int i = 0; i < yyleng; ++i) {
                                        unsigned char c = static_cast<unsigned char>(yytext[i]);
                                        if (std::isupper(c)) has_upper = true;
                                        else if (std::islower(c)) has_lower = true;
                                        low.push_back(static_cast<char>(std::tolower(c)));
                                    }
                                    int kw = keyword_kind(low);
                                    if (kw) {
                                        if (has_upper && has_lower) {
                                            yyextra->error_msg = "Mixed case in keyword '";
                                            yyextra->error_msg.append(yytext, yyleng);
                                            yyextra->error_msg.push_back('\'');
                                            yyextra->lex_error    = true;
                                            yyextra->error_line   = yylloc->first_line;
                                            yyextra->error_column = yylloc->first_column;
                                            return LEX_ERROR;
                                        }
                                        yylval->sval = new std::string(yytext, yyleng);
                                        return kw;
                                    }
                                    yylval->sval = new std::string(yytext, yyleng);
                                    return IDENT;
                                }

"=="                            { return EQEQ; }
"="                             { return EQ;   }
"!="                            { return NEQ;  }
"<="                            { return LEQ;  }
">="                            { return GEQ;  }
"<"                             { return LT;   }
">"                             { return GT;   }
"("                             { return LPAREN; }
")"                             { return RPAREN; }
","                             { return COMMA;  }
";"                             { return SEMI;   }
"."                             { return DOT;    }
"*"                             { return STAR;   }

.                               {
                                    yyextra->error_msg = std::string("Unexpected character '") + yytext[0] + "'";
                                    yyextra->lex_error    = true;
                                    yyextra->error_line   = yylloc->first_line;
                                    yyextra->error_column = yylloc->first_column;
                                    return LEX_ERROR;
                                }

%%

/* ----------------------------------------------------------------------- */
/* User code: keyword table, location tracking, string decoding.           */
/* ----------------------------------------------------------------------- */

namespace {

const std::unordered_map<std::string, int>& kw_table() {
    static const std::unordered_map<std::string, int> t = {
        {"create",    CREATE},
        {"database",  DATABASE_KW},
        {"drop",      DROP},
        {"use",       USE},
        {"table",     TABLE},
        {"insert",    INSERT},
        {"into",      INTO},
        {"value",     VALUE_KW},
        {"values",    VALUE_KW},  /* common SQL plural is accepted */
        {"update",    UPDATE},
        {"set",       SET},
        {"where",     WHERE},
        {"delete",    DELETE},
        {"from",      FROM},
        {"select",    SELECT},
        {"as",        AS},
        {"not_null",  NOT_NULL},
        {"indexed",   INDEXED},
        {"default",   DEFAULT},
        {"and",       AND_KW},
        {"or",        OR_KW},
        {"between",   BETWEEN},
        {"like",      LIKE},
        {"int",       INT_KW},
        {"integer",   INT_KW},
        {"string",    STRING_KW},
        {"sum",       SUM},
        {"count",     COUNT},
        {"avg",       AVG},
        {"null",      NULL_KW},
    };
    return t;
}

}  /* namespace */

static int keyword_kind(const std::string& lower) {
    auto it = kw_table().find(lower);
    return (it == kw_table().end()) ? 0 : it->second;
}

static std::string decode_string(const char* text, int len) {
    /* Skip the surrounding quotes. */
    std::string out;
    out.reserve(len > 2 ? len - 2 : 0);
    for (int i = 1; i < len - 1; ++i) {
        if (text[i] == '\\' && i + 1 < len - 1) {
            char e = text[++i];
            switch (e) {
                case 'n':  out.push_back('\n'); break;
                case 't':  out.push_back('\t'); break;
                case 'r':  out.push_back('\r'); break;
                case '\\': out.push_back('\\'); break;
                case '"':  out.push_back('"');  break;
                default:   out.push_back(e);    break;
            }
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

/* Per-scanner line/column counter. Stored inside the ParseContext (via
 * yyextra) so multiple parsers can coexist without sharing state. We do not
 * use %option yylineno because we also need columns. */

static int yyextra_col_line(yyscan_t scanner) {
    auto* ctx = static_cast<cw_db::ParseContext*>(yyget_extra(scanner));
    /* Re-purpose error_line/column as the live cursor before yyparse sets
     * them on diagnostic; on entry they're zero and we lazily initialise to
     * 1 / 1. */
    if (ctx->error_line == 0) { ctx->error_line = 1; ctx->error_column = 1; }
    return ctx->error_line;
}

static int yyextra_col_col(yyscan_t scanner) {
    auto* ctx = static_cast<cw_db::ParseContext*>(yyget_extra(scanner));
    if (ctx->error_column == 0) ctx->error_column = 1;
    return ctx->error_column;
}

static void yyextra_col_advance(yyscan_t scanner, int n) {
    auto* ctx = static_cast<cw_db::ParseContext*>(yyget_extra(scanner));
    ctx->error_column += n;
}

static void yyextra_col_newline(yyscan_t scanner) {
    auto* ctx = static_cast<cw_db::ParseContext*>(yyget_extra(scanner));
    ctx->error_line   += 1;
    ctx->error_column  = 1;
}
