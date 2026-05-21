#include "../builtins.h"
#include "../interpreter.h"
#include "../praia_plugin.h"
#include "../url.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/time.h>
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
    // Cumulative bytes successfully read from the peer. The session
    // retry path checks whether this advanced during a failed request
    // to decide if the request is replayable on a fresh socket — see
    // doOneHttpRequestImpl's catch block.
    size_t bytesRead = 0;

    SocketConn() = default;
    // Resource-owning: disable copy/move so a stray copy can't lead
    // to a double shutdown_close in two destructors. Every code path
    // uses SocketConn by reference or via shared_ptr; deleting these
    // catches future regressions at compile time.
    SocketConn(const SocketConn&) = delete;
    SocketConn& operator=(const SocketConn&) = delete;
    SocketConn(SocketConn&&) = delete;
    SocketConn& operator=(SocketConn&&) = delete;

    ssize_t write(const void* buf, size_t len) {
#ifdef HAVE_OPENSSL
        if (ssl) return SSL_write(ssl, buf, static_cast<int>(len));
#endif
        return ::send(fd, buf, len, 0);
    }

    ssize_t read(void* buf, size_t len) {
#ifdef HAVE_OPENSSL
        if (ssl) {
            ssize_t n = SSL_read(ssl, buf, static_cast<int>(len));
            if (n > 0) bytesRead += static_cast<size_t>(n);
            return n;
        }
#endif
        ssize_t n = ::recv(fd, buf, len, 0);
        if (n > 0) bytesRead += static_cast<size_t>(n);
        return n;
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

// Non-blocking connect with poll-based timeout. Tries each address
// returned by getaddrinfo in order until one connects (matches what
// libcurl/Python/Go do for happy-eyeballs-lite handling of multi-A
// records and IPv4/IPv6 dual-stack hosts).
//
// timeoutMs of -1 means no timeout (block forever); 0 is effectively
// "give up immediately on EINPROGRESS" which is mostly useful for
// testing.
int connectToHost(const std::string& host, int port, int timeoutMs) {
    struct addrinfo hints = {};
    // AF_UNSPEC lets the resolver return both IPv4 and IPv6 records;
    // pre-Phase-2 this was hard-coded to AF_INET, which made IPv6-only
    // hosts unreachable.
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    praia::AddrGuard ag;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &ag.res) != 0)
        throw RuntimeError("Cannot resolve host: " + host, 0);

    std::string lastErr;
    for (struct addrinfo* ai = ag.res; ai != nullptr; ai = ai->ai_next) {
        praia::FdGuard sock(socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (!sock) { lastErr = std::strerror(errno); continue; }

        // Flip to non-blocking so connect() returns immediately with
        // EINPROGRESS; poll() then handles the actual timeout. We
        // flip back to blocking before returning so reads/writes use
        // the standard blocking-with-SO_RCVTIMEO model.
        int flags = fcntl(sock.get(), F_GETFL, 0);
        if (flags < 0 ||
            fcntl(sock.get(), F_SETFL, flags | O_NONBLOCK) < 0) {
            lastErr = std::strerror(errno);
            continue;
        }

        int r = connect(sock.get(), ai->ai_addr, ai->ai_addrlen);
        if (r == 0) {
            // Connected immediately (e.g. localhost) — restore blocking.
            if (fcntl(sock.get(), F_SETFL, flags) < 0) {
                lastErr = std::strerror(errno);
                continue;
            }
            return sock.release();
        }
        if (errno != EINPROGRESS) {
            lastErr = std::strerror(errno);
            continue;
        }

        // Wait for writability (= connect completed). poll's -1
        // means "no timeout" which matches our convention.
        struct pollfd pfd;
        pfd.fd = sock.get();
        pfd.events = POLLOUT;
        pfd.revents = 0;
        int polled;
        do {
            polled = poll(&pfd, 1, timeoutMs);
        } while (polled < 0 && errno == EINTR);

        if (polled == 0) {
            // Timed out on THIS address — try the next one rather
            // than giving up. Real-world: A and AAAA records often
            // both exist; one may be unreachable due to NAT64 or
            // missing v6 transit.
            lastErr = "connect timed out after " + std::to_string(timeoutMs) + "ms";
            continue;
        }
        if (polled < 0) {
            lastErr = std::strerror(errno);
            continue;
        }

        // poll fired — check SO_ERROR to learn whether connect
        // actually succeeded or hit a refusal/unreachable.
        int soerr = 0;
        socklen_t slen = sizeof(soerr);
        if (getsockopt(sock.get(), SOL_SOCKET, SO_ERROR, &soerr, &slen) < 0) {
            lastErr = std::strerror(errno);
            continue;
        }
        if (soerr != 0) {
            lastErr = std::strerror(soerr);
            continue;
        }

        // Connected. Restore the original (blocking) file-status flags.
        if (fcntl(sock.get(), F_SETFL, flags) < 0) {
            lastErr = std::strerror(errno);
            continue;
        }
        return sock.release();
    }

    throw RuntimeError("Cannot connect to " + host + ":" + std::to_string(port) +
                       (lastErr.empty() ? "" : " (" + lastErr + ")"), 0);
}

// Apply a per-operation receive/send timeout to a connected socket.
// Both directions get the same value so a slow peer can't stall the
// request half indefinitely. `ms <= 0` disables the timeout.
void setSocketTimeouts(int fd, int ms) {
    if (ms <= 0) return;
    struct timeval tv;
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

// Parse just status + headers + cookies from a response header section
// (no trailing \r\n\r\n). Separated from parseHttpResponse so the
// streaming path (http.openStream) can read its headers without
// triggering an eager body decode on a partial first chunk —
// previously that path called parseHttpResponse with header+prefix,
// which then ran the chunked decoder over a tiny prefix and threw
// "chunk data truncated" before the stream could be returned.
struct ResponseHeaderFields {
    int status;
    std::shared_ptr<PraiaMap> headers;
    std::shared_ptr<PraiaArray> cookies;
};

static ResponseHeaderFields parseResponseHeaders(const std::string& headerSection) {
    ResponseHeaderFields out;
    out.status = 0;
    out.headers = gcNew<PraiaMap>();
    out.cookies = gcNew<PraiaArray>();

    auto sp1 = headerSection.find(' ');
    if (sp1 != std::string::npos) {
        auto sp2 = headerSection.find(' ', sp1 + 1);
        try {
            out.status = std::stoi(headerSection.substr(sp1 + 1, sp2 - sp1 - 1));
        } catch (...) {}
    }

    // Set-Cookie is the one header that legitimately repeats — every
    // cookie a server sets in one response is a separate Set-Cookie
    // line. The headers map can only hold one value per key, so we
    // expose the full list as a dedicated `cookies` array (in arrival
    // order). For back-compat the headers map still carries the
    // last value at "set-cookie" too.
    std::istringstream hs(headerSection);
    std::string line;
    std::getline(hs, line);
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // RFC 7230 §3.2: field-name ":" OWS field-value OWS. We find the
        // first colon (not ": "), reject any whitespace between name and
        // colon (forbidden by §3.2.4), then strip leading + trailing OWS
        // from the value. Earlier code required a literal ": " and
        // silently dropped headers like "Location:/final" (no space).
        auto c = line.find(':');
        if (c == std::string::npos || c == 0) continue;
        std::string key = line.substr(0, c);
        if (key.back() == ' ' || key.back() == '\t') continue;
        size_t vs = c + 1;
        while (vs < line.size() && (line[vs] == ' ' || line[vs] == '\t')) vs++;
        size_t ve = line.size();
        while (ve > vs && (line[ve - 1] == ' ' || line[ve - 1] == '\t')) ve--;
        std::string val = line.substr(vs, ve - vs);
        // Cast through unsigned char — passing a negative char to
        // std::tolower is UB. Same pattern used elsewhere in this file.
        std::transform(key.begin(), key.end(), key.begin(),
                       [](char c) { return (char)std::tolower((unsigned char)c); });
        if (key == "set-cookie")
            out.cookies->elements.push_back(Value(val));
        out.headers->entries[Value(key)] = Value(val);
    }
    return out;
}

// Does the response declare Transfer-Encoding: chunked? Exact-token
// match on the comma-separated list. Shared by parseHttpResponse and
// detectStreamMode so the two engines stay in lockstep.
static bool responseIsChunked(const std::shared_ptr<PraiaMap>& headers) {
    auto teIt = headers->entries.find(Value(std::string("transfer-encoding")));
    if (teIt == headers->entries.end() || !teIt->second.isString()) return false;
    std::string te = teIt->second.asString();
    std::transform(te.begin(), te.end(), te.begin(),
                   [](char c) { return (char)std::tolower((unsigned char)c); });
    size_t pos = 0;
    while (pos < te.size()) {
        size_t comma = te.find(',', pos);
        size_t end = (comma == std::string::npos) ? te.size() : comma;
        size_t s = pos;
        while (s < end && (te[s] == ' ' || te[s] == '\t')) s++;
        size_t e = end;
        while (e > s && (te[e - 1] == ' ' || te[e - 1] == '\t')) e--;
        if (e - s == 7 && te.compare(s, 7, "chunked") == 0) return true;
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return false;
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
                if (std::tolower((unsigned char)key[i]) !=
                    std::tolower((unsigned char)name[i])) { match = false; break; }
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
            // Cast through unsigned char — passing a negative char to
            // std::tolower is UB. Matches the lambda pattern used at
            // the other corrected sites in this file.
            std::transform(key.begin(), key.end(), key.begin(),
                           [](char ch) { return (char)std::tolower((unsigned char)ch); });
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

// Forward declaration: defined below the streaming helpers (HttpStreamState
// + detectStreamMode + streamEnsure) because it leans on them. Reads the
// full response off `conn` using the framed streaming decoder rather than
// blocking on EOF, then returns parsed headers and the decoded body.
// `method` is the request method — HEAD responses get a zero-byte body
// regardless of any framing headers (see detectStreamMode).
static std::pair<ResponseHeaderFields, std::string>
readFramedResponse(SocketConn& conn, const std::shared_ptr<SocketConn>& connPtr,
                   const std::string& method);

// Forward declaration: is the response framed in a way that lets us
// safely return its socket to a keepalive pool? Defined alongside
// detectStreamMode so both share the same framing-mode classification.
static bool responseAllowsKeepalive(const ResponseHeaderFields& fields,
                                    const std::string& method);

// ── Public API ───────────────────────────────────────────────

// RFC 3986 §5.2.4 remove_dot_segments. Operates on an input path,
// stripping "." / ".." segments to canonicalize. Pure-string algorithm
// straight from the RFC pseudocode — easy to audit against the spec.
static std::string removeDotSegments(std::string in) {
    std::string out;
    while (!in.empty()) {
        if (in.compare(0, 3, "../") == 0)        in.erase(0, 3);
        else if (in.compare(0, 2, "./") == 0)    in.erase(0, 2);
        else if (in.compare(0, 3, "/./") == 0)   in.replace(0, 3, "/");
        else if (in == "/.")                     in = "/";
        else if (in.compare(0, 4, "/../") == 0) {
            in.replace(0, 4, "/");
            auto slash = out.rfind('/');
            if (slash != std::string::npos) out.resize(slash);
            else out.clear();
        }
        else if (in == "/..") {
            in = "/";
            auto slash = out.rfind('/');
            if (slash != std::string::npos) out.resize(slash);
            else out.clear();
        }
        else if (in == "." || in == "..") {
            in.clear();
        }
        else {
            // Move first segment (including leading '/' if any) to output.
            size_t end = in.find('/', in[0] == '/' ? 1 : 0);
            if (end == std::string::npos) { out += in; in.clear(); }
            else { out.append(in, 0, end); in.erase(0, end); }
        }
    }
    return out;
}

// RFC 3986 §5.2.3 merge — for a relative reference whose authority is
// undefined and path is not empty. When the base has an authority and
// an empty path, treat as "/"; otherwise drop everything after the last
// '/' in the base path and append the relative path.
static std::string mergePaths(const praia::url::ParsedUrl& base,
                              const std::string& refPath) {
    bool baseHasAuthority = !base.host.empty();
    if (baseHasAuthority && base.path.empty()) return "/" + refPath;
    auto slash = base.path.rfind('/');
    if (slash == std::string::npos) return refPath;
    return base.path.substr(0, slash + 1) + refPath;
}

// Resolve a redirect Location header against the base URL.
//   - absolute URL (has scheme)            → use as-is
//   - protocol-relative (//host/path)      → prepend base scheme
//   - absolute path (/foo)                 → keep base scheme/host/port
//   - relative path (foo, ../bar)          → RFC 3986 §5.2 reference
//                                            resolution against base path
//   - empty                                → throw
static std::string resolveLocation(const std::string& baseUrl,
                                   const std::string& location) {
    if (location.empty())
        throw RuntimeError("HTTP redirect: empty Location header", 0);

    praia::url::ParsedUrl loc;
    try {
        loc = praia::url::parse(location);
    } catch (const praia::url::UrlParseError& e) {
        throw RuntimeError(std::string("HTTP redirect: invalid Location: ") + e.what(), 0);
    }
    if (!loc.scheme.empty()) {
        return location;  // already absolute
    }

    praia::url::ParsedUrl base;
    try {
        base = praia::url::parse(baseUrl);
    } catch (const praia::url::UrlParseError& e) {
        throw RuntimeError(std::string("HTTP redirect: invalid base URL: ") + e.what(), 0);
    }

    // Protocol-relative — //host[:port]/path
    if (!loc.host.empty()) {
        std::string out = base.scheme + "://";
        if (loc.hostIsIPv6) out += "[" + loc.host + "]";
        else                out += loc.host;
        if (loc.hasPort) out += ":" + std::to_string(loc.port);
        out += loc.path.empty() ? "/" : loc.path;
        if (!loc.query.empty())    out += "?" + loc.query;
        if (!loc.fragment.empty()) out += "#" + loc.fragment;
        return out;
    }

    // Same-authority. Path may be absolute ("/x") or relative
    // ("final" / "../bar"); RFC 3986 §5.2.2 + §5.2.4 handle both via
    // merge + remove_dot_segments. If the reference path is empty,
    // inherit the base path (and query, if the reference has none).
    std::string targetPath;
    std::string targetQuery;
    if (loc.path.empty()) {
        targetPath  = base.path;
        targetQuery = loc.query.empty() ? base.query : loc.query;
    } else if (loc.path[0] == '/') {
        targetPath  = removeDotSegments(loc.path);
        targetQuery = loc.query;
    } else {
        targetPath  = removeDotSegments(mergePaths(base, loc.path));
        targetQuery = loc.query;
    }

    std::string out = base.scheme + "://";
    if (base.hostIsIPv6) out += "[" + base.host + "]";
    else                 out += base.host;
    if (base.hasPort) {
        bool isDefault = (base.scheme == "http"  && base.port == 80) ||
                         (base.scheme == "https" && base.port == 443);
        if (!isDefault) out += ":" + std::to_string(base.port);
    }
    out += targetPath;
    if (!targetQuery.empty())  out += "?" + targetQuery;
    if (!loc.fragment.empty()) out += "#" + loc.fragment;
    return out;
}

// Validate the parts of a request that are about to hit the wire. Runs
// BEFORE any DNS / TCP / TLS work so garbage input fails fast and with
// a useful error rather than after a 30-second handshake timeout.
//
// Also rejects caller-supplied versions of headers the client manages
// itself (Host, Connection, Content-Length, Transfer-Encoding, Upgrade).
// doOneHttpRequestImpl always emits these from its own computed values;
// a duplicate caller version would either be ignored, conflict, or
// produce header-smuggling-class behavior on lenient servers.
static void validateRequestParts(
    const std::string& method, const ParsedUrl& p,
    const std::unordered_map<std::string, std::string>& headers) {
    for (char c : method) if (c == '\r' || c == '\n' || c == '\0')
        throw RuntimeError("Invalid HTTP method: contains CR/LF/NUL", 0);
    for (char c : p.path) if (c == '\r' || c == '\n' || c == '\0')
        throw RuntimeError("Invalid URL path: contains CR/LF/NUL", 0);
    for (char c : p.host) if (c == '\r' || c == '\n' || c == '\0')
        throw RuntimeError("Invalid host: contains CR/LF/NUL", 0);

    auto eqIgnoreCase = [](const std::string& a, const char* b) {
        size_t n = std::strlen(b);
        if (a.size() != n) return false;
        for (size_t i = 0; i < n; i++) {
            char lhs = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
            char rhs = static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
            if (lhs != rhs) return false;
        }
        return true;
    };
    static const char* kReservedHeaders[] = {
        "host", "connection", "content-length", "transfer-encoding", "upgrade",
    };

    for (auto& [k, v] : headers) {
        if (!headerFieldSafe(k, v))
            throw RuntimeError("Invalid request header \"" + k +
                               "\": contains CR/LF/NUL (header injection)", 0);
        for (const char* reserved : kReservedHeaders) {
            if (eqIgnoreCase(k, reserved))
                throw RuntimeError("Header \"" + k +
                    "\" is reserved (set by the http client). "
                    "Remove it from your headers map.", 0);
        }
    }
}

// Open a TCP socket + (for https) run the TLS handshake. Pulled out
// of doOneHttpRequest so the keep-alive session path can call it
// directly when its pool has no idle connection for the target host.
// On any failure the conn's resources are cleaned up before throwing.
static void openSocketAndTls(SocketConn& conn, const ParsedUrl& p,
                             const HttpOptions& opts) {
    conn.fd = connectToHost(p.host, p.port, opts.connectTimeoutMs);
    // After connect, apply the read/write timeout to the socket so
    // subsequent recv()/send() calls (and the SSL_read/SSL_write
    // wrappers around them) bound their wait.
    setSocketTimeouts(conn.fd, opts.readTimeoutMs);

#ifdef HAVE_OPENSSL
    if (p.tls) {
        ensureSSLInit();
        conn.ctx = SSL_CTX_new(TLS_client_method());
        if (!conn.ctx) { conn.shutdown_close(); throw RuntimeError("Failed to create SSL context", 0); }

        // Trust store: custom CA bundle path takes priority over
        // system defaults. Both can be loaded simultaneously without
        // harm (OpenSSL accumulates trust anchors).
        if (!opts.caBundle.empty()) {
            if (SSL_CTX_load_verify_locations(conn.ctx, opts.caBundle.c_str(), nullptr) != 1) {
                conn.shutdown_close();
                throw RuntimeError("Cannot load CA bundle: " + opts.caBundle, 0);
            }
        } else {
            SSL_CTX_set_default_verify_paths(conn.ctx);
        }

        // Verification flag — `insecure` is the testing/dev escape
        // hatch. We still go through the handshake and learn the
        // peer's identity; we just don't reject on cert problems.
        // This is the SAME knob curl -k / Python verify=False expose
        // and carries the same warnings.
        SSL_CTX_set_verify(conn.ctx,
                           opts.insecure ? SSL_VERIFY_NONE : SSL_VERIFY_PEER,
                           nullptr);

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
        //
        // Skip this when insecure — pointless to do hostname matching
        // if we're not verifying the cert chain anyway.
        if (!opts.insecure) {
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
            if (!opts.insecure && vr != X509_V_OK) {
                throw RuntimeError("SSL certificate verification failed for " + p.host +
                                   ": " + X509_verify_cert_error_string(vr), 0);
            }
            throw RuntimeError("SSL handshake failed for " + p.host, 0);
        }
        if (!opts.insecure) {
            long vr = SSL_get_verify_result(conn.ssl);
            if (vr != X509_V_OK) {
                conn.shutdown_close();
                throw RuntimeError("SSL certificate verification failed for " + p.host +
                                   ": " + X509_verify_cert_error_string(vr), 0);
            }
        }
    }
#endif
}

// File-local sentinel: thrown by doOneHttpRequestImpl when the request
// fails in a way that *could* be the server having closed the keepalive
// connection between when we last checked it and our send/recv. The
// session wrapper catches this and retries once on a fresh socket; the
// stateless wrapper never sees it (always passes isFreshSocket=true).
struct HttpRetryOnFresh {};

// RFC 7231 §4.2.2 — request methods whose intended effect is the same
// whether the request fires once or many times. The retry path uses
// this to decide whether replaying a request on a fresh socket is safe
// when an error fires AFTER the server may have already processed it.
// Non-idempotent methods (POST, PATCH, ...) are still retried when no
// response bytes were observed — that's a different signal entirely
// (the server can't have reacted yet).
static bool isIdempotentMethod(const std::string& m) {
    return m == "GET" || m == "HEAD" || m == "OPTIONS" ||
           m == "PUT" || m == "TRACE";
}

struct OneRequestResult {
    Value response;
    bool keepalive;   // true if the socket may be returned to a pool
};

// Drive one HTTP request on an already-open SocketConn. The caller
// owns the socket; this function NEVER calls shutdown_close itself
// — it only signals via return value (keepalive viability) and via
// exceptions (caller's job to close on throw).
//
// `wantKeepalive=false` mirrors the legacy behavior: emit
// "Connection: close", always returns keepalive=false. `wantKeepalive=true`
// is the session path: emit "Connection: keep-alive", and the returned
// flag reflects whether the framing + headers allow reuse.
static OneRequestResult doOneHttpRequestImpl(
    SocketConn& conn, bool isFreshSocket,
    const std::string& method, const ParsedUrl& p,
    const std::string& body,
    const std::unordered_map<std::string, std::string>& extraHeaders,
    bool wantKeepalive) {

    // Serialize straight to the wire — header validation already ran in
    // the caller (validateRequestParts) before any socket I/O.
    std::string req = method + " " + p.path + " HTTP/1.1\r\n";
    req += "Host: " + p.hostHeader + "\r\n";
    req += wantKeepalive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    for (auto& [k, v] : extraHeaders) req += k + ": " + v + "\r\n";
    if (!body.empty() && extraHeaders.find("Content-Type") == extraHeaders.end())
        req += "Content-Type: text/plain\r\n";
    if (!body.empty())
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;

    if (conn.write(req.c_str(), req.size()) < 0) {
        // Reused-socket send failure is the classic stale-conn signature:
        // peer closed between the pool's last probe and our write. Signal
        // the wrapper to retry once on a fresh socket. Fresh-socket writes
        // always surface as the original error.
        if (!isFreshSocket) throw HttpRetryOnFresh{};
        throw RuntimeError("Failed to send HTTP request", 0);
    }

    // shared_ptr-with-noop-deleter so readFramedResponse can take its
    // shared ownership without forcing a heap allocation. The caller
    // still owns the SocketConn's lifetime.
    std::shared_ptr<SocketConn> connPtr(&conn, [](SocketConn*){});
    ResponseHeaderFields fields;
    std::string respBody;
    // SocketConn::bytesRead is cumulative; capture its value before the
    // read so the catch block can tell whether the failure happened
    // before any response bytes arrived (= server didn't react) vs
    // after (= server may have started a response). The distinction
    // gates whether a non-idempotent retry is safe.
    size_t bytesBefore = conn.bytesRead;
    try {
        auto pair = readFramedResponse(conn, connPtr, method);
        fields   = std::move(pair.first);
        respBody = std::move(pair.second);
    } catch (...) {
        bool responseStarted = conn.bytesRead > bytesBefore;
        // Retry on a reused socket only when replay is safe:
        //   - method is idempotent → replaying changes nothing, OR
        //   - no response bytes were observed → server can't have reacted
        // Otherwise propagate; the caller (often a POST/PATCH) must not
        // be replayed when the server might have already committed.
        if (!isFreshSocket && (isIdempotentMethod(method) || !responseStarted))
            throw HttpRetryOnFresh{};
        throw;
    }

    // Keepalive viability is only computed when the caller asked for it.
    bool keepalive = wantKeepalive && responseAllowsKeepalive(fields, method);

    auto result = gcNew<PraiaMap>();
    result->entries[Value("status")]  = Value(static_cast<double>(fields.status));
    result->entries[Value("body")]    = Value(std::move(respBody));
    result->entries[Value("headers")] = Value(fields.headers);
    result->entries[Value("cookies")] = Value(fields.cookies);
    return {Value(result), keepalive};
}

// Single HTTP request — one connect, one send, one parse. No redirect
// handling; that's the caller's loop. Stateless: opens a fresh socket
// per call, sends Connection: close, shuts the socket down on exit
// regardless of outcome.
static Value doOneHttpRequest(const std::string& method, const std::string& url,
                              const std::string& body,
                              const std::unordered_map<std::string, std::string>& extraHeaders,
                              const HttpOptions& opts) {
    auto p = parseUrl(url);
    validateRequestParts(method, p, extraHeaders);

    SocketConn conn;
    openSocketAndTls(conn, p, opts);

    // RAII close — non-session path always shuts down on exit. Mirrors
    // the original behavior; doOneHttpRequestImpl never closes itself.
    struct ConnCloseGuard {
        SocketConn& c;
        ~ConnCloseGuard() { c.shutdown_close(); }
    } close_guard{conn};

    auto r = doOneHttpRequestImpl(conn, /*isFreshSocket=*/true,
                                  method, p, body, extraHeaders,
                                  /*wantKeepalive=*/false);
    return r.response;
}

// ── HTTP session: long-lived keepalive pool + default opts/headers ─

// File-local session state. The struct stays here (rather than in
// builtins.h) so it can hold the file-private SocketConn type and so
// interpreter_setup.cpp sees only opaque accessors.
struct HttpSession {
    // Idle keep-alive sockets, one per "scheme://host:port". A more
    // sophisticated pool (per-host queue, LRU, idle timeout) is a
    // documented follow-up; v1 is intentionally minimal.
    std::unordered_map<std::string, std::unique_ptr<SocketConn>> idle;
    HttpOptions defaultOpts;
    std::unordered_map<std::string, std::string> defaultHeaders;
    bool closed = false;
};
static constexpr const char* kHttpSessionTypeTag = "http.session";

// Pool key. scheme/host/port locates the peer; insecure + caBundle
// segregate connections by TLS-handshake identity so a per-call override
// like `{insecure: true}` can't accidentally pick up a strictly-verified
// pooled socket (or vice versa). Default values (insecure=false, empty
// caBundle) keep the key shape unchanged for the common case.
static std::string sessionPoolKey(const ParsedUrl& p, const HttpOptions& opts) {
    std::string key = (p.tls ? "https://" : "http://") +
                      p.host + ":" + std::to_string(p.port);
    if (p.tls) {
        // Suffix only for TLS where the knobs matter. The caBundle path
        // identifies a distinct trust store; we don't hash its contents
        // (the file may rotate without invalidating in-flight sessions —
        // hashing would defeat the cache more often than it'd help).
        key += "|insecure=";
        key += opts.insecure ? "1" : "0";
        key += "|ca=";
        key += opts.caBundle;   // empty string when system defaults
    }
    return key;
}

// Take an idle conn for `key` out of the pool, probing it for staleness
// before returning. A zero-timeout poll() catches the easy case where
// the peer closed since we stored the socket; the subtler case where
// the peer closes between this probe and the next send is handled by
// the once-and-only-once retry in httpSessionRequest.
static std::unique_ptr<SocketConn> sessionTakeIdle(HttpSession& s,
                                                   const std::string& key) {
    auto it = s.idle.find(key);
    if (it == s.idle.end()) return nullptr;
    auto conn = std::move(it->second);
    s.idle.erase(it);

    struct pollfd pfd;
    pfd.fd = conn->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int r;
    do { r = poll(&pfd, 1, 0); } while (r < 0 && errno == EINTR);
    if (r < 0) {
        conn->shutdown_close();
        return nullptr;
    }
    if (r > 0) {
        // POLLIN on an idle keep-alive socket means either the peer
        // closed (recv would return 0) or sent something unsolicited
        // (server-push, garbage). Both are reasons to discard — a
        // well-behaved server is silent on an idle conn.
        conn->shutdown_close();
        return nullptr;
    }
    return conn;
}

// Return a freshly-used conn to the idle pool. If there's already a
// stored conn for the same key, prefer the newer one — simpler than a
// queue and good enough for the dominant "talk to the same host
// repeatedly" workload that motivated sessions in the first place.
static void sessionReturnIdle(HttpSession& s, const std::string& key,
                              std::unique_ptr<SocketConn> conn) {
    auto it = s.idle.find(key);
    if (it != s.idle.end()) {
        it->second->shutdown_close();
        it->second = std::move(conn);
    } else {
        s.idle.emplace(key, std::move(conn));
    }
}

static HttpSession* sessionFromValue(const Value& v) {
    return praia::getExternal<HttpSession>(v, kHttpSessionTypeTag);
}

Value httpCreateSession(const HttpOptions& defaultOpts,
                        const std::unordered_map<std::string, std::string>& defaultHeaders) {
    auto* s = new HttpSession();
    s->defaultOpts = defaultOpts;
    s->defaultHeaders = defaultHeaders;
    return praia::makeExternal<HttpSession>(s, kHttpSessionTypeTag,
        [](HttpSession* p) {
            // Drain the pool — every stored fd / SSL_CTX gets released.
            for (auto& [_, conn] : p->idle) {
                if (conn) conn->shutdown_close();
            }
            p->idle.clear();
            delete p;
        });
}

bool httpIsSession(const Value& v) {
    if (!v.isExternal()) return false;
    return v.asExternal()->typeName == kHttpSessionTypeTag;
}

const HttpOptions& httpSessionGetDefaultOpts(const Value& session) {
    return sessionFromValue(session)->defaultOpts;
}

const std::unordered_map<std::string, std::string>&
httpSessionGetDefaultHeaders(const Value& session) {
    return sessionFromValue(session)->defaultHeaders;
}

void httpSessionClose(const Value& session) {
    auto* s = sessionFromValue(session);
    if (s->closed) return;
    for (auto& [_, conn] : s->idle) {
        if (conn) conn->shutdown_close();
    }
    s->idle.clear();
    s->closed = true;
}

// Drive one HTTP request through the session's connection pool. The
// caller has already layered session defaults under per-call values
// for both headers and HttpOptions, so the merge is invisible here.
//
// Retry policy: at most one retry on a reused socket. If the first
// attempt was on a reused conn and the impl signals HttpRetryOnFresh
// (send / recv failed in a way that could be peer-side closure), we
// open a fresh socket and try again. A fresh-socket failure always
// surfaces — no retry on what's likely a real network error.
static Value httpSessionRequestOne(HttpSession& s, const std::string& method,
                                   const std::string& url,
                                   const std::string& body,
                                   const std::unordered_map<std::string, std::string>& headers,
                                   const HttpOptions& opts) {
    auto p = parseUrl(url);
    validateRequestParts(method, p, headers);
    std::string key = sessionPoolKey(p, opts);

    for (int attempt = 0; attempt < 2; ++attempt) {
        auto conn = sessionTakeIdle(s, key);
        bool isFresh = !conn;
        if (!conn) {
            conn = std::make_unique<SocketConn>();
            openSocketAndTls(*conn, p, opts);
        } else {
            // Reused socket — refresh recv/send timeouts so per-call
            // readTimeoutMs overrides take effect on this attempt
            // instead of inheriting the value baked in when the conn
            // was first opened.
            setSocketTimeouts(conn->fd, opts.readTimeoutMs);
        }
        try {
            auto r = doOneHttpRequestImpl(*conn, isFresh,
                                          method, p, body, headers,
                                          /*wantKeepalive=*/true);
            if (r.keepalive) sessionReturnIdle(s, key, std::move(conn));
            else conn->shutdown_close();
            return r.response;
        } catch (const HttpRetryOnFresh&) {
            // Reused socket failure — discard and let the loop retry on
            // a fresh socket. (isFresh==true never throws this.)
            conn->shutdown_close();
        } catch (...) {
            // Any other failure mid-request: close the conn, propagate.
            // We never return a half-used socket to the pool.
            conn->shutdown_close();
            throw;
        }
    }
    // Unreachable: HttpRetryOnFresh only fires when isFresh==false, and
    // the second iteration always uses isFresh==true.
    throw RuntimeError("http session: retry exhausted", 0);
}

Value httpSessionRequest(const Value& session,
                         const std::string& method,
                         const std::string& url,
                         const std::string& body,
                         const std::unordered_map<std::string, std::string>& headers,
                         const HttpOptions& opts) {
    auto* s = sessionFromValue(session);
    if (s->closed) throw RuntimeError("http session is closed", 0);

    // Mirror doHttpRequest's redirect loop. Cross-host redirects pick
    // up a different pool key automatically; same-host redirects reuse
    // the existing idle conn.
    auto deadline = (opts.totalTimeoutMs > 0)
        ? std::chrono::steady_clock::now() +
          std::chrono::milliseconds(opts.totalTimeoutMs)
        : std::chrono::steady_clock::time_point::max();

    std::string currentUrl    = url;
    std::string currentMethod = method;
    std::string currentBody   = body;
    auto        currentHeaders = headers;
    int         redirectCount = 0;

    while (true) {
        HttpOptions perReq = opts;
        if (opts.totalTimeoutMs > 0) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0)
                throw RuntimeError("HTTP total timeout exceeded", 0);
            int remMs = static_cast<int>(remaining);
            if (perReq.connectTimeoutMs <= 0 || perReq.connectTimeoutMs > remMs)
                perReq.connectTimeoutMs = remMs;
            if (perReq.readTimeoutMs    <= 0 || perReq.readTimeoutMs    > remMs)
                perReq.readTimeoutMs    = remMs;
        }

        Value response = httpSessionRequestOne(
            *s, currentMethod, currentUrl, currentBody, currentHeaders, perReq);

        if (!opts.followRedirects) return response;

        auto& respMap = response.asMap()->entries;
        auto statusIt = respMap.find(Value("status"));
        if (statusIt == respMap.end() || !statusIt->second.isNumber()) return response;
        int status = static_cast<int>(statusIt->second.asNumber());
        if (status < 300 || status >= 400) return response;

        auto headersIt = respMap.find(Value("headers"));
        if (headersIt == respMap.end() || !headersIt->second.isMap()) return response;
        auto& respHeaders = headersIt->second.asMap()->entries;
        auto locIt = respHeaders.find(Value("location"));
        if (locIt == respHeaders.end() || !locIt->second.isString()) return response;
        std::string location = locIt->second.asString();

        if (redirectCount >= opts.maxRedirects)
            throw RuntimeError("HTTP redirect limit exceeded (" +
                               std::to_string(opts.maxRedirects) + ")", 0);

        std::string newUrl = resolveLocation(currentUrl, location);

        bool curIsTls = parseUrl(currentUrl).tls;
        bool newIsTls = parseUrl(newUrl).tls;
        if (curIsTls && !newIsTls)
            throw RuntimeError("HTTP redirect: refusing https→http downgrade to " + newUrl, 0);

        if (status == 303 ||
            ((status == 301 || status == 302) &&
              currentMethod != "GET" && currentMethod != "HEAD")) {
            currentMethod = "GET";
            currentBody.clear();
            currentHeaders.erase("Content-Length");
            currentHeaders.erase("Content-Type");
        }
        currentUrl = newUrl;
        ++redirectCount;
    }
}

Value doHttpRequest(const std::string& method, const std::string& url,
                    const std::string& body,
                    const std::unordered_map<std::string, std::string>& extraHeaders,
                    const HttpOptions& opts) {
    auto deadline = (opts.totalTimeoutMs > 0)
        ? std::chrono::steady_clock::now() +
          std::chrono::milliseconds(opts.totalTimeoutMs)
        : std::chrono::steady_clock::time_point::max();

    std::string currentUrl     = url;
    std::string currentMethod  = method;
    std::string currentBody    = body;
    auto        currentHeaders = extraHeaders;
    int         redirectCount  = 0;

    while (true) {
        // Cap per-request timeouts at the remaining total budget so a
        // pathological mid-redirect server can't outlast the deadline.
        HttpOptions perReq = opts;
        if (opts.totalTimeoutMs > 0) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0)
                throw RuntimeError("HTTP total timeout exceeded", 0);
            int remMs = static_cast<int>(remaining);
            if (perReq.connectTimeoutMs <= 0 || perReq.connectTimeoutMs > remMs)
                perReq.connectTimeoutMs = remMs;
            if (perReq.readTimeoutMs    <= 0 || perReq.readTimeoutMs    > remMs)
                perReq.readTimeoutMs    = remMs;
        }

        Value response = doOneHttpRequest(currentMethod, currentUrl,
                                          currentBody, currentHeaders, perReq);

        if (!opts.followRedirects) return response;

        // Status check — only 3xx with a Location triggers a follow.
        // The response shape is {status: number, headers: map, body: string}.
        auto& respMap = response.asMap()->entries;
        auto statusIt = respMap.find(Value("status"));
        if (statusIt == respMap.end() || !statusIt->second.isNumber()) return response;
        int status = static_cast<int>(statusIt->second.asNumber());
        if (status < 300 || status >= 400) return response;

        auto headersIt = respMap.find(Value("headers"));
        if (headersIt == respMap.end() || !headersIt->second.isMap()) return response;
        auto& respHeaders = headersIt->second.asMap()->entries;
        auto locIt = respHeaders.find(Value("location"));
        if (locIt == respHeaders.end() || !locIt->second.isString()) return response;
        std::string location = locIt->second.asString();

        if (redirectCount >= opts.maxRedirects)
            throw RuntimeError("HTTP redirect limit exceeded (" +
                               std::to_string(opts.maxRedirects) + ")", 0);

        std::string newUrl = resolveLocation(currentUrl, location);

        // Security: refuse a downgrade from https to http. Catches
        // open-redirect attacks that try to leak credentials/cookies
        // over plaintext, and matches curl --proto-redir behavior.
        bool curIsTls = parseUrl(currentUrl).tls;
        bool newIsTls = parseUrl(newUrl).tls;
        if (curIsTls && !newIsTls)
            throw RuntimeError("HTTP redirect: refusing https→http downgrade to " + newUrl, 0);

        // Method/body transformation per RFC 7231:
        //   303 → always GET (this is the explicit "make a GET" code)
        //   301/302 → de-facto "switch to GET if it was a POST" per
        //             every browser and HTTP library since ~1995.
        //             Strict RFC 7231 says preserve, but the world
        //             didn't read that.
        //   307/308 → preserve method AND body (these codes exist
        //             precisely to fix the 301/302 ambiguity)
        if (status == 303 ||
            ((status == 301 || status == 302) &&
              currentMethod != "GET" && currentMethod != "HEAD")) {
            currentMethod = "GET";
            currentBody.clear();
            currentHeaders.erase("Content-Length");
            currentHeaders.erase("Content-Type");
            // (Transfer-Encoding would also need clearing for chunked
            //  bodies, but we don't currently emit chunked.)
        }
        currentUrl = newUrl;
        ++redirectCount;
    }
}

// ── Streaming response reader ─────────────────────────────────
//
// Reads the request out the door like doOneHttpRequest, but stops
// after parsing response headers — the body comes out incrementally
// through the returned handle's .read(n) / .readLine() / .readAll().
//
// Three body framings are handled transparently:
//   - Content-Length: read exactly N bytes, then EOF.
//   - Transfer-Encoding: chunked: parse HEXSIZE\r\n<data>\r\n
//     repeatedly until the 0\r\n terminator + (ignored) trailers.
//   - Neither header present: read until the server closes the
//     connection (HTTP/1.0 style; HTTP/1.1 default with
//     Connection: close).
//
// The state is held in HttpStreamState (in a shared_ptr that the
// handle's method lambdas all capture), so multiple methods on the
// same handle share the same socket and decode position.

struct HttpStreamState {
    std::shared_ptr<SocketConn> conn;
    enum class Mode { ContentLength, Chunked, Close };
    Mode mode = Mode::Close;
    int64_t contentRemaining = -1;
    int64_t chunkRemaining = 0;
    bool chunkedDone = false;
    bool eofReached = false;
    bool closed = false;

    // Bytes ready to hand to the caller (already de-chunked if
    // applicable). bufPos is how many have been delivered.
    std::string buf;
    size_t bufPos = 0;

    // Scratch buffer for socket bytes that haven't yet been parsed
    // (chunk headers in particular accumulate here until a CRLF
    // makes them complete). Distinct from `buf` because chunk
    // headers don't get delivered to the caller as body bytes.
    std::string scratch;
    size_t scratchPos = 0;

    // Status / headers / cookies populated once at open time; the
    // handle map exposes these as plain fields, no lazy fetch.
    int status = 0;
    std::shared_ptr<PraiaMap>  headersMap;
    std::shared_ptr<PraiaArray> cookiesArr;

    // RAII fallback: if the caller drops the stream handle without
    // calling .close(), the handle's closures destruct, the state's
    // last shared_ptr ref drops, and this destructor runs — releasing
    // the underlying socket / SSL state through shutdown_close.
    // shutdown_close is idempotent, so the explicit .close() handler
    // (line ~1670) and this destructor coexist safely. Scoped to
    // HttpStreamState rather than SocketConn so the rest of the
    // request path (which uses stack-local SocketConns and various
    // throw-and-explicit-close patterns) isn't affected.
    ~HttpStreamState() { if (conn) conn->shutdown_close(); }
};

// Pull one chunk of bytes from the socket into `out`. Returns the
// number appended; 0 = clean EOF; -1 = EINTR (retry). Throws on
// timeout / hard error so the caller doesn't have to special-case
// errno.
static ssize_t streamRecv(SocketConn& conn, std::string& out, size_t maxBytes) {
    char tmp[8192];
    if (maxBytes > sizeof(tmp)) maxBytes = sizeof(tmp);
    errno = 0;
    ssize_t n = conn.read(tmp, maxBytes);
    if (n > 0) { out.append(tmp, n); return n; }
    if (n == 0) return 0;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
        throw RuntimeError("HTTP stream: read timed out", 0);
    if (errno == EINTR) return -1;
    throw RuntimeError("HTTP stream: read error: " +
                       std::string(errno ? std::strerror(errno) : "connection broken"), 0);
}

// Read one CRLF-terminated line out of the scratch buffer, refilling
// from the socket as needed. Returns the line WITHOUT the CRLF.
// Throws if the socket closes mid-line (chunked framing requires
// well-formed line endings).
static std::string streamReadSocketLine(HttpStreamState& s) {
    while (true) {
        size_t crlf = s.scratch.find("\r\n", s.scratchPos);
        if (crlf != std::string::npos) {
            std::string line = s.scratch.substr(s.scratchPos, crlf - s.scratchPos);
            s.scratchPos = crlf + 2;
            // Compact periodically so a long-running stream doesn't
            // pin unbounded memory in the prefix of `scratch`.
            if (s.scratchPos > 4096) {
                s.scratch.erase(0, s.scratchPos);
                s.scratchPos = 0;
            }
            return line;
        }
        ssize_t n;
        do { n = streamRecv(*s.conn, s.scratch, 4096); } while (n == -1);
        if (n == 0)
            throw RuntimeError("HTTP stream: unexpected EOF parsing chunk framing", 0);
    }
}

// Read exactly `count` bytes from the wire into `out`. Used for the
// data section of a chunk (we know its length up front from the
// chunk header).
static void streamReadExact(HttpStreamState& s, std::string& out, size_t count) {
    while (count > 0) {
        size_t avail = s.scratch.size() - s.scratchPos;
        if (avail > 0) {
            size_t take = std::min(avail, count);
            out.append(s.scratch.data() + s.scratchPos, take);
            s.scratchPos += take;
            count -= take;
            if (count == 0) {
                if (s.scratchPos > 4096) {
                    s.scratch.erase(0, s.scratchPos);
                    s.scratchPos = 0;
                }
                return;
            }
        }
        ssize_t n;
        do { n = streamRecv(*s.conn, s.scratch, std::max<size_t>(4096, count)); }
        while (n == -1);
        if (n == 0)
            throw RuntimeError("HTTP stream: unexpected EOF in chunk body", 0);
    }
}

// Ensure at least one delivered byte sits in `buf` past `bufPos`,
// or set eofReached if the body is exhausted. The framing-specific
// logic lives here so the public read/readLine/readAll methods can
// stay framing-agnostic.
static void streamEnsure(HttpStreamState& s) {
    if (s.bufPos < s.buf.size()) return;
    s.buf.clear();
    s.bufPos = 0;
    if (s.eofReached) return;

    if (s.mode == HttpStreamState::Mode::ContentLength) {
        if (s.contentRemaining <= 0) { s.eofReached = true; return; }
        size_t want = std::min<int64_t>(s.contentRemaining, 8192);
        size_t avail = s.scratch.size() - s.scratchPos;
        if (avail > 0) {
            size_t take = std::min(avail, want);
            s.buf.append(s.scratch.data() + s.scratchPos, take);
            s.scratchPos += take;
            s.contentRemaining -= take;
            if (s.scratchPos > 4096) {
                s.scratch.erase(0, s.scratchPos);
                s.scratchPos = 0;
            }
            return;
        }
        ssize_t n;
        do { n = streamRecv(*s.conn, s.buf, want); } while (n == -1);
        if (n == 0) { s.eofReached = true; return; }
        s.contentRemaining -= n;
        if (s.contentRemaining < 0) s.contentRemaining = 0;
        return;
    }

    if (s.mode == HttpStreamState::Mode::Chunked) {
        while (s.buf.empty() && !s.chunkedDone) {
            if (s.chunkRemaining == 0) {
                std::string line = streamReadSocketLine(s);
                // Chunk extensions follow a ';'; we ignore them per
                // RFC 7230 §4.1.1 (they're rare and informational).
                size_t semi = line.find(';');
                if (semi != std::string::npos) line.resize(semi);
                while (!line.empty() &&
                       (line.back() == ' ' || line.back() == '\t'))
                    line.pop_back();
                if (line.empty())
                    throw RuntimeError("HTTP stream: empty chunk size line", 0);
                int64_t sz = 0;
                for (char c : line) {
                    int d;
                    if      (c >= '0' && c <= '9') d = c - '0';
                    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                    else throw RuntimeError(
                        std::string("HTTP stream: invalid hex in chunk size: '") +
                        c + "'", 0);
                    // Pre-shift overflow guard — see decodeChunkedBody.
                    if (sz > (INT64_MAX >> 4))
                        throw RuntimeError("HTTP stream: chunk size overflow", 0);
                    sz = (sz << 4) | d;
                }
                if (sz == 0) {
                    // Terminator chunk: consume trailers until empty
                    // line, then we're done. We don't expose trailers
                    // to the caller — they're rare and roughly
                    // headers-after-the-body semantics that callers
                    // who care about should be using a different API.
                    while (true) {
                        std::string trailer = streamReadSocketLine(s);
                        if (trailer.empty()) break;
                    }
                    s.chunkedDone = true;
                    s.eofReached = true;
                    return;
                }
                s.chunkRemaining = sz;
            }
            size_t want = std::min<int64_t>(s.chunkRemaining, 8192);
            streamReadExact(s, s.buf, want);
            s.chunkRemaining -= want;
            if (s.chunkRemaining == 0) {
                std::string trailer = streamReadSocketLine(s);
                if (!trailer.empty())
                    throw RuntimeError("HTTP stream: missing CRLF after chunk data", 0);
            }
        }
        if (s.chunkedDone && s.buf.empty()) s.eofReached = true;
        return;
    }

    // Mode::Close — read whatever the socket gives until it closes.
    size_t avail = s.scratch.size() - s.scratchPos;
    if (avail > 0) {
        s.buf.append(s.scratch.data() + s.scratchPos, avail);
        s.scratchPos = s.scratch.size();
        return;
    }
    ssize_t n;
    do { n = streamRecv(*s.conn, s.buf, 8192); } while (n == -1);
    if (n == 0) s.eofReached = true;
}

// Detect the body framing from the parsed response headers. Per RFC
// 7230 §3.3.3, Transfer-Encoding (when present) takes precedence
// over Content-Length. Anything without either is read-until-close.
//
// `method` is the request method (uppercased): HEAD responses MUST
// have no body regardless of any framing headers the server sets,
// and the same is true for 1xx / 204 / 205 / 304 status responses.
// We short-circuit those to a zero-length ContentLength mode so
// streamEnsure reports EOF immediately — otherwise a misbehaving
// keep-alive server could leave us blocked forever waiting for
// bytes that aren't coming. 205 Reset Content is included per
// RFC 9110 §15.3.6 ("it cannot contain content or trailers").
static void detectStreamMode(HttpStreamState& s, const std::string& method) {
    bool noBody = (method == "HEAD") ||
                  (s.status >= 100 && s.status < 200) ||
                  (s.status == 204) ||
                  (s.status == 205) ||
                  (s.status == 304);
    if (noBody) {
        s.mode = HttpStreamState::Mode::ContentLength;
        s.contentRemaining = 0;
        return;
    }
    if (responseIsChunked(s.headersMap)) {
        s.mode = HttpStreamState::Mode::Chunked;
        return;
    }
    auto& h = s.headersMap->entries;
    auto clIt = h.find(Value(std::string("content-length")));
    if (clIt != h.end() && clIt->second.isString()) {
        try {
            s.contentRemaining = std::stoll(clIt->second.asString());
            if (s.contentRemaining < 0)
                throw RuntimeError("HTTP stream: negative Content-Length", 0);
            s.mode = HttpStreamState::Mode::ContentLength;
            return;
        } catch (const std::invalid_argument&) {
            throw RuntimeError("HTTP stream: malformed Content-Length", 0);
        } catch (const std::out_of_range&) {
            throw RuntimeError("HTTP stream: Content-Length out of range", 0);
        }
    }
    s.mode = HttpStreamState::Mode::Close;
}

// Decide whether the socket that produced `fields` can be returned to a
// keepalive pool. Reuse needs two conditions:
//   1. The response was framed (Content-Length or chunked), not
//      read-until-EOF — Mode::Close means the server's only way to
//      signal end-of-body is to close the socket.
//   2. The server didn't include "close" as a Connection: token.
// The header may carry multiple tokens (e.g. "keep-alive, close");
// "close" anywhere in the value disables reuse.
static bool responseAllowsKeepalive(const ResponseHeaderFields& fields,
                                    const std::string& method) {
    HttpStreamState tmp;
    tmp.status     = fields.status;
    tmp.headersMap = fields.headers;
    detectStreamMode(tmp, method);
    if (tmp.mode == HttpStreamState::Mode::Close) return false;

    auto it = fields.headers->entries.find(Value(std::string("connection")));
    if (it != fields.headers->entries.end() && it->second.isString()) {
        std::string v = it->second.asString();
        std::transform(v.begin(), v.end(), v.begin(),
                       [](char c) { return (char)std::tolower((unsigned char)c); });
        if (v.find("close") != std::string::npos) return false;
    }
    return true;
}

// Read raw bytes from the connection until "\r\n\r\n" appears, then
// split into header text + body prefix. Throws on EOF before the
// terminator (header truncation).
static void readHeadersInto(SocketConn& conn, std::string& headerText,
                            std::string& bodyPrefix) {
    std::string acc;
    while (true) {
        size_t end = acc.find("\r\n\r\n");
        if (end != std::string::npos) {
            headerText = acc.substr(0, end + 4);
            bodyPrefix = acc.substr(end + 4);
            return;
        }
        ssize_t n;
        do { n = streamRecv(conn, acc, 8192); } while (n == -1);
        if (n == 0)
            throw RuntimeError("HTTP stream: EOF before response headers complete", 0);
        // Defense-in-depth: a server that floods headers indefinitely
        // would otherwise tie up memory. 1 MiB is hugely beyond any
        // legitimate HTTP header section.
        if (acc.size() > 1024 * 1024)
            throw RuntimeError("HTTP stream: response headers exceed 1 MiB", 0);
    }
}

// Read a full response off `conn` using the framed streaming decoder.
// Used by doOneHttpRequest (the non-streaming http.get / http.request
// path) so the body read terminates at Content-Length or the chunked
// 0-terminator rather than waiting for the peer to close. `connPtr`
// is the same connection wrapped in a shared_ptr — HttpStreamState
// expects shared ownership.
static std::pair<ResponseHeaderFields, std::string>
readFramedResponse(SocketConn& conn, const std::shared_ptr<SocketConn>& connPtr,
                   const std::string& method) {
    std::string headerText, bodyPrefix;
    readHeadersInto(conn, headerText, bodyPrefix);
    std::string headerSection = (headerText.size() >= 4)
        ? headerText.substr(0, headerText.size() - 4) : headerText;
    auto fields = parseResponseHeaders(headerSection);

    HttpStreamState state;
    state.conn       = connPtr;
    state.status     = fields.status;
    state.headersMap = fields.headers;
    state.cookiesArr = fields.cookies;
    state.scratch    = std::move(bodyPrefix);
    state.scratchPos = 0;
    detectStreamMode(state, method);

    std::string body;
    while (true) {
        streamEnsure(state);
        if (state.bufPos >= state.buf.size()) break;
        body.append(state.buf.data() + state.bufPos,
                    state.buf.size() - state.bufPos);
        state.bufPos = state.buf.size();
    }

    // Truncation guard: if the headers framed the body but the peer
    // closed before delivering it in full, the bytes we have are
    // unreliable — surface that rather than silently returning a
    // truncated payload. streamEnsure sets eofReached on a clean EOF
    // and leaves contentRemaining / chunkedDone intact so we can
    // distinguish "complete" from "cut short" here.
    if (state.mode == HttpStreamState::Mode::ContentLength &&
        state.contentRemaining > 0) {
        int64_t expected = static_cast<int64_t>(body.size()) + state.contentRemaining;
        throw RuntimeError("HTTP response truncated: Content-Length=" +
                           std::to_string(expected) + " but got " +
                           std::to_string(body.size()) + " bytes", 0);
    }
    if (state.mode == HttpStreamState::Mode::Chunked && !state.chunkedDone) {
        throw RuntimeError("HTTP response truncated: chunked body ended "
                           "before 0-size terminator chunk", 0);
    }

    // Release the conn from the local HttpStreamState so its destructor
    // doesn't shutdown_close the socket — the caller (doOneHttpRequest
    // for the stateless path, httpSessionRequestOne for the keepalive
    // path) owns the SocketConn's lifetime past this return. Pre-session
    // refactor this was harmless because the stateless path always
    // closed via its ConnCloseGuard; with keepalive the close here would
    // tear down a socket the session pool is about to store.
    state.conn.reset();
    return {std::move(fields), std::move(body)};
}

// Drain whatever body is on the wire for a redirect response. We
// don't want to leak it (or leave it half-consumed and trip up the
// next connection). The bytes themselves get dropped — only the
// 3xx + Location headers mattered.
static void drainRedirectBody(HttpStreamState& s) {
    while (!s.eofReached) {
        streamEnsure(s);
        s.bufPos = s.buf.size();
    }
}

Value httpOpenStream(const std::string& method, const std::string& url,
                     const std::string& body,
                     const std::unordered_map<std::string, std::string>& extraHeaders,
                     const HttpOptions& opts) {
    auto deadline = (opts.totalTimeoutMs > 0)
        ? std::chrono::steady_clock::now() +
          std::chrono::milliseconds(opts.totalTimeoutMs)
        : std::chrono::steady_clock::time_point::max();

    std::string currentUrl     = url;
    std::string currentMethod  = method;
    std::string currentBody    = body;
    auto        currentHeaders = extraHeaders;
    int         redirectCount  = 0;

    while (true) {
        HttpOptions perReq = opts;
        if (opts.totalTimeoutMs > 0) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0)
                throw RuntimeError("HTTP total timeout exceeded", 0);
            int remMs = static_cast<int>(remaining);
            if (perReq.connectTimeoutMs <= 0 || perReq.connectTimeoutMs > remMs)
                perReq.connectTimeoutMs = remMs;
            if (perReq.readTimeoutMs <= 0 || perReq.readTimeoutMs > remMs)
                perReq.readTimeoutMs = remMs;
        }

        auto p = parseUrl(currentUrl);
        for (char c : currentMethod) if (c == '\r' || c == '\n' || c == '\0')
            throw RuntimeError("Invalid HTTP method: contains CR/LF/NUL", 0);
        for (char c : p.path) if (c == '\r' || c == '\n' || c == '\0')
            throw RuntimeError("Invalid URL path: contains CR/LF/NUL", 0);
        for (char c : p.host) if (c == '\r' || c == '\n' || c == '\0')
            throw RuntimeError("Invalid host: contains CR/LF/NUL", 0);
        for (auto& [k, v] : currentHeaders) {
            if (!headerFieldSafe(k, v))
                throw RuntimeError("Invalid request header \"" + k +
                                   "\": contains CR/LF/NUL", 0);
        }

        auto conn = std::make_shared<SocketConn>();
        conn->fd = connectToHost(p.host, p.port, perReq.connectTimeoutMs);
        setSocketTimeouts(conn->fd, perReq.readTimeoutMs);

#ifdef HAVE_OPENSSL
        if (p.tls) {
            ensureSSLInit();
            conn->ctx = SSL_CTX_new(TLS_client_method());
            if (!conn->ctx) {
                conn->shutdown_close();
                throw RuntimeError("Failed to create SSL context", 0);
            }
            if (!perReq.caBundle.empty()) {
                if (SSL_CTX_load_verify_locations(conn->ctx, perReq.caBundle.c_str(), nullptr) != 1) {
                    conn->shutdown_close();
                    throw RuntimeError("Cannot load CA bundle: " + perReq.caBundle, 0);
                }
            } else {
                SSL_CTX_set_default_verify_paths(conn->ctx);
            }
            SSL_CTX_set_verify(conn->ctx,
                               perReq.insecure ? SSL_VERIFY_NONE : SSL_VERIFY_PEER,
                               nullptr);
            conn->ssl = SSL_new(conn->ctx);
            SSL_set_fd(conn->ssl, conn->fd);
            SSL_set_tlsext_host_name(conn->ssl, p.host.c_str());
            if (!perReq.insecure) {
                X509_VERIFY_PARAM* vp = SSL_get0_param(conn->ssl);
                unsigned char ipbuf[16];
                bool isIp = (inet_pton(AF_INET,  p.host.c_str(), ipbuf) == 1 ||
                             inet_pton(AF_INET6, p.host.c_str(), ipbuf) == 1);
                if (isIp) X509_VERIFY_PARAM_set1_ip_asc(vp, p.host.c_str());
                else {
                    X509_VERIFY_PARAM_set1_host(vp, p.host.c_str(), p.host.size());
                    X509_VERIFY_PARAM_set_hostflags(vp, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
                }
            }
            if (SSL_connect(conn->ssl) <= 0) {
                long vr = SSL_get_verify_result(conn->ssl);
                conn->shutdown_close();
                if (!perReq.insecure && vr != X509_V_OK) {
                    throw RuntimeError("SSL certificate verification failed for " + p.host +
                                       ": " + X509_verify_cert_error_string(vr), 0);
                }
                throw RuntimeError("SSL handshake failed for " + p.host, 0);
            }
            if (!perReq.insecure) {
                long vr = SSL_get_verify_result(conn->ssl);
                if (vr != X509_V_OK) {
                    conn->shutdown_close();
                    throw RuntimeError("SSL certificate verification failed for " + p.host +
                                       ": " + X509_verify_cert_error_string(vr), 0);
                }
            }
        }
#endif

        std::string req = currentMethod + " " + p.path + " HTTP/1.1\r\n";
        req += "Host: " + p.hostHeader + "\r\n";
        req += "Connection: close\r\n";
        for (auto& [k, v] : currentHeaders) req += k + ": " + v + "\r\n";
        if (!currentBody.empty() &&
            currentHeaders.find("Content-Type") == currentHeaders.end())
            req += "Content-Type: text/plain\r\n";
        if (!currentBody.empty())
            req += "Content-Length: " + std::to_string(currentBody.size()) + "\r\n";
        req += "\r\n" + currentBody;

        if (conn->write(req.c_str(), req.size()) < 0) {
            conn->shutdown_close();
            throw RuntimeError("Failed to send HTTP request", 0);
        }

        // RAII guard so any throw between here and the hand-off to
        // HttpStreamState (or to the explicit redirect-path close)
        // releases the socket. readHeadersInto / parseResponseHeaders /
        // detectStreamMode / drainRedirectBody all throw on malformed
        // responses; without this, those paths leaked the fd + SSL.
        // shutdown_close is idempotent, so explicit closes below don't
        // collide with the guard's destructor.
        struct ConnCloseGuard {
            std::shared_ptr<SocketConn> c;
            ~ConnCloseGuard() { if (c) c->shutdown_close(); }
        } close_guard{conn};

        std::string headerText, bodyPrefix;
        readHeadersInto(*conn, headerText, bodyPrefix);

        // Header-only parse — DO NOT call parseHttpResponse here. That
        // would re-attach bodyPrefix as the "body" and run the chunked
        // decoder over a possibly-truncated first chunk, throwing
        // "chunk data truncated" before we ever return the stream.
        // The streaming decoder (HttpStreamState) consumes the rest of
        // the body incrementally; the prefix is its first input.
        // readHeadersInto includes the trailing \r\n\r\n in headerText,
        // so slice it back off before parsing.
        std::string headerSection = (headerText.size() >= 4)
            ? headerText.substr(0, headerText.size() - 4)
            : headerText;
        auto parsedHeaders = parseResponseHeaders(headerSection);
        int status = parsedHeaders.status;

        // Redirect handling — fully drain the redirect's body via the
        // streaming path so the connection state is clean before we
        // open the next one.
        if (opts.followRedirects) {
            auto& respHeaders = parsedHeaders.headers->entries;
            if (status >= 300 && status < 400) {
                auto locIt = respHeaders.find(Value("location"));
                if (locIt != respHeaders.end() && locIt->second.isString()) {
                    if (redirectCount >= opts.maxRedirects) {
                        conn->shutdown_close();
                        throw RuntimeError("HTTP redirect limit exceeded (" +
                                           std::to_string(opts.maxRedirects) + ")", 0);
                    }
                    std::string newUrl = resolveLocation(currentUrl, locIt->second.asString());
                    bool curIsTls = parseUrl(currentUrl).tls;
                    bool newIsTls = parseUrl(newUrl).tls;
                    if (curIsTls && !newIsTls) {
                        conn->shutdown_close();
                        throw RuntimeError("HTTP redirect: refusing https→http downgrade to " +
                                           newUrl, 0);
                    }

                    // Drain whatever body this 3xx had, then close.
                    // drainRedirectBody can throw (e.g. truncated chunked
                    // framing on a misbehaving server); guarantee the
                    // socket is closed in both paths so we don't leak
                    // the fd/SSL state across the redirect-loop iteration.
                    HttpStreamState tmp;
                    tmp.conn = conn;
                    tmp.status = status;
                    tmp.headersMap = parsedHeaders.headers;
                    tmp.scratch = std::move(bodyPrefix);
                    tmp.scratchPos = 0;
                    detectStreamMode(tmp, currentMethod);
                    try {
                        drainRedirectBody(tmp);
                    } catch (...) {
                        conn->shutdown_close();
                        throw;
                    }
                    conn->shutdown_close();

                    if (status == 303 ||
                        ((status == 301 || status == 302) &&
                          currentMethod != "GET" && currentMethod != "HEAD")) {
                        currentMethod = "GET";
                        currentBody.clear();
                        currentHeaders.erase("Content-Length");
                        currentHeaders.erase("Content-Type");
                    }
                    currentUrl = newUrl;
                    ++redirectCount;
                    continue;
                }
            }
        }

        // Final response — build the stream handle. Ownership of conn
        // transfers to HttpStreamState; the stream handle's close()
        // method (or the state going out of scope) is responsible for
        // shutdown_close from here on. Release the guard so its
        // destructor doesn't double-close before the handle is even used.
        auto state = std::make_shared<HttpStreamState>();
        state->conn       = conn;
        state->status     = status;
        state->headersMap = parsedHeaders.headers;
        state->cookiesArr = parsedHeaders.cookies;
        state->scratch    = std::move(bodyPrefix);
        state->scratchPos = 0;
        detectStreamMode(*state, currentMethod);
        close_guard.c.reset();

        auto handle = gcNew<PraiaMap>();
        handle->entries[Value("status")]  = Value(static_cast<int64_t>(state->status));
        handle->entries[Value("headers")] = Value(state->headersMap);
        handle->entries[Value("cookies")] = Value(state->cookiesArr);

        handle->entries[Value("read")] = Value(makeNative("HttpStream.read", 1,
            [state](const std::vector<Value>& args) -> Value {
                if (state->closed) throw RuntimeError("HttpStream is closed", 0);
                if (!args[0].isNumber())
                    throw RuntimeError("HttpStream.read(n): n must be a number", 0);
                int64_t want = args[0].toInt64ForBitwise();
                if (want < 0) throw RuntimeError("HttpStream.read(n): n must be non-negative", 0);
                if (want == 0) return Value(std::string(""));
                std::string out;
                out.reserve(static_cast<size_t>(want));
                while (want > 0) {
                    streamEnsure(*state);
                    if (state->bufPos >= state->buf.size()) break;
                    size_t avail = state->buf.size() - state->bufPos;
                    size_t take = std::min<size_t>(avail, static_cast<size_t>(want));
                    out.append(state->buf.data() + state->bufPos, take);
                    state->bufPos += take;
                    want -= take;
                }
                return Value(std::move(out));
            }));

        handle->entries[Value("readLine")] = Value(makeNative("HttpStream.readLine", 0,
            [state](const std::vector<Value>&) -> Value {
                if (state->closed) throw RuntimeError("HttpStream is closed", 0);
                std::string out;
                while (true) {
                    streamEnsure(*state);
                    if (state->bufPos >= state->buf.size()) {
                        if (out.empty()) return Value();  // EOF
                        return Value(std::move(out));
                    }
                    char c = state->buf[state->bufPos++];
                    if (c == '\n') return Value(std::move(out));
                    if (c != '\r') out += c;
                }
            }));

        handle->entries[Value("readAll")] = Value(makeNative("HttpStream.readAll", 0,
            [state](const std::vector<Value>&) -> Value {
                if (state->closed) throw RuntimeError("HttpStream is closed", 0);
                std::string out;
                while (true) {
                    streamEnsure(*state);
                    if (state->bufPos >= state->buf.size()) break;
                    out.append(state->buf.data() + state->bufPos,
                               state->buf.size() - state->bufPos);
                    state->bufPos = state->buf.size();
                }
                return Value(std::move(out));
            }));

        handle->entries[Value("eof")] = Value(makeNative("HttpStream.eof", 0,
            [state](const std::vector<Value>&) -> Value {
                if (state->closed) return Value(true);
                if (state->bufPos < state->buf.size()) return Value(false);
                streamEnsure(*state);
                return Value(state->bufPos >= state->buf.size());
            }));

        handle->entries[Value("close")] = Value(makeNative("HttpStream.close", 0,
            [state](const std::vector<Value>&) -> Value {
                if (state->closed) return Value();
                state->closed = true;
                if (state->conn) state->conn->shutdown_close();
                return Value();
            }));

        return Value(handle);
    }
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
