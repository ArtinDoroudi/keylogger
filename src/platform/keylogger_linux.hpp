#pragma once
#ifdef __linux__

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "keylogger.hpp"

class LinuxKeylogger : public IKeylogger {
public:
    ~LinuxKeylogger() override;
    bool start() override;
    void stop() override;
    void setEventCallback(std::function<void(const KeyEvent&)> cb) override;

private:
    void threadLoop();

    std::vector<int>                     m_fds;
    int                                  m_stopFd{-1};
    std::thread                          m_thread;
    std::atomic<bool>                    m_running{false};
    std::mutex                           m_mu;
    std::function<void(const KeyEvent&)> m_callback;
};

#endif // __linux__
