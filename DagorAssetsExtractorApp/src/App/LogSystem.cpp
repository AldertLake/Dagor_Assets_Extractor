#include "App/LogSystem.h"
#include <chrono>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <Windows.h>

namespace fs = std::filesystem;

namespace DagorAssetsExtractor::App
{
    LogSystem::LogSystem()
    {
    }

    LogSystem::~LogSystem()
    {
        if (m_fileStream.is_open())
        {
            m_fileStream.close();
        }
    }

    void LogSystem::Initialize(const fs::path& rootDir)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        fs::path logDir = rootDir / "Log";
        std::error_code ec;
        fs::create_directories(logDir, ec);

        int maxIndex = 0;
        if (!ec)
        {
            for (const auto& entry : fs::directory_iterator(logDir, ec))
            {
                if (entry.is_regular_file())
                {
                    std::string filename = entry.path().filename().string();
                    if (filename.starts_with("Log") && filename.ends_with(".txt"))
                    {
                        try {
                            std::string numStr = filename.substr(3, filename.length() - 7);
                            int num = std::stoi(numStr);
                            maxIndex = std::max(maxIndex, num);
                        } catch (...) {}
                    }
                }
            }
        }

        m_logFilePath = logDir / ("Log" + std::to_string(maxIndex + 1) + ".txt");
        m_fileStream.open(m_logFilePath, std::ios::out | std::ios::app);
    }

    std::string LogSystem::GetCurrentDateString() const
    {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::tm localTime = {};
        localtime_s(&localTime, &time);
        char buffer[32];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &localTime);
        return buffer;
    }

    std::string LogSystem::GetCurrentTimeString() const
    {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::tm localTime = {};
        localtime_s(&localTime, &time);
        char buffer[32];
        std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &localTime);
        return buffer;
    }

    void LogSystem::AddLog(LogLevel level, const std::string& message)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        LogEntry entry;
        entry.time = GetCurrentTimeString();
        entry.level = level;
        entry.message = message;
        entry.formatted = GetCurrentDateString() + " - " + entry.time + " : " + message;

        m_logs.push_back(entry);
        
        if (m_logs.size() > 1000)
        {
            m_logs.erase(m_logs.begin(), m_logs.begin() + 250);
        }

        if (m_fileStream.is_open())
        {
            m_fileStream << entry.formatted << "\n";
            m_fileStream.flush();
        }

        // Also output to debug
        std::string debugMsg = "[DagorExtractor] ";
        switch (level)
        {
        case LogLevel::Info:    debugMsg += "[INFO] "; break;
        case LogLevel::Warning: debugMsg += "[WARN] "; break;
        case LogLevel::Error:   debugMsg += "[ERR]  "; break;
        }
        debugMsg += message + "\n";
        ::OutputDebugStringA(debugMsg.c_str());
    }

    void LogSystem::ClearUI()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_logs.clear();
        
        if (m_fileStream.is_open())
        {
            m_fileStream.close();
            m_fileStream.open(m_logFilePath, std::ios::out | std::ios::trunc);
        }
    }

    std::vector<LogEntry> LogSystem::GetSnapshot() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_logs;
    }

    const std::vector<LogEntry>& LogSystem::GetLogsDirect() const
    {
        return m_logs;
    }
}
