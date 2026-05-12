#include "url.h"
#include <cctype>

namespace praia::url {

namespace {

bool isSchemeStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool isSchemeChar(char c) {
    return isSchemeStart(c) || (c >= '0' && c <= '9') ||
           c == '+' || c == '-' || c == '.';
}

// Parse a decimal port, allowing only ASCII digits and rejecting empty
// strings, leading-plus, range overflow, or stray characters. The
// `full` URL is included in errors so failures from inside parse() name
// the offending input.
int parsePort(const std::string& s, const std::string& full) {
    if (s.empty())
        throw UrlParseError("empty port in \"" + full + "\"");
    if (s.size() > 5)
        throw UrlParseError("port out of range \"" + s + "\" in \"" + full + "\"");
    for (char c : s)
        if (c < '0' || c > '9')
            throw UrlParseError("non-numeric port \"" + s + "\" in \"" + full + "\"");
    int p = std::stoi(s);
    if (p < 0 || p > 65535)
        throw UrlParseError("port out of range \"" + s + "\" in \"" + full + "\"");
    return p;
}

// Reject control characters anywhere in the input. CR/LF/NUL in a URL
// is almost always a header-injection attempt or a copy-paste bug;
// either way we'd rather fail loud than silently smuggle the bytes
// into the path/host/query.
void rejectControl(const std::string& input) {
    for (char c : input) {
        if (c == '\0' || c == '\r' || c == '\n')
            throw UrlParseError("URL contains control character (NUL/CR/LF)");
    }
}

}  // namespace

ParsedUrl parse(const std::string& input) {
    rejectControl(input);
    ParsedUrl r;
    std::string rest = input;

    // Fragment (RFC 3986 §3.5): first '#' wins; fragment runs to EOS.
    // Stripping it first means '?' inside a fragment can't be mistaken
    // for the query delimiter.
    auto hash = rest.find('#');
    if (hash != std::string::npos) {
        r.fragment = rest.substr(hash + 1);
        rest = rest.substr(0, hash);
    }

    // Query (RFC 3986 §3.4): first '?' wins. Kept raw — the caller
    // (or url.decode / query-string helper) decides decoding policy.
    auto qm = rest.find('?');
    if (qm != std::string::npos) {
        r.query = rest.substr(qm + 1);
        rest = rest.substr(0, qm);
    }

    // Scheme (RFC 3986 §3.1): ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":".
    // We only treat the leading word as a scheme when it actually ends
    // in ':' — otherwise input like `foo/bar` would mis-read `foo` as
    // a scheme. Case-insensitive, so lowercase to normalize.
    if (!rest.empty() && isSchemeStart(rest[0])) {
        size_t i = 1;
        while (i < rest.size() && isSchemeChar(rest[i])) ++i;
        if (i < rest.size() && rest[i] == ':') {
            r.scheme = rest.substr(0, i);
            for (auto& c : r.scheme) c = (char)std::tolower((unsigned char)c);
            rest = rest.substr(i + 1);
        }
    }

    // Authority is only present when the scheme/path is followed by
    // "//" (the hier-part form `// authority path-abempty`). Without
    // "//", the rest is treated as path (handles `mailto:foo@bar`,
    // `tel:+1234`, plain `/a/b`, etc.).
    if (rest.size() >= 2 && rest[0] == '/' && rest[1] == '/') {
        rest = rest.substr(2);
        // Authority ends at the first '/' (which begins path-abempty)
        // or end-of-string.
        auto slash = rest.find('/');
        std::string authority;
        if (slash == std::string::npos) {
            authority = rest;
            rest = "";
        } else {
            authority = rest.substr(0, slash);
            rest = rest.substr(slash);
        }

        // Userinfo: split on LAST '@'. Userinfo may legitimately contain
        // ':' (the `user:pass` form), and the host can't contain '@' in
        // a valid URL. Splitting on the last '@' tolerates `@` inside
        // userinfo (e.g. emails-as-usernames, percent-encoded but still
        // unambiguous because the host can't have one).
        auto at = authority.rfind('@');
        std::string hostPort;
        if (at != std::string::npos) {
            r.userinfo = authority.substr(0, at);
            hostPort = authority.substr(at + 1);
        } else {
            hostPort = authority;
        }

        // Host + port. Two shapes per RFC 3986 §3.2.2:
        //   1. "[" IPv6address "]" [ ":" port ]    — bracketed v6 literal
        //   2. host [ ":" port ]                   — IPv4 or reg-name
        if (!hostPort.empty() && hostPort[0] == '[') {
            auto close = hostPort.find(']');
            if (close == std::string::npos)
                throw UrlParseError("unterminated IPv6 literal in \"" + input + "\"");
            r.host = hostPort.substr(1, close - 1);
            r.hostIsIPv6 = true;
            std::string after = hostPort.substr(close + 1);
            if (!after.empty()) {
                if (after[0] != ':')
                    throw UrlParseError("unexpected text after IPv6 literal in \"" + input + "\"");
                r.port = parsePort(after.substr(1), input);
                r.hasPort = true;
            }
        } else {
            // Non-bracketed: exactly zero or one ':'. Multiple colons
            // means the caller wrote a bare IPv6 address — almost
            // always unintentional. Failing loud here catches both the
            // "forgot brackets" mistake and the legacy ad-hoc parser's
            // silent first-colon split bug.
            auto firstColon = hostPort.find(':');
            auto lastColon  = hostPort.rfind(':');
            if (firstColon != lastColon)
                throw UrlParseError("ambiguous host \"" + hostPort +
                                    "\" in \"" + input +
                                    "\" — bracket IPv6 literals as [..]");
            if (firstColon == std::string::npos) {
                r.host = hostPort;
            } else {
                r.host = hostPort.substr(0, firstColon);
                r.port = parsePort(hostPort.substr(firstColon + 1), input);
                r.hasPort = true;
            }
        }
    }

    // Whatever's left after stripping fragment, query, scheme, authority
    // is the path. May be empty (e.g. `http://example.com`) — callers
    // that need a leading '/' should supply their own default.
    r.path = rest;
    return r;
}

std::string hostHeader(const ParsedUrl& u) {
    return u.hostIsIPv6 ? ("[" + u.host + "]") : u.host;
}

}  // namespace praia::url
