#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <memory>

#include "AssetIndex/AssetIndex.h"
#include "AssetIndex/BlkRelationships.h"
#include "AssetIndex/DependencyGraph.h"
#include "AssetIndex/PackContents.h"
#include "Jobs/JobSystem.h"

namespace DagorAssetsExtractor::App
{
    constexpr size_t PathBufferSize = 1024;
    constexpr size_t SearchBufferSize = 256;

    struct AppSettings
    {
        std::filesystem::path gameRoot;
        std::filesystem::path exportRoot;
        std::filesystem::path oodleDllPath;
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
        std::shared_ptr<const std::vector<AssetIndex::ResourceRecord>> records;
        std::thread worker;
    };

    struct BlkPreview
    {
        std::filesystem::path path;
        bool loaded = false;
        std::string error;
        size_t propertyCount = 0;
        size_t blockCount = 0;
        std::vector<std::string> lines;
    };

    struct BlkPreviewState
    {
        std::mutex mutex;
        std::filesystem::path loadingPath;
        bool loading = false;
        std::shared_ptr<const BlkPreview> preview;
    };

    struct BlkResourceLinkState
    {
        std::mutex mutex;
        bool loading = false;
        std::string status = "Not built";
        std::shared_ptr<const std::vector<AssetIndex::ResourceRecord>> loadingRecords;
        std::shared_ptr<const std::vector<AssetIndex::BlkResourceLink>> links;
    };

    struct DescriptorResourceLinkState
    {
        std::mutex mutex;
        bool loading = false;
        std::string status = "Not built";
        std::shared_ptr<const std::vector<AssetIndex::ResourceRecord>> loadingRecords;
        std::shared_ptr<const std::vector<AssetIndex::BlkResourceLink>> loadingBlkLinks;
        std::shared_ptr<const std::vector<AssetIndex::DescriptorResourceLink>> links;
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
        std::array<char, PathBufferSize> oodleDllPathBuffer = {};
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
        bool groupByBlk = false;
        float inspectorHeight = -1.0f;


        // UI Cache
        std::shared_ptr<const std::vector<AssetIndex::ResourceRecord>> lastRecords;
        std::shared_ptr<const std::vector<AssetIndex::BlkResourceLink>> lastLinks;
        std::string lastSearch;
        bool lastGroupByBlk = false;
        std::vector<size_t> filteredIndices;

        // Lazy-loaded pack contents cache (sub-assets parsed on demand)
        AssetIndex::PackContentsCache packContentsCache;
        std::atomic_bool packContentsParsing{false};
        BlkPreviewState blkPreview;
        BlkResourceLinkState blkLinks;
        DescriptorResourceLinkState descriptorLinks;
        Jobs::JobSystem backgroundJobs;
    };

    void AddLog(AppState& app, LogLevel level, std::string message);
    std::shared_ptr<const std::vector<AssetIndex::ResourceRecord>> SnapshotRecords(ScanState& scan);
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
    void RequestPackContents(AppState& app, const std::filesystem::path& packPath, AssetIndex::AssetType type);
    void RequestTextureExport(AppState& app, const std::filesystem::path& packPath, const AssetIndex::SubAssetRecord& entry);
    void RequestModelExport(AppState& app, const std::filesystem::path& packPath, const AssetIndex::SubAssetRecord& entry);
    void RequestModelExportByName(AppState& app, const std::filesystem::path& packPath, const std::string& entryName);
    void RequestBatchTextureExport(AppState& app, const std::filesystem::path& packPath, std::shared_ptr<const AssetIndex::PackContents> contents);
    bool IsOodleAvailable(const AppState& app);
    std::shared_ptr<const BlkPreview> SnapshotBlkPreview(AppState& app, const std::filesystem::path& path);
    bool IsBlkPreviewLoading(AppState& app, const std::filesystem::path& path);
    void RequestBlkPreview(AppState& app, const std::filesystem::path& path);
    void RequestBlkResourceLinks(AppState& app, std::shared_ptr<const std::vector<AssetIndex::ResourceRecord>> records);
    std::shared_ptr<const std::vector<AssetIndex::BlkResourceLink>> SnapshotBlkResourceLinks(AppState& app);
    std::string GetBlkResourceLinkStatus(AppState& app);
    bool IsBlkResourceLinksLoading(AppState& app);
    void RequestDescriptorResourceLinks(AppState& app, std::shared_ptr<const std::vector<AssetIndex::ResourceRecord>> records);
    std::shared_ptr<const std::vector<AssetIndex::DescriptorResourceLink>> SnapshotDescriptorResourceLinks(AppState& app);
    std::string GetDescriptorResourceLinkStatus(AppState& app);
    bool IsDescriptorResourceLinksLoading(AppState& app);
    void JoinFinishedBackgroundJobs(AppState& app);
    void WaitForBackgroundJobs(AppState& app);
}
