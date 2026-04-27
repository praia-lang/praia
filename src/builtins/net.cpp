#include "../builtins.h"
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include "../gc_heap.h"

struct AddrGuard {
    struct addrinfo* res = nullptr;
    ~AddrGuard() { if (res) freeaddrinfo(res); }
    struct addrinfo* operator->() { return res; }
    struct addrinfo* get() { return res; }
};

static std::string sysErr(const std::string& msg) {
    return msg + ": " + std::strerror(errno);
}

static int validatePort(double val, const char* funcName) {
    int port = static_cast<int>(val);
    if (port < 0 || port > 65535)
        throw RuntimeError(std::string(funcName) + " port must be 0–65535, got " + std::to_string(port), 0);
    return port;
}

void registerNetBuiltins(std::shared_ptr<PraiaMap> netMap) {
    netMap->entries[Value("connect")] = Value(makeNative("net.connect", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("net.connect() requires a host string", 0);
            if (!args[1].isNumber())
                throw RuntimeError("net.connect() requires a port number", 0);
            std::string host = args[0].asString();
            int port = validatePort(args[1].asNumber(), "net.connect()");

            struct addrinfo hints = {};
            hints.ai_family = AF_UNSPEC; // IPv4 or IPv6
            hints.ai_socktype = SOCK_STREAM;
            AddrGuard ag;
            if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &ag.res) != 0)
                throw RuntimeError("Cannot resolve host: " + host, 0);
            // Try each address until one connects
            int sock = -1;
            for (auto* p = ag.get(); p; p = p->ai_next) {
                sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
                if (sock < 0) continue;
                if (connect(sock, p->ai_addr, p->ai_addrlen) == 0) break;
                close(sock);
                sock = -1;
            }
            if (sock < 0)
                throw RuntimeError(sysErr("Cannot connect to " + host + ":" + std::to_string(port)), 0);
            return Value(static_cast<int64_t>(sock));
        }));

    netMap->entries[Value("listen")] = Value(makeNative("net.listen", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber())
                throw RuntimeError("net.listen() requires a port number", 0);
            int port = validatePort(args[0].asNumber(), "net.listen()");

            // Try IPv6 dual-stack first (accepts both v4 and v6), fall back to IPv4
            int fd = socket(AF_INET6, SOCK_STREAM, 0);
            if (fd >= 0) {
                int opt = 1;
                setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
                int v6only = 0; // dual-stack: accept IPv4-mapped addresses
                setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));

                struct sockaddr_in6 addr = {};
                addr.sin6_family = AF_INET6;
                addr.sin6_addr = in6addr_any;
                addr.sin6_port = htons(port);

                if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0 &&
                    ::listen(fd, 64) == 0) {
                    return Value(static_cast<int64_t>(fd));
                }
                close(fd);
            }
            // Fallback: IPv4 only
            fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0)
                throw RuntimeError(sysErr("Cannot create socket"), 0);
            int opt = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

            struct sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(port);

            if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                close(fd);
                throw RuntimeError(sysErr("Cannot bind to port " + std::to_string(port)), 0);
            }
            if (::listen(fd, 64) < 0) {
                close(fd);
                throw RuntimeError(sysErr("Cannot listen on port " + std::to_string(port)), 0);
            }
            return Value(static_cast<int64_t>(fd));
        }));

    netMap->entries[Value("accept")] = Value(makeNative("net.accept", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber())
                throw RuntimeError("net.accept() requires a socket", 0);
            int fd = static_cast<int>(args[0].asNumber());
            struct sockaddr_storage ca;
            socklen_t cl = sizeof(ca);
            int client = accept(fd, (struct sockaddr*)&ca, &cl);
            if (client < 0)
                throw RuntimeError(sysErr("Accept failed"), 0);
            return Value(static_cast<int64_t>(client));
        }));

    netMap->entries[Value("send")] = Value(makeNative("net.send", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber())
                throw RuntimeError("net.send() requires a socket", 0);
            if (!args[1].isString())
                throw RuntimeError("net.send() requires a string", 0);
            int fd = static_cast<int>(args[0].asNumber());
            auto& data = args[1].asString();
            ssize_t sent = ::send(fd, data.c_str(), data.size(), 0);
            if (sent < 0)
                throw RuntimeError(sysErr("Send failed"), 0);
            return Value(static_cast<int64_t>(sent));
        }));

    netMap->entries[Value("recv")] = Value(makeNative("net.recv", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isNumber())
                throw RuntimeError("net.recv() requires a socket", 0);
            int fd = static_cast<int>(args[0].asNumber());
            int maxBytes = 4096;
            if (args.size() > 1 && args[1].isNumber())
                maxBytes = static_cast<int>(args[1].asNumber());

            std::vector<char> buf(maxBytes);
            ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
            if (n < 0)
                throw RuntimeError(sysErr("Recv failed"), 0);
            if (n == 0) return Value(std::string(""));
            return Value(std::string(buf.data(), n));
        }));

    netMap->entries[Value("recvAll")] = Value(makeNative("net.recvAll", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber())
                throw RuntimeError("net.recvAll() requires a socket", 0);
            int fd = static_cast<int>(args[0].asNumber());
            std::string data;
            char buf[4096];
            ssize_t n;
            while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0)
                data.append(buf, n);
            return Value(std::move(data));
        }));

    netMap->entries[Value("close")] = Value(makeNative("net.close", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber())
                throw RuntimeError("net.close() requires a socket", 0);
            close(static_cast<int>(args[0].asNumber()));
            return Value();
        }));

    // ── UDP ──

    // net.udp() creates an IPv4 socket (portable default).
    // IPv6 dual-stack behavior varies by OS (Linux defaults to v6only=1),
    // so client sockets use IPv4 for reliability. Servers (udpBind) use dual-stack.
    netMap->entries[Value("udp")] = Value(makeNative("net.udp", 0,
        [](const std::vector<Value>&) -> Value {
            int sock = socket(AF_INET, SOCK_DGRAM, 0);
            if (sock < 0)
                throw RuntimeError(sysErr("Cannot create UDP socket"), 0);
            return Value(static_cast<int64_t>(sock));
        }));

    // net.udp6() creates an IPv6 UDP socket
    netMap->entries[Value("udp6")] = Value(makeNative("net.udp6", 0,
        [](const std::vector<Value>&) -> Value {
            int sock = socket(AF_INET6, SOCK_DGRAM, 0);
            if (sock < 0)
                throw RuntimeError(sysErr("Cannot create IPv6 UDP socket"), 0);
            return Value(static_cast<int64_t>(sock));
        }));

    netMap->entries[Value("udpBind")] = Value(makeNative("net.udpBind", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber())
                throw RuntimeError("net.udpBind() requires a port number", 0);
            int port = validatePort(args[0].asNumber(), "net.udpBind()");

            // Try IPv6 dual-stack first
            int sock = socket(AF_INET6, SOCK_DGRAM, 0);
            if (sock >= 0) {
                int opt = 1;
                setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
                int v6only = 0;
                setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
                struct sockaddr_in6 addr = {};
                addr.sin6_family = AF_INET6;
                addr.sin6_addr = in6addr_any;
                addr.sin6_port = htons(port);
                if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0)
                    return Value(static_cast<int64_t>(sock));
                close(sock);
            }
            // Fallback to IPv4
            sock = socket(AF_INET, SOCK_DGRAM, 0);
            if (sock < 0)
                throw RuntimeError(sysErr("Cannot create UDP socket"), 0);
            int opt = 1;
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            struct sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(port);
            if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                close(sock);
                throw RuntimeError(sysErr("Cannot bind UDP to port " + std::to_string(port)), 0);
            }
            return Value(static_cast<int64_t>(sock));
        }));

    netMap->entries[Value("sendTo")] = Value(makeNative("net.sendTo", 4,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber()) throw RuntimeError("net.sendTo() requires a socket", 0);
            if (!args[1].isString()) throw RuntimeError("net.sendTo() requires a host string", 0);
            if (!args[2].isNumber()) throw RuntimeError("net.sendTo() requires a port number", 0);
            if (!args[3].isString()) throw RuntimeError("net.sendTo() requires data string", 0);
            int sock = static_cast<int>(args[0].asNumber());
            std::string host = args[1].asString();
            int port = validatePort(args[2].asNumber(), "net.sendTo()");
            auto& data = args[3].asString();

            // Detect socket family to match resolution, fall back to AF_UNSPEC
            struct sockaddr_storage ss;
            socklen_t sslen = sizeof(ss);
            int family = AF_UNSPEC;
            if (getsockname(sock, (struct sockaddr*)&ss, &sslen) == 0)
                family = ss.ss_family;

            struct addrinfo hints = {};
            hints.ai_family = family;
            hints.ai_socktype = SOCK_DGRAM;
            AddrGuard ag;
            if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &ag.res) != 0) {
                // Socket family didn't match host (e.g. IPv6 socket, IPv4 host) — retry unspec
                if (family != AF_UNSPEC) {
                    hints.ai_family = AF_UNSPEC;
                    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &ag.res) != 0)
                        throw RuntimeError("Cannot resolve host: " + host, 0);
                } else {
                    throw RuntimeError("Cannot resolve host: " + host, 0);
                }
            }
            ssize_t sent = sendto(sock, data.c_str(), data.size(), 0, ag->ai_addr, ag->ai_addrlen);
            if (sent < 0)
                throw RuntimeError(sysErr("sendTo failed"), 0);
            return Value(static_cast<int64_t>(sent));
        }));

    netMap->entries[Value("recvFrom")] = Value(makeNative("net.recvFrom", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isNumber())
                throw RuntimeError("net.recvFrom() requires a socket", 0);
            int sock = static_cast<int>(args[0].asNumber());
            int maxBytes = 4096;
            if (args.size() > 1 && args[1].isNumber())
                maxBytes = static_cast<int>(args[1].asNumber());

            std::vector<char> buf(maxBytes);
            struct sockaddr_storage from = {};
            socklen_t fromLen = sizeof(from);
            ssize_t n = recvfrom(sock, buf.data(), buf.size(), 0,
                                  (struct sockaddr*)&from, &fromLen);
            if (n < 0) {
                if (errno == EINTR) throw RuntimeError("Interrupted", 0);
                throw RuntimeError(sysErr("recvFrom failed"), 0);
            }

            // Extract sender address (IPv4 or IPv6)
            char addrBuf[INET6_ADDRSTRLEN];
            int port = 0;
            if (from.ss_family == AF_INET) {
                auto* s = (struct sockaddr_in*)&from;
                inet_ntop(AF_INET, &s->sin_addr, addrBuf, sizeof(addrBuf));
                port = ntohs(s->sin_port);
            } else {
                auto* s = (struct sockaddr_in6*)&from;
                inet_ntop(AF_INET6, &s->sin6_addr, addrBuf, sizeof(addrBuf));
                port = ntohs(s->sin6_port);
                // Strip ::ffff: prefix for IPv4-mapped addresses on dual-stack sockets
                if (IN6_IS_ADDR_V4MAPPED(&s->sin6_addr)) {
                    inet_ntop(AF_INET, &s->sin6_addr.s6_addr[12], addrBuf, sizeof(addrBuf));
                }
            }

            auto result = gcNew<PraiaMap>();
            result->entries[Value("data")] = Value(std::string(buf.data(), n));
            result->entries[Value("host")] = Value(std::string(addrBuf));
            result->entries[Value("port")] = Value(static_cast<int64_t>(port));
            return Value(result);
        }));

    // ── DNS + socket options ──

    netMap->entries[Value("resolve")] = Value(makeNative("net.resolve", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("net.resolve() requires a hostname string", 0);
            std::string host = args[0].asString();
            struct addrinfo hints = {};
            hints.ai_family = AF_UNSPEC; // return both IPv4 and IPv6
            AddrGuard ag;
            if (getaddrinfo(host.c_str(), nullptr, &hints, &ag.res) != 0)
                throw RuntimeError("Cannot resolve: " + host, 0);
            auto result = gcNew<PraiaArray>();
            for (auto* p = ag.get(); p; p = p->ai_next) {
                char buf[INET6_ADDRSTRLEN];
                if (p->ai_family == AF_INET) {
                    auto* addr = (struct sockaddr_in*)p->ai_addr;
                    inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf));
                } else if (p->ai_family == AF_INET6) {
                    auto* addr = (struct sockaddr_in6*)p->ai_addr;
                    inet_ntop(AF_INET6, &addr->sin6_addr, buf, sizeof(buf));
                } else {
                    continue;
                }
                result->elements.push_back(Value(std::string(buf)));
            }
            return Value(result);
        }));

    netMap->entries[Value("setTimeout")] = Value(makeNative("net.setTimeout", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber() || !args[1].isNumber())
                throw RuntimeError("net.setTimeout(socket, ms) requires two numbers", 0);
            int sock = static_cast<int>(args[0].asNumber());
            int ms = static_cast<int>(args[1].asNumber());
            struct timeval tv;
            tv.tv_sec = ms / 1000;
            tv.tv_usec = (ms % 1000) * 1000;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            return Value();
        }));

    // ── Raw sockets ──

    // Helper: map protocol name to IPPROTO_* constant
    auto protoFromName = [](const std::string& name) -> int {
        if (name == "icmp")  return IPPROTO_ICMP;
        if (name == "icmp6") return IPPROTO_ICMPV6;
        if (name == "tcp")   return IPPROTO_TCP;
        if (name == "udp")   return IPPROTO_UDP;
        if (name == "raw")   return IPPROTO_RAW;
        return -1;
    };

    // net.rawSocket(protocol) — create a raw socket.
    // protocol: "icmp", "icmp6", "tcp", "udp", "raw", or a number.
    // Requires root/CAP_NET_RAW. On macOS, ICMP falls back to SOCK_DGRAM (unprivileged).
    netMap->entries[Value("rawSocket")] = Value(makeNative("net.rawSocket", 1,
        [protoFromName](const std::vector<Value>& args) -> Value {
            int proto;
            if (args[0].isString()) {
                proto = protoFromName(args[0].asString());
                if (proto < 0)
                    throw RuntimeError("net.rawSocket(): unknown protocol '" + args[0].asString() +
                        "'. Valid: icmp, icmp6, tcp, udp, raw, or a number", 0);
            } else if (args[0].isNumber()) {
                proto = static_cast<int>(args[0].asNumber());
            } else {
                throw RuntimeError("net.rawSocket() requires a protocol string or number", 0);
            }

            int family = (proto == IPPROTO_ICMPV6) ? AF_INET6 : AF_INET;

            // Try SOCK_RAW first (requires root)
            int sock = socket(family, SOCK_RAW, proto);
            if (sock >= 0) return Value(static_cast<int64_t>(sock));

#ifdef __APPLE__
            // macOS: SOCK_DGRAM with IPPROTO_ICMP works unprivileged
            if (proto == IPPROTO_ICMP || proto == IPPROTO_ICMPV6) {
                sock = socket(family, SOCK_DGRAM, proto);
                if (sock >= 0) return Value(static_cast<int64_t>(sock));
            }
#endif

            if (errno == EPERM || errno == EACCES)
                throw RuntimeError("net.rawSocket(): permission denied — requires root or CAP_NET_RAW", 0);
            throw RuntimeError(sysErr("net.rawSocket(): cannot create socket"), 0);
        }));

    // net.rawSend(sock, host, data) — send raw data to a host
    netMap->entries[Value("rawSend")] = Value(makeNative("net.rawSend", 3,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber()) throw RuntimeError("net.rawSend() requires a socket", 0);
            if (!args[1].isString()) throw RuntimeError("net.rawSend() requires a host string", 0);
            if (!args[2].isString()) throw RuntimeError("net.rawSend() requires data string", 0);
            int sock = static_cast<int>(args[0].asNumber());
            std::string host = args[1].asString();
            auto& data = args[2].asString();

            // Detect socket family
            struct sockaddr_storage ss;
            socklen_t sslen = sizeof(ss);
            int family = AF_INET;
            if (getsockname(sock, (struct sockaddr*)&ss, &sslen) == 0 && ss.ss_family != 0)
                family = ss.ss_family;

            struct addrinfo hints = {};
            hints.ai_family = family;
            AddrGuard ag;
            if (getaddrinfo(host.c_str(), nullptr, &hints, &ag.res) != 0)
                throw RuntimeError("net.rawSend(): cannot resolve host: " + host, 0);
            ssize_t sent = sendto(sock, data.data(), data.size(), 0, ag->ai_addr, ag->ai_addrlen);
            if (sent < 0)
                throw RuntimeError(sysErr("net.rawSend() failed"), 0);
            return Value(static_cast<int64_t>(sent));
        }));

    // net.rawRecv(sock, maxBytes?) — receive raw data, returns {data, host}
    netMap->entries[Value("rawRecv")] = Value(makeNative("net.rawRecv", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty() || !args[0].isNumber())
                throw RuntimeError("net.rawRecv() requires a socket", 0);
            int sock = static_cast<int>(args[0].asNumber());
            int maxBytes = 65536;
            if (args.size() > 1 && args[1].isNumber())
                maxBytes = static_cast<int>(args[1].asNumber());

            std::vector<char> buf(maxBytes);
            struct sockaddr_storage from = {};
            socklen_t fromLen = sizeof(from);
            ssize_t n = recvfrom(sock, buf.data(), buf.size(), 0,
                                  (struct sockaddr*)&from, &fromLen);
            if (n < 0)
                throw RuntimeError(sysErr("net.rawRecv() failed"), 0);

            char addrBuf[INET6_ADDRSTRLEN];
            if (from.ss_family == AF_INET) {
                auto* s = (struct sockaddr_in*)&from;
                inet_ntop(AF_INET, &s->sin_addr, addrBuf, sizeof(addrBuf));
            } else if (from.ss_family == AF_INET6) {
                auto* s = (struct sockaddr_in6*)&from;
                inet_ntop(AF_INET6, &s->sin6_addr, addrBuf, sizeof(addrBuf));
            } else {
                std::snprintf(addrBuf, sizeof(addrBuf), "unknown");
            }

            auto result = gcNew<PraiaMap>();
            result->entries[Value("data")] = Value(std::string(buf.data(), n));
            result->entries[Value("host")] = Value(std::string(addrBuf));
            return Value(result);
        }));
}
