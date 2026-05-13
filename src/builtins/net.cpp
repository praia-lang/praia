#include "../builtins.h"
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <set>
#include <poll.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <resolv.h>
#include <arpa/nameser.h>
#include "../gc_heap.h"
#include "../signal_state.h"
#include "scope_guards.h"
#include <mutex>
#include <map>

#ifdef HAVE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#endif

// ── TLS connection registry ──
// TLS handles are negative integers to distinguish from raw FDs.
// net.send/recv/close check this registry to route through SSL.

#ifdef HAVE_OPENSSL
struct TlsConn {
    SSL* ssl = nullptr;
    SSL_CTX* ctx = nullptr;
    int fd = -1;       // underlying TCP socket
};

static std::mutex g_tlsMutex;
static std::map<int, TlsConn> g_tlsConns;
static int g_nextTlsId = -1;  // decrements: -1, -2, -3, ...

static std::once_flag g_sslInitFlag;
static void ensureSSLInit() {
    std::call_once(g_sslInitFlag, [] {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
    });
}

static int registerTls(TlsConn conn) {
    std::lock_guard<std::mutex> lock(g_tlsMutex);
    int id = g_nextTlsId--;
    g_tlsConns[id] = conn;
    return id;
}

static TlsConn* lookupTls(int id) {
    if (id >= 0) return nullptr;
    std::lock_guard<std::mutex> lock(g_tlsMutex);
    auto it = g_tlsConns.find(id);
    return (it != g_tlsConns.end()) ? &it->second : nullptr;
}

static void closeTls(int id) {
    std::lock_guard<std::mutex> lock(g_tlsMutex);
    auto it = g_tlsConns.find(id);
    if (it == g_tlsConns.end()) return;
    auto& c = it->second;
    if (c.ssl) { SSL_shutdown(c.ssl); SSL_free(c.ssl); }
    if (c.ctx) SSL_CTX_free(c.ctx);
    if (c.fd >= 0) ::close(c.fd);
    g_tlsConns.erase(it);
}
#endif

static std::string sysErr(const std::string& msg) {
    return msg + ": " + std::strerror(errno);
}

static int validatePort(double val, const char* funcName) {
    int port = static_cast<int>(val);
    if (port < 0 || port > 65535)
        throw RuntimeError(std::string(funcName) + " port must be 0–65535, got " + std::to_string(port), 0);
    return port;
}

static uint16_t icmpChecksum(const void* data, int len) {
    auto* p = (const uint16_t*)data;
    uint32_t sum = 0;
    for (; len > 1; len -= 2) sum += *p++;
    if (len == 1) sum += *(const uint8_t*)p;
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    return static_cast<uint16_t>(~sum);
}

// Create an ICMP socket — tries SOCK_DGRAM first (unprivileged on macOS),
// falls back to SOCK_RAW (requires root/CAP_NET_RAW on Linux).
// ICMP socket: returns fd. Sets isRaw to true if SOCK_RAW (Linux with root).
// SOCK_DGRAM: kernel filters replies by ID, no IP header in recv.
// SOCK_RAW: receives ALL ICMP, includes IP header, kernel may rewrite our ID.
static bool g_icmpIsRaw = false;

static int createIcmpSocket(const char* caller = "net.ping()") {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (sock >= 0) { g_icmpIsRaw = false; return sock; }
    sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock >= 0) { g_icmpIsRaw = true; return sock; }
    if (errno == EPERM || errno == EACCES)
        throw RuntimeError(std::string(caller) + ": permission denied — requires root or CAP_NET_RAW on Linux", 0);
    throw RuntimeError(sysErr(std::string(caller) + ": cannot create ICMP socket"), 0);
}

static int msElapsed(const struct timeval& start) {
    struct timeval now;
    gettimeofday(&now, nullptr);
    return static_cast<int>((now.tv_sec - start.tv_sec) * 1000 +
                            (now.tv_usec - start.tv_usec) / 1000);
}

// Build an ICMP echo request. Returns packet length.
static int buildEchoRequest(uint8_t* pkt, uint16_t id, uint16_t seq) {
    auto* hdr = (struct icmp*)pkt;
    hdr->icmp_type = ICMP_ECHO;
    hdr->icmp_code = 0;
    hdr->icmp_id = htons(id);
    hdr->icmp_seq = htons(seq);
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    memcpy(pkt + 8, &tv, sizeof(tv));
    int pktLen = 8 + static_cast<int>(sizeof(tv));
    hdr->icmp_cksum = 0;
    hdr->icmp_cksum = icmpChecksum(pkt, pktLen);
    return pktLen;
}

// Parse ICMP echo reply. Checks type == ECHOREPLY only.
// ID checking is skipped: SOCK_DGRAM kernels filter for us,
// SOCK_RAW matches by sender IP. Linux rewrites ICMP ID on both socket types.
static bool parseEchoReply(const uint8_t* buf, int n, double& rttMs) {
    int offset = 0;
    if (n > 0 && (buf[0] >> 4) == 4)
        offset = (buf[0] & 0x0F) * 4;
    if (n < offset + 8) return false;
    auto* reply = (const struct icmp*)(buf + offset);
    if (reply->icmp_type != ICMP_ECHOREPLY) return false;

    if (n >= offset + 8 + static_cast<int>(sizeof(struct timeval))) {
        struct timeval sent;
        memcpy(&sent, buf + offset + 8, sizeof(sent));
        struct timeval now;
        gettimeofday(&now, nullptr);
        rttMs = (now.tv_sec - sent.tv_sec) * 1000.0 +
                (now.tv_usec - sent.tv_usec) / 1000.0;
    } else {
        rttMs = 0;
    }
    return true;
}

static int dnsTypeFromString(const std::string& type) {
    if (type == "A")     return ns_t_a;
    if (type == "AAAA")  return ns_t_aaaa;
    if (type == "MX")    return ns_t_mx;
    if (type == "TXT")   return ns_t_txt;
    if (type == "NS")    return ns_t_ns;
    if (type == "CNAME") return ns_t_cname;
    if (type == "SOA")   return ns_t_soa;
    if (type == "PTR")   return ns_t_ptr;
    if (type == "SRV")   return ns_t_srv;
    return -1;
}

static const char* dnsTypeToString(int type) {
    switch (type) {
        case ns_t_a:     return "A";
        case ns_t_aaaa:  return "AAAA";
        case ns_t_mx:    return "MX";
        case ns_t_txt:   return "TXT";
        case ns_t_ns:    return "NS";
        case ns_t_cname: return "CNAME";
        case ns_t_soa:   return "SOA";
        case ns_t_ptr:   return "PTR";
        case ns_t_srv:   return "SRV";
        default:         return "UNKNOWN";
    }
}

// Expand a compressed DNS name from a response into a dotted string
static std::string dnsExpandName(const unsigned char* msg, int msgLen, const unsigned char* ptr) {
    char name[NS_MAXDNAME];
    if (dn_expand(msg, msg + msgLen, ptr, name, sizeof(name)) < 0)
        return "";
    return std::string(name);
}

void registerNetBuiltins(std::shared_ptr<PraiaMap> netMap) {
    netMap->entries[Value("connect")] = Value(makeNative("net.connect", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.size() < 2 || args.size() > 3)
                throw RuntimeError("net.connect(host, port, timeout?) takes 2-3 arguments", 0);
            if (!args[0].isString())
                throw RuntimeError("net.connect() requires a host string", 0);
            if (!args[1].isNumber())
                throw RuntimeError("net.connect() requires a port number", 0);
            std::string host = args[0].asString();
            int port = validatePort(args[1].asNumber(), "net.connect()");
            int timeoutMs = -1; // -1 = no timeout (blocking)
            if (args.size() == 3) {
                if (!args[2].isNumber())
                    throw RuntimeError("net.connect() timeout must be a number (ms)", 0);
                timeoutMs = static_cast<int>(args[2].asNumber());
                if (timeoutMs < 0)
                    throw RuntimeError("net.connect() timeout must be >= 0", 0);
            }

            struct addrinfo hints = {};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            praia::AddrGuard ag;
            if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &ag.res) != 0)
                throw RuntimeError("Cannot resolve host: " + host, 0);

            int sock = -1;
            for (auto* p = ag.get(); p; p = p->ai_next) {
                sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
                if (sock < 0) continue;

                if (timeoutMs < 0) {
                    // No timeout — blocking connect
                    if (connect(sock, p->ai_addr, p->ai_addrlen) == 0) break;
                    close(sock);
                    sock = -1;
                    continue;
                }

                // Non-blocking connect with timeout
                int flags = fcntl(sock, F_GETFL, 0);
                fcntl(sock, F_SETFL, flags | O_NONBLOCK);

                int rc = connect(sock, p->ai_addr, p->ai_addrlen);
                if (rc == 0) {
                    // Connected immediately
                    fcntl(sock, F_SETFL, flags);
                    break;
                }
                if (errno != EINPROGRESS) {
                    close(sock);
                    sock = -1;
                    continue;
                }

                // Wait for connection with poll, checking for SIGINT
                struct pollfd pfd = {sock, POLLOUT, 0};
                int remaining = timeoutMs;
                int connectErr = ETIMEDOUT; // default if we exhaust remaining
                bool connected = false;
                for (;;) {
                    int chunk = (remaining > 100) ? 100 : remaining;
                    int pr = poll(&pfd, 1, chunk);
                    if (pr > 0) {
                        int err = 0;
                        socklen_t errlen = sizeof(err);
                        getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &errlen);
                        if (err == 0) { connected = true; }
                        else { connectErr = err; }
                        break;
                    }
                    if (pr < 0 && errno != EINTR) { connectErr = errno; break; }
                    if (pr == 0) remaining -= chunk;
                    if (remaining <= 0) break;
                    if (g_pendingSignals.load(std::memory_order_relaxed) & (1u << SIGINT)) {
                        close(sock);
                        throw RuntimeError("Interrupted", 0);
                    }
                }

                if (connected) {
                    fcntl(sock, F_SETFL, flags); // restore blocking
                    break;
                }
                close(sock);
                sock = -1;
                // If we got a definitive error (not timeout), try next address
                // If timeout, no point trying other addresses
                if (connectErr == ETIMEDOUT) {
                    throw RuntimeError("Connection timed out: " + host + ":" + std::to_string(port), 0);
                }
                errno = connectErr; // for sysErr() if this was the last address
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
            {
                praia::FdGuard fd(socket(AF_INET6, SOCK_STREAM, 0));
                if (fd) {
                    praia::setCloexec(fd.get());
                    int opt = 1;
                    setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
                    int v6only = 0; // dual-stack: accept IPv4-mapped addresses
                    setsockopt(fd.get(), IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));

                    struct sockaddr_in6 addr = {};
                    addr.sin6_family = AF_INET6;
                    addr.sin6_addr = in6addr_any;
                    addr.sin6_port = htons(port);

                    if (bind(fd.get(), (struct sockaddr*)&addr, sizeof(addr)) == 0 &&
                        ::listen(fd.get(), 64) == 0) {
                        return Value(static_cast<int64_t>(fd.release()));
                    }
                    // FdGuard closes on scope exit
                }
            }
            // Fallback: IPv4 only
            praia::FdGuard fd(socket(AF_INET, SOCK_STREAM, 0));
            if (!fd)
                throw RuntimeError(sysErr("Cannot create socket"), 0);
            praia::setCloexec(fd.get());
            int opt = 1;
            setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

            struct sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(port);

            if (bind(fd.get(), (struct sockaddr*)&addr, sizeof(addr)) < 0)
                throw RuntimeError(sysErr("Cannot bind to port " + std::to_string(port)), 0);
            if (::listen(fd.get(), 64) < 0)
                throw RuntimeError(sysErr("Cannot listen on port " + std::to_string(port)), 0);
            return Value(static_cast<int64_t>(fd.release()));
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
            praia::setCloexec(client);
            return Value(static_cast<int64_t>(client));
        }));

    // net.localAddr(fd) → {host, port}. Wraps getsockname(2). Used to
    // discover the port chosen when binding with port=0 — tests want
    // ephemeral binding so they don't collide with whatever's running.
    netMap->entries[Value("localAddr")] = Value(makeNative("net.localAddr", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber())
                throw RuntimeError("net.localAddr() requires a socket fd", 0);
            int fd = static_cast<int>(args[0].asNumber());
            struct sockaddr_storage ss;
            socklen_t sl = sizeof(ss);
            if (getsockname(fd, (struct sockaddr*)&ss, &sl) < 0)
                throw RuntimeError(sysErr("getsockname failed"), 0);
            char addrBuf[INET6_ADDRSTRLEN] = {};
            int port = 0;
            if (ss.ss_family == AF_INET) {
                auto* s = reinterpret_cast<sockaddr_in*>(&ss);
                inet_ntop(AF_INET, &s->sin_addr, addrBuf, sizeof(addrBuf));
                port = ntohs(s->sin_port);
            } else if (ss.ss_family == AF_INET6) {
                auto* s = reinterpret_cast<sockaddr_in6*>(&ss);
                inet_ntop(AF_INET6, &s->sin6_addr, addrBuf, sizeof(addrBuf));
                port = ntohs(s->sin6_port);
                if (IN6_IS_ADDR_V4MAPPED(&s->sin6_addr)) {
                    inet_ntop(AF_INET, &s->sin6_addr.s6_addr[12], addrBuf, sizeof(addrBuf));
                }
            } else {
                throw RuntimeError("net.localAddr(): unknown address family", 0);
            }
            auto result = gcNew<PraiaMap>();
            result->entries[Value("host")] = Value(std::string(addrBuf));
            result->entries[Value("port")] = Value(static_cast<int64_t>(port));
            return Value(result);
        }));

    netMap->entries[Value("send")] = Value(makeNative("net.send", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber())
                throw RuntimeError("net.send() requires a socket", 0);
            if (!args[1].isString())
                throw RuntimeError("net.send() requires a string", 0);
            int fd = static_cast<int>(args[0].asNumber());
            auto& data = args[1].asString();
            ssize_t sent;
#ifdef HAVE_OPENSSL
            if (auto* tls = lookupTls(fd)) {
                sent = SSL_write(tls->ssl, data.c_str(), static_cast<int>(data.size()));
                if (sent <= 0) {
                    int err = SSL_get_error(tls->ssl, static_cast<int>(sent));
                    if (err == SSL_ERROR_WANT_WRITE ||
                        (err == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK)))
                        throw RuntimeError("TLS send timed out", 0);
                    throw RuntimeError("TLS send failed (SSL error " + std::to_string(err) + ")", 0);
                }
            } else
#endif
            {
                sent = ::send(fd, data.c_str(), data.size(), 0);
                if (sent < 0)
                    throw RuntimeError(sysErr("Send failed"), 0);
            }
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
            ssize_t n;
#ifdef HAVE_OPENSSL
            if (auto* tls = lookupTls(fd)) {
                n = SSL_read(tls->ssl, buf.data(), static_cast<int>(buf.size()));
                if (n <= 0) {
                    int err = SSL_get_error(tls->ssl, static_cast<int>(n));
                    if (err == SSL_ERROR_ZERO_RETURN) return Value(std::string(""));
                    // Timeout: WANT_READ (OpenSSL 1.x) or SYSCALL+EAGAIN (OpenSSL 3.x)
                    if (err == SSL_ERROR_WANT_READ) return Value(std::string(""));
                    if (err == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK))
                        return Value(std::string(""));
                    throw RuntimeError("TLS recv failed (SSL error " + std::to_string(err) + ")", 0);
                }
            } else
#endif
            {
                n = ::recv(fd, buf.data(), buf.size(), 0);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        return Value(std::string("")); // timeout — not an error
                    throw RuntimeError(sysErr("Recv failed"), 0);
                }
            }
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
#ifdef HAVE_OPENSSL
            if (auto* tls = lookupTls(fd)) {
                while ((n = SSL_read(tls->ssl, buf, sizeof(buf))) > 0)
                    data.append(buf, n);
            } else
#endif
            {
                while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0)
                    data.append(buf, n);
            }
            return Value(std::move(data));
        }));

    netMap->entries[Value("close")] = Value(makeNative("net.close", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber())
                throw RuntimeError("net.close() requires a socket", 0);
            int fd = static_cast<int>(args[0].asNumber());
#ifdef HAVE_OPENSSL
            if (fd < 0) {
                closeTls(fd); // no-op if not found
                return Value();
            }
#endif
            ::close(fd);
            return Value();
        }));

    // ── TLS ──

    // net.tls(sock, hostname?) — wrap a TCP socket with TLS.
    // Returns a TLS handle (negative integer) that works with send/recv/close.
    // hostname enables SNI and certificate verification.
    // net.tls(sock, hostname, options?) -> TLS handle
    //
    // Default secure: requires a hostname (used for both SNI and certificate
    // verification). To intentionally skip verification (e.g. for banner
    // grabbing or other pentesting stuff), pass {verify: false} explicitly. An empty
    // hostname is only allowed alongside {verify: false}.
    netMap->entries[Value("tls")] = Value(makeNative("net.tls", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty() || args.size() > 3)
                throw RuntimeError("net.tls(sock, hostname, options?) takes 2-3 arguments", 0);
            if (!args[0].isNumber())
                throw RuntimeError("net.tls() requires a socket", 0);
            int fd = static_cast<int>(args[0].asNumber());
            if (fd < 0)
                throw RuntimeError("net.tls() requires a plain TCP socket, not a TLS handle", 0);

            if (args.size() < 2)
                throw RuntimeError("net.tls(): hostname required for verification "
                                   "(or pass an empty hostname with {verify: false} to opt out)", 0);
            if (!args[1].isString())
                throw RuntimeError("net.tls() hostname must be a string", 0);
            std::string hostname = args[1].asString();

            bool verify = true;
            if (args.size() == 3) {
                if (!args[2].isMap())
                    throw RuntimeError("net.tls() options must be a map", 0);
                auto& opts = args[2].asMap()->entries;
                auto it = opts.find(Value(std::string("verify")));
                if (it != opts.end()) {
                    if (!it->second.isBool())
                        throw RuntimeError("net.tls() options.verify must be a bool", 0);
                    verify = it->second.asBool();
                }
            }

            if (hostname.empty() && verify)
                throw RuntimeError("net.tls(): hostname required for verification "
                                   "(or pass {verify: false} to opt out)", 0);
#ifdef HAVE_OPENSSL
            ensureSSLInit();
            praia::SslCtxGuard ctx(SSL_CTX_new(TLS_client_method()));
            if (!ctx)
                throw RuntimeError("net.tls(): failed to create SSL context", 0);
            SSL_CTX_set_default_verify_paths(ctx.get());
            SSL_CTX_set_verify(ctx.get(), verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);

            praia::SslGuard ssl(SSL_new(ctx.get()));
            if (!ssl)
                throw RuntimeError("net.tls(): failed to create SSL object", 0);
            SSL_set_fd(ssl.get(), fd);
            // SNI uses hostname even when verify=false (most servers require it).
            if (!hostname.empty()) {
                SSL_set_tlsext_host_name(ssl.get(), hostname.c_str()); // SNI
                if (verify)
                    SSL_set1_host(ssl.get(), hostname.c_str());        // hostname verification
            }

            if (SSL_connect(ssl.get()) <= 0) {
                unsigned long err = ERR_get_error();
                char errBuf[256];
                ERR_error_string_n(err, errBuf, sizeof(errBuf));
                throw RuntimeError("TLS handshake failed: " + std::string(errBuf), 0);
            }

            if (verify) {
                long vr = SSL_get_verify_result(ssl.get());
                if (vr != X509_V_OK) {
                    std::string reason = X509_verify_cert_error_string(vr);
                    SSL_shutdown(ssl.get());
                    throw RuntimeError("TLS certificate verification failed: " + reason, 0);
                }
                // Verify hostname matches certificate CN/SAN
                X509* cert = SSL_get_peer_certificate(ssl.get());
                if (!cert) {
                    SSL_shutdown(ssl.get());
                    throw RuntimeError("TLS: server presented no certificate", 0);
                }
                int match = X509_check_host(cert, hostname.c_str(), hostname.size(), 0, nullptr);
                X509_free(cert);
                if (match != 1) {
                    SSL_shutdown(ssl.get());
                    throw RuntimeError("TLS hostname mismatch: certificate does not match '" + hostname + "'", 0);
                }
            }

            TlsConn conn;
            conn.ssl = ssl.release();
            conn.ctx = ctx.release();
            conn.fd = fd;
            return Value(static_cast<int64_t>(registerTls(conn)));
#else
            (void)fd; (void)hostname; (void)verify;
            throw RuntimeError("net.tls() requires OpenSSL (build with HAVE_OPENSSL)", 0);
#endif
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
            {
                praia::FdGuard sock(socket(AF_INET6, SOCK_DGRAM, 0));
                if (sock) {
                    int opt = 1;
                    setsockopt(sock.get(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
                    int v6only = 0;
                    setsockopt(sock.get(), IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
                    struct sockaddr_in6 addr = {};
                    addr.sin6_family = AF_INET6;
                    addr.sin6_addr = in6addr_any;
                    addr.sin6_port = htons(port);
                    if (bind(sock.get(), (struct sockaddr*)&addr, sizeof(addr)) == 0)
                        return Value(static_cast<int64_t>(sock.release()));
                    // FdGuard closes on scope exit
                }
            }
            // Fallback to IPv4
            praia::FdGuard sock(socket(AF_INET, SOCK_DGRAM, 0));
            if (!sock)
                throw RuntimeError(sysErr("Cannot create UDP socket"), 0);
            int opt = 1;
            setsockopt(sock.get(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            struct sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(port);
            if (bind(sock.get(), (struct sockaddr*)&addr, sizeof(addr)) < 0)
                throw RuntimeError(sysErr("Cannot bind UDP to port " + std::to_string(port)), 0);
            return Value(static_cast<int64_t>(sock.release()));
        }));

    // net.bindInterface(sock, ifname) — bind a socket to a specific network interface.
    // Must be called immediately after net.udp(), before any send/recv.
    // On Linux, requires root or CAP_NET_RAW for SO_BINDTODEVICE.
    // On macOS/BSD, binds to the interface's IPv4 address.
    netMap->entries[Value("bindInterface")] = Value(makeNative("net.bindInterface", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber()) throw RuntimeError("net.bindInterface() requires a socket", 0);
            if (!args[1].isString()) throw RuntimeError("net.bindInterface() requires an interface name", 0);
            int sock = static_cast<int>(args[0].asNumber());
            std::string ifname = args[1].asString();

#ifdef __linux__
            // Linux: use SO_BINDTODEVICE (requires root or CAP_NET_RAW)
            if (setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, ifname.c_str(), static_cast<socklen_t>(ifname.size() + 1)) < 0) {
                if (errno == EPERM)
                    throw RuntimeError("net.bindInterface() requires root or CAP_NET_RAW on Linux for '" + ifname + "'", 0);
                throw RuntimeError(sysErr("net.bindInterface() failed for '" + ifname + "'"), 0);
            }
#else
            // macOS/BSD: look up the interface's IPv4 address and bind to it
            struct ifaddrs* ifas = nullptr;
            if (getifaddrs(&ifas) != 0)
                throw RuntimeError(sysErr("net.bindInterface() getifaddrs failed"), 0);
            bool found = false;
            for (auto* ifa = ifas; ifa; ifa = ifa->ifa_next) {
                if (ifa->ifa_name && ifname == ifa->ifa_name &&
                    ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET) {
                    if (bind(sock, ifa->ifa_addr, sizeof(struct sockaddr_in)) < 0) {
                        int err = errno;
                        freeifaddrs(ifas);
                        if (err == EINVAL)
                            throw RuntimeError("net.bindInterface() socket already bound — call before send/recv", 0);
                        throw RuntimeError(sysErr("net.bindInterface() bind failed for '" + ifname + "'"), 0);
                    }
                    found = true;
                    break;
                }
            }
            freeifaddrs(ifas);
            if (!found)
                throw RuntimeError("net.bindInterface() interface '" + ifname + "' not found or has no IPv4 address", 0);
#endif
            return Value();
        }));

    // net.interfaces() — list network interfaces with their addresses
    netMap->entries[Value("interfaces")] = Value(makeNative("net.interfaces", 0,
        [](const std::vector<Value>&) -> Value {
            struct ifaddrs* ifas = nullptr;
            if (getifaddrs(&ifas) != 0)
                throw RuntimeError(sysErr("net.interfaces() failed"), 0);
            auto result = gcNew<PraiaArray>();
            std::set<std::string> seen;
            for (auto* ifa = ifas; ifa; ifa = ifa->ifa_next) {
                if (!ifa->ifa_name || !ifa->ifa_addr) continue;
                if (ifa->ifa_addr->sa_family != AF_INET) continue;
                std::string name = ifa->ifa_name;
                if (seen.count(name)) continue;
                seen.insert(name);
                char addrBuf[INET_ADDRSTRLEN];
                auto* sa = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
                inet_ntop(AF_INET, &sa->sin_addr, addrBuf, sizeof(addrBuf));
                auto entry = gcNew<PraiaMap>();
                entry->entries[Value("name")] = Value(name);
                entry->entries[Value("address")] = Value(std::string(addrBuf));
                bool up = (ifa->ifa_flags & IFF_UP) != 0;
                bool loopback = (ifa->ifa_flags & IFF_LOOPBACK) != 0;
                entry->entries[Value("up")] = Value(up);
                entry->entries[Value("loopback")] = Value(loopback);
                result->elements.push_back(Value(entry));
            }
            freeifaddrs(ifas);
            return Value(result);
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
            praia::AddrGuard ag;
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
                if (errno == EAGAIN || errno == EWOULDBLOCK) return Value(); // timeout → nil
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
            praia::AddrGuard ag;
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

    // net.query(name, type) — raw DNS queries
    // type: "A", "AAAA", "MX", "TXT", "NS", "CNAME", "SOA", "PTR", "SRV"
    // For PTR, pass an IP address — it's automatically converted to in-addr.arpa form.
    // Returns an array of maps, each with at least {name, type, ttl} plus type-specific fields.
    netMap->entries[Value("query")] = Value(makeNative("net.query", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("net.query() requires a name string", 0);
            if (!args[1].isString())
                throw RuntimeError("net.query() requires a type string (A, AAAA, MX, TXT, NS, CNAME, SOA, PTR, SRV)", 0);
            std::string name = args[0].asString();
            std::string typeStr = args[1].asString();
            int qtype = dnsTypeFromString(typeStr);
            if (qtype < 0)
                throw RuntimeError("net.query() unknown type: " + typeStr + " (expected A, AAAA, MX, TXT, NS, CNAME, SOA, PTR, SRV)", 0);

            // For PTR lookups, convert IP to reverse form
            std::string queryName = name;
            if (qtype == ns_t_ptr) {
                // Try IPv4 first
                struct in_addr addr4;
                struct in6_addr addr6;
                if (inet_pton(AF_INET, name.c_str(), &addr4) == 1) {
                    unsigned char* b = (unsigned char*)&addr4.s_addr;
                    queryName = std::to_string(b[3]) + "." + std::to_string(b[2]) + "." +
                                std::to_string(b[1]) + "." + std::to_string(b[0]) + ".in-addr.arpa";
                } else if (inet_pton(AF_INET6, name.c_str(), &addr6) == 1) {
                    // Build nibble-reversed ip6.arpa name
                    std::string rev;
                    for (int i = 15; i >= 0; i--) {
                        unsigned char byte = addr6.s6_addr[i];
                        char lo[3], hi[3];
                        snprintf(lo, sizeof(lo), "%x", byte & 0xf);
                        snprintf(hi, sizeof(hi), "%x", (byte >> 4) & 0xf);
                        rev += lo; rev += '.';
                        rev += hi; rev += '.';
                    }
                    rev += "ip6.arpa";
                    queryName = rev;
                }
                // If neither, assume user passed a pre-formed arpa name
            }

            unsigned char buf[4096];
            int len = res_query(queryName.c_str(), ns_c_in, qtype, buf, sizeof(buf));
            if (len < 0) {
                // HOST_NOT_FOUND (NXDOMAIN) and NO_DATA → empty array
                // TRY_AGAIN, NO_RECOVERY → throw
                if (h_errno == HOST_NOT_FOUND || h_errno == NO_DATA)
                    return Value(gcNew<PraiaArray>());
                throw RuntimeError("DNS query failed for " + name + " type " + typeStr, 0);
            }

            ns_msg msg;
            if (ns_initparse(buf, len, &msg) < 0)
                throw RuntimeError("DNS parse error for " + name, 0);

            int count = ns_msg_count(msg, ns_s_an);
            auto result = gcNew<PraiaArray>();

            for (int i = 0; i < count; i++) {
                ns_rr rr;
                if (ns_parserr(&msg, ns_s_an, i, &rr) < 0) continue;

                auto entry = gcNew<PraiaMap>();
                std::string rrName(ns_rr_name(rr));
                if (!rrName.empty() && rrName.back() == '.') rrName.pop_back();
                entry->entries[Value("name")] = Value(rrName);
                int rrtype = ns_rr_type(rr);
                entry->entries[Value("type")] = Value(std::string(dnsTypeToString(rrtype)));
                entry->entries[Value("ttl")]  = Value(static_cast<int64_t>(ns_rr_ttl(rr)));

                const unsigned char* rdata = ns_rr_rdata(rr);
                int rdlen = ns_rr_rdlen(rr);

                if (rrtype == ns_t_a && rdlen >= 4) {
                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, rdata, ip, sizeof(ip));
                    entry->entries[Value("address")] = Value(std::string(ip));

                } else if (rrtype == ns_t_aaaa && rdlen >= 16) {
                    char ip[INET6_ADDRSTRLEN];
                    inet_ntop(AF_INET6, rdata, ip, sizeof(ip));
                    entry->entries[Value("address")] = Value(std::string(ip));

                } else if (rrtype == ns_t_mx && rdlen >= 3) {
                    int pref = (rdata[0] << 8) | rdata[1];
                    std::string exchange = dnsExpandName(buf, len, rdata + 2);
                    if (!exchange.empty() && exchange.back() == '.') exchange.pop_back();
                    entry->entries[Value("priority")] = Value(static_cast<int64_t>(pref));
                    entry->entries[Value("exchange")] = Value(exchange);

                } else if (rrtype == ns_t_txt) {
                    // TXT records: one or more length-prefixed strings
                    std::string txt;
                    int off = 0;
                    while (off < rdlen) {
                        int slen = rdata[off++];
                        if (off + slen > rdlen) break;
                        if (!txt.empty()) txt += ' ';
                        txt.append(reinterpret_cast<const char*>(rdata + off), slen);
                        off += slen;
                    }
                    entry->entries[Value("text")] = Value(txt);

                } else if (rrtype == ns_t_ns || rrtype == ns_t_cname || rrtype == ns_t_ptr) {
                    std::string target = dnsExpandName(buf, len, rdata);
                    if (!target.empty() && target.back() == '.') target.pop_back();
                    const char* key = (rrtype == ns_t_ptr) ? "hostname" : "target";
                    entry->entries[Value(std::string(key))] = Value(target);

                } else if (rrtype == ns_t_soa && rdlen > 0) {
                    const unsigned char* p = rdata;
                    const unsigned char* end = buf + len;
                    std::string mname = dnsExpandName(buf, len, p);
                    int skip = dn_skipname(p, end);
                    if (skip < 0) { result->elements.push_back(Value(entry)); continue; }
                    p += skip;
                    std::string rname = dnsExpandName(buf, len, p);
                    skip = dn_skipname(p, end);
                    if (skip < 0) { result->elements.push_back(Value(entry)); continue; }
                    p += skip;
                    if (!mname.empty() && mname.back() == '.') mname.pop_back();
                    if (!rname.empty() && rname.back() == '.') rname.pop_back();
                    entry->entries[Value("mname")] = Value(mname);
                    entry->entries[Value("rname")] = Value(rname);
                    if (p + 20 <= rdata + rdlen) {
                        auto readU32 = [](const unsigned char* d) -> uint32_t {
                            return (uint32_t(d[0]) << 24) | (uint32_t(d[1]) << 16) |
                                   (uint32_t(d[2]) << 8) | d[3];
                        };
                        entry->entries[Value("serial")]  = Value(static_cast<int64_t>(readU32(p)));
                        entry->entries[Value("refresh")] = Value(static_cast<int64_t>(readU32(p + 4)));
                        entry->entries[Value("retry")]   = Value(static_cast<int64_t>(readU32(p + 8)));
                        entry->entries[Value("expire")]  = Value(static_cast<int64_t>(readU32(p + 12)));
                        entry->entries[Value("minimum")] = Value(static_cast<int64_t>(readU32(p + 16)));
                    }

                } else if (rrtype == ns_t_srv && rdlen >= 7) {
                    int priority = (rdata[0] << 8) | rdata[1];
                    int weight   = (rdata[2] << 8) | rdata[3];
                    int srvPort  = (rdata[4] << 8) | rdata[5];
                    std::string target = dnsExpandName(buf, len, rdata + 6);
                    if (!target.empty() && target.back() == '.') target.pop_back();
                    entry->entries[Value("priority")] = Value(static_cast<int64_t>(priority));
                    entry->entries[Value("weight")]   = Value(static_cast<int64_t>(weight));
                    entry->entries[Value("port")]     = Value(static_cast<int64_t>(srvPort));
                    entry->entries[Value("target")]   = Value(target);
                }

                result->elements.push_back(Value(entry));
            }

            return Value(result);
        }));

    // net.connectAll(targets, timeout) — concurrent TCP connect scan
    // targets: array of {host, port} or [host, port]
    // Uses non-blocking connect + poll() multiplexing (no threads).
    // Returns array of {host, port, open}.
    netMap->entries[Value("connectAll")] = Value(makeNative("net.connectAll", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("net.connectAll() requires an array of targets", 0);
            if (!args[1].isNumber())
                throw RuntimeError("net.connectAll() requires a timeout in ms", 0);

            auto& targets = args[0].asArray()->elements;
            int timeoutMs = static_cast<int>(args[1].asNumber());
            if (timeoutMs < 0)
                throw RuntimeError("net.connectAll() timeout must be >= 0", 0);

            struct ConnAttempt {
                std::string host;
                int port;
                int sock = -1;
                bool connected = false;
                bool done = false;
            };

            std::vector<ConnAttempt> attempts;
            attempts.reserve(targets.size());

            // Parse targets: accept {host, port} maps or [host, port] arrays
            for (auto& t : targets) {
                ConnAttempt a;
                if (t.isArray()) {
                    auto& arr = t.asArray()->elements;
                    if (arr.size() < 2 || !arr[0].isString() || !arr[1].isNumber())
                        throw RuntimeError("net.connectAll() array target must be [host, port]", 0);
                    a.host = arr[0].asString();
                    a.port = static_cast<int>(arr[1].asNumber());
                } else if (t.isMap()) {
                    auto& m = t.asMap()->entries;
                    auto hi = m.find(Value("host"));
                    auto pi = m.find(Value("port"));
                    if (hi == m.end() || pi == m.end() || !hi->second.isString() || !pi->second.isNumber())
                        throw RuntimeError("net.connectAll() map target must have host (string) and port (number)", 0);
                    a.host = hi->second.asString();
                    a.port = static_cast<int>(pi->second.asNumber());
                } else {
                    throw RuntimeError("net.connectAll() targets must be {host, port} maps or [host, port] arrays", 0);
                }
                attempts.push_back(std::move(a));
            }

            // Determine batch size from FD limit (leave headroom for other FDs)
            int batchSize = 128;
            struct rlimit rl;
            if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur > 256 && rl.rlim_cur < 100000)
                batchSize = std::min(static_cast<int>(rl.rlim_cur / 2), 512);

            for (size_t batchStart = 0; batchStart < attempts.size(); batchStart += batchSize) {
                if (g_pendingSignals.load(std::memory_order_relaxed) & (1u << SIGINT)) {
                    for (auto& a : attempts)
                        if (a.sock >= 0) { close(a.sock); a.sock = -1; }
                    throw RuntimeError("Interrupted", 0);
                }

                size_t batchEnd = std::min(batchStart + (size_t)batchSize, attempts.size());

                // Initiate non-blocking connects
                for (size_t i = batchStart; i < batchEnd; i++) {
                    auto& a = attempts[i];
                    struct addrinfo hints = {};
                    hints.ai_family = AF_UNSPEC;
                    hints.ai_socktype = SOCK_STREAM;
                    struct addrinfo* res = nullptr;
                    if (getaddrinfo(a.host.c_str(), std::to_string(a.port).c_str(), &hints, &res) != 0) {
                        a.done = true;
                        continue;
                    }
                    // Use first usable address
                    bool initiated = false;
                    for (auto* p = res; p && !initiated; p = p->ai_next) {
                        a.sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
                        if (a.sock < 0) continue;
                        int flags = fcntl(a.sock, F_GETFL, 0);
                        fcntl(a.sock, F_SETFL, flags | O_NONBLOCK);
                        int rc = connect(a.sock, p->ai_addr, p->ai_addrlen);
                        if (rc == 0) {
                            a.connected = true;
                            a.done = true;
                            close(a.sock);
                            a.sock = -1;
                            initiated = true;
                        } else if (errno == EINPROGRESS) {
                            initiated = true;
                        } else {
                            close(a.sock);
                            a.sock = -1;
                        }
                    }
                    freeaddrinfo(res);
                    if (!initiated) a.done = true;
                }

                // Poll loop — wait for all pending connects or timeout
                int remaining = timeoutMs;
                std::vector<struct pollfd> pfds;
                std::vector<size_t> pfdIdx;
                for (;;) {
                    pfds.clear();
                    pfdIdx.clear();
                    for (size_t i = batchStart; i < batchEnd; i++) {
                        if (!attempts[i].done && attempts[i].sock >= 0) {
                            pfds.push_back({attempts[i].sock, POLLOUT, 0});
                            pfdIdx.push_back(i);
                        }
                    }
                    if (pfds.empty()) break;

                    int chunk = (remaining > 100) ? 100 : remaining;
                    int pr = poll(pfds.data(), (nfds_t)pfds.size(), chunk);

                    if (pr > 0) {
                        for (size_t j = 0; j < pfds.size(); j++) {
                            if (pfds[j].revents) {
                                auto& a = attempts[pfdIdx[j]];
                                int err = 0;
                                socklen_t errlen = sizeof(err);
                                getsockopt(a.sock, SOL_SOCKET, SO_ERROR, &err, &errlen);
                                a.connected = (err == 0);
                                a.done = true;
                                close(a.sock);
                                a.sock = -1;
                            }
                        }
                    }
                    if (pr < 0 && errno != EINTR) break;
                    if (pr == 0) remaining -= chunk;
                    if (remaining <= 0) break;

                    if (g_pendingSignals.load(std::memory_order_relaxed) & (1u << SIGINT)) {
                        for (auto& a : attempts)
                            if (a.sock >= 0) { close(a.sock); a.sock = -1; }
                        throw RuntimeError("Interrupted", 0);
                    }
                }

                // Clean up timed-out sockets
                for (size_t i = batchStart; i < batchEnd; i++) {
                    if (attempts[i].sock >= 0) {
                        close(attempts[i].sock);
                        attempts[i].sock = -1;
                        attempts[i].done = true;
                    }
                }
            }

            auto result = gcNew<PraiaArray>();
            for (auto& a : attempts) {
                auto entry = gcNew<PraiaMap>();
                entry->entries[Value("host")] = Value(a.host);
                entry->entries[Value("port")] = Value(static_cast<int64_t>(a.port));
                entry->entries[Value("open")] = Value(a.connected);
                result->elements.push_back(Value(entry));
            }
            return Value(result);
        }));

    netMap->entries[Value("setTimeout")] = Value(makeNative("net.setTimeout", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isNumber() || !args[1].isNumber())
                throw RuntimeError("net.setTimeout(socket, ms) requires two numbers", 0);
            int sock = static_cast<int>(args[0].asNumber());
            int ms = static_cast<int>(args[1].asNumber());
            // For TLS handles, set timeout on the underlying TCP socket
            int rawFd = sock;
#ifdef HAVE_OPENSSL
            if (auto* tls = lookupTls(sock)) rawFd = tls->fd;
#endif
            struct timeval tv;
            tv.tv_sec = ms / 1000;
            tv.tv_usec = (ms % 1000) * 1000;
            setsockopt(rawFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(rawFd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
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
            praia::AddrGuard ag;
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

    // ── ICMP Ping ──

    // net.ping(host, timeout?) — send ICMP echo request, return {alive, rtt}
    // timeout defaults to 1500ms. Unprivileged on macOS, needs root on Linux.
    netMap->entries[Value("ping")] = Value(makeNative("net.ping", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty() || args.size() > 2)
                throw RuntimeError("net.ping(host, timeout?) takes 1-2 arguments", 0);
            if (!args[0].isString())
                throw RuntimeError("net.ping() requires a host string", 0);
            std::string host = args[0].asString();
            int timeoutMs = 1500;
            if (args.size() == 2) {
                if (!args[1].isNumber())
                    throw RuntimeError("net.ping() timeout must be a number (ms)", 0);
                timeoutMs = static_cast<int>(args[1].asNumber());
                if (timeoutMs < 0)
                    throw RuntimeError("net.ping() timeout must be >= 0", 0);
            }

            // Resolve host to IPv4
            struct sockaddr_in dest = {};
            dest.sin_family = AF_INET;
            if (inet_pton(AF_INET, host.c_str(), &dest.sin_addr) != 1) {
                struct addrinfo hints = {};
                hints.ai_family = AF_INET;
                praia::AddrGuard ag;
                if (getaddrinfo(host.c_str(), nullptr, &hints, &ag.res) != 0 || !ag.get())
                    throw RuntimeError("net.ping(): cannot resolve host: " + host, 0);
                auto* addr = (struct sockaddr_in*)ag.get()->ai_addr;
                dest.sin_addr = addr->sin_addr;
            }

            auto result = gcNew<PraiaMap>();
            result->entries[Value("alive")] = Value(false);

            int sock = createIcmpSocket();
            bool isRaw = g_icmpIsRaw;
            uint16_t id = static_cast<uint16_t>(getpid() & 0xFFFF);

            uint8_t pkt[64] = {};
            int pktLen = buildEchoRequest(pkt, id, 1);

            if (sendto(sock, pkt, pktLen, 0, (struct sockaddr*)&dest, sizeof(dest)) < 0) {
                close(sock);
                return Value(result);
            }

            struct pollfd pfd = {sock, POLLIN, 0};
            struct timeval deadline;
            gettimeofday(&deadline, nullptr);
            for (;;) {
                int remaining = timeoutMs - msElapsed(deadline);
                if (remaining <= 0) break;
                int chunk = (remaining > 100) ? 100 : remaining;
                int pr = poll(&pfd, 1, chunk);
                if (pr > 0) {
                    uint8_t buf[256];
                    struct sockaddr_in from = {};
                    socklen_t fromLen = sizeof(from);
                    int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*)&from, &fromLen);
                    double rtt = 0;
                    if (n > 0 && parseEchoReply(buf, n, rtt)) {
                        // SOCK_RAW gets all ICMP — verify sender matches target
                        if (!isRaw || from.sin_addr.s_addr == dest.sin_addr.s_addr) {
                            result->entries[Value("alive")] = Value(true);
                            result->entries[Value("rtt")] = Value(rtt);
                            break;
                        }
                    }
                    continue;
                }
                if (pr < 0 && errno != EINTR) break;
                if (g_pendingSignals.load(std::memory_order_relaxed) & (1u << SIGINT)) {
                    close(sock);
                    throw RuntimeError("Interrupted", 0);
                }
            }

            close(sock);
            return Value(result);
        }));

    // net.pingAll(hosts, timeout?) — concurrent ICMP ping sweep
    // hosts: array of IP/hostname strings. timeout defaults to 1500ms.
    // Returns array of {host, alive, rtt?}
    netMap->entries[Value("pingAll")] = Value(makeNative("net.pingAll", -1,
        [](const std::vector<Value>& args) -> Value {
            if (args.empty() || args.size() > 2)
                throw RuntimeError("net.pingAll(hosts, timeout?) takes 1-2 arguments", 0);
            if (!args[0].isArray())
                throw RuntimeError("net.pingAll() requires an array of host strings", 0);
            auto& hosts = args[0].asArray()->elements;
            int timeoutMs = 1500;
            if (args.size() == 2) {
                if (!args[1].isNumber())
                    throw RuntimeError("net.pingAll() timeout must be a number (ms)", 0);
                timeoutMs = static_cast<int>(args[1].asNumber());
                if (timeoutMs < 0)
                    throw RuntimeError("net.pingAll() timeout must be >= 0", 0);
            }

            struct PingTarget {
                std::string host;
                struct sockaddr_in addr = {};
                bool resolved = false;
                bool alive = false;
                double rtt = 0;
            };

            std::vector<PingTarget> targets;
            targets.reserve(hosts.size());
            for (auto& h : hosts) {
                if (!h.isString())
                    throw RuntimeError("net.pingAll() hosts must be strings", 0);
                PingTarget t;
                t.host = h.asString();
                t.addr.sin_family = AF_INET;
                if (inet_pton(AF_INET, t.host.c_str(), &t.addr.sin_addr) == 1) {
                    t.resolved = true;
                } else {
                    struct addrinfo hints = {};
                    hints.ai_family = AF_INET;
                    struct addrinfo* res = nullptr;
                    if (getaddrinfo(t.host.c_str(), nullptr, &hints, &res) == 0 && res) {
                        t.addr.sin_addr = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
                        t.resolved = true;
                        freeaddrinfo(res);
                    }
                }
                targets.push_back(std::move(t));
            }

            // Single ICMP socket for all pings
            int sock = createIcmpSocket("net.pingAll()");
            bool isRaw = g_icmpIsRaw;
            uint16_t baseId = static_cast<uint16_t>(getpid() & 0xFFFF);

            // Build address lookup for SOCK_RAW (match replies by sender IP)
            std::unordered_map<uint32_t, size_t> addrToIdx;
            for (size_t i = 0; i < targets.size(); i++) {
                if (!targets[i].resolved) continue;
                addrToIdx[targets[i].addr.sin_addr.s_addr] = i;
            }

            // Send all echo requests
            for (size_t i = 0; i < targets.size(); i++) {
                if (!targets[i].resolved) continue;
                uint8_t pkt[64] = {};
                int pktLen = buildEchoRequest(pkt, baseId, static_cast<uint16_t>(i + 1));
                sendto(sock, pkt, pktLen, 0,
                       (struct sockaddr*)&targets[i].addr, sizeof(targets[i].addr));
            }

            // Receive replies
            struct pollfd pfd = {sock, POLLIN, 0};
            struct timeval deadline;
            gettimeofday(&deadline, nullptr);
            size_t repliesNeeded = 0;
            for (auto& t : targets) if (t.resolved) repliesNeeded++;
            size_t repliesGot = 0;

            for (;;) {
                if (repliesGot >= repliesNeeded) break;
                int remaining = timeoutMs - msElapsed(deadline);
                if (remaining <= 0) break;
                int chunk = (remaining > 100) ? 100 : remaining;
                int pr = poll(&pfd, 1, chunk);
                if (pr > 0) {
                    uint8_t buf[256];
                    struct sockaddr_in from = {};
                    socklen_t fromLen = sizeof(from);
                    int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*)&from, &fromLen);
                    if (n <= 0) continue;

                    // Find ICMP header (skip IP header if present)
                    int offset = 0;
                    if ((buf[0] >> 4) == 4) offset = (buf[0] & 0x0F) * 4;
                    if (n < offset + 8) continue;
                    auto* reply = (const struct icmp*)(buf + offset);
                    if (reply->icmp_type != ICMP_ECHOREPLY) continue;

                    // Identify which target replied
                    size_t idx;
                    if (isRaw) {
                        // SOCK_RAW: receives all ICMP, match by sender IP
                        auto it = addrToIdx.find(from.sin_addr.s_addr);
                        if (it == addrToIdx.end()) continue;
                        idx = it->second;
                    } else {
                        // SOCK_DGRAM: kernel filters to our socket, use seq
                        uint16_t seq = ntohs(reply->icmp_seq);
                        if (seq < 1 || seq > targets.size()) continue;
                        idx = seq - 1;
                    }

                    auto& t = targets[idx];
                    if (t.alive) continue; // duplicate

                    if (n >= offset + 8 + static_cast<int>(sizeof(struct timeval))) {
                        struct timeval sent;
                        memcpy(&sent, buf + offset + 8, sizeof(sent));
                        struct timeval now;
                        gettimeofday(&now, nullptr);
                        t.rtt = (now.tv_sec - sent.tv_sec) * 1000.0 +
                                (now.tv_usec - sent.tv_usec) / 1000.0;
                    }
                    t.alive = true;
                    repliesGot++;
                    continue;
                }
                if (pr < 0 && errno != EINTR) break;
                if (g_pendingSignals.load(std::memory_order_relaxed) & (1u << SIGINT)) {
                    close(sock);
                    throw RuntimeError("Interrupted", 0);
                }
            }

            close(sock);

            auto result = gcNew<PraiaArray>();
            for (auto& t : targets) {
                auto entry = gcNew<PraiaMap>();
                entry->entries[Value("host")] = Value(t.host);
                entry->entries[Value("alive")] = Value(t.alive);
                if (t.alive)
                    entry->entries[Value("rtt")] = Value(t.rtt);
                result->elements.push_back(Value(entry));
            }
            return Value(result);
        }));
}
