#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <thread>

#include "logger.hpp"
#include "platform/keylogger.hpp"

static std::atomic<bool> g_running{true};

static void signalHandler(int) {
    g_running = false;
}

#ifdef _WIN32
#include <windows.h>
static BOOL WINAPI consoleCtrlHandler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_CLOSE_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}
#endif

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

    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);
#ifdef _WIN32
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
#endif

#ifdef _WIN32
    static constexpr const char* OS_NAME = "windows";
#elif defined(__APPLE__)
    static constexpr const char* OS_NAME = "macos";
#else
    static constexpr const char* OS_NAME = "linux";
#endif
    Logger logger(OS_NAME);
    logger.start();

    {
        auto nowMs = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        char hdr[256];
        snprintf(hdr, sizeof(hdr),
                 R"({"event":"session_start","os":"%s","version":"0.1.0","ts":"%s"})",
                 OS_NAME, msToIso8601(nowMs).c_str());
        logger.logEvent(hdr);
    }

    IKeylogger* keylogger = createPlatformKeylogger();
    if (!keylogger) {
        logger.stop();
        return 1;
    }

    keylogger->setEventCallback([&logger](const KeyEvent& ev) {
        char buf[512];
        snprintf(buf, sizeof(buf),
                 R"({"ts":"%s","vk":%u,"key":"%s","down":%s,"os":"%s"})",
                 msToIso8601(ev.timestampMs).c_str(),
                 ev.virtualKey,
                 jsonEscape(ev.translated).c_str(),
                 ev.keyDown ? "true" : "false",
                 OS_NAME);
        logger.logEvent(buf);
    });

    if (!keylogger->start()) {
        std::cerr << "Failed to start keylogger\n";
        delete keylogger;
        logger.stop();
        return 1;
    }

    std::cout << "Capturing — press Ctrl+C to stop.\n";
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    keylogger->stop();
    delete keylogger;
    logger.stop();

    std::cout << "Stopped. Log saved to: " << logger.currentFilename() << "\n";
    return 0;
}