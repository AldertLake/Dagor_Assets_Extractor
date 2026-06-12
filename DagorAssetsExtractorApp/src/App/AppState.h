#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <memory>

#include "AssetIndex/AssetIndex.h"
#include "AssetIndex/BlkRelationships.h"
#include "AssetIndex/DependencyGraph.h"
#include "AssetIndex/PackContents.h"
#include "Jobs/JobSystem.h"
#include "App/LogSystem.h"

namespace DagorAssetsExtractor::Export
{
    enum class TextureMapKind : int;
}

namespace DagorAssetsExtractor::App
{
    constexpr size_t PathBufferSize = 1024;
    constexpr size_t SearchBufferSize = 256;

    struct AppSettings
    {
        std::filesystem::path gameRoot;
        std::filesystem::path exportRoot;
        std::string textureExportFormat = "DDS";
        int staticModelMaxLods = 1;
        bool exportTexturesWithModels = true;
        bool embedTexturesInGlb = true;
        bool exportJsonManifest = false;
        bool preserveOriginalDDS = true;
        bool indexCacheEnabled = true;
        size_t maxLoadedPackContents = 8;
        bool showVromfs = true;
        bool showGrp = true;
        bool showDxp = true;
        bool showDescriptors = true;
        bool showBlk = true;
        bool groupByBlk = false;
        std::string lastOpenedPackFilters;
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
        bool cacheLoading = false;
        std::string status = "Not built";
        std::atomic_bool cancelRequested = false;
        std::shared_ptr<const std::vector<AssetIndex::ResourceRecord>> loadingRecords;
        std::shared_ptr<const std::vector<AssetIndex::ResourceRecord>> cacheLoadingRecords;
        std::shared_ptr<const std::vector<AssetIndex::BlkResourceLink>> loadingBlkLinks;
        std::shared_ptr<const std::vector<AssetIndex::DescriptorResourceLink>> links;
        bool globalLinks = false;
        std::optional<std::filesystem::path> scopePath;
        std::string cacheStatus = "Not loaded";
    };

    struct ProgressJobSnapshot
    {
        bool running = false;
        bool cancelRequested = false;
        bool windowOpen = false;
        std::string title;
        std::string status;
        std::string currentItem;
        uint64_t current = 0;
        uint64_t total = 0;
    };

    struct ProgressJobState
    {
        std::mutex mutex;
        std::atomic_bool running = false;
        std::atomic_bool cancelRequested = false;
        bool windowOpen = false;
        std::string title;
        std::string status;
        std::string currentItem;
        uint64_t current = 0;
        uint64_t total = 0;
    };

    struct ExportProgressState
    {
        std::mutex mutex;
        std::atomic_bool running = false;
        std::atomic_bool cancelRequested = false;
        std::string title;
        std::string status;
        std::string currentItem;
        std::string outputDir;
        uint64_t current = 0;
        uint64_t total = 0;
    };

    struct PendingDynModelExport
    {
        std::filesystem::path packPath;
        AssetIndex::SubAssetRecord entry;
        bool includeTextures = true;
        bool includeSkeletonRig = true;
        bool includeAnimations = false;
        bool openRequested = false;
    };

    struct TexturePreviewImage
    {
        uint64_t generation = 0;
        std::filesystem::path packPath;
        std::string entryName;
        uint32_t entryOffset = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t stride = 0;
        bool srgb = false;
        std::vector<uint8_t> bgraPixels;
        struct MapPreview
        {
            Export::TextureMapKind kind;
            uint32_t width = 0;
            uint32_t height = 0;
            uint32_t stride = 0;
            std::vector<uint8_t> bgraPixels;
        };
        std::vector<MapPreview> mapPreviews;
    };

    struct TexturePreviewSnapshot
    {
        uint64_t generation = 0;
        std::filesystem::path packPath;
        std::string entryName;
        uint32_t entryOffset = 0;
        uint32_t sourceWidth = 0;
        uint32_t sourceHeight = 0;
        uint32_t sourceSize = 0;
        uint8_t mipCount = 0;
        std::string format;
        std::string compression;
        bool srgb = false;
        bool loading = false;
        std::string error;
        std::vector<std::string> warnings;
    };

    struct TexturePreviewState
    {
        std::mutex mutex;
        uint64_t generation = 0;
        std::filesystem::path packPath;
        std::string entryName;
        uint32_t entryOffset = 0;
        uint32_t sourceWidth = 0;
        uint32_t sourceHeight = 0;
        uint32_t sourceSize = 0;
        uint8_t mipCount = 0;
        std::string format;
        std::string compression;
        bool srgb = false;
        bool loading = false;
        std::string error;
        std::vector<std::string> warnings;
        std::shared_ptr<TexturePreviewImage> pendingImage;

        // Main-thread-only viewer preferences.
        bool windowOpen = false;
        bool fitToWindow = true;
        bool checkerboard = true;
        float zoom = 1.0f;
        uint32_t selectedMapMask = 0;
        uint64_t uploadedGeneration = 0;
    };

    struct ModelPreviewSubmesh
    {
        std::string name;
        uint32_t startIndex = 0;
        uint32_t indexCount = 0;
        float bboxMin[3] = { 1e30f, 1e30f, 1e30f };
        float bboxMax[3] = { -1e30f, -1e30f, -1e30f };
    };

    struct ModelPreviewBone
    {
        std::string name;
        uint16_t parentIndex = 0xFFFFu;
        float worldPos[3] = { 0.0f, 0.0f, 0.0f };
    };

    struct ModelPreviewVertex
    {
        float pos[3];
        float normal[3];
    };

    struct ModelPreviewData
    {
        uint64_t generation = 0;
        std::filesystem::path packPath;
        std::string entryName;
        std::vector<ModelPreviewVertex> vertices;
        std::vector<uint32_t> indices;
        float bboxMin[3] = { 0.0f, 0.0f, 0.0f };
        float bboxMax[3] = { 0.0f, 0.0f, 0.0f };
        uint32_t grpClassId = 0;
        uint32_t subMeshCount = 0;
        size_t fileSize = 0;
        std::vector<ModelPreviewSubmesh> submeshes;
        std::vector<ModelPreviewBone> bones;
    };

    struct ModelPreviewLighting
    {
        float lightYaw = 45.0f;
        float lightPitch = 45.0f;
        float keyColor[3] = { 1.00f, 0.96f, 0.90f };
        float fillColor[3] = { 0.60f, 0.72f, 0.85f };
        float ambientColor[3] = { 0.18f, 0.20f, 0.23f };
        float baseColor[3] = { 0.62f, 0.66f, 0.72f };
        float specIntensity = 0.45f;
        float specPower = 32.0f;
        float backgroundColor[3] = { 0.12f, 0.13f, 0.15f };
        bool showGround = true;
    };

    struct ModelPreviewState
    {
        std::mutex mutex;
        uint64_t generation = 0;
        std::filesystem::path packPath;
        std::string entryName;
        bool loading = false;
        bool popupOpen = false;
        std::string error;
        std::shared_ptr<ModelPreviewData> pendingData;

        // Current loaded model stats
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        uint32_t grpClassId = 0;
        uint32_t subMeshCount = 0;
        size_t fileSize = 0;
        float bboxMin[3] = { 0.0f, 0.0f, 0.0f };
        float bboxMax[3] = { 0.0f, 0.0f, 0.0f };
        std::vector<ModelPreviewSubmesh> submeshes;
        int hoveredSubmeshIndex = -1;
        std::vector<ModelPreviewVertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<ModelPreviewBone> bones;
        bool showBones = false;

        // Camera control preferences.
        float yaw = 45.0f;
        float pitch = 30.0f;
        float zoom = 5.0f;
        float panWorldX = 0.0f;
        float panWorldY = 0.0f;
        float panWorldZ = 0.0f;
        int renderMode = 0;

        // Performance statistics
        float renderLatencyMs = 0.0f;
        float fpsHistory[90] = {};
        float latencyHistory[90] = {};
        float frameTimeHistory[90] = {};
        int statOffset = 0;
        ModelPreviewLighting lighting;
    };


    struct AppState
    {
        void* hwnd = nullptr; // Using void* to avoid Windows.h include here
        AppSettings settings;
        std::filesystem::path settingsPath;
        std::filesystem::path indexCachePath;
        std::filesystem::path descriptorLinksCachePath;
        AssetIndex::ValidationResult validation;
        ScanState scan;
        LogSystem logger;

        std::array<char, PathBufferSize> gameRootBuffer = {};
        std::array<char, PathBufferSize> exportRootBuffer = {};
        std::array<char, SearchBufferSize> searchBuffer = {};
        std::array<char, SearchBufferSize> subAssetSearchBuffer = {};
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
        float bottomPanelHeight = -1.0f;


        // UI Cache
        std::shared_ptr<const std::vector<AssetIndex::ResourceRecord>> lastRecords;
        std::shared_ptr<const std::vector<AssetIndex::BlkResourceLink>> lastLinks;
        std::string lastSearch;
        bool lastGroupByBlk = false;
        std::vector<size_t> filteredIndices;

        std::wstring subAssetFilterPath;
        std::string lastSubAssetSearch;
        size_t lastSubAssetCount = 0;
        std::vector<size_t> filteredSubAssetIndices;

        std::shared_ptr<const std::vector<AssetIndex::DescriptorResourceLink>> descriptorUiLinks;
        std::wstring descriptorUiSelectedPath;
        AssetIndex::AssetType descriptorUiSelectedType = AssetIndex::AssetType::Unknown;
        std::vector<size_t> descriptorUiDescriptorMatches;
        std::vector<size_t> descriptorUiGrpMatches;
        std::vector<std::pair<size_t, size_t>> descriptorUiDxpMatches;

        // Lazy-loaded pack contents cache (sub-assets parsed on demand)
        AssetIndex::PackContentsCache packContentsCache;
        std::atomic_bool packContentsParsing{false};
        BlkPreviewState blkPreview;
        BlkResourceLinkState blkLinks;
        DescriptorResourceLinkState descriptorLinks;
        ProgressJobState progressJob;
        ExportProgressState exportProgress;
        TexturePreviewState texturePreview;
        ModelPreviewState modelPreview;
        std::optional<PendingDynModelExport> pendingDynModelExport;
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
    std::string LoadImGuiSettings(const std::filesystem::path& path);
    bool SaveSharedSettings(const AppSettings& settings, const std::filesystem::path& path, std::string_view imguiIniData, std::string& error);
    void ApplySettingsToUiState(AppState& app);
    void UpdateSettingsFromUiState(AppState& app);
    void SetGameRoot(AppState& app, const std::filesystem::path& gameRoot);
    void SaveSettingsWithLog(AppState& app);
    void SaveSettingsWithLog(AppState& app, std::string_view imguiIniData);
    void ApplyGameRootBuffer(AppState& app);
    void SyncSettingsBuffers(AppState& app);
    void LoadIndexCacheIntoApp(AppState& app);
    void StartScan(AppState& app);
    void StopScan(AppState& app);
    void JoinFinishedScan(AppState& app);
    void ClearResourceIndex(AppState& app, std::string status);
    void RequestPackContents(AppState& app, const std::filesystem::path& packPath, AssetIndex::AssetType type);
    void RequestTexturePreview(AppState& app, const std::filesystem::path& packPath, const AssetIndex::SubAssetRecord& entry);
    void ClearTexturePreview(AppState& app);
    TexturePreviewSnapshot SnapshotTexturePreview(AppState& app);
    std::shared_ptr<TexturePreviewImage> TakePendingTexturePreview(AppState& app);
    void SetTexturePreviewUploadError(AppState& app, uint64_t generation, std::string error);

    void RequestModelPreview(AppState& app, const std::filesystem::path& packPath, const AssetIndex::SubAssetRecord& entry);
    void ClearModelPreview(AppState& app);
    std::shared_ptr<ModelPreviewData> TakePendingModelPreview(AppState& app);
    void SetModelPreviewUploadError(AppState& app, uint64_t generation, std::string error);

    void RequestTextureExport(AppState& app, const std::filesystem::path& packPath, const AssetIndex::SubAssetRecord& entry);
    void RequestTextureMapExport(AppState& app, const std::filesystem::path& packPath, const AssetIndex::SubAssetRecord& entry, Export::TextureMapKind mapKind);
    void RequestTextureMapsExport(AppState& app, const std::filesystem::path& packPath, const AssetIndex::SubAssetRecord& entry, std::vector<Export::TextureMapKind> mapKinds);
    void RequestModelExport(AppState& app, const std::filesystem::path& packPath, const AssetIndex::SubAssetRecord& entry);
    void RequestModelExport(AppState& app, const std::filesystem::path& packPath, const AssetIndex::SubAssetRecord& entry, bool includeTexturesForDynModel);
    void RequestModelExport(AppState& app, const std::filesystem::path& packPath, const AssetIndex::SubAssetRecord& entry, bool includeTexturesForDynModel, bool includeSkeletonRigForDynModel);
    void RequestModelExport(AppState& app, const std::filesystem::path& packPath, const AssetIndex::SubAssetRecord& entry, bool includeTexturesForDynModel, bool includeSkeletonRigForDynModel, bool includeAnimationsForDynModel);
    void RequestModelExportByName(AppState& app, const std::filesystem::path& packPath, const std::string& entryName);
    void OpenDynModelExportModal(AppState& app, const std::filesystem::path& packPath, const AssetIndex::SubAssetRecord& entry);
    bool OpenDynModelExportModalByName(AppState& app, const std::filesystem::path& packPath, const std::string& entryName);
    void RequestBatchTextureExport(AppState& app, const std::filesystem::path& packPath, std::shared_ptr<const AssetIndex::PackContents> contents);
    bool IsOodleAvailable(const AppState& app);
    std::shared_ptr<const BlkPreview> SnapshotBlkPreview(AppState& app, const std::filesystem::path& path);
    bool IsBlkPreviewLoading(AppState& app, const std::filesystem::path& path);
    void RequestBlkPreview(AppState& app, const std::filesystem::path& path);
    void RequestBlkResourceLinks(AppState& app, std::shared_ptr<const std::vector<AssetIndex::ResourceRecord>> records, bool invalidateDescriptors = true);
    std::shared_ptr<const std::vector<AssetIndex::BlkResourceLink>> SnapshotBlkResourceLinks(AppState& app);
    std::string GetBlkResourceLinkStatus(AppState& app);
    bool IsBlkResourceLinksLoading(AppState& app);
    void RequestDescriptorResourceLinks(AppState& app, std::shared_ptr<const std::vector<AssetIndex::ResourceRecord>> records);
    void RequestDescriptorResourceLinksForSelected(AppState& app, std::shared_ptr<const std::vector<AssetIndex::ResourceRecord>> records, AssetIndex::ResourceRecord selected);
    std::shared_ptr<const std::vector<AssetIndex::DescriptorResourceLink>> SnapshotDescriptorResourceLinks(AppState& app);
    std::string GetDescriptorResourceLinkStatus(AppState& app);
    bool HasGlobalDescriptorResourceLinks(AppState& app);
    bool IsDescriptorResourceLinksLoading(AppState& app);
    void RequestDescriptorResourceLinkCancel(AppState& app);
    ProgressJobSnapshot SnapshotProgressJob(AppState& app);
    void RequestProgressJobCancel(AppState& app);
    void BeginExportProgress(AppState& app, std::string title, std::string status, uint64_t total);
    void UpdateExportProgress(AppState& app, uint64_t current, uint64_t total, std::string status, std::string item);
    void AddExportProgressLog(AppState& app, std::string message);
    void EndExportProgress(AppState& app);
    void SetExportProgressOutputDir(AppState& app, std::string dir);
    void JoinFinishedBackgroundJobs(AppState& app);
    void WaitForBackgroundJobs(AppState& app);
    void InvalidateAndClearCache(AppState& app);
}
