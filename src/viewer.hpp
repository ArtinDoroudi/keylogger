#pragma once

#include <string>

namespace viewer {

struct Config {
    std::string bindHost   = "127.0.0.1";
    int         bindPort   = 8765;
    bool        listenPublic = false;
    std::string token;            // required for non-loopback binds
    std::string tlsCert;          // optional TLS cert (PEM)
    std::string tlsKey;           // optional TLS key  (PEM)
    std::string storageDir;       // optional .jsonl persistence
    int         maxClients = 32;
};

// Run the viewer server in the foreground. Blocks until SIGINT/SIGTERM.
// Returns process exit code (0 success, 2 validation, 1 runtime).
int run(const Config& cfg);

}  // namespace viewer
