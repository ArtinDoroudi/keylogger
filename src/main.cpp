#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

#include "logger.hpp"
#include "platform/keylogger.hpp"

void printBanner() {
    std::cout << "======================================\n";
    std::cout << " ethical-keylogger-demo\n";
    std::cout << "======================================\n";
    std::cout << " WHAT IT DOES:    Logs keystrokes to a local file\n";
    std::cout << " WHAT IT DOESN'T: No network transmission, no stealth\n";
    std::cout << "                  mode, no persistence (no startup entry)\n";
    std::cout << " LOG LOCATION:    Current working directory (keylog.txt)\n";
    std::cout << " HOW TO STOP:     Press Ctrl+C\n";
    std::cout << "======================================\n";
}

void printUsage(const char* argv0) {
    std::cout << "Usage: " << argv0 << " [OPTIONS]\n\n";
    std::cout << "Options:\n";
    std::cout << "  --consent    Acknowledge consent and start logging\n";
    std::cout << "  --help, -h   Print this message and exit\n\n";
    std::cout << "You must pass --consent to run the logger.\n";
}

int main(int argc, char* argv[]) {
    bool consent = false;
    bool help = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--consent") == 0) {
            consent = true;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            help = true;
        }
    }

    if (help) {
        printUsage(argv[0]);
        return 0;
    }

    printBanner();

    if (!consent) {
        std::cout << "\nRun with --consent to acknowledge the above and start logging.\n";
        return 0;
    }

    std::cout << "\nConsent acknowledged.\n";

    IKeylogger* keylogger = createPlatformKeylogger();
    if (!keylogger) {
        Logger logger("macos");
        logger.start();
        logger.stop();
        return 1;
    }

    Logger logger("macos");
    logger.start();
    logger.logEvent(R"({"event":"test","msg":"logger working"})");
    std::this_thread::sleep_for(std::chrono::seconds(2));
    logger.stop();

    std::cout << "Log written to: " << logger.currentFilename() << "\n";
    return 0;
}
