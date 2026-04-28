#pragma once

#include <string>

struct ServiceInstallParams {
    std::string binaryPath;     // absolute path to keylogger binary
    std::string consentFile;    // absolute path to consent file
    std::string logDir;         // absolute path to log directory
    std::string sessionTag;     // optional, may be empty
    bool force = false;         // overwrite existing registration
};

namespace service {

// Returns true if the current process has the privileges required to
// install/uninstall a system service (root on POSIX, Administrator on Windows).
bool isElevated();

// Returns true if a service registration currently exists.
bool isRegistered();

// Install (and start) the service. Prints step-by-step progress to stdout.
// Returns process exit code (0 on success, 2 on validation/privilege errors,
// 1 on operational failures).
int install(const ServiceInstallParams& params);

// Stop, disable, and remove the service registration. Idempotent: returns 0
// with a warning if no registration exists.
int uninstall();

// Print a human-readable status summary. Returns 0 if registered, 1 if not.
int status();

}  // namespace service
