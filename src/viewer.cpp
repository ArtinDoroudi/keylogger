#include "viewer.hpp"

#include "net_common.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
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

namespace viewer {

namespace {

std::atomic<bool> g_run{true};

void sigHandler(int) { g_run = false; }

#ifdef _WIN32
struct WsaInit {
    WsaInit()  { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
    ~WsaInit() { WSACleanup(); }
};
static WsaInit g_wsaInit;
#endif

// ---- I/O abstraction over plain TCP and TLS -------------------------------

class Stream {
public:
    virtual ~Stream() = default;
    virtual int  read(char* buf, int n) = 0;     // -1 on error/close
    virtual bool write(const char* buf, int n) = 0;
    virtual void close() = 0;
};

class PlainStream : public Stream {
public:
    explicit PlainStream(socket_t s) : m_s(s) {}
    ~PlainStream() override { close(); }
    int read(char* buf, int n) override {
        int r = ::recv(m_s, buf, n, 0);
        return r;
    }
    bool write(const char* buf, int n) override {
        int off = 0;
        while (off < n) {
            int w = ::send(m_s, buf + off, n - off, 0);
            if (w <= 0) {
#ifdef _WIN32
                int err = WSAGetLastError();
                if (err == WSAEINTR) continue;
#else
                if (errno == EINTR) continue;
#endif
                return false;
            }
            off += w;
        }
        return true;
    }
    void close() override {
        if (m_s != INVALID_SOCK) { CLOSESOCK(m_s); m_s = INVALID_SOCK; }
    }
private:
    socket_t m_s;
};

#ifdef KEYLOGGER_HAS_TLS
class TlsStream : public Stream {
public:
    TlsStream(SSL* ssl, socket_t s) : m_ssl(ssl), m_s(s) {}
    ~TlsStream() override { close(); }
    int read(char* buf, int n) override {
        int r = SSL_read(m_ssl, buf, n);
        if (r <= 0) return -1;
        return r;
    }
    bool write(const char* buf, int n) override {
        int off = 0;
        while (off < n) {
            int w = SSL_write(m_ssl, buf + off, n - off);
            if (w <= 0) return false;
            off += w;
        }
        return true;
    }
    void close() override {
        if (m_ssl) { SSL_shutdown(m_ssl); SSL_free(m_ssl); m_ssl = nullptr; }
        if (m_s != INVALID_SOCK) { CLOSESOCK(m_s); m_s = INVALID_SOCK; }
    }
private:
    SSL*     m_ssl;
    socket_t m_s;
};
#endif

// ---- Broadcast channel for SSE clients ------------------------------------

struct SseClient {
    std::shared_ptr<Stream>             stream;
    std::mutex                          writeMu;
    std::atomic<bool>                   alive{true};
};

class Broadcaster {
public:
    void add(const std::shared_ptr<SseClient>& c) {
        std::lock_guard<std::mutex> lk(m_mu);
        m_clients.insert(c);
    }
    void remove(const std::shared_ptr<SseClient>& c) {
        std::lock_guard<std::mutex> lk(m_mu);
        m_clients.erase(c);
    }
    size_t count() {
        std::lock_guard<std::mutex> lk(m_mu);
        return m_clients.size();
    }
    void publish(const std::string& jsonLine) {
        // Build SSE frame: "data: <line>\n\n"
        std::string frame = "data: " + jsonLine + "\n\n";
        std::vector<std::shared_ptr<SseClient>> snap;
        {
            std::lock_guard<std::mutex> lk(m_mu);
            snap.assign(m_clients.begin(), m_clients.end());
        }
        for (auto& c : snap) {
            if (!c->alive.load()) continue;
            std::lock_guard<std::mutex> lk(c->writeMu);
            if (!c->stream->write(frame.data(), static_cast<int>(frame.size()))) {
                c->alive = false;
            }
        }
    }
private:
    std::mutex                              m_mu;
    std::set<std::shared_ptr<SseClient>>    m_clients;
};

// ---- HTTP request parsing -------------------------------------------------

struct Request {
    std::string method;
    std::string path;
    std::string version;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    std::string header(const std::string& name) const {
        for (auto& kv : headers) {
            if (kv.first.size() == name.size()) {
                bool eq = true;
                for (size_t i = 0; i < name.size(); ++i) {
                    if (std::tolower((unsigned char)kv.first[i]) !=
                        std::tolower((unsigned char)name[i])) { eq = false; break; }
                }
                if (eq) return kv.second;
            }
        }
        return {};
    }
};

bool readUntil(Stream& s, std::string& buf, const std::string& sentinel,
               size_t maxBytes) {
    char tmp[1024];
    while (buf.find(sentinel) == std::string::npos) {
        int n = s.read(tmp, sizeof(tmp));
        if (n <= 0) return false;
        buf.append(tmp, tmp + n);
        if (buf.size() > maxBytes) return false;
    }
    return true;
}

bool parseRequest(Stream& s, Request& req, std::string& rawHeaders) {
    if (!readUntil(s, rawHeaders, "\r\n\r\n", 64 * 1024)) return false;
    auto headerEnd = rawHeaders.find("\r\n\r\n");
    std::string head = rawHeaders.substr(0, headerEnd);
    std::string remainder = rawHeaders.substr(headerEnd + 4);
    rawHeaders = remainder;  // any body bytes already pulled in

    auto firstEol = head.find("\r\n");
    if (firstEol == std::string::npos) return false;
    std::string startLine = head.substr(0, firstEol);
    std::istringstream sl(startLine);
    if (!(sl >> req.method >> req.path >> req.version)) return false;

    std::istringstream is(head.substr(firstEol + 2));
    std::string line;
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = line.substr(0, colon);
        std::string v = line.substr(colon + 1);
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
        req.headers.emplace_back(std::move(k), std::move(v));
    }
    return true;
}

bool readBody(Stream& s, Request& req, std::string& already, size_t maxBytes) {
    std::string clHeader = req.header("Content-Length");
    if (clHeader.empty()) {
        req.body = std::move(already);
        return true;
    }
    size_t cl = static_cast<size_t>(std::strtoul(clHeader.c_str(), nullptr, 10));
    if (cl > maxBytes) return false;
    req.body = std::move(already);
    char tmp[2048];
    while (req.body.size() < cl) {
        int want = static_cast<int>(std::min<size_t>(sizeof(tmp), cl - req.body.size()));
        int n = s.read(tmp, want);
        if (n <= 0) return false;
        req.body.append(tmp, tmp + n);
    }
    return true;
}

// ---- Response helpers -----------------------------------------------------

void writeStatus(Stream& s, int code, const char* reason,
                 const std::string& body, const char* contentType = "text/plain") {
    std::ostringstream r;
    r << "HTTP/1.1 " << code << " " << reason << "\r\n"
      << "Content-Type: " << contentType << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Connection: close\r\n"
      << "\r\n"
      << body;
    std::string wire = r.str();
    s.write(wire.data(), static_cast<int>(wire.size()));
}

// ---- Auth -----------------------------------------------------------------

bool authOk(const Request& req, const std::string& expected) {
    if (expected.empty()) return true;
    std::string h = req.header("Authorization");
    if (h.rfind("Bearer ", 0) != 0) return false;
    std::string presented = h.substr(7);
    // Constant-time-ish compare.
    if (presented.size() != expected.size()) return false;
    unsigned diff = 0;
    for (size_t i = 0; i < expected.size(); ++i) {
        diff |= static_cast<unsigned>(presented[i]) ^ static_cast<unsigned>(expected[i]);
    }
    return diff == 0;
}

// ---- NDJSON parsing -------------------------------------------------------

void splitLines(const std::string& body, std::vector<std::string>& out) {
    std::string cur;
    for (char c : body) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            if (!cur.empty()) out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) {
        if (!cur.empty() && cur.back() == '\r') cur.pop_back();
        if (!cur.empty()) out.push_back(std::move(cur));
    }
}

// Best-effort sanity check: line begins with '{' and ends with '}'.
bool looksLikeJsonObject(const std::string& s) {
    if (s.empty()) return false;
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
    size_t j = s.size();
    while (j > i && std::isspace((unsigned char)s[j - 1])) --j;
    return i < j && s[i] == '{' && s[j - 1] == '}';
}

// ---- Persistence ----------------------------------------------------------

class DailyJsonl {
public:
    explicit DailyJsonl(const std::string& dir) : m_dir(dir) {}
    void append(const std::string& line) {
        if (m_dir.empty()) return;
        std::lock_guard<std::mutex> lk(m_mu);
        std::time_t t = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        char date[16];
        strftime(date, sizeof(date), "%Y-%m-%d", &tm);
        std::string path = m_dir;
#ifdef _WIN32
        if (!path.empty() && path.back() != '\\' && path.back() != '/') path += '\\';
#else
        if (!path.empty() && path.back() != '/') path += '/';
#endif
        path += "viewer-";
        path += date;
        path += ".jsonl";
        std::ofstream f(path, std::ios::app);
        if (f) { f << line << "\n"; }
    }
private:
    std::string m_dir;
    std::mutex  m_mu;
};

// ---- Connection handler ---------------------------------------------------

struct ServerCtx {
    Config         cfg;
    Broadcaster    bcast;
    DailyJsonl*    storage = nullptr;
    std::atomic<int> activeConns{0};
};

void handleConnection(std::shared_ptr<Stream> stream, ServerCtx* ctx,
                      const std::string& peerHost) {
    struct ConnGuard {
        ServerCtx* c;
        ConnGuard(ServerCtx* x) : c(x) { c->activeConns.fetch_add(1); }
        ~ConnGuard() { c->activeConns.fetch_sub(1); }
    } guard(ctx);

    Request req;
    std::string rawTail;
    if (!parseRequest(*stream, req, rawTail)) {
        return;
    }
    bool peerLoopback = netcommon::isLoopbackHost(peerHost);

    if (req.method == "GET" && req.path == "/health") {
        if (!peerLoopback && !authOk(req, ctx->cfg.token)) {
            writeStatus(*stream, 401, "Unauthorized", "missing or invalid token\n");
            return;
        }
        std::ostringstream js;
        js << "{\"ok\":true,\"clients\":" << ctx->bcast.count() << "}";
        writeStatus(*stream, 200, "OK", js.str(), "application/json");
        return;
    }

    if (req.method == "POST" && req.path == "/ingest") {
        if (!authOk(req, ctx->cfg.token)) {
            writeStatus(*stream, 401, "Unauthorized", "missing or invalid token\n");
            return;
        }
        if (!readBody(*stream, req, rawTail, 4 * 1024 * 1024)) {
            writeStatus(*stream, 400, "Bad Request", "body too large or truncated\n");
            return;
        }
        std::string ct = req.header("Content-Type");
        std::vector<std::string> events;
        if (ct.find("application/x-ndjson") != std::string::npos ||
            req.body.find('\n') != std::string::npos) {
            splitLines(req.body, events);
        } else {
            std::string b = req.body;
            while (!b.empty() && std::isspace((unsigned char)b.back())) b.pop_back();
            if (!b.empty()) events.push_back(std::move(b));
        }
        int accepted = 0;
        for (auto& ev : events) {
            if (!looksLikeJsonObject(ev)) continue;
            ctx->bcast.publish(ev);
            if (ctx->storage) ctx->storage->append(ev);
            ++accepted;
        }
        std::ostringstream js;
        js << "{\"accepted\":" << accepted << "}";
        writeStatus(*stream, 200, "OK", js.str(), "application/json");
        return;
    }

    if (req.method == "GET" && req.path == "/events") {
        if (!authOk(req, ctx->cfg.token)) {
            writeStatus(*stream, 401, "Unauthorized", "missing or invalid token\n");
            return;
        }
        if (static_cast<int>(ctx->bcast.count()) >= ctx->cfg.maxClients) {
            writeStatus(*stream, 503, "Service Unavailable", "too many SSE clients\n");
            return;
        }
        std::string head =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "X-Accel-Buffering: no\r\n"
            "\r\n"
            ": connected\n\n";
        if (!stream->write(head.data(), (int)head.size())) return;

        auto client = std::make_shared<SseClient>();
        client->stream = stream;
        ctx->bcast.add(client);

        // Keep the connection open with periodic heartbeats until the client
        // disconnects or the client object is marked dead by a failed publish.
        auto last = std::chrono::steady_clock::now();
        while (g_run.load() && client->alive.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            auto now = std::chrono::steady_clock::now();
            if (now - last > std::chrono::seconds(15)) {
                std::lock_guard<std::mutex> lk(client->writeMu);
                if (!stream->write(": ping\n\n", 8)) {
                    client->alive = false;
                    break;
                }
                last = now;
            }
        }
        ctx->bcast.remove(client);
        return;
    }

    writeStatus(*stream, 404, "Not Found", "not found\n");
}

// ---- TLS context ----------------------------------------------------------

#ifdef KEYLOGGER_HAS_TLS
struct ServerTls {
    SSL_CTX* ctx = nullptr;
    ~ServerTls() { if (ctx) SSL_CTX_free(ctx); }
};

bool serverTlsInit(ServerTls& tc, const std::string& cert, const std::string& key,
                   std::string& errorOut) {
    static std::once_flag once;
    std::call_once(once, [] {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
    });
    tc.ctx = SSL_CTX_new(TLS_server_method());
    if (!tc.ctx) { errorOut = "SSL_CTX_new failed"; return false; }
    if (SSL_CTX_use_certificate_file(tc.ctx, cert.c_str(), SSL_FILETYPE_PEM) != 1) {
        errorOut = "failed to load TLS cert: " + cert;
        return false;
    }
    if (SSL_CTX_use_PrivateKey_file(tc.ctx, key.c_str(), SSL_FILETYPE_PEM) != 1) {
        errorOut = "failed to load TLS key: " + key;
        return false;
    }
    if (SSL_CTX_check_private_key(tc.ctx) != 1) {
        errorOut = "TLS cert/key mismatch";
        return false;
    }
    return true;
}
#endif

// ---- Listening socket -----------------------------------------------------

socket_t openListener(const std::string& host, int port, std::string& errorOut) {
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;
    addrinfo* res = nullptr;
    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%d", port);
    int gai = getaddrinfo(host.c_str(), portStr, &hints, &res);
    if (gai != 0 || !res) {
        errorOut = "cannot resolve bind address: " + host;
        if (res) freeaddrinfo(res);
        return INVALID_SOCK;
    }
    socket_t s = INVALID_SOCK;
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCK) continue;
        int yes = 1;
        ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&yes), sizeof(yes));
        if (::bind(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) != 0) {
            CLOSESOCK(s); s = INVALID_SOCK; continue;
        }
        if (::listen(s, 16) != 0) {
            CLOSESOCK(s); s = INVALID_SOCK; continue;
        }
        break;
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCK) errorOut = "cannot bind " + host + ":" + std::to_string(port);
    return s;
}

}  // namespace

int run(const Config& cfg) {
    if (cfg.token.empty() && !netcommon::isLoopbackHost(cfg.bindHost)) {
        std::cerr << "error: viewer requires --viewer-token for non-loopback binds\n";
        return 2;
    }
    bool tlsRequested = !cfg.tlsCert.empty() || !cfg.tlsKey.empty();
    if (tlsRequested && (cfg.tlsCert.empty() || cfg.tlsKey.empty())) {
        std::cerr << "error: --tls-cert and --tls-key must both be provided\n";
        return 2;
    }
#ifndef KEYLOGGER_HAS_TLS
    if (tlsRequested) {
        std::cerr << "error: this build does not support TLS (no OpenSSL at compile time)\n";
        return 2;
    }
#endif

    std::signal(SIGINT,  sigHandler);
    std::signal(SIGTERM, sigHandler);
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGHUP,  sigHandler);
#endif

    socket_t listener;
    std::string err;
    listener = openListener(cfg.bindHost, cfg.bindPort, err);
    if (listener == INVALID_SOCK) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }

#ifdef KEYLOGGER_HAS_TLS
    ServerTls tls;
    if (tlsRequested && !serverTlsInit(tls, cfg.tlsCert, cfg.tlsKey, err)) {
        std::cerr << "error: " << err << "\n";
        CLOSESOCK(listener);
        return 1;
    }
#endif

    DailyJsonl storage(cfg.storageDir);
    ServerCtx ctx;
    ctx.cfg = cfg;
    if (!cfg.storageDir.empty()) ctx.storage = &storage;

    std::cerr << "[viewer] listening on " << cfg.bindHost << ":" << cfg.bindPort
              << " tls=" << (tlsRequested ? "on" : "off")
              << " storage=" << (cfg.storageDir.empty() ? "(none)" : cfg.storageDir)
              << "\n";

    while (g_run.load()) {
        // Use select so SIGTERM unblocks accept() loop.
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(listener, &rf);
        timeval tv{0, 250 * 1000};
        int rc = select(static_cast<int>(listener) + 1, &rf, nullptr, nullptr, &tv);
        if (rc <= 0) continue;
        sockaddr_storage addr{};
        socklen_t alen = sizeof(addr);
        socket_t c = ::accept(listener, reinterpret_cast<sockaddr*>(&addr), &alen);
        if (c == INVALID_SOCK) continue;

        if (ctx.activeConns.load() >= cfg.maxClients) {
            const char* msg = "HTTP/1.1 503 Service Unavailable\r\n"
                              "Content-Length: 0\r\nConnection: close\r\n\r\n";
            ::send(c, msg, static_cast<int>(strlen(msg)), 0);
            CLOSESOCK(c);
            continue;
        }

        char hostBuf[NI_MAXHOST] = {0};
        getnameinfo(reinterpret_cast<sockaddr*>(&addr), alen,
                    hostBuf, sizeof(hostBuf), nullptr, 0, NI_NUMERICHOST);
        std::string peerHost = hostBuf;

        std::shared_ptr<Stream> stream;
#ifdef KEYLOGGER_HAS_TLS
        if (tlsRequested) {
            SSL* ssl = SSL_new(tls.ctx);
            if (!ssl) { CLOSESOCK(c); continue; }
            SSL_set_fd(ssl, static_cast<int>(c));
            if (SSL_accept(ssl) <= 0) {
                SSL_free(ssl);
                CLOSESOCK(c);
                continue;
            }
            stream = std::make_shared<TlsStream>(ssl, c);
        } else
#endif
        {
            stream = std::make_shared<PlainStream>(c);
        }

        std::thread([stream, peerHost, ptr = &ctx]() {
            try { handleConnection(stream, ptr, peerHost); } catch (...) {}
        }).detach();
    }

    CLOSESOCK(listener);
    std::cerr << "[viewer] shutting down\n";
    return 0;
}

}  // namespace viewer
