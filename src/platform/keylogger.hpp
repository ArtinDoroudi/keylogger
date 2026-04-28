#pragma once

#include <cstdint>
#include <functional>
#include <string>

struct KeyEvent {
    uint32_t    virtualKey;
    std::string translated;
    int64_t     timestampMs;
    bool        keyDown;
};

class IKeylogger {
public:
    virtual ~IKeylogger() = default;
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual void setEventCallback(std::function<void(const KeyEvent&)>) = 0;
};

IKeylogger* createPlatformKeylogger();
