#include "logger.hpp"

#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

Logger::Logger(std::string osName)
    : m_osName(std::move(osName)), m_tempPath(".keylog.partial.jsonl") {}

void Logger::start() {
    m_startTime = std::chrono::system_clock::now();
    m_file.open(m_tempPath, std::ios::out | std::ios::trunc);
    if (!m_file.is_open()) {
        throw std::runtime_error("Logger: cannot open temp file: " + m_tempPath);
    }
}

void Logger::logEvent(const std::string& jsonObject) {
    m_file << jsonObject << "\n";
    m_file.flush();
}

void Logger::stop() {
    m_file.close();

    auto endTime = std::chrono::system_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(endTime - m_startTime);

    long long totalSecs = duration.count();
    long long hh = totalSecs / 3600;
    long long mm = (totalSecs % 3600) / 60;
    long long ss = totalSecs % 60;

    std::time_t startTt = std::chrono::system_clock::to_time_t(m_startTime);
    std::tm startTm{};
#ifdef _WIN32
    localtime_s(&startTm, &startTt);
#else
    localtime_r(&startTt, &startTm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&startTm, "%Y-%m-%d_%H-%M-%S")
        << "-" << hh << "h"
        << std::setw(2) << std::setfill('0') << mm << "m"
        << std::setw(2) << std::setfill('0') << ss << "s"
        << "-" << m_osName << ".jsonl";

    m_finalPath = oss.str();
    if (std::rename(m_tempPath.c_str(), m_finalPath.c_str()) != 0) {
        throw std::runtime_error("Logger: cannot rename temp file to: " + m_finalPath);
    }
}

std::string Logger::currentFilename() const {
    return m_finalPath.empty() ? m_tempPath : m_finalPath;
}
