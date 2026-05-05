#pragma once

// Move-only RAII guards for resources used across the builtins. Each guard
// owns its resource and releases it on destruction unless `release()` was
// called first to hand ownership to a caller.

#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>

#ifdef HAVE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#endif

namespace praia {

// Mark a file descriptor as close-on-exec. Anything that fork()+exec()s
// (sys.exec, sys.spawn, native plugins) must not inherit unrelated
// long-lived fds — listen sockets, accepted clients, parent-side pipe ends.
// Linux's SOCK_CLOEXEC / O_CLOEXEC / pipe2() can do this atomically; macOS
// can't, so we use fcntl() universally for portability.
inline void setCloexec(int fd) {
    if (fd < 0) return;
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

// Owns a file descriptor; close()s on destruction unless released.
class FdGuard {
    int fd_;
public:
    explicit FdGuard(int fd = -1) : fd_(fd) {}
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
    FdGuard(FdGuard&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    FdGuard& operator=(FdGuard&& other) noexcept {
        if (this != &other) { reset(); fd_ = other.fd_; other.fd_ = -1; }
        return *this;
    }
    ~FdGuard() { reset(); }
    int get() const { return fd_; }
    int release() { int t = fd_; fd_ = -1; return t; }
    void reset(int fd = -1) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = fd;
    }
    explicit operator bool() const { return fd_ >= 0; }
};

// Owns an addrinfo* from getaddrinfo(); freeaddrinfo()s on destruction.
struct AddrGuard {
    addrinfo* res = nullptr;
    AddrGuard() = default;
    AddrGuard(const AddrGuard&) = delete;
    AddrGuard& operator=(const AddrGuard&) = delete;
    AddrGuard(AddrGuard&& other) noexcept : res(other.res) { other.res = nullptr; }
    AddrGuard& operator=(AddrGuard&& other) noexcept {
        if (this != &other) {
            if (res) freeaddrinfo(res);
            res = other.res;
            other.res = nullptr;
        }
        return *this;
    }
    ~AddrGuard() { if (res) freeaddrinfo(res); }
    addrinfo* operator->() const { return res; }
    addrinfo* get() const { return res; }
};

#ifdef HAVE_OPENSSL

// Owns an SSL_CTX*; SSL_CTX_free()s on destruction unless released.
class SslCtxGuard {
    SSL_CTX* p_;
public:
    explicit SslCtxGuard(SSL_CTX* p = nullptr) : p_(p) {}
    SslCtxGuard(const SslCtxGuard&) = delete;
    SslCtxGuard& operator=(const SslCtxGuard&) = delete;
    SslCtxGuard(SslCtxGuard&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    SslCtxGuard& operator=(SslCtxGuard&& o) noexcept {
        if (this != &o) { if (p_) SSL_CTX_free(p_); p_ = o.p_; o.p_ = nullptr; }
        return *this;
    }
    ~SslCtxGuard() { if (p_) SSL_CTX_free(p_); }
    SSL_CTX* get() const { return p_; }
    SSL_CTX* release() { auto t = p_; p_ = nullptr; return t; }
    explicit operator bool() const { return p_ != nullptr; }
};

// Owns an SSL*; SSL_free()s on destruction unless released.
class SslGuard {
    SSL* p_;
public:
    explicit SslGuard(SSL* p = nullptr) : p_(p) {}
    SslGuard(const SslGuard&) = delete;
    SslGuard& operator=(const SslGuard&) = delete;
    SslGuard(SslGuard&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    SslGuard& operator=(SslGuard&& o) noexcept {
        if (this != &o) { if (p_) SSL_free(p_); p_ = o.p_; o.p_ = nullptr; }
        return *this;
    }
    ~SslGuard() { if (p_) SSL_free(p_); }
    SSL* get() const { return p_; }
    SSL* release() { auto t = p_; p_ = nullptr; return t; }
    explicit operator bool() const { return p_ != nullptr; }
};

// Generic move-only guard for an OpenSSL pointer with a free function.
// Specializations below cover EVP_CIPHER_CTX, EVP_MD_CTX, EVP_PKEY,
// EVP_PKEY_CTX, and BIO. Use the typedefs at the bottom rather than
// instantiating directly.
template <typename T, void (*Free)(T*)>
class OsslPtrGuard {
    T* p_;
public:
    explicit OsslPtrGuard(T* p = nullptr) : p_(p) {}
    OsslPtrGuard(const OsslPtrGuard&) = delete;
    OsslPtrGuard& operator=(const OsslPtrGuard&) = delete;
    OsslPtrGuard(OsslPtrGuard&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    OsslPtrGuard& operator=(OsslPtrGuard&& o) noexcept {
        if (this != &o) { if (p_) Free(p_); p_ = o.p_; o.p_ = nullptr; }
        return *this;
    }
    ~OsslPtrGuard() { if (p_) Free(p_); }
    T* get() const { return p_; }
    T* release() { auto t = p_; p_ = nullptr; return t; }
    explicit operator bool() const { return p_ != nullptr; }
};

// BIO_free returns int; wrap it so it matches the void(*)(T*) signature.
inline void osslBioFree(BIO* b) { BIO_free(b); }

using EvpCipherCtxGuard = OsslPtrGuard<EVP_CIPHER_CTX, EVP_CIPHER_CTX_free>;
using EvpMdCtxGuard     = OsslPtrGuard<EVP_MD_CTX,     EVP_MD_CTX_free>;
using EvpPkeyGuard      = OsslPtrGuard<EVP_PKEY,       EVP_PKEY_free>;
using EvpPkeyCtxGuard   = OsslPtrGuard<EVP_PKEY_CTX,   EVP_PKEY_CTX_free>;
using BioGuard          = OsslPtrGuard<BIO,            osslBioFree>;

#endif // HAVE_OPENSSL

} // namespace praia
