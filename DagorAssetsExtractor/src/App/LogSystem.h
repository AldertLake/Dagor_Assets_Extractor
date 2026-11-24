#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

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
        static constexpr size_t MaxUiLogEntries = 1000;

        LogSystem();
        ~LogSystem();

        void Initialize(const std::filesystem::path& rootDir);
        void AddLog(LogLevel level, const std::string& message);
        void ClearUI();
        void Flush();

        [[nodiscard]] uint64_t GetVersion() const noexcept
        {
            return m_version.load(std::memory_order_acquire);
        }

        std::vector<LogEntry> GetSnapshot() const;
        const std::deque<LogEntry>& GetLogsDirect() const;
        std::mutex& GetMutex() const { return m_mutex; }
        std::filesystem::path GetLogFilePath() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_logFilePath;
        }

    private:
        mutable std::mutex m_mutex;
        std::deque<LogEntry> m_logs;
        std::filesystem::path m_logFilePath;
        std::ofstream m_fileStream;
        std::atomic<uint64_t> m_version{ 0 };
        size_t m_unflushedWrites = 0;
        std::chrono::steady_clock::time_point m_lastFlushTime;
    };
}
