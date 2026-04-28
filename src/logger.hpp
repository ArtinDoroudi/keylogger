#pragma once
#include <chrono>
#include <fstream>
#include <string>

class Logger {
public:
    explicit Logger(std::string osName,
                    std::string logDir = ".",
                    std::string sessionTag = "");

    void start();
    void logEvent(const std::string& jsonObject);
    void stop();
    std::string currentFilename() const;
    const std::string& sessionTag() const { return m_sessionTag; }

private:
    std::string m_osName;
    std::string m_logDir;
    std::string m_sessionTag;
    std::string m_tempPath;
    std::string m_finalPath;
    std::ofstream m_file;
    std::chrono::system_clock::time_point m_startTime;
};
