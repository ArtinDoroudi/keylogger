#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <sys/types.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "logger.hpp"
#include "platform/keylogger.hpp"
#include "service.hpp"

#ifndef _WIN32
#include <climits>
#include <cstdlib>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

static std::atomic<bool> g_running{true};

static void signalHandler(int) {
    g_running = false;
}

#ifdef _WIN32
static BOOL WINAPI consoleCtrlHandler(DWORD event) {
    switch (event) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_running = false;
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

static const char* BANNER_TEXT =
    "======================================\n"
    " ethical-keylogger-demo\n"
    "======================================\n"
    " WHAT IT DOES:    Logs keystrokes to a local file\n"
    " WHAT IT DOESN'T: No network transmission, no stealth\n"
    "                  mode, no persistence (no startup entry)\n"
    " LOG LOCATION:    Current working directory (keylog.txt)\n"
    " HOW TO STOP:     Press Ctrl+C\n"
    "======================================";

static const char* CONSENT_MAGIC = "I OWN THIS SYSTEM AND CONSENT TO LOGGING";

static void printBanner() {
    std::cout << BANNER_TEXT << "\n";
}

static void printUsage(const char* argv0) {
    std::cout << "Usage: " << argv0 << " [OPTIONS]\n\n";
    std::cout << "Interactive mode (default):\n";
    std::cout << "  --consent               Acknowledge consent and start logging\n\n";
    std::cout << "Service mode (unattended deployments):\n";
    std::cout << "  --service               Run in unattended mode (requires --consent-file)\n";
    std::cout << "  --consent-file <path>   Path to a file whose first line is exactly:\n";
    std::cout << "                          \"" << CONSENT_MAGIC << "\"\n";
    std::cout << "  --log-dir <path>        Directory to write logs to (default: cwd)\n";
    std::cout << "  --session-tag <string>  Tag written into every JSON event (max 64 chars,\n";
    std::cout << "                          alphanumeric + dash + underscore only)\n\n";
    std::cout << "Service installation (auto-start at boot):\n";
    std::cout << "  --install               Register a system service and enable auto-start.\n";
    std::cout << "                          Requires elevation (sudo/Administrator) and\n";
    std::cout << "                          --consent-file + --log-dir.\n";
    std::cout << "  --uninstall             Stop, disable, and remove the service registration.\n";
    std::cout << "                          Requires elevation. Idempotent (warns if missing).\n";
    std::cout << "  --status                Print whether the service is registered, enabled,\n";
    std::cout << "                          and running. Exit 0 if registered, 1 if not.\n";
    std::cout << "  --force                 With --install, overwrite an existing registration.\n\n";
    std::cout << "General:\n";
    std::cout << "  --help, -h              Print this message and exit\n\n";
    std::cout << "Examples:\n";
    std::cout << "  # Foreground service mode\n";
    std::cout << "  " << argv0 << " --service --consent-file /etc/keylogger/consent.txt \\\n";
    std::cout << "              --log-dir /var/log/keylogger \\\n";
    std::cout << "              --session-tag honeypot-web-01\n\n";
    std::cout << "  # Install as a managed system service (auto-start at boot)\n";
    std::cout << "  sudo " << argv0 << " --install --consent-file /etc/keylogger/consent.txt \\\n";
    std::cout << "                       --log-dir /var/log/keylogger \\\n";
    std::cout << "                       --session-tag honeypot-web-01\n\n";
    std::cout << "  # Inspect / remove the installed service\n";
    std::cout << "  " << argv0 << " --status\n";
    std::cout << "  sudo " << argv0 << " --uninstall\n";
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
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:   out += c;      break;
        }
    }
    return out;
}

static bool isValidSessionTag(const std::string& tag) {
    if (tag.empty() || tag.size() > 64) return false;
    return std::all_of(tag.begin(), tag.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
    });
}

static bool isRegularFileReadable(const std::string& path) {
#ifdef _WIN32
    struct _stat st;
    if (_stat(path.c_str(), &st) != 0) return false;
    if (!(st.st_mode & _S_IFREG)) return false;
    return (_access(path.c_str(), 4) == 0);
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    if (!S_ISREG(st.st_mode)) return false;
    return (access(path.c_str(), R_OK) == 0);
#endif
}

static bool isDirectoryWritable(const std::string& path) {
#ifdef _WIN32
    struct _stat st;
    if (_stat(path.c_str(), &st) != 0) return false;
    if (!(st.st_mode & _S_IFDIR)) return false;
    return (_access(path.c_str(), 2) == 0);
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) return false;
    return (access(path.c_str(), W_OK) == 0);
#endif
}

static std::string toAbsolutePath(const std::string& path) {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetFullPathNameA(path.c_str(), MAX_PATH, buf, nullptr);
    if (n == 0 || n >= MAX_PATH) return path;
    return std::string(buf, n);
#else
    char buf[PATH_MAX];
    if (realpath(path.c_str(), buf) != nullptr) {
        return std::string(buf);
    }
    // realpath fails if any path component does not exist. Fall back to a
    // best-effort absolute join against cwd so callers still get an absolute
    // value (the caller will have already validated existence separately).
    if (!path.empty() && path[0] == '/') return path;
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == nullptr) return path;
    return std::string(cwd) + "/" + path;
#endif
}

static std::string getExecutablePath(const char* argv0) {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return std::string(buf, n);
    return toAbsolutePath(argv0);
#elif defined(__APPLE__)
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        char real[PATH_MAX];
        if (realpath(buf, real) != nullptr) return std::string(real);
        return std::string(buf);
    }
    return toAbsolutePath(argv0);
#else
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        return std::string(buf);
    }
    return toAbsolutePath(argv0);
#endif
}

static bool validateConsentFile(const std::string& path) {
    if (!isRegularFileReadable(path)) {
        std::cerr << "error: consent file not found or not readable: " << path << "\n";
        return false;
    }
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "error: cannot open consent file: " << path << "\n";
        return false;
    }
    std::string firstLine;
    std::getline(f, firstLine);
    while (!firstLine.empty() && (firstLine.back() == '\r' || firstLine.back() == ' '))
        firstLine.pop_back();
    if (firstLine != CONSENT_MAGIC) {
        std::cerr << "error: consent file first line does not match required text.\n";
        std::cerr << "  expected: \"" << CONSENT_MAGIC << "\"\n";
        std::cerr << "  got:      \"" << firstLine << "\"\n";
        return false;
    }
    return true;
}

int main(int argc, char* argv[]) {
    bool consent = false;
    bool help = false;
    bool serviceMode = false;
    bool installMode = false;
    bool uninstallMode = false;
    bool statusMode = false;
    bool force = false;
    std::string consentFilePath;
    std::string logDir = ".";
    bool logDirExplicit = false;
    std::string sessionTag;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--consent") == 0) {
            consent = true;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            help = true;
        } else if (std::strcmp(argv[i], "--service") == 0) {
            serviceMode = true;
        } else if (std::strcmp(argv[i], "--install") == 0) {
            installMode = true;
        } else if (std::strcmp(argv[i], "--uninstall") == 0) {
            uninstallMode = true;
        } else if (std::strcmp(argv[i], "--status") == 0) {
            statusMode = true;
        } else if (std::strcmp(argv[i], "--force") == 0) {
            force = true;
        } else if (std::strcmp(argv[i], "--consent-file") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "error: --consent-file requires a path argument\n";
                return 2;
            }
            consentFilePath = argv[++i];
        } else if (std::strcmp(argv[i], "--log-dir") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "error: --log-dir requires a path argument\n";
                return 2;
            }
            logDir = argv[++i];
            logDirExplicit = true;
        } else if (std::strcmp(argv[i], "--session-tag") == 0) {
            if (i + 1 >= argc) {
                std::cerr << "error: --session-tag requires a string argument\n";
                return 2;
            }
            sessionTag = argv[++i];
        } else {
            std::cerr << "error: unknown option: " << argv[i] << "\n";
            return 2;
        }
    }

    if (help) {
        printUsage(argv[0]);
        return 0;
    }

    // ---- Mutual-exclusion: --install / --uninstall / --status / --consent ----
    {
        int exclusive = (installMode ? 1 : 0)
                      + (uninstallMode ? 1 : 0)
                      + (statusMode ? 1 : 0)
                      + (consent ? 1 : 0);
        if (exclusive > 1) {
            std::cerr << "error: --install, --uninstall, --status, and --consent are "
                         "mutually exclusive.\n";
            return 2;
        }
    }

    // ---- Validate session-tag if provided (applies to both modes) ----
    if (!sessionTag.empty() && !isValidSessionTag(sessionTag)) {
        std::cerr << "error: --session-tag must be 1-64 characters, "
                     "alphanumeric, dash, or underscore only. Got: \""
                  << sessionTag << "\"\n";
        return 2;
    }

#ifdef _WIN32
    static constexpr const char* OS_NAME = "windows";
#elif defined(__APPLE__)
    static constexpr const char* OS_NAME = "macos";
#else
    static constexpr const char* OS_NAME = "linux";
#endif

    // ---- Service management commands (--install / --uninstall / --status) ----
    if (statusMode) {
        if (force || installMode || uninstallMode || serviceMode ||
            !consentFilePath.empty() || logDirExplicit || !sessionTag.empty()) {
            // Be lenient: --status ignores other args, but warn on misuse.
            // (Mutual exclusion vs --install/--uninstall/--consent already enforced.)
        }
        return service::status();
    }

    if (uninstallMode) {
#ifdef _WIN32
        const char* elevHint = "error: --uninstall requires Administrator privileges. "
                               "Run as Administrator.";
#else
        const char* elevHint = "error: --uninstall requires root privileges. "
                               "Run with sudo.";
#endif
        if (!service::isElevated()) {
            std::cerr << elevHint << "\n";
            return 2;
        }
        return service::uninstall();
    }

    if (installMode) {
#ifdef _WIN32
        const char* elevHint = "error: --install requires Administrator privileges. "
                               "Run as Administrator.";
#else
        const char* elevHint = "error: --install requires root privileges. "
                               "Run with sudo.";
#endif
        if (!service::isElevated()) {
            std::cerr << elevHint << "\n";
            return 2;
        }

        if (consentFilePath.empty()) {
            std::cerr << "error: --install requires --consent-file <path>\n";
            return 2;
        }
        if (!logDirExplicit) {
            std::cerr << "error: --install requires --log-dir <path>\n";
            return 2;
        }

        // Validate before writing anything to disk.
        if (!validateConsentFile(consentFilePath)) {
            return 2;
        }
        if (!isDirectoryWritable(logDir)) {
            std::cerr << "error: --log-dir does not exist or is not writable: "
                      << logDir << "\n";
            return 2;
        }

        ServiceInstallParams params;
        params.binaryPath  = getExecutablePath(argv[0]);
        params.consentFile = toAbsolutePath(consentFilePath);
        params.logDir      = toAbsolutePath(logDir);
        params.sessionTag  = sessionTag;
        params.force       = force;

        return service::install(params);
    }

    // --force is only meaningful with --install.
    if (force) {
        std::cerr << "error: --force is only valid with --install\n";
        return 2;
    }

    // ---- Service mode path ----
    if (serviceMode) {
        if (consent) {
            std::cerr << "warning: --consent is ignored in --service mode\n";
        }

        if (consentFilePath.empty()) {
            std::cerr << "error: --service requires --consent-file <path>\n";
            return 2;
        }

        if (!validateConsentFile(consentFilePath)) {
            return 2;
        }

        if (!isDirectoryWritable(logDir)) {
            std::cerr << "error: --log-dir does not exist or is not writable: "
                      << logDir << "\n";
            return 2;
        }

        // Signals
        std::signal(SIGINT,  signalHandler);
        std::signal(SIGTERM, signalHandler);
#if !defined(_WIN32)
        std::signal(SIGHUP, signalHandler);
#else
        SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
#endif

        Logger logger(OS_NAME, logDir, sessionTag);
        logger.start();

        // Write service_start event with embedded banner
        {
            auto nowMs = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());

            std::string tagField = sessionTag.empty()
                ? "null"
                : "\"" + jsonEscape(sessionTag) + "\"";

            std::string event;
            event.reserve(1024);
            event += R"({"event":"service_start","ts":")";
            event += msToIso8601(nowMs);
            event += R"(","os":")";
            event += OS_NAME;
            event += R"(","consent_file":")";
            event += jsonEscape(consentFilePath);
            event += R"(","session_tag":)";
            event += tagField;
            event += R"(,"banner":")";
            event += jsonEscape(BANNER_TEXT);
            event += R"("})";

            logger.logEvent(event);
        }

        IKeylogger* keylogger = createPlatformKeylogger();
        if (!keylogger) {
            logger.stop();
            return 1;
        }

        keylogger->setEventCallback([&logger, &sessionTag](const KeyEvent& ev) {
            std::string tagField = sessionTag.empty()
                ? "null"
                : "\"" + jsonEscape(sessionTag) + "\"";

            char buf[640];
            snprintf(buf, sizeof(buf),
                     R"({"ts":"%s","vk":%u,"key":"%s","down":%s,"os":"%s","session_tag":%s})",
                     msToIso8601(ev.timestampMs).c_str(),
                     ev.virtualKey,
                     jsonEscape(ev.translated).c_str(),
                     ev.keyDown ? "true" : "false",
                     OS_NAME,
                     tagField.c_str());
            logger.logEvent(buf);
        });

        if (!keylogger->start()) {
            std::cerr << "Failed to start keylogger\n";
            delete keylogger;
            logger.stop();
            return 1;
        }

        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        keylogger->stop();
        delete keylogger;
        logger.stop();
        return 0;
    }

    // ---- Interactive mode path (unchanged default) ----
    printBanner();

    if (!consent) {
        std::cout << "\nRun with --consent to acknowledge the above and start logging.\n";
        return 0;
    }

    std::cout << "\nConsent acknowledged.\n";

    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);
#if !defined(_WIN32)
    std::signal(SIGHUP, signalHandler);
#else
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
#endif

    Logger logger(OS_NAME, logDir, sessionTag);
    logger.start();

    {
        auto nowMs = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        std::string tagField = sessionTag.empty()
            ? "null"
            : "\"" + jsonEscape(sessionTag) + "\"";

        char hdr[512];
        snprintf(hdr, sizeof(hdr),
                 R"({"event":"session_start","os":"%s","version":"0.1.0","ts":"%s","session_tag":%s})",
                 OS_NAME, msToIso8601(nowMs).c_str(), tagField.c_str());
        logger.logEvent(hdr);
    }

    IKeylogger* keylogger = createPlatformKeylogger();
    if (!keylogger) {
        logger.stop();
        return 1;
    }

    keylogger->setEventCallback([&logger, &sessionTag](const KeyEvent& ev) {
        std::string tagField = sessionTag.empty()
            ? "null"
            : "\"" + jsonEscape(sessionTag) + "\"";

        char buf[640];
        snprintf(buf, sizeof(buf),
                 R"({"ts":"%s","vk":%u,"key":"%s","down":%s,"os":"%s","session_tag":%s})",
                 msToIso8601(ev.timestampMs).c_str(),
                 ev.virtualKey,
                 jsonEscape(ev.translated).c_str(),
                 ev.keyDown ? "true" : "false",
                 OS_NAME,
                 tagField.c_str());
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
