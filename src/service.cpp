#include "service.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

#ifdef _WIN32
constexpr const char* kServiceName = "EthicalKeylogger";
#elif defined(__APPLE__)
constexpr const char* kPlistPath   = "/Library/LaunchDaemons/com.ethical-keylogger.plist";
constexpr const char* kPlistLabel  = "com.ethical-keylogger";
#else
constexpr const char* kUnitPath    = "/etc/systemd/system/keylogger.service";
constexpr const char* kUnitName    = "keylogger.service";
#endif

// Run a shell command and return its exit code. Output goes to the inherited
// stdout/stderr so the user sees progress directly.
int runCmd(const std::string& cmd) {
    int rc = std::system(cmd.c_str());
#ifdef _WIN32
    return rc;
#else
    if (rc == -1) return -1;
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    return rc;
#endif
}

// Capture stdout of a command. Returns the trimmed string. exitCode is set to
// the command's exit code.
std::string captureCmd(const std::string& cmd, int* exitCode) {
#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) {
        if (exitCode) *exitCode = -1;
        return {};
    }
    std::string out;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe)) {
        out += buf;
    }
#ifdef _WIN32
    int rc = _pclose(pipe);
#else
    int rc = pclose(pipe);
    if (rc != -1 && WIFEXITED(rc)) rc = WEXITSTATUS(rc);
#endif
    if (exitCode) *exitCode = rc;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return out;
}

// Quote a string for use as a single shell argument inside a command line that
// will be executed via `system()` or written into a service definition.
std::string shellQuote(const std::string& s) {
#ifdef _WIN32
    // Windows: wrap in double quotes and escape embedded quotes.
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    out += "\"";
    return out;
#else
    // POSIX: single-quote, escape embedded single quotes.
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
#endif
}

}  // namespace

namespace service {

bool isElevated() {
#ifdef _WIN32
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuthority, 2,
                                 SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS,
                                 0, 0, 0, 0, 0, 0, &adminGroup)) {
        if (!CheckTokenMembership(nullptr, adminGroup, &isAdmin)) {
            isAdmin = FALSE;
        }
        FreeSid(adminGroup);
    }
    return isAdmin == TRUE;
#else
    return geteuid() == 0;
#endif
}

#ifdef __linux__

bool isRegistered() {
    struct stat st;
    return stat(kUnitPath, &st) == 0;
}

int install(const ServiceInstallParams& params) {
    if (isRegistered() && !params.force) {
        std::cerr << "error: service is already registered. Use --uninstall first "
                     "or pass --force to overwrite.\n";
        return 2;
    }

    std::ostringstream unit;
    unit << "[Unit]\n"
         << "Description=ethical-keylogger-demo (service mode)\n"
         << "After=network.target\n"
         << "\n"
         << "[Service]\n"
         << "Type=simple\n"
         << "ExecStart=" << params.binaryPath
         << " --service"
         << " --consent-file " << params.consentFile
         << " --log-dir " << params.logDir;
    if (!params.sessionTag.empty()) {
        unit << " --session-tag " << params.sessionTag;
    }
    unit << "\n"
         << "Restart=on-failure\n"
         << "RestartSec=5\n"
         << "StandardOutput=journal\n"
         << "StandardError=journal\n"
         << "\n"
         << "[Install]\n"
         << "WantedBy=multi-user.target\n";

    std::cout << "Writing unit file to " << kUnitPath << " ... " << std::flush;
    {
        std::ofstream f(kUnitPath, std::ios::trunc);
        if (!f.is_open()) {
            std::cout << "FAILED\n";
            std::cerr << "error: could not open " << kUnitPath << " for writing\n";
            return 1;
        }
        f << unit.str();
        if (!f.good()) {
            std::cout << "FAILED\n";
            std::cerr << "error: write to " << kUnitPath << " failed\n";
            return 1;
        }
    }
    std::cout << "done\n";

    auto step = [](const char* desc, const std::string& cmd) -> int {
        std::cout << "Running " << desc << " ... " << std::flush;
        int rc = runCmd(cmd + " >/dev/null 2>&1");
        if (rc != 0) {
            std::cout << "FAILED (exit " << rc << ")\n";
            return 1;
        }
        std::cout << "done\n";
        return 0;
    };

    if (step("systemctl daemon-reload",       "systemctl daemon-reload")        != 0) return 1;
    if (step("systemctl enable keylogger.service",
             std::string("systemctl enable ") + kUnitName)                       != 0) return 1;
    if (step("systemctl start keylogger.service",
             std::string("systemctl start ")  + kUnitName)                       != 0) return 1;

    std::cout << "Service installed and running.\n";
    std::cout << "Unit file: " << kUnitPath << "\n";
    return 0;
}

int uninstall() {
    if (!isRegistered()) {
        std::cout << "warning: no service is registered (nothing to do).\n";
        return 0;
    }

    std::cout << "Running systemctl stop keylogger.service ... " << std::flush;
    runCmd(std::string("systemctl stop ") + kUnitName + " >/dev/null 2>&1");
    std::cout << "done\n";

    std::cout << "Running systemctl disable keylogger.service ... " << std::flush;
    runCmd(std::string("systemctl disable ") + kUnitName + " >/dev/null 2>&1");
    std::cout << "done\n";

    std::cout << "Removing " << kUnitPath << " ... " << std::flush;
    if (std::remove(kUnitPath) != 0) {
        std::cout << "FAILED\n";
        return 1;
    }
    std::cout << "done\n";

    std::cout << "Running systemctl daemon-reload ... " << std::flush;
    runCmd("systemctl daemon-reload >/dev/null 2>&1");
    std::cout << "done\n";

    std::cout << "Service uninstalled.\n";
    return 0;
}

int status() {
    if (!isRegistered()) {
        std::cout << "Service: not registered\n";
        return 1;
    }

    int rc = 0;
    std::string enabled = captureCmd(std::string("systemctl is-enabled ") + kUnitName +
                                     " 2>/dev/null", &rc);
    std::string active  = captureCmd(std::string("systemctl is-active ")  + kUnitName +
                                     " 2>/dev/null", &rc);

    if (enabled.empty()) enabled = "unknown";
    if (active.empty())  active  = "unknown";

    std::cout << "Service:    registered (" << kUnitPath << ")\n";
    std::cout << "Enabled:    " << enabled << "\n";
    std::cout << "Active:     " << active << "\n";
    return 0;
}

#elif defined(__APPLE__)

bool isRegistered() {
    struct stat st;
    return stat(kPlistPath, &st) == 0;
}

int install(const ServiceInstallParams& params) {
    if (isRegistered() && !params.force) {
        std::cerr << "error: service is already registered. Use --uninstall first "
                     "or pass --force to overwrite.\n";
        return 2;
    }

    std::string stdoutLog = params.logDir + "/launchd-stdout.log";
    std::string stderrLog = params.logDir + "/launchd-stderr.log";

    auto xml = [](const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            switch (c) {
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '&':  out += "&amp;";  break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c;        break;
            }
        }
        return out;
    };

    std::ostringstream plist;
    plist << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
          << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
             "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
          << "<plist version=\"1.0\">\n"
          << "<dict>\n"
          << "    <key>Label</key>\n"
          << "    <string>" << kPlistLabel << "</string>\n"
          << "    <key>ProgramArguments</key>\n"
          << "    <array>\n"
          << "        <string>" << xml(params.binaryPath)  << "</string>\n"
          << "        <string>--service</string>\n"
          << "        <string>--consent-file</string>\n"
          << "        <string>" << xml(params.consentFile) << "</string>\n"
          << "        <string>--log-dir</string>\n"
          << "        <string>" << xml(params.logDir)      << "</string>\n";
    if (!params.sessionTag.empty()) {
        plist << "        <string>--session-tag</string>\n"
              << "        <string>" << xml(params.sessionTag) << "</string>\n";
    }
    plist << "    </array>\n"
          << "    <key>RunAtLoad</key>\n"
          << "    <true/>\n"
          << "    <key>KeepAlive</key>\n"
          << "    <true/>\n"
          << "    <key>StandardOutPath</key>\n"
          << "    <string>" << xml(stdoutLog) << "</string>\n"
          << "    <key>StandardErrorPath</key>\n"
          << "    <string>" << xml(stderrLog) << "</string>\n"
          << "</dict>\n"
          << "</plist>\n";

    // If overwriting, unload any existing instance first so launchctl load -w
    // does not race against an already-loaded daemon.
    if (isRegistered() && params.force) {
        std::cout << "Unloading existing daemon ... " << std::flush;
        runCmd(std::string("launchctl unload ") + kPlistPath + " >/dev/null 2>&1");
        std::cout << "done\n";
    }

    std::cout << "Writing plist to " << kPlistPath << " ... " << std::flush;
    {
        std::ofstream f(kPlistPath, std::ios::trunc);
        if (!f.is_open()) {
            std::cout << "FAILED\n";
            std::cerr << "error: could not open " << kPlistPath << " for writing\n";
            return 1;
        }
        f << plist.str();
        if (!f.good()) {
            std::cout << "FAILED\n";
            return 1;
        }
    }
    std::cout << "done\n";

    // Match the documented permissions for LaunchDaemons.
    chmod(kPlistPath, 0644);

    std::cout << "Running launchctl load -w " << kPlistPath << " ... " << std::flush;
    int rc = runCmd(std::string("launchctl load -w ") + kPlistPath);
    if (rc != 0) {
        std::cout << "FAILED (exit " << rc << ")\n";
        return 1;
    }
    std::cout << "done\n";

    std::cout << "Service installed and running.\n";
    std::cout << "Plist: " << kPlistPath << "\n";
    return 0;
}

int uninstall() {
    if (!isRegistered()) {
        std::cout << "warning: no service is registered (nothing to do).\n";
        return 0;
    }

    std::cout << "Running launchctl unload " << kPlistPath << " ... " << std::flush;
    runCmd(std::string("launchctl unload ") + kPlistPath + " >/dev/null 2>&1");
    std::cout << "done\n";

    std::cout << "Removing " << kPlistPath << " ... " << std::flush;
    if (std::remove(kPlistPath) != 0) {
        std::cout << "FAILED\n";
        return 1;
    }
    std::cout << "done\n";

    std::cout << "Service uninstalled.\n";
    return 0;
}

int status() {
    if (!isRegistered()) {
        std::cout << "Service: not registered\n";
        return 1;
    }

    int rc = 0;
    std::string out = captureCmd(std::string("launchctl list ") + kPlistLabel +
                                 " 2>/dev/null", &rc);

    std::cout << "Service:    registered (" << kPlistPath << ")\n";
    if (rc == 0) {
        std::cout << "Loaded:     yes\n";
        // launchctl list emits a small plist-like dict; surface the PID line if present.
        std::istringstream is(out);
        std::string line;
        while (std::getline(is, line)) {
            if (line.find("\"PID\"") != std::string::npos ||
                line.find("\"LastExitStatus\"") != std::string::npos) {
                std::cout << "  " << line << "\n";
            }
        }
    } else {
        std::cout << "Loaded:     no\n";
    }
    return 0;
}

#elif defined(_WIN32)

bool isRegistered() {
    SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceA(scm, kServiceName, SERVICE_QUERY_STATUS);
    bool registered = (svc != nullptr);
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return registered;
}

int install(const ServiceInstallParams& params) {
    if (isRegistered() && !params.force) {
        std::cerr << "error: service is already registered. Use --uninstall first "
                     "or pass --force to overwrite.\n";
        return 2;
    }

    if (isRegistered() && params.force) {
        std::cout << "Removing existing registration ... " << std::flush;
        runCmd(std::string("sc stop ")   + kServiceName + " >NUL 2>&1");
        runCmd(std::string("sc delete ") + kServiceName + " >NUL 2>&1");
        std::cout << "done\n";
    }

    std::ostringstream binPath;
    binPath << "\"" << params.binaryPath << "\""
            << " --service"
            << " --consent-file \"" << params.consentFile << "\""
            << " --log-dir \"" << params.logDir << "\"";
    if (!params.sessionTag.empty()) {
        binPath << " --session-tag \"" << params.sessionTag << "\"";
    }

    std::ostringstream createCmd;
    // sc.exe requires a space after `binPath=` (the value follows the equals
    // sign with a single space, by design).
    createCmd << "sc create " << kServiceName
              << " binPath= \"" << binPath.str() << "\""
              << " start= auto";

    std::cout << "Running sc create " << kServiceName << " ... " << std::flush;
    int rc = runCmd(createCmd.str() + " >NUL 2>&1");
    if (rc != 0) {
        std::cout << "FAILED (exit " << rc << ")\n";
        return 1;
    }
    std::cout << "done\n";

    std::cout << "Running sc start " << kServiceName << " ... " << std::flush;
    rc = runCmd(std::string("sc start ") + kServiceName + " >NUL 2>&1");
    if (rc != 0) {
        std::cout << "FAILED (exit " << rc << ")\n";
        return 1;
    }
    std::cout << "done\n";

    std::cout << "Service installed and running.\n";
    std::cout << "Service name: " << kServiceName << "\n";
    return 0;
}

int uninstall() {
    if (!isRegistered()) {
        std::cout << "warning: no service is registered (nothing to do).\n";
        return 0;
    }

    std::cout << "Running sc stop "   << kServiceName << " ... " << std::flush;
    runCmd(std::string("sc stop ")   + kServiceName + " >NUL 2>&1");
    std::cout << "done\n";

    std::cout << "Running sc delete " << kServiceName << " ... " << std::flush;
    int rc = runCmd(std::string("sc delete ") + kServiceName + " >NUL 2>&1");
    if (rc != 0) {
        std::cout << "FAILED (exit " << rc << ")\n";
        return 1;
    }
    std::cout << "done\n";

    std::cout << "Service uninstalled.\n";
    return 0;
}

int status() {
    if (!isRegistered()) {
        std::cout << "Service: not registered\n";
        return 1;
    }

    int rc = 0;
    std::string out = captureCmd(std::string("sc query ") + kServiceName, &rc);

    std::cout << "Service:    registered (" << kServiceName << ")\n";
    std::cout << out << "\n";
    return 0;
}

#else

bool isRegistered() { return false; }
int install(const ServiceInstallParams&) {
    std::cerr << "error: service install is not supported on this platform\n";
    return 1;
}
int uninstall() {
    std::cerr << "error: service uninstall is not supported on this platform\n";
    return 1;
}
int status() {
    std::cerr << "error: service status is not supported on this platform\n";
    return 1;
}

#endif

}  // namespace service
