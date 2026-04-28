#include "keylogger.hpp"

#ifdef _WIN32

#include "keylogger_win.hpp"

IKeylogger* createPlatformKeylogger() {
    return new WindowsKeylogger();
}

#elif defined(__APPLE__)

#include "keylogger_mac.hpp"

IKeylogger* createPlatformKeylogger() {
    return new MacKeylogger();
}

#elif defined(__linux__)

#include "keylogger_linux.hpp"

IKeylogger* createPlatformKeylogger() {
    return new LinuxKeylogger();
}

#else

#include <iostream>

IKeylogger* createPlatformKeylogger() {
    std::cout << "platform implementation not yet linked\n";
    return nullptr;
}

#endif
