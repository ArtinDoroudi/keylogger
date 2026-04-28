#include "net_common.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace netcommon {

std::string asciiLower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool parseUrl(const std::string& url, ParsedUrl& out, std::string& errorOut) {
    out = {};
    auto schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) {
        errorOut = "URL is missing scheme (expected http://, https://, or tcp://)";
        return false;
    }
    out.scheme = asciiLower(url.substr(0, schemeEnd));
    if (out.scheme != "http" && out.scheme != "https" && out.scheme != "tcp") {
        errorOut = "unsupported URL scheme: " + out.scheme;
        return false;
    }

    std::string rest = url.substr(schemeEnd + 3);
    if (rest.empty()) {
        errorOut = "URL has no host";
        return false;
    }

    auto pathStart = rest.find('/');
    std::string hostport = (pathStart == std::string::npos) ? rest : rest.substr(0, pathStart);
    std::string path     = (pathStart == std::string::npos) ? "" : rest.substr(pathStart);

    if (hostport.empty()) {
        errorOut = "URL has no host";
        return false;
    }

    // Bracketed IPv6 host: [::1]:8080
    if (hostport.front() == '[') {
        auto rb = hostport.find(']');
        if (rb == std::string::npos) {
            errorOut = "malformed bracketed IPv6 host";
            return false;
        }
        out.host = hostport.substr(1, rb - 1);
        std::string portPart = hostport.substr(rb + 1);
        if (!portPart.empty()) {
            if (portPart.front() != ':') {
                errorOut = "expected ':' after bracketed IPv6 host";
                return false;
            }
            out.port = std::atoi(portPart.c_str() + 1);
        }
    } else {
        auto colon = hostport.rfind(':');
        if (colon == std::string::npos) {
            out.host = hostport;
        } else {
            out.host = hostport.substr(0, colon);
            out.port = std::atoi(hostport.c_str() + colon + 1);
        }
    }

    if (out.host.empty()) {
        errorOut = "URL has no host";
        return false;
    }

    if (out.port == 0) {
        if (out.scheme == "http")       out.port = 80;
        else if (out.scheme == "https") out.port = 443;
        else {
            errorOut = "tcp:// URL requires an explicit port";
            return false;
        }
    }
    if (out.port <= 0 || out.port > 65535) {
        errorOut = "URL port out of range";
        return false;
    }

    if (out.scheme == "http" || out.scheme == "https") {
        out.path = path.empty() ? "/" : path;
    }
    return true;
}

bool isLoopbackHost(const std::string& host) {
    std::string h = asciiLower(host);
    if (h == "localhost") return true;
    if (h == "::1")       return true;
    if (h.rfind("127.", 0) == 0) {
        // 127.0.0.0/8
        int a = 0, b = 0, c = 0, d = 0;
        if (sscanf(h.c_str(), "%d.%d.%d.%d", &a, &b, &c, &d) == 4) {
            return (a == 127 && b >= 0 && b < 256 && c >= 0 && c < 256 && d >= 0 && d < 256);
        }
    }
    return false;
}

bool isSafeToken(const std::string& token) {
    if (token.empty()) return false;
    for (char c : token) {
        if (std::isalnum(static_cast<unsigned char>(c))) continue;
        switch (c) {
        case '-': case '_': case '.': case '+': case '/': case '=':
            continue;
        default:
            return false;
        }
    }
    return true;
}

}  // namespace netcommon
