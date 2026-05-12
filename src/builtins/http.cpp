#include "../builtins.h"
#include "../interpreter.h"
#include "../url.h"
#include <algorithm>
#include <atomic>
#include <csignal>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include "../gc_heap.h"
#include "scope_guards.h"
#ifdef HAVE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#endif

namespace {

// Socket abstraction — wraps a plain fd or an SSL connection
struct SocketConn {
    int fd = -1;
#ifdef HAVE_OPENSSL
    SSL* ssl = nullptr;
    SSL_CTX* ctx = nullptr;
#endif

    ssize_t write(const void* buf, size_t len) {
#ifdef HAVE_OPENSSL
        if (ssl) return SSL_write(ssl, buf, static_cast<int>(len));
#endif
        return ::send(fd, buf, len, 0);
    }

    ssize_t read(void* buf, size_t len) {
#ifdef HAVE_OPENSSL
        if (ssl) return SSL_read(ssl, buf, static_cast<int>(len));
#endif
        return ::recv(fd, buf, len, 0);
    }

    void shutdown_close() {
#ifdef HAVE_OPENSSL
        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); ssl = nullptr; }
        if (ctx) { SSL_CTX_free(ctx); ctx = nullptr; }
#endif
        if (fd >= 0) { ::close(fd); fd = -1; }
    }
};

#ifdef HAVE_OPENSSL
static std::once_flag sslInitFlag;
static void ensureSSLInit() {
    std::call_once(sslInitFlag, [] {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
    });
}
#endif

// Global server fd for signal handler to interrupt accept()
static std::atomic<int> g_serverFd{-1};
static std::atomic<bool> g_shutdownRequested{false};

void shutdownSignalHandler(int) {
    g_shutdownRequested.store(true);
    // Close the server socket to unblock accept()
    int fd = g_serverFd.load();
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
        g_serverFd.store(-1);
    }
}

// URL-decode a percent-encoded string. Handles %XX and + → space (form encoding).
std::string urlDecode(const std::string& str) {
    std::string result;
    result.reserve(str.size());
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '%' && i + 2 < str.size()) {
            int hi = hexVal(str[i + 1]), lo = hexVal(str[i + 2]);
            if (hi >= 0 && lo >= 0) {
                result += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        result += (str[i] == '+') ? ' ' : str[i];
    }
    return result;
}

// Parse "key=val&key2=val2" into a PraiaMap with URL-decoded keys/values.
std::shared_ptr<PraiaMap> parseQueryString(const std::string& query) {
    auto qmap = gcNew<PraiaMap>();
    if (query.empty()) return qmap;
    std::string key, value;
    bool inValue = false;
    for (size_t i = 0; i <= query.size(); ++i) {
        char c = (i < query.size()) ? query[i] : '&';
        if (c == '=') {
            inValue = true;
        } else if (c == '&') {
            if (!key.empty())
                qmap->entries[Value(urlDecode(key))] = Value(urlDecode(value));
            key.clear(); value.clear(); inValue = false;
        } else {
            if (inValue) value += c; else key += c;
        }
    }
    return qmap;
}

// Local view of a parsed URL — preserves the fields the HTTP client
// cares about (host for socket/TLS, hostHeader for the Host: line,
// resolved port + tls flag from scheme defaults, path with the leading
// '/' substituted when the input had none). Everything else
// (userinfo / query / fragment) is currently ignored by this builtin.
struct ParsedUrl {
    std::string host;        // unbracketed (suitable for getaddrinfo/SNI/inet_pton)
    std::string hostHeader;  // bracketed when IPv6 (suitable for Host:)
    int port = 80;
    std::string path = "/";
    bool tls = false;
};

ParsedUrl parseUrl(const std::string& url) {
    praia::url::ParsedUrl u;
    try {
        u = praia::url::parse(url);
    } catch (const praia::url::UrlParseError& e) {
        throw RuntimeError(std::string("Invalid URL: ") + e.what(), 0);
    }

    ParsedUrl r;
    if (u.scheme == "https") {
#ifdef HAVE_OPENSSL
        r.tls = true;
        r.port = 443;
#else
        throw RuntimeError("HTTPS not supported (build with OpenSSL for TLS)", 0);
#endif
    } else if (u.scheme.empty() || u.scheme == "http") {
        // Default — treat unspecified scheme as http for back-compat.
    } else {
        throw RuntimeError("Unsupported URL scheme \"" + u.scheme + "\" in \"" + url + "\"", 0);
    }

    if (u.hasPort) r.port = u.port;

    r.host = u.host;
    r.hostHeader = praia::url::hostHeader(u);
    // path-abempty is allowed to be empty in RFC 3986; HTTP/1.1 needs
    // an origin-form with at least "/" on the request line.
    r.path = u.path.empty() ? "/" : u.path;
    // Re-attach query (raw, not decoded) so the request line carries
    // it. Fragment is intentionally dropped — clients MUST NOT send it
    // per RFC 3986 §3.5.
    if (!u.query.empty()) r.path += "?" + u.query;
    return r;
}

int connectToHost(const std::string& host, int port) {
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    praia::AddrGuard ag;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &ag.res) != 0)
        throw RuntimeError("Cannot resolve host: " + host, 0);
    praia::FdGuard sock(socket(ag->ai_family, ag->ai_socktype, ag->ai_protocol));
    if (!sock || connect(sock.get(), ag->ai_addr, ag->ai_addrlen) < 0)
        throw RuntimeError("Cannot connect to " + host + ":" + std::to_string(port), 0);
    return sock.release();
}

std::string readAll(SocketConn& conn) {
    std::string data;
    char buf[8192];
    ssize_t n;
    while ((n = conn.read(buf, sizeof(buf))) > 0)
        data.append(buf, n);
    return data;
}

Value parseHttpResponse(const std::string& raw) {
    auto headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos)
        throw RuntimeError("Invalid HTTP response", 0);

    std::string headerSection = raw.substr(0, headerEnd);
    std::string body = raw.substr(headerEnd + 4);

    int status = 0;
    auto sp1 = headerSection.find(' ');
    if (sp1 != std::string::npos) {
        auto sp2 = headerSection.find(' ', sp1 + 1);
        try { status = std::stoi(headerSection.substr(sp1 + 1, sp2 - sp1 - 1)); } catch (...) {}
    }

    auto hdrs = gcNew<PraiaMap>();
    std::istringstream hs(headerSection);
    std::string line;
    std::getline(hs, line);
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto c = line.find(": ");
        if (c != std::string::npos) {
            std::string key = line.substr(0, c);
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            hdrs->entries[Value(key)] = Value(line.substr(c + 2));
        }
    }

    auto result = gcNew<PraiaMap>();
    result->entries[Value("status")] = Value(static_cast<double>(status));
    result->entries[Value("body")] = Value(body);
    result->entries[Value("headers")] = Value(hdrs);
    return Value(result);
}

// Limits for incoming requests
// No body-size cap — handlers/middleware (e.g. middleware.bodyLimit) decide policy.
// A reverse proxy is the right place for DoS-grade limits.
static constexpr size_t MAX_HEADER_SIZE = 64 * 1024;   // 64 KB headers
static constexpr int    RECV_TIMEOUT_SECS = 30;

// Case-insensitive search for a header name in raw header text.
// Returns the value or empty string if not found.
static std::string findHeaderValue(const std::string& headers, const std::string& name) {
    // Search for "\nName:" (case-insensitive) — the first line has no \n prefix, handle separately
    for (size_t searchFrom = 0; ;) {
        size_t pos = headers.find(':', searchFrom);
        if (pos == std::string::npos) break;
        // Walk back to find the start of this header name
        size_t lineStart = headers.rfind('\n', pos);
        lineStart = (lineStart == std::string::npos) ? 0 : lineStart + 1;
        std::string key = headers.substr(lineStart, pos - lineStart);
        // Trim trailing \r
        if (!key.empty() && key.back() == '\r') key.pop_back();
        // Case-insensitive compare
        if (key.size() == name.size()) {
            bool match = true;
            for (size_t i = 0; i < key.size(); i++) {
                if (std::tolower(key[i]) != std::tolower(name[i])) { match = false; break; }
            }
            if (match) {
                size_t valStart = pos + 1;
                while (valStart < headers.size() && headers[valStart] == ' ') valStart++;
                size_t valEnd = headers.find('\r', valStart);
                if (valEnd == std::string::npos) valEnd = headers.find('\n', valStart);
                if (valEnd == std::string::npos) valEnd = headers.size();
                return headers.substr(valStart, valEnd - valStart);
            }
        }
        searchFrom = pos + 1;
    }
    return "";
}

// Parse a raw HTTP request from a client socket.
// Returns a PraiaMap with method, path, query, headers, body.
// Also outputs the raw client fd for SSE use.
std::shared_ptr<PraiaMap> readAndParseRequest(int client) {
    // Set a receive timeout so slow/idle clients don't block the server
    struct timeval tv;
    tv.tv_sec = RECV_TIMEOUT_SECS;
    tv.tv_usec = 0;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string data;
    char buf[8192];
    while (true) {
        ssize_t n = recv(client, buf, sizeof(buf), 0);
        if (n <= 0) break;
        data.append(buf, n);

        // Enforce header size limit before we find the end of headers
        auto hend = data.find("\r\n\r\n");
        if (hend == std::string::npos) {
            if (data.size() > MAX_HEADER_SIZE) return nullptr; // headers too large
            continue;
        }

        // Headers complete — check for body
        std::string hdr = data.substr(0, hend);
        std::string clVal = findHeaderValue(hdr, "content-length");
        if (!clVal.empty()) {
            size_t clen = 0;
            try { clen = std::stoul(clVal); } catch (...) {}
            size_t bodyStart = hend + 4;
            while (data.size() - bodyStart < clen) {
                n = recv(client, buf, sizeof(buf), 0);
                if (n <= 0) break;
                data.append(buf, n);
            }
        }
        break;
    }

    if (data.empty()) return nullptr;

    auto firstLine = data.substr(0, data.find("\r\n"));
    std::string method, path;
    auto sp1 = firstLine.find(' ');
    auto sp2 = firstLine.find(' ', sp1 + 1);
    if (sp1 != std::string::npos && sp2 != std::string::npos) {
        method = firstLine.substr(0, sp1);
        path = firstLine.substr(sp1 + 1, sp2 - sp1 - 1);
    }

    std::string query;
    auto qpos = path.find('?');
    if (qpos != std::string::npos) {
        query = path.substr(qpos + 1);
        path = path.substr(0, qpos);
    }

    auto hend = data.find("\r\n\r\n");
    auto reqHeaders = gcNew<PraiaMap>();
    std::istringstream hs(data.substr(0, hend));
    std::string line;
    std::getline(hs, line); // skip request line
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Accept "Key: Value" or "Key:Value" (colon with optional space)
        auto c = line.find(':');
        if (c != std::string::npos) {
            std::string key = line.substr(0, c);
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            size_t valStart = c + 1;
            while (valStart < line.size() && line[valStart] == ' ') valStart++;
            reqHeaders->entries[Value(key)] = Value(line.substr(valStart));
        }
    }

    std::string reqBody = (hend != std::string::npos) ? data.substr(hend + 4) : "";

    auto req = gcNew<PraiaMap>();
    req->entries[Value("method")] = Value(method);
    req->entries[Value("path")] = Value(path);
    req->entries[Value("query")] = Value(parseQueryString(query));
    req->entries[Value("headers")] = Value(reqHeaders);
    req->entries[Value("body")] = Value(reqBody);
    return req;
}

static const char* reasonPhrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 422: return "Unprocessable Entity";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default:  return "Unknown";
    }
}

// True if the header field is safe to emit on the wire. Refuses CR/LF/NUL
// in values, and CR/LF/NUL/colon in names. CRLF in either is a header-
// injection (response-splitting) attempt: an attacker controlling part
// of a header could close the current header line and inject further
// headers or a fake response body.
//
// Exposed via anonymous namespace at file scope so all four serialization
// sites and the user-facing http.redirect / outbound client path can
// share the same predicate.
bool headerFieldSafe(const std::string& name, const std::string& value) {
    if (name.empty()) return false;
    for (char c : name) {
        if (c == '\r' || c == '\n' || c == '\0' || c == ':') return false;
    }
    for (char c : value) {
        if (c == '\r' || c == '\n' || c == '\0') return false;
    }
    return true;
}

void sendHttpResponse(int client, int status, const std::string& body,
                       const std::unordered_map<std::string, std::string>& headers) {
    std::string resp = "HTTP/1.1 " + std::to_string(status) + " " + reasonPhrase(status) + "\r\n";
    auto hdrs = headers;
    hdrs["Content-Length"] = std::to_string(body.size());
    hdrs["Connection"] = "close";
    // Defense-in-depth: skip any header containing CR/LF/NUL. Upstream
    // validation should have rejected these already, but this final
    // gate ensures injection never reaches the wire even if a new
    // header-producing code path forgets to validate.
    for (auto& [k, v] : hdrs) {
        if (!headerFieldSafe(k, v)) continue;
        resp += k + ": " + v + "\r\n";
    }
    resp += "\r\n" + body;
    // Loop until the whole response is written. send() can return short on
    // congested sockets; a bare send() without the loop silently truncates.
    // SIGPIPE is ignored process-wide, so a closed peer just returns -1
    // here instead of killing the server.
    size_t off = 0;
    while (off < resp.size()) {
        ssize_t sent = ::send(client, resp.c_str() + off, resp.size() - off, 0);
        if (sent < 0) break;
        off += static_cast<size_t>(sent);
    }
}

// Stream a file to the client without loading the whole thing into memory.
// Sends headers (with Content-Length from stat) followed by 64 KB chunks.
// Returns false if the file couldn't be opened (caller should send 404/500).
bool sendHttpFileResponse(int client, int status, const std::string& path,
                           const std::unordered_map<std::string, std::string>& extraHeaders) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return false;
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;

    // Headers first.
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " +
                       reasonPhrase(status) + "\r\n";
    auto hdrs = extraHeaders;
    hdrs["Content-Length"] = std::to_string(static_cast<long long>(st.st_size));
    hdrs["Connection"] = "close";
    // Same injection guard as sendHttpResponse — skip CRLF-tainted headers.
    for (auto& [k, v] : hdrs) {
        if (!headerFieldSafe(k, v)) continue;
        head += k + ": " + v + "\r\n";
    }
    head += "\r\n";

    // sendAll: loop until the whole buffer is written or the peer disconnects.
    // send() can return less than asked on slow/congested sockets; without
    // this loop a long Content-Disposition filename would be truncated.
    auto sendAll = [&](const char* data, size_t len) -> bool {
        size_t off = 0;
        while (off < len) {
            ssize_t sent = ::send(client, data + off, len - off, 0);
            if (sent < 0) return false;
            off += static_cast<size_t>(sent);
        }
        return true;
    };

    if (!sendAll(head.c_str(), head.size())) { ::close(fd); return true; }

    // Body in 64 KB chunks. Stop on read error or client disconnect.
    char buf[64 * 1024];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0) {
        if (!sendAll(buf, static_cast<size_t>(n))) { ::close(fd); return true; }
    }
    ::close(fd);
    return true;
}

} // anonymous namespace

// ── Public API ───────────────────────────────────────────────

Value doHttpRequest(const std::string& method, const std::string& url,
                    const std::string& body,
                    const std::unordered_map<std::string, std::string>& extraHeaders) {
    auto p = parseUrl(url);
    // Reject method/path/host/header CR/LF/NUL BEFORE any network I/O.
    // Failing fast saves a DNS lookup + TCP/TLS handshake on garbage
    // input and lets the user see the real error.
    for (char c : method) if (c == '\r' || c == '\n' || c == '\0')
        throw RuntimeError("Invalid HTTP method: contains CR/LF/NUL", 0);
    for (char c : p.path) if (c == '\r' || c == '\n' || c == '\0')
        throw RuntimeError("Invalid URL path: contains CR/LF/NUL", 0);
    for (char c : p.host) if (c == '\r' || c == '\n' || c == '\0')
        throw RuntimeError("Invalid host: contains CR/LF/NUL", 0);
    for (auto& [k, v] : extraHeaders) {
        if (!headerFieldSafe(k, v))
            throw RuntimeError("Invalid request header \"" + k +
                               "\": contains CR/LF/NUL (header injection)", 0);
    }

    SocketConn conn;
    conn.fd = connectToHost(p.host, p.port);

#ifdef HAVE_OPENSSL
    if (p.tls) {
        ensureSSLInit();
        conn.ctx = SSL_CTX_new(TLS_client_method());
        if (!conn.ctx) { conn.shutdown_close(); throw RuntimeError("Failed to create SSL context", 0); }
        SSL_CTX_set_default_verify_paths(conn.ctx);
        SSL_CTX_set_verify(conn.ctx, SSL_VERIFY_PEER, nullptr);
        conn.ssl = SSL_new(conn.ctx);
        SSL_set_fd(conn.ssl, conn.fd);

        // SNI — server-name extension; tells the server which vhost we want.
        SSL_set_tlsext_host_name(conn.ssl, p.host.c_str());

        // Cert verification: tell OpenSSL to also check that the cert's
        // Subject/SAN matches the host/IP we connected to. SNI + chain
        // verification alone is NOT enough — without this, a valid cert
        // for ANY hostname signed by a trusted CA would pass. The check
        // runs as part of SSL_connect's verification; a mismatch surfaces
        // as X509_V_ERR_HOSTNAME_MISMATCH / X509_V_ERR_IP_ADDRESS_MISMATCH
        // in SSL_get_verify_result.
        {
            X509_VERIFY_PARAM* vp = SSL_get0_param(conn.ssl);
            // IP literals (IPv4 or IPv6) take the IP-match path; everything
            // else is treated as a DNS name. inet_pton returns 1 on success.
            unsigned char ipbuf[16];
            bool isIp = (inet_pton(AF_INET,  p.host.c_str(), ipbuf) == 1 ||
                         inet_pton(AF_INET6, p.host.c_str(), ipbuf) == 1);
            if (isIp) {
                X509_VERIFY_PARAM_set1_ip_asc(vp, p.host.c_str());
            } else {
                X509_VERIFY_PARAM_set1_host(vp, p.host.c_str(), p.host.size());
                // Reject partial-wildcard patterns (e.g. `f*.example.com`);
                // only full-label wildcards (`*.example.com`) are allowed.
                X509_VERIFY_PARAM_set_hostflags(vp, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
            }
        }

        if (SSL_connect(conn.ssl) <= 0) {
            // Verification errors (expired cert, hostname mismatch, etc.)
            // surface as SSL_connect failures. Surface the verify result
            // if it explains the failure; otherwise generic message.
            long vr = SSL_get_verify_result(conn.ssl);
            conn.shutdown_close();
            if (vr != X509_V_OK) {
                throw RuntimeError("SSL certificate verification failed for " + p.host +
                                   ": " + X509_verify_cert_error_string(vr), 0);
            }
            throw RuntimeError("SSL handshake failed for " + p.host, 0);
        }
        long vr = SSL_get_verify_result(conn.ssl);
        if (vr != X509_V_OK) {
            conn.shutdown_close();
            throw RuntimeError("SSL certificate verification failed for " + p.host +
                               ": " + X509_verify_cert_error_string(vr), 0);
        }
    }
#endif

    // CR/LF/NUL validation already happened above before connect — trust
    // these inputs here. Serialize straight to the wire.
    std::string req = method + " " + p.path + " HTTP/1.1\r\n";
    req += "Host: " + p.hostHeader + "\r\n";
    req += "Connection: close\r\n";
    for (auto& [k, v] : extraHeaders) req += k + ": " + v + "\r\n";
    if (!body.empty() && extraHeaders.find("Content-Type") == extraHeaders.end())
        req += "Content-Type: text/plain\r\n";
    if (!body.empty())
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;

    if (conn.write(req.c_str(), req.size()) < 0) {
        conn.shutdown_close();
        throw RuntimeError("Failed to send HTTP request", 0);
    }
    std::string raw = readAll(conn);
    conn.shutdown_close();
    return parseHttpResponse(raw);
}

void httpServerListen(int port, std::shared_ptr<Callable> handler, Interpreter& interp) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw RuntimeError("Cannot create server socket", 0);
    praia::setCloexec(fd);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd); throw RuntimeError("Cannot bind to port " + std::to_string(port), 0);
    }
    if (::listen(fd, 64) < 0) {
        close(fd); throw RuntimeError("Cannot listen on port " + std::to_string(port), 0);
    }

    // Install signal handler for graceful shutdown
    g_serverFd.store(fd);
    g_shutdownRequested.store(false);
    struct sigaction sa = {};
    sa.sa_handler = shutdownSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    std::cout << "Server listening on port " << port << std::endl;

    while (!g_shutdownRequested.load()) {
        struct sockaddr_in ca;
        socklen_t cl = sizeof(ca);
        int client = accept(fd, (struct sockaddr*)&ca, &cl);
        if (client < 0) {
            if (g_shutdownRequested.load()) break;
            continue;
        }
        praia::setCloexec(client);

        auto req = readAndParseRequest(client);
        if (!req) { close(client); continue; }

        // Check if the handler returns an SSE response
        std::string respBody = "Internal Server Error";
        int respStatus = 500;
        std::unordered_map<std::string, std::string> respHeaders;
        std::string streamFilePath;  // non-empty → stream this file as the body

        bool sseHandled = false;
        // Collected before sendHttpResponse, fired after — the response map
        // can attach `afterResponse: fn` or `afterResponses: [fn, ...]` for
        // post-send work (typically temp-file cleanup).
        std::vector<std::shared_ptr<Callable>> afterCallbacks;
        try {
            // Attach the raw client fd to the request so http.sse can use it
            req->entries[Value("__clientFd")] = Value(static_cast<double>(client));

            std::vector<Value> args = {Value(req)};
            Value result = handler->call(interp, args);

            // Check if SSE handled the response (it sends headers itself)
            if (result.isMap() && result.asMap()->entries.count(Value("__sse"))) {
                sseHandled = true;
            }

            if (result.isMap()) {
                auto& e = result.asMap()->entries;
                if (e.count("status") && e["status"].isNumber())
                    respStatus = static_cast<int>(e["status"].asNumber());
                else
                    respStatus = 200;
                // __streamFile takes precedence over body — the listen loop
                // streams this file directly without buffering it in memory.
                auto sf = e.find(Value("__streamFile"));
                if (sf != e.end() && sf->second.isString()) {
                    streamFilePath = sf->second.asString();
                } else if (e.count("body")) {
                    respBody = e["body"].toString();
                } else {
                    respBody = "";
                }
                if (e.count("headers") && e["headers"].isMap()) {
                    for (auto& [k, v] : e["headers"].asMap()->entries) {
                        std::string kn = k.toString(), vn = v.toString();
                        if (!headerFieldSafe(kn, vn))
                            throw RuntimeError("Invalid response header \"" + kn +
                                               "\": contains CR/LF/NUL (header injection)", 0);
                        respHeaders[kn] = vn;
                    }
                }
                if (respHeaders.find("Content-Type") == respHeaders.end() &&
                    respHeaders.find("content-type") == respHeaders.end())
                    respHeaders["Content-Type"] = "text/plain";

                // After-response callbacks: a single callable or an array of them.
                auto collect = [&afterCallbacks](const Value& v) {
                    if (v.isCallable()) afterCallbacks.push_back(v.asCallable());
                    else if (v.isArray()) {
                        for (auto& el : v.asArray()->elements)
                            if (el.isCallable()) afterCallbacks.push_back(el.asCallable());
                    }
                };
                if (e.count(Value("afterResponse")))  collect(e[Value("afterResponse")]);
                if (e.count(Value("afterResponses"))) collect(e[Value("afterResponses")]);
            } else if (result.isString()) {
                respStatus = 200;
                respBody = result.asString();
                respHeaders["Content-Type"] = "text/plain";
            }
        } catch (const ThrowSignal& t) {
            respStatus = 500;
            respBody = "Error: " + t.value.toString();
        } catch (const RuntimeError& e) {
            respStatus = 500;
            respBody = "Error: " + std::string(e.what());
        }

        if (sseHandled) {
            // SSE handler should have closed the socket; close again
            // as a safety net (harmless if already closed).
            close(client);
            // afterResponse runs after socket close for SSE too
            for (auto& cb : afterCallbacks) {
                try { cb->call(interp, {}); }
                catch (...) { /* swallow — request already finished */ }
            }
            continue;
        }
        if (!streamFilePath.empty()) {
            if (!sendHttpFileResponse(client, respStatus, streamFilePath, respHeaders)) {
                // File couldn't be opened — fall back to a 500.
                sendHttpResponse(client, 500,
                    "Error: cannot open file: " + streamFilePath,
                    {{"Content-Type", "text/plain"}});
            }
        } else {
            sendHttpResponse(client, respStatus, respBody, respHeaders);
        }
        close(client);

        // After-response callbacks run on the server thread, after the
        // bytes have been handed to the kernel and the socket is closed.
        // The handler thus gets the OS's "best effort" guarantee that
        // the client received the response before cleanup runs. Errors
        // in callbacks are swallowed so a buggy cleanup can't break
        // the server.
        for (auto& cb : afterCallbacks) {
            try { cb->call(interp, {}); }
            catch (...) { /* swallow */ }
        }
    }

    // Graceful shutdown complete
    if (g_serverFd.load() >= 0) {
        close(g_serverFd.load());
        g_serverFd.store(-1);
    }
    std::cout << "\nServer stopped." << std::endl;
}
