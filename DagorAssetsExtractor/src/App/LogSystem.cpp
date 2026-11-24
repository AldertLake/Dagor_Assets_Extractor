#include "App/LogSystem.h"

#include <algorithm>
#include <ctime>
#include <windows.h>

namespace fs = std::filesystem;

namespace DagorAssetsExtractor::App
{
    namespace
    {
        constexpr size_t MaxUnflushedLines = 64;
        constexpr auto MaxUnflushedDuration = std::chrono::milliseconds(500);
    }

    LogSystem::LogSystem()
        : m_lastFlushTime(std::chrono::steady_clock::now())
    {
    }

    LogSystem::~LogSystem()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_fileStream.is_open())
        {
            if (m_unflushedWrites > 0)
            {
                m_fileStream.flush();
                m_unflushedWrites = 0;
            }
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
                        try
                        {
                            std::string numStr = filename.substr(3, filename.length() - 7);
                            int num = std::stoi(numStr);
                            maxIndex = std::max(maxIndex, num);
                        }
                        catch (...)
                        {
                        }
                    }
                }
            }
        }

        if (m_fileStream.is_open())
        {
            if (m_unflushedWrites > 0)
            {
                m_fileStream.flush();
            }
            m_fileStream.close();
        }

        m_logFilePath = logDir / ("Log" + std::to_string(maxIndex + 1) + ".txt");
        m_fileStream.open(m_logFilePath, std::ios::out | std::ios::app);
        m_unflushedWrites = 0;
        m_lastFlushTime = std::chrono::steady_clock::now();
    }

    void LogSystem::AddLog(LogLevel level, const std::string& message)
    {
        const auto systemNow = std::chrono::system_clock::now();
        const auto timeValue = std::chrono::system_clock::to_time_t(systemNow);
        std::tm localTime = {};
        localtime_s(&localTime, &timeValue);

        char dateBuffer[16] = {};
        char timeBuffer[16] = {};
        std::strftime(dateBuffer, sizeof(dateBuffer), "%Y-%m-%d", &localTime);
        std::strftime(timeBuffer, sizeof(timeBuffer), "%H:%M:%S", &localTime);

        LogEntry entry;
        entry.time = timeBuffer;
        entry.level = level;
        entry.message = message;
        entry.formatted.reserve(32 + message.size());
        entry.formatted = dateBuffer;
        entry.formatted += " - ";
        entry.formatted += entry.time;
        entry.formatted += " : ";
        entry.formatted += message;

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            if (m_fileStream.is_open())
            {
                m_fileStream << entry.formatted << '\n';
                ++m_unflushedWrites;

                const auto steadyNow = std::chrono::steady_clock::now();
                if (level == LogLevel::Error ||
                    m_unflushedWrites >= MaxUnflushedLines ||
                    (steadyNow - m_lastFlushTime) >= MaxUnflushedDuration)
                {
                    m_fileStream.flush();
                    m_unflushedWrites = 0;
                    m_lastFlushTime = steadyNow;
                }
            }

            m_logs.push_back(std::move(entry));
            if (m_logs.size() > MaxUiLogEntries)
            {
                m_logs.pop_front();
            }

            m_version.fetch_add(1, std::memory_order_release);
        }

#ifdef _DEBUG
        std::string debugMsg = "[DagorExtractor] ";
        switch (level)
        {
        case LogLevel::Info:    debugMsg += "[INFO] "; break;
        case LogLevel::Warning: debugMsg += "[WARN] "; break;
        case LogLevel::Error:   debugMsg += "[ERR]  "; break;
        }
        debugMsg += message;
        debugMsg += '\n';
        ::OutputDebugStringA(debugMsg.c_str());
#endif
    }

    void LogSystem::ClearUI()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_logs.clear();
        m_version.fetch_add(1, std::memory_order_release);

        if (m_fileStream.is_open())
        {
            m_fileStream.close();
            m_fileStream.open(m_logFilePath, std::ios::out | std::ios::trunc);
        }
        m_unflushedWrites = 0;
        m_lastFlushTime = std::chrono::steady_clock::now();
    }

    void LogSystem::Flush()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_fileStream.is_open() && m_unflushedWrites > 0)
        {
            m_fileStream.flush();
            m_unflushedWrites = 0;
            m_lastFlushTime = std::chrono::steady_clock::now();
        }
    }

    std::vector<LogEntry> LogSystem::GetSnapshot() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return std::vector<LogEntry>(m_logs.begin(), m_logs.end());
    }

    const std::deque<LogEntry>& LogSystem::GetLogsDirect() const
    {
        return m_logs;
    }
}
