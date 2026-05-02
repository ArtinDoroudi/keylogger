#include "net_sender.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
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

namespace net {

namespace {

#ifdef _WIN32
struct WsaInit {
    WsaInit()  { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
    ~WsaInit() { WSACleanup(); }
};
static WsaInit g_wsaInit;
#endif

// Parse "http://host:port/path" into components.
bool parseEndpoint(const std::string& url, std::string& host, int& port,
                   std::string& path, std::string& err) {
    const std::string prefix = "http://";
    if (url.substr(0, prefix.size()) != prefix) {
        err = "endpoint must start with http://";
        return false;
    }
    std::string rest = url.substr(prefix.size());
    auto slashPos = rest.find('/');
    std::string hostPort = (slashPos != std::string::npos)
                           ? rest.substr(0, slashPos)
                           : rest;
    path = (slashPos != std::string::npos) ? rest.substr(slashPos) : "/";

    auto colonPos = hostPort.rfind(':');
    if (colonPos != std::string::npos) {
        host = hostPort.substr(0, colonPos);
        port = std::atoi(hostPort.substr(colonPos + 1).c_str());
        if (port <= 0 || port > 65535) {
            err = "invalid port in endpoint";
            return false;
        }
    } else {
        host = hostPort;
        port = 80;
    }
    if (host.empty()) {
        err = "empty host in endpoint";
        return false;
    }
    return true;
}

bool waitWritable(socket_t s, int timeoutMs) {
    fd_set wf;
    FD_ZERO(&wf);
    FD_SET(s, &wf);
    timeval tv{};
    tv.tv_sec  = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    return select(static_cast<int>(s) + 1, nullptr, &wf, nullptr, &tv) > 0;
}

bool waitReadable(socket_t s, int timeoutMs) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(s, &rf);
    timeval tv{};
    tv.tv_sec  = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    return select(static_cast<int>(s) + 1, &rf, nullptr, nullptr, &tv) > 0;
}

socket_t connectTimeout(const std::string& host, int port, int timeoutMs,
                        std::string& errorOut) {
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    char portStr[16];
    std::snprintf(portStr, sizeof(portStr), "%d", port);
    if (getaddrinfo(host.c_str(), portStr, &hints, &res) != 0 || !res) {
        errorOut = "DNS resolution failed for " + host;
        if (res) freeaddrinfo(res);
        return INVALID_SOCK;
    }

    socket_t s = INVALID_SOCK;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCK) continue;
#ifdef _WIN32
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
#else
        fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
        int rc = ::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        if (rc == 0) break;
#ifdef _WIN32
        bool pending = (WSAGetLastError() == WSAEWOULDBLOCK);
#else
        bool pending = (errno == EINPROGRESS);
#endif
        if (pending && waitWritable(s, timeoutMs)) {
            int soerr = 0;
            socklen_t len = sizeof(soerr);
            if (getsockopt(s, SOL_SOCKET, SO_ERROR,
                           reinterpret_cast<char*>(&soerr), &len) == 0 && soerr == 0)
                break;
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
            if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
#else
            if (errno == EAGAIN || errno == EINTR) continue;
#endif
            return false;
        }
        off += static_cast<size_t>(sent);
    }
    return true;
}

int recvStatusCode(socket_t s, int timeoutMs) {
    std::string buf;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs);
    int code = 0;
    while (true) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                             deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) return 0;
        if (!waitReadable(s, static_cast<int>(remaining))) return 0;
        char tmp[512];
        int n = ::recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.append(tmp, static_cast<size_t>(n));
        if (code == 0) {
            auto eol = buf.find("\r\n");
            if (eol != std::string::npos) {
                int maj, min, c;
                if (std::sscanf(buf.c_str(), "HTTP/%d.%d %d", &maj, &min, &c) == 3)
                    code = c;
            }
        }
        if (code && buf.find("\r\n\r\n") != std::string::npos) break;
        if (buf.size() > 16384) break;
    }
    return code;
}

}  // namespace

Sender::Sender() = default;
Sender::~Sender() { stop(); }

bool Sender::start(const SenderConfig& cfg, std::string& errorOut) {
    m_cfg = cfg;
    if (!parseEndpoint(cfg.endpoint, m_host, m_port, m_path, errorOut))
        return false;
    m_running  = true;
    m_stopFlag = false;
    m_thread   = std::thread(&Sender::workerLoop, this);
    return true;
}

void Sender::enqueue(const std::string& jsonObject) {
    if (!m_running.load()) return;
    {
        std::lock_guard<std::mutex> lk(m_mu);
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
        std::string event;
        {
            std::unique_lock<std::mutex> lk(m_mu);
            if (m_queue.empty() && !m_stopFlag.load())
                m_cv.wait_for(lk, std::chrono::seconds(1));
            if (m_queue.empty() && m_stopFlag.load()) return;
            if (m_queue.empty()) continue;
            event = std::move(m_queue.front());
            m_queue.pop_front();
        }

        bool ok = false;
        std::string err;
        for (int attempt = 0; attempt <= m_cfg.retryMax; ++attempt) {
            err.clear();
            if (sendOne(event, err)) { ok = true; break; }
            int waitMs = std::min(2000, 100 * (1 << attempt));
            std::unique_lock<std::mutex> lk(m_mu);
            if (m_cv.wait_for(lk, std::chrono::milliseconds(waitMs),
                              [&] { return m_stopFlag.load(); }))
                break;
        }
        if (ok) {
            m_sent.fetch_add(1);
        } else {
            m_dropped.fetch_add(1);
            std::cerr << "[net] dropped event: " << err << "\n";
        }
    }
}

bool Sender::sendOne(const std::string& body, std::string& errorOut) {
    socket_t s = connectTimeout(m_host, m_port, m_cfg.timeoutMs, errorOut);
    if (s == INVALID_SOCK) return false;

    std::ostringstream req;
    req << "POST " << m_path << " HTTP/1.1\r\n"
        << "Host: " << m_host;
    if (m_port != 80) req << ":" << m_port;
    req << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << body;
    std::string wire = req.str();

    bool ok = false;
    if (sendAll(s, wire.c_str(), wire.size(), m_cfg.timeoutMs)) {
        int code = recvStatusCode(s, m_cfg.timeoutMs);
        if (code >= 200 && code < 300) {
            ok = true;
        } else if (code > 0) {
            errorOut = "HTTP " + std::to_string(code);
        } else {
            errorOut = "no HTTP response";
        }
    } else {
        errorOut = "send failed";
    }
    CLOSESOCK(s);
    return ok;
}

}  // namespace net
