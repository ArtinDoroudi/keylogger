#include <iostream>

#include "keylogger.hpp"

IKeylogger* createPlatformKeylogger() {
    std::cout << "platform implementation not yet linked\n";
    return nullptr;
}
