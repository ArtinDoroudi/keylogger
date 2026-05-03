#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

// ---- JSON field extraction -------------------------------------------------

static std::string extractString(const std::string& line, const std::string& field) {
    std::string needle = "\"" + field + "\":\"";
    auto pos = line.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    std::string val;
    for (; pos < line.size(); ++pos) {
        if (line[pos] == '\\' && pos + 1 < line.size()) {
            ++pos;
            if      (line[pos] == '"')  val += '"';
            else if (line[pos] == '\\') val += '\\';
            else  { val += '\\'; val += line[pos]; }
        } else if (line[pos] == '"') {
            break;
        } else {
            val += line[pos];
        }
    }
    return val;
}

static bool extractBool(const std::string& line, const std::string& field) {
    std::string needle = "\"" + field + "\":";
    auto pos = line.find(needle);
    if (pos == std::string::npos) return false;
    return line.compare(pos + needle.size(), 4, "true") == 0;
}

// ---- timestamp helpers -----------------------------------------------------

struct HMSms { int h, m, s; };

static HMSms parseTime(const std::string& ts) {
    HMSms t{};
    if (ts.size() >= 19)
        sscanf(ts.c_str() + 11, "%d:%d:%d", &t.h, &t.m, &t.s);
    return t;
}

static long elapsedSec(const HMSms& a, const HMSms& b) {
    return (b.h * 3600L + b.m * 60 + b.s) - (a.h * 3600L + a.m * 60 + a.s);
}

// ---- main ------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: log_viewer <file.jsonl>\n";
        return 1;
    }

    std::ifstream f(argv[1]);
    if (!f) {
        std::cerr << "Cannot open: " << argv[1] << "\n";
        return 1;
    }

    std::string sessionTs, sessionOs, sessionVer;
    std::string firstEventTs, lastEventTs;
    int         totalEvents = 0;
    std::string reconstructed;
    std::string lastApp, lastWindow;

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;

        if (extractString(line, "event") == "session_start") {
            sessionTs  = extractString(line, "ts");
            sessionOs  = extractString(line, "os");
            sessionVer = extractString(line, "version");
            continue;
        }

        if (!extractBool(line, "down")) continue;

        std::string key    = extractString(line, "key");
        std::string ts     = extractString(line, "ts");
        std::string app    = extractString(line, "app");
        std::string window = extractString(line, "window");

        if (app != lastApp || window != lastWindow) {
            if (!reconstructed.empty()) reconstructed += "\n";
            reconstructed += "--- [" + (app.empty() ? "?" : app) + "] "
                           + (window.empty() ? "(untitled)" : window) + " ---\n";
            lastApp    = app;
            lastWindow = window;
        }

        if (firstEventTs.empty()) firstEventTs = ts;
        lastEventTs = ts;
        ++totalEvents;
        reconstructed += key;
    }

    std::cout << "Session : " << (sessionTs.empty() ? "(no session_start)" : sessionTs) << "\n";
    std::cout << "OS      : " << (sessionOs.empty() ? "(unknown)" : sessionOs);
    if (!sessionVer.empty()) std::cout << "  v" << sessionVer;
    std::cout << "\n";
    std::cout << "Events  : " << totalEvents << " key-down\n";
    if (!firstEventTs.empty() && !lastEventTs.empty())
        std::cout << "Duration: " << elapsedSec(parseTime(firstEventTs), parseTime(lastEventTs)) << "s\n";
    std::cout << "---\n";
    std::cout << reconstructed << "\n";

    return 0;
}
