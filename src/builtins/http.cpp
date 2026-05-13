#include "../builtins.h"
#include "../interpreter.h"
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

std::string readAll(SocketConn& conn) {
    std::string data;
    char buf[8192];
    while (true) {
        errno = 0;
        ssize_t n = conn.read(buf, sizeof(buf));
        if (n > 0) { data.append(buf, n); continue; }
        if (n == 0) break;  // clean EOF (server closed)
        // n < 0 — distinguish timeout (SO_RCVTIMEO fired) from other
        // error. EAGAIN/EWOULDBLOCK is what SO_RCVTIMEO surfaces on
        // Linux/macOS; SSL_read inherits the underlying recv errno.
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            throw RuntimeError("HTTP read timed out", 0);
        if (errno == EINTR) continue;
        throw RuntimeError("HTTP read error: " +
                           std::string(errno ? std::strerror(errno) : "connection broken"), 0);
    }
    return data;
}

// Decode RFC 7230 §4.1 chunked transfer encoding from an in-memory body.
// Mirrors the streaming decoder in openStream — same chunk-extension and
// trailer handling, just over a std::string slice instead of a socket.
// Throws on malformed framing so callers get a clear error rather than
// silently truncated bytes.
static std::string decodeChunkedBody(const std::string& enc) {
    std::string out;
    out.reserve(enc.size());
    size_t i = 0;
    while (i < enc.size()) {
        // Find end of size line.
        size_t lineEnd = enc.find("\r\n", i);
        if (lineEnd == std::string::npos)
            throw RuntimeError("HTTP chunked: missing CRLF after chunk size", 0);
        std::string sizeLine = enc.substr(i, lineEnd - i);
        // Strip chunk extensions after ';' (RFC 7230 §4.1.1) and trailing WS.
        size_t semi = sizeLine.find(';');
        if (semi != std::string::npos) sizeLine.resize(semi);
        while (!sizeLine.empty() &&
               (sizeLine.back() == ' ' || sizeLine.back() == '\t'))
            sizeLine.pop_back();
        if (sizeLine.empty())
            throw RuntimeError("HTTP chunked: empty chunk size line", 0);
        int64_t sz = 0;
        for (char c : sizeLine) {
            int d;
            if      (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else throw RuntimeError(
                std::string("HTTP chunked: invalid hex in chunk size: '") +
                c + "'", 0);
            sz = (sz << 4) | d;
            if (sz < 0)
                throw RuntimeError("HTTP chunked: chunk size overflow", 0);
        }
        i = lineEnd + 2;
        if (sz == 0) {
            // Terminator: consume trailers until the empty line then stop.
            // Trailers aren't surfaced — same policy as openStream.
            while (i < enc.size()) {
                size_t tEnd = enc.find("\r\n", i);
                if (tEnd == std::string::npos) break;
                bool empty = (tEnd == i);
                i = tEnd + 2;
                if (empty) break;
            }
            return out;
        }
        if (i + static_cast<size_t>(sz) > enc.size())
            throw RuntimeError("HTTP chunked: chunk data truncated", 0);
        out.append(enc, i, static_cast<size_t>(sz));
        i += static_cast<size_t>(sz);
        if (i + 2 > enc.size() || enc[i] != '\r' || enc[i + 1] != '\n')
            throw RuntimeError("HTTP chunked: missing CRLF after chunk data", 0);
        i += 2;
    }
    // Reached end of buffer without seeing the 0-sized terminator. Server
    // closed mid-chunk — the data we *did* decode is still useful, but
    // most callers want to know about it.
    throw RuntimeError("HTTP chunked: stream ended before terminator chunk", 0);
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
    // Set-Cookie is the one header that legitimately repeats — every
    // cookie a server sets in one response is a separate Set-Cookie
    // line. The headers map can only hold one value per key, so we
    // expose the full list as a dedicated `cookies` array (in arrival
    // order). For back-compat the headers map still carries the
    // last value at "set-cookie" too.
    auto cookies = gcNew<PraiaArray>();
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
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        if (key == "set-cookie")
            cookies->elements.push_back(Value(val));
        hdrs->entries[Value(key)] = Value(val);
    }

    // Decode Transfer-Encoding: chunked before handing the body to the
    // caller. Per RFC 7230 §3.3.3, TE-chunked overrides Content-Length
    // for framing; we matched on lowercase header keys above. Multiple
    // codings other than "chunked" are not in scope (no compression
    // here), so we just look for the chunked token.
    auto teIt = hdrs->entries.find(Value(std::string("transfer-encoding")));
    if (teIt != hdrs->entries.end() && teIt->second.isString()) {
        const std::string& te = teIt->second.asString();
        std::string teLower(te);
        std::transform(teLower.begin(), teLower.end(), teLower.begin(), ::tolower);
        if (teLower.find("chunked") != std::string::npos) {
            body = decodeChunkedBody(body);
        }
    }

    auto result = gcNew<PraiaMap>();
    result->entries[Value("status")]  = Value(static_cast<double>(status));
    result->entries[Value("body")]    = Value(body);
    result->entries[Value("headers")] = Value(hdrs);
    result->entries[Value("cookies")] = Value(cookies);
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

// Single HTTP request — one connect, one send, one parse. No redirect
// handling; that's the caller's loop. `remainingTotalMs` is the
// per-request cap derived from the overall budget (or the user's
// readTimeoutMs if no overall budget was set); it's passed through
// SO_RCVTIMEO/SO_SNDTIMEO so a slow server doesn't outlast the budget.
static Value doOneHttpRequest(const std::string& method, const std::string& url,
                              const std::string& body,
                              const std::unordered_map<std::string, std::string>& extraHeaders,
                              const HttpOptions& opts) {
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
                    sz = (sz << 4) | d;
                    if (sz < 0)
                        throw RuntimeError("HTTP stream: chunk size overflow", 0);
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
static void detectStreamMode(HttpStreamState& s) {
    auto& h = s.headersMap->entries;
    auto teIt = h.find(Value(std::string("transfer-encoding")));
    if (teIt != h.end() && teIt->second.isString()) {
        std::string te = teIt->second.asString();
        for (auto& c : te) c = (char)std::tolower((unsigned char)c);
        if (te.find("chunked") != std::string::npos) {
            s.mode = HttpStreamState::Mode::Chunked;
            return;
        }
    }
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

        std::string headerText, bodyPrefix;
        readHeadersInto(*conn, headerText, bodyPrefix);

        Value parsed = parseHttpResponse(headerText + bodyPrefix);
        // parseHttpResponse parses headers AND splits its `raw` into
        // headerSection+body. We pass headers+prefix so it sees the
        // \r\n\r\n boundary, but we re-extract status/headers/cookies
        // here and keep the prefix as the FIRST body bytes for the
        // stream — we don't want to slurp the full body in.
        auto& parsedMap = parsed.asMap()->entries;
        int status = static_cast<int>(parsedMap[Value("status")].asNumber());

        // Redirect handling — fully drain the redirect's body via the
        // streaming path so the connection state is clean before we
        // open the next one.
        if (opts.followRedirects) {
            auto& respHeaders = parsedMap[Value("headers")].asMap()->entries;
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
                    HttpStreamState tmp;
                    tmp.conn = conn;
                    tmp.headersMap = parsedMap[Value("headers")].asMap();
                    tmp.scratch = std::move(bodyPrefix);
                    tmp.scratchPos = 0;
                    detectStreamMode(tmp);
                    drainRedirectBody(tmp);
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

        // Final response — build the stream handle.
        auto state = std::make_shared<HttpStreamState>();
        state->conn       = conn;
        state->status     = status;
        state->headersMap = parsedMap[Value("headers")].asMap();
        state->cookiesArr = parsedMap[Value("cookies")].asArray();
        state->scratch    = std::move(bodyPrefix);
        state->scratchPos = 0;
        detectStreamMode(*state);

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
