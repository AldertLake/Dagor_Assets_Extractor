#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <filesystem>
#include <fstream>

namespace DagorAssetsExtractor::App
{
    enum class LogLevel
    {
        Info,
        Warning,
        Error
    };

    struct LogEntry
    {
        std::string time;
        LogLevel level = LogLevel::Info;
        std::string message;
        std::string formatted;
    };

    class LogSystem
    {
    public:
        LogSystem();
        ~LogSystem();

        void Initialize(const std::filesystem::path& rootDir);
        void AddLog(LogLevel level, const std::string& message);
        void ClearUI();

        std::vector<LogEntry> GetSnapshot() const;
        const std::vector<LogEntry>& GetLogsDirect() const;
        std::mutex& GetMutex() const { return m_mutex; }

    private:
        mutable std::mutex m_mutex;
        std::vector<LogEntry> m_logs;
        std::filesystem::path m_logFilePath;
        std::ofstream m_fileStream;

        std::string GetCurrentDateString() const;
        std::string GetCurrentTimeString() const;
    };
}
