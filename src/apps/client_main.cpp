// HTTP client for prog-server: send SQL, print JSON response.

#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using json = nlohmann::ordered_json;

int connect_to(const std::string& host, int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool send_all(int fd, const char* p, std::size_t n) {
    while (n) {
        ssize_t k = ::send(fd, p, n, 0);
        if (k <= 0) return false;
        p += k; n -= static_cast<std::size_t>(k);
    }
    return true;
}

// Read until a CRLFCRLF terminator; the surrounding HTTP-1.1 response is
// "<status-line>\r\n<headers>\r\n\r\n<body>".
bool read_until(int fd, const std::string& terminator, std::string& acc) {
    char ch;
    while (acc.size() < terminator.size() ||
           acc.compare(acc.size() - terminator.size(), terminator.size(), terminator) != 0) {
        ssize_t k = ::recv(fd, &ch, 1, 0);
        if (k <= 0) return false;
        acc.push_back(ch);
    }
    return true;
}

bool read_exact(int fd, std::string& body, std::size_t n) {
    body.assign(n, '\0');
    std::size_t got = 0;
    while (got < n) {
        ssize_t k = ::recv(fd, body.data() + got, n - got, 0);
        if (k <= 0) return false;
        got += static_cast<std::size_t>(k);
    }
    return true;
}

struct HttpResponse {
    int         status = 0;
    std::string body;
};

bool http_post_query(const std::string& host, int port, const std::string& sql,
                     HttpResponse& out) {
    int fd = connect_to(host, port);
    if (fd < 0) return false;

    std::ostringstream req;
    req << "POST /query HTTP/1.1\r\n"
        << "Host: " << host << ":" << port << "\r\n"
        << "Content-Type: application/sql\r\n"
        << "Content-Length: " << sql.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << sql;
    const std::string s = req.str();
    if (!send_all(fd, s.data(), s.size())) { ::close(fd); return false; }

    std::string head;
    if (!read_until(fd, "\r\n\r\n", head)) { ::close(fd); return false; }

    // Parse "HTTP/1.1 <code> ..."
    std::size_t sp1 = head.find(' ');
    std::size_t sp2 = head.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) { ::close(fd); return false; }
    out.status = std::atoi(head.substr(sp1 + 1, sp2 - sp1 - 1).c_str());

    // Parse Content-Length.
    std::size_t cl = 0;
    {
        std::string lower; lower.reserve(head.size());
        for (char c : head) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        auto pos = lower.find("content-length:");
        if (pos != std::string::npos) {
            pos += std::strlen("content-length:");
            while (pos < lower.size() && (lower[pos] == ' ' || lower[pos] == '\t')) ++pos;
            cl = static_cast<std::size_t>(std::atol(lower.c_str() + pos));
        }
    }

    if (cl > 0 && !read_exact(fd, out.body, cl)) {
        ::close(fd);
        return false;
    }
    ::close(fd);
    return true;
}

void print_response(const HttpResponse& resp) {
    json body;
    try {
        body = json::parse(resp.body);
    } catch (const std::exception&) {
        std::cerr << "ERROR: server returned non-JSON (status " << resp.status
                  << "): " << resp.body << "\n";
        return;
    }
    const bool ok = body.value("ok", false);
    if (ok) {
        if (body.contains("data")) {
            // SELECT result -> dump compactly to stdout
            std::cout << body["data"].dump() << "\n";
        } else if (body.contains("text")) {
            std::cout << "-- " << body["text"].get<std::string>() << "\n";
        }
    } else {
        std::cerr << "ERROR: " << body.value("error", std::string("unknown error")) << "\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    int port = 5432;
    std::string script;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--host" && i + 1 < argc) host = argv[++i];
        else if (a == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
        else script = a;
    }

    std::istream* in = &std::cin;
    std::ifstream file;
    if (!script.empty()) {
        file.open(script);
        if (!file) { std::cerr << "Cannot open " << script << "\n"; return 1; }
        in = &file;
    }

    // Statement splitter (same state machine as main.cpp's StreamSplitter):
    // honour string literals and SQL comments so a stray ';' inside them does
    // not terminate a statement prematurely.
    enum class State { Code, String, LineComment, BlockComment };
    State st = State::Code;
    std::string buf;
    char c;
    while (in->get(c)) {
        buf.push_back(c);
        switch (st) {
            case State::Code:
                if (c == '"') st = State::String;
                else if (c == '-' && in->peek() == '-') {
                    buf.push_back(static_cast<char>(in->get()));
                    st = State::LineComment;
                } else if (c == '/' && in->peek() == '*') {
                    buf.push_back(static_cast<char>(in->get()));
                    st = State::BlockComment;
                } else if (c == ';') {
                    HttpResponse resp;
                    if (!http_post_query(host, port, buf, resp)) {
                        std::cerr << "ERROR: failed to talk to " << host << ":" << port << "\n";
                        return 1;
                    }
                    print_response(resp);
                    buf.clear();
                }
                break;
            case State::String:
                if (c == '\\' && in->peek() != EOF) buf.push_back(static_cast<char>(in->get()));
                else if (c == '"') st = State::Code;
                break;
            case State::LineComment:
                if (c == '\n') st = State::Code;
                break;
            case State::BlockComment:
                if (c == '*' && in->peek() == '/') {
                    buf.push_back(static_cast<char>(in->get()));
                    st = State::Code;
                }
                break;
        }
    }
    return 0;
}
