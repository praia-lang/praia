#pragma once
// Shared URL parser used by `http.*` builtins and the `url.parse` builtin.
// Aims for RFC 3986 §3 component-level correctness — scheme, authority
// (userinfo / host / port), path, query, fragment — including bracketed
// IPv6 literals (RFC 3986 §3.2.2) and ambiguous-colon detection. The
// parser does NOT percent-decode anything: query and userinfo are kept
// raw so callers decide how to interpret them.

#include <string>
#include <stdexcept>

namespace praia::url {

struct ParsedUrl {
    std::string scheme;          // lowercased; "" if absent
    std::string userinfo;        // raw "user" or "user:pass"; "" if absent
    std::string host;            // unbracketed (e.g. "::1", not "[::1]")
    bool hostIsIPv6 = false;     // true when the authority used "[v6]"
    bool hasPort = false;
    int port = -1;               // -1 when absent
    std::string path;            // may be empty; never percent-decoded
    std::string query;           // "" if absent; raw, never decoded
    std::string fragment;        // "" if absent
};

class UrlParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

ParsedUrl parse(const std::string& input);

// Render the host suitable for an HTTP `Host:` header value: bracketed
// when the authority used an IPv6 literal, bare otherwise. Appends
// `:port` iff the URL had an explicit port (`u.hasPort`) — matches
// curl's behaviour and is required by RFC 7230 §5.4 for non-default
// ports (virtual-host routing keys on Host:port).
std::string hostHeader(const ParsedUrl& u);

}  // namespace praia::url
