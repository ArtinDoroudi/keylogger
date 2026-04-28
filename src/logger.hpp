#pragma once
#include <chrono>
#include <fstream>
#include <string>

class Logger {
public:
    explicit Logger(std::string osName);

    void start();
    void logEvent(const std::string& jsonObject);
    void stop();
    std::string currentFilename() const;

private:
    std::string m_osName;
    std::string m_tempPath;
    std::string m_finalPath;
    std::ofstream m_file;
    std::chrono::system_clock::time_point m_startTime;
};
