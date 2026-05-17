#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "AssetIndex/AssetIndex.h"

namespace DagorAssetsExtractor::App
{
    constexpr size_t PathBufferSize = 1024;
    constexpr size_t SearchBufferSize = 256;

    struct AppSettings
    {
        std::filesystem::path gameRoot;
        std::filesystem::path exportRoot;
        std::string textureExportFormat = "DDS";
        std::string modelExportFormat = "GLB";
        bool exportTexturesWithModels = true;
        bool preserveOriginalDDS = true;
        bool useOodleFromGameFolder = true;
        bool indexCacheEnabled = true;
        std::string lastOpenedPackFilters;
    };

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
    };

    struct ScanState
    {
        std::atomic_bool running = false;
        std::atomic_bool cancelRequested = false;
        std::atomic_bool completed = false;
        std::atomic_bool failed = false;
        std::atomic<uint64_t> filesVisited = 0;
        std::atomic<uint64_t> recordsFound = 0;
        std::mutex mutex;
        std::string status = "Idle";
        std::string cacheStatus = "Not loaded";
        bool cacheLoaded = false;
        std::vector<AssetIndex::ResourceRecord> records;
        std::thread worker;
    };

    struct AppState
    {
        void* hwnd = nullptr; // Using void* to avoid Windows.h include here
        AppSettings settings;
        std::filesystem::path settingsPath;
        std::filesystem::path indexCachePath;
        AssetIndex::ValidationResult validation;
        ScanState scan;
        std::mutex logMutex;
        std::vector<LogEntry> logs;

        std::array<char, PathBufferSize> gameRootBuffer = {};
        std::array<char, PathBufferSize> exportRootBuffer = {};
        std::array<char, SearchBufferSize> searchBuffer = {};
        std::wstring selectedPath;

        bool firstRunPromptOpen = false;
        bool settingsWindowOpen = false;
        bool aboutWindowOpen = false;
        bool showVromfs = true;
        bool showGrp = true;
        bool showDxp = true;
        bool showDescriptors = true;
        bool showBlk = true;
    };

    void AddLog(AppState& app, LogLevel level, std::string message);
    std::vector<AssetIndex::ResourceRecord> SnapshotRecords(ScanState& scan);
    std::string GetScanStatus(ScanState& scan);
    std::string GetCacheStatus(ScanState& scan);
    void SetCacheStatus(ScanState& scan, std::string status, bool loaded);
    void SetScanStatus(ScanState& scan, std::string status);
    
    // Core actions that UI can trigger
    void LoadSettings(AppSettings& settings, const std::filesystem::path& path);
    void SetGameRoot(AppState& app, const std::filesystem::path& gameRoot);
    void SaveSettingsWithLog(AppState& app);
    void ApplyGameRootBuffer(AppState& app);
    void SyncSettingsBuffers(AppState& app);
    void LoadIndexCacheIntoApp(AppState& app);
    void StartScan(AppState& app);
    void StopScan(AppState& app);
    void JoinFinishedScan(AppState& app);
    void ClearResourceIndex(AppState& app, std::string status);
}
