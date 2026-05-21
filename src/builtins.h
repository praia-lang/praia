#pragma once

#include "interpreter.h"
#include "value.h"
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
    int totalTimeoutMs   = -1;      // overall request budget (incl. redirects)
    bool followRedirects = true;
    int maxRedirects     = 10;
    bool insecure        = false;   // skip TLS cert verification (testing only!)
    std::string caBundle;           // path to custom CA bundle PEM (empty = system defaults)
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

void httpServerListen(int port, std::shared_ptr<Callable> handler, Interpreter& interp);

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

Value httpCreateSession(const HttpOptions& defaultOpts,
                        const std::unordered_map<std::string, std::string>& defaultHeaders);

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

