// Local prog: interactive stdin or batch SQL file (statements end with ';').

#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "core/dbms.h"
#include "net/logger.h"
#include "net/telemetry.h"
#include "sql/executor.h"

namespace {

// Split SQL stream into statements at top-level ';'.
class StreamSplitter {
public:
    explicit StreamSplitter(std::istream& in) : in_(in) {}

    bool next(std::string& out) {
        out.clear();
        enum class State { Code, String, LineComment, BlockComment };
        State st = State::Code;
        char c;
        while (in_.get(c)) {
            out.push_back(c);
            switch (st) {
                case State::Code:
                    if (c == '"') {
                        st = State::String;
                    } else if (c == '-' && in_.peek() == '-') {
                        out.push_back(static_cast<char>(in_.get()));
                        st = State::LineComment;
                    } else if (c == '/' && in_.peek() == '*') {
                        out.push_back(static_cast<char>(in_.get()));
                        st = State::BlockComment;
                    } else if (c == ';') {
                        return true;
                    }
                    break;
                case State::String:
                    if (c == '\\' && in_.peek() != EOF) {
                        out.push_back(static_cast<char>(in_.get()));
                    } else if (c == '"') {
                        st = State::Code;
                    }
                    break;
                case State::LineComment:
                    if (c == '\n') st = State::Code;
                    break;
                case State::BlockComment:
                    if (c == '*' && in_.peek() == '/') {
                        out.push_back(static_cast<char>(in_.get()));
                        st = State::Code;
                    }
                    break;
            }
        }
        for (char ch : out) if (!std::isspace(static_cast<unsigned char>(ch))) return true;
        out.clear();
        return false;
    }

private:
    std::istream& in_;
};

void print_result(const cw_db::ExecutionResult& r, bool to_stderr_for_status) {
    if (r.ok) {
        if (!r.output.empty() && r.output.front() == '[') {
            // JSON array (SELECT result) -> stdout
            std::cout << r.output << "\n";
        } else if (to_stderr_for_status) {
            std::cerr << "-- " << r.output << "\n";
        } else {
            std::cout << r.output << "\n";
        }
    } else {
        std::cerr << "ERROR: " << r.error << "\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string data_dir;
    std::string script_path;
    bool quiet_status = false;
    bool enable_log   = false;
    std::string log_path = "./access.log";

    // Argument parsing: positional argument is the script path (batch mode).
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--data" && i + 1 < argc) data_dir = argv[++i];
        else if (a == "--quiet") quiet_status = true;
        else if (a == "--log") { enable_log = true; if (i + 1 < argc && argv[i+1][0] != '-') log_path = argv[++i]; }
        else if (a == "--help" || a == "-h") {
            std::cerr << "Usage: prog [script.sql] [--data DIR] [--quiet] [--log [path]]\n";
            return 0;
        } else {
            script_path = a;
        }
    }
    if (data_dir.empty()) data_dir = "./data";
    if (enable_log) cw_db::AccessLog::instance().open(log_path);

    cw_db::Dbms dbms(data_dir);
    try {
        dbms.load_all();
    } catch (const std::exception& e) {
        std::cerr << "Failed to load state from " << data_dir << ": " << e.what() << "\n";
        return 1;
    }

    std::istream* in = &std::cin;
    std::ifstream file;
    bool interactive = script_path.empty();
    if (!interactive) {
        file.open(script_path);
        if (!file) {
            std::cerr << "Cannot open " << script_path << "\n";
            return 1;
        }
        in = &file;
    }

    if (interactive && isatty(STDIN_FILENO)) {
        std::cerr << "cw_db interactive mode. End statements with ';', Ctrl-D to exit.\n";
    }

    StreamSplitter splitter(*in);
    std::string sql;
    std::uint64_t rid_seq = 0;
    while (splitter.next(sql)) {
        // Skip whitespace-only segments without touching telemetry/logs.
        bool non_ws = false;
        for (char c : sql) { if (!std::isspace(static_cast<unsigned char>(c))) { non_ws = true; break; } }
        if (!non_ws) continue;

        auto t0 = std::chrono::system_clock::now();
        auto s0 = cw_db::Telemetry::Clock::now();
        cw_db::ExecutionResult r = cw_db::run_query(dbms, sql);
        auto t1 = std::chrono::system_clock::now();
        auto s1 = cw_db::Telemetry::Clock::now();

        if (cw_db::AccessLog::instance().is_open()) {
            cw_db::AccessLog::instance().log(
                ++rid_seq, "local", "main", sql, t0, t1,
                r.ok ? 200 : 400, r.ok ? "OK" : r.error);
        }
        cw_db::Telemetry::instance().observe(s0, s1, r.ok);
        print_result(r, quiet_status);
    }
    cw_db::AccessLog::instance().close();
    return 0;
}
