#pragma once

#include "interpreter.h"
#include "value.h"
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

inline std::string argStr(int n) { return n == 1 ? "argument" : "arguments"; }

// Factory for native (C++) functions exposed as Praia callables.
//
// `paramNames` is optional. When provided, native calls support
// named-argument syntax (`mod.fn(x: 1, y: 2)`); when omitted, the
// engine throws the standard "Named arguments not supported" error
// for any named-arg dispatch into this function — matches the
// pre-existing behavior so callers that don't opt in are unaffected.
inline std::shared_ptr<NativeFunction> makeNative(
    const std::string& name, int arity,
    std::function<Value(const std::vector<Value>&)> fn,
    std::vector<std::string> paramNames = {}) {
    auto f = std::make_shared<NativeFunction>();
    f->funcName = name;
    f->numArgs = arity;
    f->fn = std::move(fn);
    f->paramNames_ = std::move(paramNames);
    return f;
}

// Safe callback invocation for tree-walker native code.
// Validates arity and pads missing args with nil before calling, preventing
// crashes from NativeFunction callbacks receiving wrong arg count.
//
// Clears `pendingArgsFilled_` before dispatch. The mask is set by the
// named-arg dispatch site for the OUTER call (the native, e.g. sort);
// when that native re-enters the engine via callSafe to invoke a
// user-supplied callback (the comparator), the callback's positional
// args should be treated as fully filled — not as "everything beyond
// position N is missing" per the outer call's mask. Without this
// reset, the comparator's second positional arg gets defaulted to
// nil and `b - a` throws "Operands of '-' must be numbers".
inline Value callSafe(Interpreter& interp, std::shared_ptr<Callable> callable,
                      const std::vector<Value>& args) {
    int arity = callable->arity();
    if (arity != -1) {
        int argc = static_cast<int>(args.size());
        if (argc > arity)
            throw RuntimeError(callable->name() + "() expected at most " +
                std::to_string(arity) + " " + argStr(arity) + " but got " + std::to_string(argc), 0);
        if (argc < arity) {
            std::vector<Value> padded = args;
            padded.resize(arity); // fills with nil (Value default)
            return callable->call(interp, padded);
        }
    }
    return callable->call(interp, args);
}

// ── Builtin registration functions (each in src/builtins/*.cpp) ──
void registerNetBuiltins(std::shared_ptr<PraiaMap> netMap);
void registerBytesBuiltins(std::shared_ptr<PraiaMap> bytesMap);
void registerCryptoBuiltins(std::shared_ptr<PraiaMap> cryptoMap);
void registerSecretsBuiltins(std::shared_ptr<PraiaMap> secretsMap);
void registerFmtBuiltins(std::shared_ptr<PraiaMap> fmtMap);
void registerZlibBuiltins(std::shared_ptr<PraiaMap> zlibMap);
void registerConcurrencyBuiltins(Interpreter* self, std::shared_ptr<Environment> globals);

// ── HTTP (builtins/http.cpp) ─────────────────────────────────

// Per-request knobs for outbound HTTP. All fields have safe defaults
// so callers that don't care can pass {}; the http.get / http.post
// wrappers do exactly that. Timeouts are milliseconds; -1 means "no
// limit" (use carefully — a hung peer with no timeout blocks the
// caller forever).
struct HttpOptions {
    int connectTimeoutMs = 30000;   // TCP/TLS handshake budget
    int readTimeoutMs    = 30000;   // single recv() / SSL_read() budget
    // Overall budget for a buffered request: connect, redirects, and
    // reading the whole response. <= 0 = no limit. Streams
    // (http.openStream) only use it until the handle is returned.
    int totalTimeoutMs   = 300000;
    bool followRedirects = true;
    int maxRedirects     = 10;
    bool insecure        = false;   // skip TLS cert verification (testing only!)
    std::string caBundle;           // path to custom CA bundle PEM (empty = system defaults)
    // SSRF protection: refuse to connect to loopback, RFC1918, link-local
    // (incl. cloud metadata at 169.254.169.254), IPv4-mapped, multicast,
    // and reserved address ranges. Re-applies per redirect hop. Set to
    // false only when you are deliberately targeting internal hosts
    // (local development, internal microservices, etc.).
    bool blockPrivateHosts = true;
    // Headers on a redirect to another origin (scheme, host or port).
    // false (`redirectHeaders: "safe"`, the default): only a fixed set of
    // standard non-credential headers is forwarded — Authorization,
    // Cookie and custom ones like X-API-Key are dropped. true
    // (`redirectHeaders: "all"`): forward everything, for redirects the
    // caller trusts (curl's --location-trusted).
    bool redirectForwardAllHeaders = false;
    // Largest response body a buffered request will read; a bigger one
    // fails instead of filling memory. <= 0 = no limit. Not applied to
    // streams, which hand the body over incrementally.
    int64_t maxResponseSize = 100ll * 1024 * 1024;
    // Internal, not a user option: the absolute deadline for the current
    // request from totalTimeoutMs, set by the redirect loops so reads
    // inside a response can be held to it.
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::time_point::max();
};

Value doHttpRequest(const std::string& method, const std::string& url,
                    const std::string& body,
                    const std::unordered_map<std::string, std::string>& extraHeaders,
                    const HttpOptions& opts = {});

// Returns a stream handle (Praia map) with .status / .headers /
// .cookies up front, plus .read(n) / .readLine() / .readAll() /
// .eof() / .close() methods that pull body bytes lazily. Decodes
// Content-Length, Transfer-Encoding: chunked, and close-delimited
// responses transparently — caller doesn't need to know the framing.
Value httpOpenStream(const std::string& method, const std::string& url,
                     const std::string& body,
                     const std::unordered_map<std::string, std::string>& extraHeaders,
                     const HttpOptions& opts = {});

// True if a header name/value pair is safe to write into a response
// (no CR, LF or NUL — header injection). Defined in builtins/http.cpp.
bool httpHeaderFieldSafe(const std::string& name, const std::string& value);

// Options for server.listen(port, opts?)
struct HttpServerOptions {
    // Address to bind. Empty = every IPv4 interface (0.0.0.0). A host name
    // binds only the first of its addresses that works, so for a server
    // only this machine reaches pass "127.0.0.1" or "::1", not "localhost".
    std::string host;
    // Largest request body accepted, for both Content-Length and chunked
    // framing; larger requests get 413. A Content-Length over the limit is
    // refused before any of the body is read; a chunked body is read and
    // decoded until its decoded size passes the limit.
    size_t maxBodySize = 64ull * 1024 * 1024;
    // Budget for receiving one request — headers and body together. The
    // server handles one connection at a time, so this is what bounds how
    // long a slow or idle client can hold it.
    int requestTimeoutMs = 30000;
};

void httpServerListen(int port, std::shared_ptr<Callable> handler, Interpreter& interp,
                      const HttpServerOptions& opts = {});

// ── HTTP client sessions (keepalive + default opts/headers) ──
//
// A session holds a pool of idle keep-alive sockets keyed by
// "scheme://host:port", plus default HttpOptions and default headers
// applied to every request made through it. Per-call values override
// session defaults; the caller is responsible for that merge — both
// the session accessors below are intentionally const-ref so the
// caller can copy and layer in its own scope.
//
// User-facing wiring lives in interpreter_setup.cpp; the HttpSession
// struct itself stays file-private in builtins/http.cpp so SocketConn
// and the OpenSSL state never leak outside the module.

// `poolMaxSize` clamps the number of idle keep-alive conns held by the
// session (LRU evicted past this). Values < 1 fall back to the struct
// default (currently 100).
//
// `poolIdleMs` is the per-conn idle TTL — conns older than this on
// next take are dropped instead of reused. Sentinel handling:
//   - poolIdleMs == -1  → use the struct default (currently 90 000 ms).
//                          This is what the wrapper passes when the user
//                          doesn't supply `poolIdleSecs` in the opts map.
//   - poolIdleMs == 0   → TTL disabled (kept forever — meaningful escape
//                          hatch for servers with unbounded keepalive).
//   - poolIdleMs <  0
//     (other than -1)   → also disabled (any non-positive surviving the
//                          sentinel check disables, by the > 0 guard in
//                          sessionTakeIdle).
//   - poolIdleMs >  0   → TTL is this many milliseconds.
Value httpCreateSession(const HttpOptions& defaultOpts,
                        const std::unordered_map<std::string, std::string>& defaultHeaders,
                        int poolMaxSize,
                        int poolIdleMs);

// Drive one request (with redirect handling) through the session's
// pool. Caller has already layered session defaults under per-call
// values for both headers and HttpOptions.
Value httpSessionRequest(const Value& session,
                         const std::string& method,
                         const std::string& url,
                         const std::string& body,
                         const std::unordered_map<std::string, std::string>& headers,
                         const HttpOptions& opts);

// Idempotent explicit close — drains the pool. The GC-time deleter
// (registered by httpCreateSession) does the same work on the
// implicit-cleanup path.
void httpSessionClose(const Value& session);

// Type predicate for polymorphic dispatch in interpreter_setup.cpp's
// http.get / http.post / http.request wrappers. Returns true iff the
// Value is an external handle with the http.session type tag — used
// to decide whether the first arg is a session or a stateless URL.
bool httpIsSession(const Value& v);

// Read-only accessors so the caller can layer session defaults under
// per-call values without exposing HttpSession's definition.
const HttpOptions& httpSessionGetDefaultOpts(const Value& session);
const std::unordered_map<std::string, std::string>&
httpSessionGetDefaultHeaders(const Value& session);

// ── JSON (builtins/json.cpp) ─────────────────────────────────
Value jsonParse(const std::string& src);
std::string jsonStringify(const Value& val, int indent = 0, int depth = 0);
Value jsonParserCreate(const Value& input);

// ── XML / plist (builtins/xml.cpp, builtins/plist.cpp) ───────
Value xmlParse(const std::string& src);
std::string xmlStringify(const Value& tree, int indent = 0);
std::string xmlEscape(const std::string& s);
std::string xmlUnescape(const std::string& s);
void registerXmlBuiltins(std::shared_ptr<PraiaMap> xmlMap);
void registerPlistBuiltins(std::shared_ptr<PraiaMap> plistMap);

// ── YAML (builtins/yaml.cpp) ─────────────────────────────────
Value yamlParse(const std::string& src);
std::string yamlStringify(const Value& val, int depth = 0);

// ── String / Array dot-method dispatch (builtins/methods.cpp) ─
class VM;
Value getStringMethod(const std::string& str, const std::string& name, int line);
Value getArrayMethod(std::shared_ptr<PraiaArray> arr, const std::string& name, int line,
                     Interpreter* interp = nullptr, VM* vm = nullptr);
Value getMapMethod(std::shared_ptr<PraiaMap> map, const std::string& name, int line);
Value getSetMethod(std::shared_ptr<PraiaSet> set, const std::string& name, int line);

// `s` repeated `n` times (n >= 0; callers word their own negative-count
// error). A result too big to make is a RuntimeError, not a C++
// exception the script can't catch.
inline std::string repeatString(const std::string& s, int64_t n) {
    if (s.empty() || n <= 0) return "";   // a huge count of nothing is still nothing
    size_t total;
    if (__builtin_mul_overflow(s.size(), static_cast<uint64_t>(n), &total) ||
        total > std::string().max_size())
        throw RuntimeError("String repeat result too large", 0);
    std::string result;
    try {
        result.reserve(total);
    } catch (const std::exception&) {   // bad_alloc, length_error
        throw RuntimeError("String repeat result too large", 0);
    }
    for (int64_t i = 0; i < n; i++) result += s;
    return result;
}

// Stable merge sort. Safe with any `less`, even one that isn't a
// consistent order (std::sort isn't). If `less` throws, what's left in
// `elems` is unspecified, so sort a copy when the original must survive.
// With no `less`: numbers by value, the rest by their string form.
void sortValues(std::vector<Value>& elems,
                const std::function<bool(const Value&, const Value&)>& less = nullptr);

