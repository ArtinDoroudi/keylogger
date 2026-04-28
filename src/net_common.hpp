#pragma once

#include <string>

namespace netcommon {

struct ParsedUrl {
    std::string scheme;     // "http", "https", "tcp"
    std::string host;       // hostname or literal IP
    int         port = 0;   // resolved (defaults: http=80, https=443, tcp required)
    std::string path;       // for http(s); empty for tcp
};

// Parse a URL of the form scheme://host[:port][/path].
// Returns true on success. Path defaults to "/" for http(s) if absent.
bool parseUrl(const std::string& url, ParsedUrl& out, std::string& errorOut);

// True if a hostname or IP literal is loopback (127.0.0.0/8, ::1, "localhost").
// Resolution is purely lexical for "localhost"; numeric checks for IPs.
bool isLoopbackHost(const std::string& host);

// Lowercase ASCII helper.
std::string asciiLower(const std::string& s);

// True when the string is non-empty and consists of safe characters for a
// shared-secret token (alphanumeric, dash, underscore, dot, plus, slash, equals).
// Minimum length is enforced separately by callers.
bool isSafeToken(const std::string& token);

}  // namespace netcommon
