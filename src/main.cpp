#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
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

static std::string msToIso8601(int64_t ms) {
    auto t = static_cast<time_t>(ms / 1000);
    auto frac = ms % 1000;
    struct tm tmInfo;
#ifdef _WIN32
    gmtime_s(&tmInfo, &t);
#else
    gmtime_r(&t, &tmInfo);
#endif
    char dateBuf[24];
    strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%dT%H:%M:%S", &tmInfo);
    char out[32];
    snprintf(out, sizeof(out), "%s.%03lldZ", dateBuf, static_cast<long long>(frac));
    return out;
}

static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else                out += c;
    }
    return out;
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

#ifdef _WIN32
    Logger logger("windows");
#elif defined(__APPLE__)
    Logger logger("macos");
#else
    Logger logger("linux");
#endif
    logger.start();

    IKeylogger* keylogger = createPlatformKeylogger();
    if (!keylogger) {
        logger.stop();
        return 1;
    }

    keylogger->setEventCallback([&logger](const KeyEvent& ev) {
        char buf[512];
        snprintf(buf, sizeof(buf),
                 R"({"ts":"%s","vk":%u,"key":"%s","down":%s})",
                 msToIso8601(ev.timestampMs).c_str(),
                 ev.virtualKey,
                 jsonEscape(ev.translated).c_str(),
                 ev.keyDown ? "true" : "false");
        logger.logEvent(buf);
    });

    if (!keylogger->start()) {
        std::cerr << "Failed to start keylogger\n";
        delete keylogger;
        logger.stop();
        return 1;
    }

    std::cout << "Capturing for 30 seconds...\n";
    std::this_thread::sleep_for(std::chrono::seconds(30));

    keylogger->stop();
    delete keylogger;
    logger.stop();

    std::cout << "Log written to: " << logger.currentFilename() << "\n";
    return 0;
}