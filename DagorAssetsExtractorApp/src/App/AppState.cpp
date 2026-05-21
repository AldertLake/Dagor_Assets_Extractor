#include "App/AppState.h"
#include "Platform/Platform.h"
#include "DagorIO/Decompression.h"
#include "Export/TextureExporter.h"
#include "Export/ModelExporter.h"
#include "DagorFormats/BlkReader.h"
#include "DagorFormats/RendInstReader.h"
#include "DagorFormats/DynModelReader.h"
#include "DagorFormats/GeomNodeTreeReader.h"
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace DagorAssetsExtractor::App
{
    using AssetIndex::ResourceRecord;
    using AssetIndex::DiscoverOptions;
    using AssetIndex::DiscoverResult;

    namespace fs = std::filesystem;

    void CopyToBuffer(std::array<char, PathBufferSize>& buffer, const std::string& value)
    {
        buffer.fill('\0');
        strncpy_s(buffer.data(), buffer.size(), value.c_str(), _TRUNCATE);
    }

    std::string Trim(std::string value)
    {
        const auto isSpace = [](unsigned char c) {
            return std::isspace(c) != 0;
        };

        value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char c) {
            return !isSpace(static_cast<unsigned char>(c));
        }));
        value.erase(std::find_if(value.rbegin(), value.rend(), [&](char c) {
            return !isSpace(static_cast<unsigned char>(c));
        }).base(), value.end());
        return value;
    }

    std::string ToLowerLocal(std::string_view value)
    {
        std::string lowered(value);
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return lowered;
    }

    std::vector<std::string> BuildBaseColorSlotPreference(std::string_view shaderClass)
    {
        const std::string lowered = ToLowerLocal(shaderClass);
        const bool layered = lowered.find("layered") != std::string::npos;
        const bool detail1IsDiffuse = layered && lowered != "rendinst_simple_layered";
        if (!detail1IsDiffuse || lowered == "dynamic_painted_by_mask")
        {
            return { "t0" };
        }

        return { "t3", "t5", "t7", "t9" };
    }

    template <typename TMesh>
    std::string BuildUvExportSummary(const std::vector<TMesh>& meshes)
    {
        size_t vertices = 0;
        size_t objShortUv = 0;
        size_t objFloatUv = 0;
        size_t zeroUv = 0;
        for (const TMesh& mesh : meshes)
        {
            for (const auto& vertex : mesh.vertices)
            {
                ++vertices;
                if (vertex.uvTransform == DagorFormats::UvTransform::ObjShortVFlipped)
                {
                    ++objShortUv;
                }
                else if (vertex.uvTransform == DagorFormats::UvTransform::ObjFloatVNegated)
                {
                    ++objFloatUv;
                }
                if (std::abs(vertex.u) <= 1e-8f && std::abs(vertex.v) <= 1e-8f)
                {
                    ++zeroUv;
                }
            }
        }

        return "vertices=" + std::to_string(vertices) +
            ", objShortUvToGltf=" + std::to_string(objShortUv) +
            ", objFloatUvToGltf=" + std::to_string(objFloatUv) +
            ", zeroUv=" + std::to_string(zeroUv);
    }

    std::string NowString()
    {
        const auto now = std::chrono::system_clock::now();
        const std::time_t time = std::chrono::system_clock::to_time_t(now);
        std::tm localTime = {};
        localtime_s(&localTime, &time);

        char buffer[32] = {};
        std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &localTime);
        return buffer;
    }

    bool HasRendInstMagic(const std::vector<uint8_t>& payload)
    {
        return payload.size() >= 4 && std::memcmp(payload.data(), "RIND", 4) == 0;
    }

    bool HasDynModelMagic(const std::vector<uint8_t>& payload)
    {
        return payload.size() >= 4 && (std::memcmp(payload.data(), "DYN3", 4) == 0 || std::memcmp(payload.data(), "DYN2", 4) == 0);
    }

    bool HasGeomNodeTreeMagic(const std::vector<uint8_t>& payload)
    {
        // GeomNodeTree often has no magic, or starts with zstd/oodle frame.
        // We'll trust the classId or caller.
        return !payload.empty();
    }

    std::string FormatPayloadMagic(const std::vector<uint8_t>& payload)
    {
        if (payload.empty())
        {
            return "<empty>";
        }

        const size_t length = std::min<size_t>(4, payload.size());
        bool printable = true;
        for (size_t i = 0; i < length; ++i)
        {
            if (std::isprint(static_cast<unsigned char>(payload[i])) == 0)
            {
                printable = false;
                break;
            }
        }

        if (printable)
        {
            return std::string(reinterpret_cast<const char*>(payload.data()), length);
        }

        std::ostringstream stream;
        stream << std::hex << std::uppercase << std::setfill('0');
        for (size_t i = 0; i < length; ++i)
        {
            if (i > 0)
            {
                stream << ' ';
            }
            stream << std::setw(2) << static_cast<unsigned int>(payload[i]);
        }
        return stream.str();
    }

    std::string FormatPayloadHeader(const std::vector<uint8_t>& payload, size_t count = 64)
    {
        if (payload.empty())
        {
            return "  <empty>";
        }

        std::ostringstream stream;
        stream << std::hex << std::uppercase << std::setfill('0');
        size_t actualCount = std::min(count, payload.size());
        
        for (size_t i = 0; i < actualCount; i += 16)
        {
            if (i > 0)
            {
                stream << "\n";
            }
            stream << "  " << std::setw(4) << static_cast<unsigned int>(i) << ": ";
            
            // Hex values
            for (size_t j = 0; j < 16; ++j)
            {
                if (i + j < actualCount)
                {
                    stream << std::setw(2) << static_cast<unsigned int>(payload[i + j]) << " ";
                }
                else
                {
                    stream << "   ";
                }
            }
            
            stream << " | ";
            
            // ASCII values
            for (size_t j = 0; j < 16; ++j)
            {
                if (i + j < actualCount)
                {
                    unsigned char c = payload[i + j];
                    if (std::isprint(c) != 0)
                    {
                        stream << static_cast<char>(c);
                    }
                    else
                    {
                        stream << ".";
                    }
                }
            }
        }
        
        return stream.str();
    }

    void AddLog(AppState& app, LogLevel level, std::string message)
    {
        std::lock_guard<std::mutex> lock(app.logMutex);
        app.logs.push_back(LogEntry{ NowString(), level, message });
        
        // Format for OutputDebugStringA
        std::string debugMsg = "[DagorExtractor] ";
        switch (level)
        {
        case LogLevel::Info:    debugMsg += "[INFO] "; break;
        case LogLevel::Warning: debugMsg += "[WARN] "; break;
        case LogLevel::Error:   debugMsg += "[ERR]  "; break;
        }
        debugMsg += message + "\n";
        ::OutputDebugStringA(debugMsg.c_str());

        if (app.logs.size() > 1000)
        {
            app.logs.erase(app.logs.begin(), app.logs.begin() + 250);
        }
    }

    void AddProgressJobLog(AppState& app, LogLevel level, std::string message)
    {
        std::lock_guard<std::mutex> lock(app.progressJob.mutex);
        app.progressJob.logs.push_back(LogEntry{ NowString(), level, std::move(message) });
        if (app.progressJob.logs.size() > 200)
        {
            app.progressJob.logs.erase(app.progressJob.logs.begin(), app.progressJob.logs.begin() + 50);
        }
    }

    void BeginProgressJob(AppState& app, std::string title, std::string status, uint64_t total)
    {
        {
            std::lock_guard<std::mutex> lock(app.progressJob.mutex);
            app.progressJob.title = std::move(title);
            app.progressJob.status = std::move(status);
            app.progressJob.currentItem.clear();
            app.progressJob.current = 0;
            app.progressJob.total = total;
            app.progressJob.logs.clear();
            app.progressJob.windowOpen = true;
        }

        app.progressJob.cancelRequested.store(false);
        app.progressJob.running.store(true);
    }

    void UpdateProgressJob(AppState& app, uint64_t current, uint64_t total, std::string status, std::string item)
    {
        std::lock_guard<std::mutex> lock(app.progressJob.mutex);
        app.progressJob.current = current;
        app.progressJob.total = total;
        app.progressJob.status = std::move(status);
        app.progressJob.currentItem = std::move(item);
    }

    void EndProgressJob(AppState& app, std::string finalStatus)
    {
        {
            std::lock_guard<std::mutex> lock(app.progressJob.mutex);
            const bool completedNormally =
                finalStatus.rfind("Canceled", 0) != 0 &&
                finalStatus.rfind("Failed", 0) != 0;
            app.progressJob.status = std::move(finalStatus);
            app.progressJob.currentItem.clear();
            if (completedNormally && app.progressJob.total > 0)
            {
                app.progressJob.current = app.progressJob.total;
            }
        }

        app.progressJob.running.store(false);
        app.progressJob.cancelRequested.store(false);
    }

    std::shared_ptr<const std::vector<ResourceRecord>> SnapshotRecords(ScanState& scan)
    {
        std::lock_guard<std::mutex> lock(scan.mutex);
        if (!scan.records)
        {
            return std::make_shared<std::vector<ResourceRecord>>();
        }
        return scan.records;
    }

    std::string GetScanStatus(ScanState& scan)
    {
        std::lock_guard<std::mutex> lock(scan.mutex);
        return scan.status;
    }

    std::string GetCacheStatus(ScanState& scan)
    {
        std::lock_guard<std::mutex> lock(scan.mutex);
        return scan.cacheStatus;
    }

    void SetCacheStatus(ScanState& scan, std::string status, bool loaded)
    {
        std::lock_guard<std::mutex> lock(scan.mutex);
        scan.cacheStatus = std::move(status);
        scan.cacheLoaded = loaded;
    }

    void SetScanStatus(ScanState& scan, std::string status)
    {
        std::lock_guard<std::mutex> lock(scan.mutex);
        scan.status = std::move(status);
    }

    AssetIndex::DescriptorResourceLinkBuildOptions DefaultDescriptorLinkBuildOptions()
    {
        AssetIndex::DescriptorResourceLinkBuildOptions options;
        return options;
    }

    void ClearDescriptorLinkUiCache(AppState& app)
    {
        app.descriptorUiLinks.reset();
        app.descriptorUiSelectedPath.clear();
        app.descriptorUiSelectedType = AssetIndex::AssetType::Unknown;
        app.descriptorUiDescriptorMatches.clear();
        app.descriptorUiGrpMatches.clear();
        app.descriptorUiDxpMatches.clear();
    }

    void TryLoadDescriptorResourceLinkCache(
        AppState& app,
        std::shared_ptr<const std::vector<ResourceRecord>> records)
    {
        if (app.descriptorLinksCachePath.empty() || !records || records->empty())
        {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(app.descriptorLinks.mutex);
            if (app.descriptorLinks.cacheLoading || app.descriptorLinks.loading)
            {
                AddLog(app, LogLevel::Info, "Descriptor-link cache load skipped because descriptor links are already busy.");
                return;
            }

            app.descriptorLinks.cacheLoading = true;
            app.descriptorLinks.cacheLoadingRecords = records;
            app.descriptorLinks.cacheStatus = "Loading descriptor-link cache...";
            if (!app.descriptorLinks.links)
            {
                app.descriptorLinks.status = "Loading cached descriptor links...";
            }
        }

        const fs::path cachePath = app.descriptorLinksCachePath;
        const fs::path gameRoot = app.settings.gameRoot;
        const AssetIndex::DescriptorResourceLinkBuildOptions options = DefaultDescriptorLinkBuildOptions();
        std::error_code ec;
        const uintmax_t cacheBytes = fs::file_size(cachePath, ec);
        AddLog(app,
            LogLevel::Info,
            "Descriptor-link cache load scheduled: " + Platform::PathToUtf8(cachePath) +
                (ec ? std::string() : (", size=" + std::to_string(cacheBytes) + " bytes")));

        app.backgroundJobs.Start("LoadDescriptorResourceLinkCache", [&app, records, cachePath, gameRoot, options]() {
            const auto started = std::chrono::steady_clock::now();
            AssetIndex::DescriptorResourceLinkCacheLoadResult result =
                AssetIndex::LoadDescriptorResourceLinkCache(cachePath, gameRoot, *records, options);

            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            result.message += " (" + std::to_string(elapsedMs) + " ms)";

            std::shared_ptr<const std::vector<AssetIndex::DescriptorResourceLink>> cachedLinks;
            if (result.loaded)
            {
                cachedLinks = std::make_shared<const std::vector<AssetIndex::DescriptorResourceLink>>(std::move(result.links));
            }

            {
                std::lock_guard<std::mutex> lock(app.descriptorLinks.mutex);
                if (app.descriptorLinks.cacheLoadingRecords != records)
                {
                    return;
                }

                app.descriptorLinks.cacheLoading = false;
                app.descriptorLinks.cacheLoadingRecords.reset();
                app.descriptorLinks.cacheStatus = result.message;
                if (cachedLinks)
                {
                    app.descriptorLinks.links = cachedLinks;
                    app.descriptorLinks.globalLinks = true;
                    app.descriptorLinks.scopePath.reset();
                    app.descriptorLinks.status = result.message;
                }
            }

            if (!result.fileFound)
            {
                AddLog(app, LogLevel::Info, result.message);
                return;
            }

            if (!result.loaded)
            {
                AddLog(app, LogLevel::Info, "Descriptor-link cache skipped: " + result.message);
                return;
            }

            ClearDescriptorLinkUiCache(app);
            AddLog(app, LogLevel::Info, result.message);
        });
    }

    void EnforcePackContentsLimit(AppState& app, const fs::path& parsedPath)
    {
        const size_t limit = std::max<size_t>(1, app.settings.maxLoadedPackContents);
        if (app.packContentsCache.Size() <= limit)
        {
            return;
        }

        fs::path keepPath = parsedPath;
        if (!app.selectedPath.empty())
        {
            keepPath = fs::path(app.selectedPath);
        }

        const size_t removed = app.packContentsCache.ClearExcept(keepPath);
        if (removed > 0)
        {
            std::ostringstream message;
            message << "Loaded pack cache limit exceeded (" << limit
                    << "); unloaded " << removed
                    << " parsed container" << (removed == 1 ? "" : "s")
                    << " and kept " << Platform::PathToUtf8(keepPath.filename()) << '.';
            AddLog(app, LogLevel::Info, message.str());
        }
    }

    bool ParseBool(std::string_view value, bool fallback)
    {
        const std::string normalized = AssetIndex::ToLowerAscii(Trim(std::string(value)));
        if (normalized == "1" || normalized == "true" || normalized == "yes")
        {
            return true;
        }
        if (normalized == "0" || normalized == "false" || normalized == "no")
        {
            return false;
        }
        return fallback;
    }

    size_t ParseSize(std::string_view value, size_t fallback)
    {
        try
        {
            size_t parsedLength = 0;
            const unsigned long parsed = std::stoul(std::string(value), &parsedLength, 10);
            if (parsedLength == 0)
            {
                return fallback;
            }
            return static_cast<size_t>(std::clamp<unsigned long>(parsed, 1ul, 256ul));
        }
        catch (...)
        {
            return fallback;
        }
    }

    void LoadSettings(AppSettings& settings, const fs::path& path)
    {
        settings.exportRoot = Platform::GetDefaultExportRoot();

        std::ifstream file(path);
        if (!file)
        {
            return;
        }

        std::string line;
        while (std::getline(file, line))
        {
            line = Trim(line);
            if (line.empty() || line[0] == '#')
            {
                continue;
            }

            const size_t separator = line.find('=');
            if (separator == std::string::npos)
            {
                continue;
            }

            const std::string key = Trim(line.substr(0, separator));
            const std::string value = Trim(line.substr(separator + 1));

            if (key == "gameRoot")
            {
                settings.gameRoot = Platform::Utf8ToPath(value);
            }
            else if (key == "exportRoot")
            {
                settings.exportRoot = Platform::Utf8ToPath(value);
            }
            else if (key == "textureExportFormat")
            {
                settings.textureExportFormat = value;
            }
            else if (key == "modelExportFormat")
            {
                settings.modelExportFormat = value;
            }
            else if (key == "exportTexturesWithModels")
            {
                settings.exportTexturesWithModels = ParseBool(value, settings.exportTexturesWithModels);
            }
            else if (key == "exportJsonManifest")
            {
                settings.exportJsonManifest = ParseBool(value, settings.exportJsonManifest);
            }
            else if (key == "preserveOriginalDDS")
            {
                settings.preserveOriginalDDS = ParseBool(value, settings.preserveOriginalDDS);
            }
            else if (key == "indexCacheEnabled")
            {
                settings.indexCacheEnabled = ParseBool(value, settings.indexCacheEnabled);
            }
            else if (key == "maxLoadedPackContents")
            {
                settings.maxLoadedPackContents = ParseSize(value, settings.maxLoadedPackContents);
            }
            else if (key == "showVromfs")
            {
                settings.showVromfs = ParseBool(value, settings.showVromfs);
            }
            else if (key == "showGrp")
            {
                settings.showGrp = ParseBool(value, settings.showGrp);
            }
            else if (key == "showDxp")
            {
                settings.showDxp = ParseBool(value, settings.showDxp);
            }
            else if (key == "showDescriptors")
            {
                settings.showDescriptors = ParseBool(value, settings.showDescriptors);
            }
            else if (key == "showBlk")
            {
                settings.showBlk = ParseBool(value, settings.showBlk);
            }
            else if (key == "groupByBlk")
            {
                settings.groupByBlk = ParseBool(value, settings.groupByBlk);
            }
            else if (key == "lastOpenedPackFilters")
            {
                settings.lastOpenedPackFilters = value;
            }
        }
    }

    bool SaveSettings(const AppSettings& settings, const fs::path& path, std::string& error)
    {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        if (ec)
        {
            error = "Could not create settings folder: " + ec.message();
            return false;
        }

        std::ofstream file(path, std::ios::trunc);
        if (!file)
        {
            error = "Could not open settings file for writing.";
            return false;
        }

        file << "gameRoot=" << Platform::PathToUtf8(settings.gameRoot) << '\n';
        file << "exportRoot=" << Platform::PathToUtf8(settings.exportRoot) << '\n';
        file << "textureExportFormat=" << settings.textureExportFormat << '\n';
        file << "modelExportFormat=" << settings.modelExportFormat << '\n';
        file << "exportTexturesWithModels=" << (settings.exportTexturesWithModels ? "true" : "false") << '\n';
        file << "exportJsonManifest=" << (settings.exportJsonManifest ? "true" : "false") << '\n';
        file << "preserveOriginalDDS=" << (settings.preserveOriginalDDS ? "true" : "false") << '\n';
        file << "indexCacheEnabled=" << (settings.indexCacheEnabled ? "true" : "false") << '\n';
        file << "maxLoadedPackContents=" << settings.maxLoadedPackContents << '\n';
        file << "showVromfs=" << (settings.showVromfs ? "true" : "false") << '\n';
        file << "showGrp=" << (settings.showGrp ? "true" : "false") << '\n';
        file << "showDxp=" << (settings.showDxp ? "true" : "false") << '\n';
        file << "showDescriptors=" << (settings.showDescriptors ? "true" : "false") << '\n';
        file << "showBlk=" << (settings.showBlk ? "true" : "false") << '\n';
        file << "groupByBlk=" << (settings.groupByBlk ? "true" : "false") << '\n';
        file << "lastOpenedPackFilters=" << settings.lastOpenedPackFilters << '\n';
        return true;
    }

    void ApplySettingsToUiState(AppState& app)
    {
        app.showVromfs = app.settings.showVromfs;
        app.showGrp = app.settings.showGrp;
        app.showDxp = app.settings.showDxp;
        app.showDescriptors = app.settings.showDescriptors;
        app.showBlk = app.settings.showBlk;
        app.groupByBlk = app.settings.groupByBlk;
    }

    void UpdateSettingsFromUiState(AppState& app)
    {
        app.settings.showVromfs = app.showVromfs;
        app.settings.showGrp = app.showGrp;
        app.settings.showDxp = app.showDxp;
        app.settings.showDescriptors = app.showDescriptors;
        app.settings.showBlk = app.showBlk;
        app.settings.groupByBlk = app.groupByBlk;
    }

    void SyncSettingsBuffers(AppState& app)
    {
        CopyToBuffer(app.gameRootBuffer, Platform::PathToUtf8(app.settings.gameRoot));
        CopyToBuffer(app.exportRootBuffer, Platform::PathToUtf8(app.settings.exportRoot));
    }

    void StopScan(AppState& app)
    {
        if (app.scan.running.load())
        {
            app.scan.cancelRequested.store(true);
        }

        if (app.scan.worker.joinable())
        {
            app.scan.worker.join();
        }
    }

    void JoinFinishedScan(AppState& app)
    {
        if (!app.scan.running.load() && app.scan.worker.joinable())
        {
            app.scan.worker.join();
        }
    }

    void ClearResourceIndex(AppState& app, std::string status)
    {
        {
            std::lock_guard<std::mutex> lock(app.scan.mutex);
            app.scan.records = nullptr;
            app.scan.status = std::move(status);
            app.scan.cacheStatus = app.settings.indexCacheEnabled ? "Not loaded" : "Disabled";
            app.scan.cacheLoaded = false;
        }

        app.selectedPath.clear();
        {
            std::lock_guard<std::mutex> lock(app.descriptorLinks.mutex);
            app.descriptorLinks.cacheLoading = false;
            app.descriptorLinks.cacheLoadingRecords.reset();
        }
        app.scan.filesVisited.store(0);
        app.scan.recordsFound.store(0);
        app.scan.completed.store(false);
        app.scan.failed.store(false);
        app.packContentsCache.Clear();

        {
            std::lock_guard<std::mutex> lock(app.blkLinks.mutex);
            app.blkLinks.loading = false;
            app.blkLinks.status = "Not built";
            app.blkLinks.loadingRecords.reset();
            app.blkLinks.links.reset();
        }

        {
            std::lock_guard<std::mutex> lock(app.descriptorLinks.mutex);
            app.descriptorLinks.loading = false;
            app.descriptorLinks.cacheLoading = false;
            app.descriptorLinks.status = "Not built";
            app.descriptorLinks.cancelRequested.store(false);
            app.descriptorLinks.loadingRecords.reset();
            app.descriptorLinks.cacheLoadingRecords.reset();
            app.descriptorLinks.loadingBlkLinks.reset();
            app.descriptorLinks.links.reset();
            app.descriptorLinks.globalLinks = false;
            app.descriptorLinks.scopePath.reset();
            app.descriptorLinks.cacheStatus = "Not loaded";
        }
        ClearDescriptorLinkUiCache(app);

        if (app.progressJob.running.load())
        {
            app.progressJob.cancelRequested.store(true);
        }
    }

    void LoadIndexCacheIntoApp(AppState& app)
    {
        if (!app.settings.indexCacheEnabled)
        {
            SetCacheStatus(app.scan, "Disabled", false);
            return;
        }

        if (!app.validation.IsValid())
        {
            SetCacheStatus(app.scan, "No valid game folder", false);
            return;
        }

        const AssetIndex::IndexCacheLoadResult result = AssetIndex::LoadIndexCacheRecords(app.indexCachePath, app.settings.gameRoot);
        if (!result.fileFound)
        {
            SetCacheStatus(app.scan, result.message, false);
            return;
        }

        if (!result.rootMatched)
        {
            SetCacheStatus(app.scan, result.message, false);
            AddLog(app, LogLevel::Warning, result.message);
            return;
        }

        auto loadedRecords = std::make_shared<std::vector<ResourceRecord>>(result.records);
        {
            std::lock_guard<std::mutex> lock(app.scan.mutex);
            app.scan.records = loadedRecords;
            app.scan.status = result.records.empty() ? "Idle" : "Loaded from cache";
            app.scan.cacheStatus = result.message;
            app.scan.cacheLoaded = !result.records.empty();
        }

        app.selectedPath.clear();
        app.scan.filesVisited.store(0);
        app.scan.recordsFound.store(static_cast<uint64_t>(result.recordsLoaded));
        app.scan.completed.store(!result.records.empty());
        app.scan.failed.store(false);

        AddLog(app, LogLevel::Info, result.message);
        TryLoadDescriptorResourceLinkCache(app, loadedRecords);
    }

    void SetGameRoot(AppState& app, const fs::path& gameRoot)
    {
        StopScan(app);
        app.settings.gameRoot = gameRoot;
        app.validation = AssetIndex::ValidateGameRoot(app.settings.gameRoot);
        SyncSettingsBuffers(app);
        ClearResourceIndex(app, "Idle");
        LoadIndexCacheIntoApp(app);
    }

    void ApplyGameRootBuffer(AppState& app)
    {
        SetGameRoot(app, Platform::Utf8ToPath(app.gameRootBuffer.data()));
    }

    void SaveSettingsWithLog(AppState& app)
    {
        std::string error;
        UpdateSettingsFromUiState(app);
        if (SaveSettings(app.settings, app.settingsPath, error))
        {
            AddLog(app, LogLevel::Info, "Settings saved to " + Platform::PathToUtf8(app.settingsPath));
        }
        else
        {
            AddLog(app, LogLevel::Error, error);
        }
    }

    void StartScan(AppState& app)
    {
        JoinFinishedScan(app);
        if (app.scan.running.load())
        {
            AddLog(app, LogLevel::Warning, "A scan is already running.");
            return;
        }

        if (!app.validation.IsValid())
        {
            AddLog(app, LogLevel::Warning, "Choose a valid War Thunder folder before scanning.");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(app.scan.mutex);
            app.scan.records = nullptr;
            app.scan.status = "Preparing scan";
            app.scan.cacheStatus = app.settings.indexCacheEnabled ? "Refreshing" : "Disabled";
            app.scan.cacheLoaded = false;
        }

        app.selectedPath.clear();
        app.scan.filesVisited.store(0);
        app.scan.recordsFound.store(0);
        app.scan.cancelRequested.store(false);
        app.scan.completed.store(false);
        app.scan.failed.store(false);
        app.scan.running.store(true);

        const fs::path root = app.settings.gameRoot;
        const fs::path cachePath = app.indexCachePath;
        const bool writeIndexCache = app.settings.indexCacheEnabled;
        AddLog(app, LogLevel::Info, "Started resource discovery for " + Platform::PathToUtf8(root));

        app.scan.worker = std::thread([&app, root, cachePath, writeIndexCache]() {
            try
            {
                DiscoverOptions options;
                options.cancelRequested = &app.scan.cancelRequested;
                options.onFileVisited = [&app](uint64_t filesVisited) {
                    app.scan.filesVisited.store(filesVisited);
                };
                std::vector<ResourceRecord> localRecords;

                options.onRecordFound = [&app, &localRecords](const ResourceRecord& record) {
                    localRecords.push_back(record);
                    const size_t count = localRecords.size();

                    if (count % 500 == 0)
                    {
                        std::lock_guard<std::mutex> lock(app.scan.mutex);
                        app.scan.records = std::make_shared<std::vector<ResourceRecord>>(localRecords);
                        std::ostringstream status;
                        status << "Scanning - " << count << " records found";
                        app.scan.status = status.str();
                    }
                    app.scan.recordsFound.store(static_cast<uint64_t>(count));
                };

                const DiscoverResult result = AssetIndex::DiscoverResources(root, options);
                if (result.canceled)
                {
                    SetScanStatus(app.scan, "Canceled");
                    AddLog(app, LogLevel::Warning, "Resource discovery was canceled.");
                }
                else if (!result.error.empty())
                {
                    app.scan.failed.store(true);
                    SetScanStatus(app.scan, "Failed");
                    AddLog(app, LogLevel::Error, result.error);
                }
                else
                {
                    auto discoveredRecords = std::make_shared<std::vector<ResourceRecord>>(result.records);
                    {
                        std::lock_guard<std::mutex> lock(app.scan.mutex);
                        app.scan.records = discoveredRecords;
                    }
                    {
                        std::lock_guard<std::mutex> lock(app.descriptorLinks.mutex);
                        app.descriptorLinks.links.reset();
                        app.descriptorLinks.globalLinks = false;
                        app.descriptorLinks.scopePath.reset();
                        app.descriptorLinks.status = "Not built";
                        app.descriptorLinks.cacheStatus = "Resource index changed";
                        app.descriptorLinks.cacheLoading = false;
                        app.descriptorLinks.cacheLoadingRecords.reset();
                    }

                    std::ostringstream message;
                    message << "Resource discovery completed: "
                            << app.scan.recordsFound.load()
                            << " relevant files found after visiting "
                            << app.scan.filesVisited.load()
                            << " paths.";
                    SetScanStatus(app.scan, "Completed");
                    AddLog(app, LogLevel::Info, message.str());
                    app.scan.completed.store(true);

                    if (writeIndexCache && result.records.size() > 0)
                    {
                        std::string cacheError;
                        // Use result.records instead of copying from scan state
                        if (AssetIndex::SaveIndexCacheRecords(cachePath, root, result.records, cacheError))
                        {
                            std::ostringstream cacheMessage;
                            cacheMessage << "Index cache saved with " << result.records.size() << " records.";
                            SetCacheStatus(app.scan, cacheMessage.str(), true);
                            AddLog(app, LogLevel::Info, cacheMessage.str());
                        }
                        else
                        {
                            SetCacheStatus(app.scan, "Cache save failed", false);
                            AddLog(app, LogLevel::Warning, cacheError);
                        }
                    }

                    TryLoadDescriptorResourceLinkCache(app, discoveredRecords);
                }
            }
            catch (const std::exception& exception)
            {
                app.scan.failed.store(true);
                SetScanStatus(app.scan, "Failed");
                AddLog(app, LogLevel::Error, std::string("Resource discovery failed: ") + exception.what());
            }

            app.scan.running.store(false);
        });
    }

    void RequestPackContents(AppState& app, const fs::path& packPath, AssetIndex::AssetType type)
    {
        // Already cached or currently parsing
        if (app.packContentsCache.Has(packPath) || app.packContentsParsing.load())
        {
            return;
        }

        app.packContentsParsing.store(true);
        AddLog(app, LogLevel::Info, "Parsing contents of " + Platform::PathToUtf8(packPath.filename()) + "...");

        std::thread([&app, packPath, type]() {
            std::shared_ptr<AssetIndex::PackContents> contents;

            if (type == AssetIndex::AssetType::Vromfs)
            {
                contents = AssetIndex::ParseVromfsContents(packPath);
            }
            else if (type == AssetIndex::AssetType::Dxp)
            {
                contents = AssetIndex::ParseDxpContents(packPath);
            }
            else if (type == AssetIndex::AssetType::Grp)
            {
                contents = AssetIndex::ParseGrpContents(packPath);
            }
            else if (type == AssetIndex::AssetType::Descriptor)
            {
                contents = AssetIndex::ParseDescriptorContents(packPath);
            }

            if (contents)
            {
                app.packContentsCache.Put(packPath, contents);
                EnforcePackContentsLimit(app, packPath);

                if (contents->error.empty())
                {
                    std::ostringstream msg;
                    msg << "Parsed " << contents->entries.size() << " sub-assets from "
                        << Platform::PathToUtf8(packPath.filename());
                    AddLog(app, LogLevel::Info, msg.str());
                }
                else
                {
                    AddLog(app, LogLevel::Warning, contents->error);
                }
            }

            app.packContentsParsing.store(false);
        }).detach();
    }

    Export::TextureExportOptions GetTextureExportOptions(const AppState& app)
    {
        Export::TextureExportOptions options;
        options.outputDir = app.settings.exportRoot;
        options.gameRoot = app.settings.gameRoot;
        options.oodleDllPath = DagorIO::Decompressor::FindBundledOodleDll();
        options.format = app.settings.textureExportFormat;
        options.preserveOriginalDDS = app.settings.preserveOriginalDDS;
        options.overwrite = true;
        options.useOodleFromGameFolder = false;
        options.exportJsonManifest = app.settings.exportJsonManifest;
        return options;
    }

    std::string MakeExportRelativeUri(const fs::path& exportRoot, const fs::path& outputPath)
    {
        std::error_code ec;
        fs::path relativePath = fs::relative(outputPath, exportRoot, ec);
        if (ec || relativePath.empty())
        {
            relativePath = outputPath.filename();
        }
        return relativePath.generic_string();
    }

    void RequestTextureExport(AppState& app, const std::filesystem::path& packPath, const AssetIndex::SubAssetRecord& entry)
    {
        AddLog(app, LogLevel::Info, "Exporting texture: " + entry.name + "...");
        Export::TextureExportOptions options = GetTextureExportOptions(app);

        app.backgroundJobs.Start("ExportTexture_" + entry.name, [&app, packPath, entry, options]() {
            auto result = Export::TextureExporter::ExportTexture(packPath, entry, options);
            if (result.success)
            {
                AddLog(app, LogLevel::Info, "Exported texture: " + entry.name + " to " + Platform::PathToUtf8(result.outputPath));
            }
            else
            {
                AddLog(app, LogLevel::Error, "Failed to export texture: " + entry.name + ". Error: " + result.error);
            }
        });
    }

    void RequestModelExportInternal(
        AppState& app,
        const std::filesystem::path& packPath,
        const AssetIndex::SubAssetRecord& entry,
        std::optional<bool> dynModelIncludeTexturesOverride)
    {
        if (entry.grpClassId != 0 && 
            !AssetIndex::CanExportGrpEntryAsStaticModel(entry) && 
            !AssetIndex::CanExportGrpEntryAsSkinnedModel(entry))
        {
            const std::string className = AssetIndex::DescribeGrpClassId(entry.grpClassId);
            AddLog(app, LogLevel::Warning,
                "Skipped export for " + entry.name + ": " +
                (className.empty() ? std::string("unsupported GRP entry type") : className) +
                ". Only RendInst and DynModel meshes can be exported right now.");
            return;
        }

        if (AssetIndex::CanExportGrpEntryAsSkinnedModel(entry) && !dynModelIncludeTexturesOverride.has_value())
        {
            AddLog(app,
                LogLevel::Warning,
                "Skipped DynModel export for " + entry.name +
                ": dynamic model export options were not confirmed.");
            return;
        }

        AddLog(app, LogLevel::Info, "Exporting model: " + entry.name + "...");

        std::string safeName = entry.name;
        std::replace(safeName.begin(), safeName.end(), '/', '_');
        std::replace(safeName.begin(), safeName.end(), '\\', '_');
        std::replace(safeName.begin(), safeName.end(), ':', '_');

        std::filesystem::path exportRoot = app.settings.exportRoot / safeName;
        std::string format = app.settings.modelExportFormat;
        bool globalExportTextures = app.settings.exportTexturesWithModels;
        bool exportJsonManifest = app.settings.exportJsonManifest;
        
        auto descriptorLinks = SnapshotDescriptorResourceLinks(app);
        auto allRecords = SnapshotRecords(app.scan);
        auto baseTextureExportOptions = GetTextureExportOptions(app);
        baseTextureExportOptions.outputDir = exportRoot / "Textures";
        const bool exportGlb = AssetIndex::ToLowerAscii(format) == "glb";

        app.backgroundJobs.Start("ExportModel_" + entry.name, [&app, packPath, entry, exportRoot, format, globalExportTextures, exportJsonManifest, descriptorLinks, allRecords, baseTextureExportOptions, exportGlb, dynModelIncludeTexturesOverride]() {
            try
            {
                std::ifstream file(packPath, std::ios::binary);
                if (!file)
                {
                    AddLog(app, LogLevel::Error, "Could not open pack file: " + Platform::PathToUtf8(packPath));
                    return;
                }

                file.seekg(static_cast<std::streamoff>(entry.offset), std::ios::beg);
                std::vector<uint8_t> payload(entry.size);
                if (!file.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size())))
                {
                    AddLog(app, LogLevel::Error, "Could not read model payload from GRP file.");
                    return;
                }

                std::string safeName = entry.name;
                std::replace(safeName.begin(), safeName.end(), '/', '_');
                std::replace(safeName.begin(), safeName.end(), '\\', '_');
                std::replace(safeName.begin(), safeName.end(), ':', '_');

                std::error_code ec;
                fs::create_directories(exportRoot, ec);

                DagorAssetsExtractor::Export::ExportManifest manifest;
                manifest.sourcePack = packPath.filename().string();
                manifest.sourceResource = entry.name;
                manifest.appVersion = "1.0.0";
                manifest.skipWrite = !exportJsonManifest;

                // Determine asset type using the GRP classId first (reliable),
                // then fall back to payload magic bytes for legacy GRP entries.
                constexpr uint32_t kDynModelClassId = 0xB4B7D9C4u;
                constexpr uint32_t kRendInstClassId = 0x77F8232Fu;

                const bool isDynModel = (entry.grpClassId == kDynModelClassId) ||
                                        (entry.grpClassId == 0 && HasDynModelMagic(payload));
                const bool isRendInst = !isDynModel &&
                                        (entry.grpClassId == kRendInstClassId ||
                                         entry.grpClassId == 0 && HasRendInstMagic(payload));

                if (isDynModel && !dynModelIncludeTexturesOverride.has_value())
                {
                    AddLog(app,
                        LogLevel::Warning,
                        "Skipped DynModel export for " + entry.name +
                        ": dynamic model export options were not confirmed.");
                    return;
                }

                const bool exportTextures = isDynModel
                    ? dynModelIncludeTexturesOverride.value()
                    : globalExportTextures;
                auto textureExportOptions = baseTextureExportOptions;
                if (exportGlb && exportTextures)
                {
                    textureExportOptions.format = "PNG";
                    textureExportOptions.preserveOriginalDDS = false;
                    AddLog(app,
                        LogLevel::Info,
                        "GLB material texture export for " + entry.name +
                        " will use PNG only; DDS/TGA are not core glTF image formats and will not be linked into GLB materials.");
                }

                std::vector<Export::MaterialInfo> materials;
                
                // Find matching descriptor link for dependencies
                const AssetIndex::DescriptorResourceLink* matchedLink = nullptr;
                if (descriptorLinks)
                {
                    for (const auto& link : *descriptorLinks)
                    {
                        bool nameMatch = (link.matchedGrpEntryName && *link.matchedGrpEntryName == entry.name) || (link.resourceName == entry.name);
                        bool packMatch = !link.matchedGrpPath || (*link.matchedGrpPath == packPath);
                        if (nameMatch && packMatch)
                        {
                            matchedLink = &link;
                            break;
                        }
                    }
                }

                if (matchedLink)
                {
                    materials.reserve(matchedLink->materialNames.size());
                    for (size_t materialIndex = 0; materialIndex < matchedLink->materialNames.size(); ++materialIndex)
                    {
                        Export::MaterialInfo mat;
                        mat.name = matchedLink->materialNames[materialIndex].empty()
                            ? ("material_" + std::to_string(materialIndex))
                            : matchedLink->materialNames[materialIndex];
                        materials.push_back(std::move(mat));
                    }
                }
                else if (exportTextures)
                {
                    manifest.decodeWarnings.push_back("No descriptor relationship matched this model; material textures cannot be resolved.");
                    AddLog(app, LogLevel::Warning, "No descriptor relationship matched " + entry.name + "; exporting geometry/material slots without texture dependencies.");
                }

                if (matchedLink && exportTextures)
                {
                    size_t exportedTextureCount = 0;
                    std::unordered_map<uint32_t, std::string> exportedTexturesByDescriptorIndex;
                    for (const auto& texLink : matchedLink->textureLinks)
                    {
                        if (texLink.matchedDxpPath && texLink.matchedDxpEntryName)
                        {
                            auto dxpContents = app.packContentsCache.Get(*texLink.matchedDxpPath);
                            if (!dxpContents)
                            {
                                auto newContents = AssetIndex::ParseDxpContents(*texLink.matchedDxpPath);
                                if (newContents)
                                {
                                    app.packContentsCache.Put(*texLink.matchedDxpPath, newContents);
                                    dxpContents = newContents;
                                }
                            }

                            if (dxpContents)
                            {
                                auto it = std::find_if(dxpContents->entries.begin(), dxpContents->entries.end(), [&](const auto& e) {
                                    return e.name == *texLink.matchedDxpEntryName;
                                });

                                if (it != dxpContents->entries.end())
                                {
                                    auto texResult = Export::TextureExporter::ExportTexture(*texLink.matchedDxpPath, *it, textureExportOptions);
                                    if (texResult.success)
                                    {
                                        const std::string textureFile = MakeExportRelativeUri(exportRoot, texResult.outputPath);
                                        exportedTexturesByDescriptorIndex.emplace(texLink.textureIndex, textureFile);
                                        manifest.textureDependencies.push_back(textureFile);
                                        ++exportedTextureCount;
                                    }
                                    else
                                    {
                                        const std::string warning =
                                            "Failed to export texture dependency: " + texLink.textureName +
                                            " (" + texResult.error + ")";
                                        manifest.decodeWarnings.push_back(warning);
                                        AddLog(app, LogLevel::Warning, warning);
                                    }
                                }
                            }
                        }
                    }

                    size_t assignedTextureCount = 0;
                    size_t skippedBindingCount = 0;
                    if (!matchedLink->materialTextureBindings.empty())
                    {
                        size_t maxBoundMaterialIndex = materials.empty() ? 0 : materials.size() - 1;
                        for (const AssetIndex::DescriptorMaterialTextureRef& binding : matchedLink->materialTextureBindings)
                        {
                            if (binding.materialIndex >= 100000)
                            {
                                ++skippedBindingCount;
                                continue;
                            }
                            maxBoundMaterialIndex = std::max(maxBoundMaterialIndex, static_cast<size_t>(binding.materialIndex));
                        }

                        if (materials.size() <= maxBoundMaterialIndex)
                        {
                            const size_t oldSize = materials.size();
                            materials.resize(maxBoundMaterialIndex + 1);
                            for (size_t fillIndex = oldSize; fillIndex < materials.size(); ++fillIndex)
                            {
                                materials[fillIndex].name = "material_" + std::to_string(fillIndex);
                            }
                        }

                        for (size_t materialIndex = 0; materialIndex <= maxBoundMaterialIndex; ++materialIndex)
                        {
                            const std::string shaderClass =
                                materialIndex < matchedLink->materialShaderClasses.size()
                                    ? matchedLink->materialShaderClasses[materialIndex]
                                    : std::string();
                            const std::vector<std::string> baseColorSlots = BuildBaseColorSlotPreference(shaderClass);
                            std::optional<std::string> selectedTexturePath;

                            for (const std::string& slotName : baseColorSlots)
                            {
                                for (const AssetIndex::DescriptorMaterialTextureRef& binding : matchedLink->materialTextureBindings)
                                {
                                    if (binding.materialIndex != materialIndex || binding.slotName != slotName)
                                    {
                                        continue;
                                    }
                                    const auto textureIt = exportedTexturesByDescriptorIndex.find(binding.textureIndex);
                                    if (textureIt != exportedTexturesByDescriptorIndex.end())
                                    {
                                        selectedTexturePath = textureIt->second;
                                        break;
                                    }
                                }
                                if (selectedTexturePath)
                                {
                                    break;
                                }
                            }

                            Export::MaterialInfo& material = materials[materialIndex];
                            if (material.name.empty())
                            {
                                material.name = "material_" + std::to_string(materialIndex);
                            }
                            if (selectedTexturePath && material.diffuseTexturePath.empty())
                            {
                                material.diffuseTexturePath = *selectedTexturePath;
                                ++assignedTextureCount;
                            }
                        }

                        for (const AssetIndex::DescriptorMaterialTextureRef& binding : matchedLink->materialTextureBindings)
                        {
                            if (!exportedTexturesByDescriptorIndex.contains(binding.textureIndex))
                            {
                                ++skippedBindingCount;
                            }
                        }
                    }
                    else if (materials.size() == 1 && exportedTexturesByDescriptorIndex.size() == 1)
                    {
                        materials.front().diffuseTexturePath = exportedTexturesByDescriptorIndex.begin()->second;
                        assignedTextureCount = 1;
                    }
                    else if (!exportedTexturesByDescriptorIndex.empty())
                    {
                        const std::string warning =
                            "Descriptor link for " + entry.name +
                            " has no per-material texture bindings; textures were exported but not assigned to avoid corrupt material slots.";
                        manifest.decodeWarnings.push_back(warning);
                        AddLog(app, LogLevel::Warning, warning);
                    }

                    AddLog(app,
                        LogLevel::Info,
                        "Model material dependency summary for " + entry.name +
                        ": descriptorMaterials=" + std::to_string(matchedLink->materialNames.size()) +
                        ", descriptorTextures=" + std::to_string(matchedLink->textureLinks.size()) +
                        ", materialTextureBindings=" + std::to_string(matchedLink->materialTextureBindings.size()) +
                        ", exportedTextures=" + std::to_string(exportedTextureCount) +
                        ", assignedTextures=" + std::to_string(assignedTextureCount) +
                        ", skippedBindings=" + std::to_string(skippedBindingCount) +
                        ", gltfMaterialSlots=" + std::to_string(materials.size()));
                }

                bool success = false;
                fs::path outPath;

                if (isRendInst || (!isDynModel && HasRendInstMagic(payload)))
                {
                    DagorFormats::RendInstReader reader(payload);
                    reader.Parse();
                    AddLog(app,
                        LogLevel::Info,
                        "RendInst UV export summary for " + entry.name + ": " +
                            BuildUvExportSummary(reader.GetLods()));

                    manifest.assetType = "RendInst Static Model";
                    manifest.formatDecoderVersion = "RendInstReader v1.0";

                    if (format == "GLB" || format == "glb")
                    {
                        outPath = exportRoot / (safeName + ".glb");
                        manifest.outputFiles.push_back(outPath.filename().string());
                        success = DagorAssetsExtractor::Export::ModelExporter::ExportToGlb(outPath, reader.GetLods(), manifest, materials);
                    }
                    else
                    {
                        outPath = exportRoot / (safeName + ".obj");
                        manifest.outputFiles.push_back(outPath.filename().string());
                        if (!reader.GetLods().empty())
                        {
                            success = DagorAssetsExtractor::Export::ModelExporter::ExportToObj(outPath, reader.GetLods()[0], manifest);
                        }
                    }
                }
                else if (isDynModel)
                {
                    DagorFormats::DynModelReader reader(payload);
                    try
                    {
                        reader.Parse();
                    }
                    catch (...)
                    {
                        for (const std::string& diagnostic : reader.GetDiagnostics())
                        {
                            AddLog(app, LogLevel::Warning, "DynModel diagnostic for " + entry.name + ": " + diagnostic);
                        }
                        throw;
                    }

                    for (const std::string& diagnostic : reader.GetDiagnostics())
                    {
                        AddLog(app, LogLevel::Info, "DynModel diagnostic for " + entry.name + ": " + diagnostic);
                    }

                    std::vector<DagorFormats::DynModelMesh> dynLods = reader.GetLods();
                    AddLog(app,
                        LogLevel::Info,
                        "DynModel UV export summary for " + entry.name + ": " +
                            BuildUvExportSummary(dynLods));
                    if (matchedLink && matchedLink->materialDomains.size() == matchedLink->materialNames.size())
                    {
                        const auto firstSkinnedIt = std::find(
                            matchedLink->materialDomains.begin(),
                            matchedLink->materialDomains.end(),
                            "skinned");
                        if (firstSkinnedIt != matchedLink->materialDomains.end())
                        {
                            const size_t firstSkinnedMaterial =
                                static_cast<size_t>(std::distance(matchedLink->materialDomains.begin(), firstSkinnedIt));
                            const size_t skinnedMaterialCount = matchedLink->materialNames.size() - firstSkinnedMaterial;
                            size_t remappedSkinnedPrimitives = 0;
                            size_t outOfRangeSkinnedPrimitives = 0;

                            if (firstSkinnedMaterial > 0 && skinnedMaterialCount > 0)
                            {
                                for (DagorFormats::DynModelMesh& mesh : dynLods)
                                {
                                    if (!mesh.useSkinning)
                                    {
                                        continue;
                                    }
                                    for (auto& primitive : mesh.primitives)
                                    {
                                        if (primitive.materialIndex < skinnedMaterialCount)
                                        {
                                            primitive.materialIndex += static_cast<uint32_t>(firstSkinnedMaterial);
                                            ++remappedSkinnedPrimitives;
                                        }
                                        else
                                        {
                                            ++outOfRangeSkinnedPrimitives;
                                        }
                                    }
                                }
                            }

                            AddLog(app,
                                LogLevel::Info,
                                "DynModel material domain summary for " + entry.name +
                                ": rigidMaterials=" + std::to_string(firstSkinnedMaterial) +
                                ", skinnedMaterials=" + std::to_string(skinnedMaterialCount) +
                                ", skinnedPrimitiveOffsetsApplied=" + std::to_string(remappedSkinnedPrimitives) +
                                ", skinnedPrimitiveOutOfRange=" + std::to_string(outOfRangeSkinnedPrimitives));
                        }
                    }

                    size_t dynVertexCount = 0;
                    size_t dynIndexCount = 0;
                    size_t dynPrimitiveCount = 0;
                    size_t dynMaxMaterialSlot = 0;
                    bool dynHasMaterialSlot = false;
                    std::unordered_set<uint32_t> dynMaterialSlots;
                    for (const auto& lod : dynLods)
                    {
                        dynVertexCount += lod.vertices.size();
                        dynIndexCount += lod.indices.size();
                        dynPrimitiveCount += lod.primitives.size();
                        for (const auto& primitive : lod.primitives)
                        {
                            if (primitive.indexCount == 0)
                            {
                                continue;
                            }
                            dynHasMaterialSlot = true;
                            dynMaxMaterialSlot = std::max(
                                dynMaxMaterialSlot,
                                static_cast<size_t>(primitive.materialIndex));
                            dynMaterialSlots.insert(primitive.materialIndex);
                        }
                    }
                    AddLog(app,
                        LogLevel::Info,
                        "DynModel export summary for " + entry.name +
                        ": meshes=" + std::to_string(dynLods.size()) +
                        ", compactVertices=" + std::to_string(dynVertexCount) +
                        ", indices=" + std::to_string(dynIndexCount) +
                        ", primitives=" + std::to_string(dynPrimitiveCount));
                    AddLog(app,
                        LogLevel::Info,
                        "DynModel material slot summary for " + entry.name +
                        ": primitiveMaterialSlots=" + std::to_string(dynMaterialSlots.size()) +
                        ", maxSlot=" + (dynHasMaterialSlot ? std::to_string(dynMaxMaterialSlot) : std::string("none")) +
                        ", descriptorMaterials=" + std::to_string(matchedLink ? matchedLink->materialNames.size() : 0) +
                        ", descriptorBindings=" + std::to_string(matchedLink ? matchedLink->materialTextureBindings.size() : 0));

                    manifest.assetType = "DynModel Skinned Model";
                    manifest.formatDecoderVersion = "DynModelReader v1.0";

                    // Try to resolve skeleton
                    std::vector<DagorFormats::GeomNodeTreeNode> skeleton;
                    std::string skeletonName;
                    constexpr uint32_t kGeomNodeTreeClassId = 0x56F81B6Du;

                    auto loadSkeletonEntry = [&](const fs::path& skeletonPackPath,
                                                 const AssetIndex::SubAssetRecord& skeletonEntry,
                                                 const std::string& sourceDescription) -> bool
                    {
                        std::ifstream skelFile(skeletonPackPath, std::ios::binary);
                        if (!skelFile)
                        {
                            AddLog(app, LogLevel::Warning, "Could not open skeleton pack: " + Platform::PathToUtf8(skeletonPackPath));
                            return false;
                        }

                        skelFile.seekg(static_cast<std::streamoff>(skeletonEntry.offset), std::ios::beg);
                        std::vector<uint8_t> skelPayload(skeletonEntry.size);
                        if (!skelFile.read(reinterpret_cast<char*>(skelPayload.data()), static_cast<std::streamsize>(skelPayload.size())))
                        {
                            AddLog(app, LogLevel::Warning, "Could not read skeleton payload: " + skeletonEntry.name);
                            return false;
                        }

                        DagorFormats::GeomNodeTreeReader skelReader(skelPayload);
                        skelReader.Parse();
                        skeleton = skelReader.GetNodes();
                        manifest.decodeWarnings.push_back("Resolved skeleton " + skeletonEntry.name + " from " + sourceDescription);
                        AddLog(app,
                            LogLevel::Info,
                            "Resolved skeleton " + skeletonEntry.name +
                            " (" + std::to_string(skeleton.size()) + " nodes) from " + sourceDescription);
                        return true;
                    };
                    
                    // If we have a descriptor link, it might specify a skeleton
                    // Since I haven't added skeletonName to DescriptorResourceLink yet,
                    // I'll try to find it from the descriptor directly if available.
                    if (matchedLink)
                    {
                        // We need to re-parse the descriptor to get skeletonName
                        // or better, use the name of the resource itself as a hint.
                        auto descContents = AssetIndex::ParseDescriptorContents(matchedLink->descriptorPath);
                        if (descContents)
                        {
                            for (const auto& descEntry : descContents->entries)
                            {
                                if (descEntry.name == matchedLink->resourceName)
                                {
                                    skeletonName = descEntry.skeletonName;
                                    break;
                                }
                            }
                        }
                    }

                    std::vector<std::string> skeletonCandidates;
                    if (!skeletonName.empty())
                    {
                        skeletonCandidates.push_back(skeletonName);
                    }

                    const std::string sameNameSkeleton = entry.name + "_skeleton";
                    if (std::find(skeletonCandidates.begin(), skeletonCandidates.end(), sameNameSkeleton) == skeletonCandidates.end())
                    {
                        skeletonCandidates.push_back(sameNameSkeleton);
                    }

                    auto samePackContents = app.packContentsCache.Get(packPath);
                    if (!samePackContents)
                    {
                        auto newContents = AssetIndex::ParseGrpContents(packPath);
                        if (newContents)
                        {
                            app.packContentsCache.Put(packPath, newContents);
                            samePackContents = newContents;
                        }
                    }

                    if (samePackContents)
                    {
                        for (const std::string& candidate : skeletonCandidates)
                        {
                            auto it = std::find_if(samePackContents->entries.begin(), samePackContents->entries.end(), [&](const auto& e) {
                                return e.name == candidate && (e.grpClassId == kGeomNodeTreeClassId || e.grpClassId == 0);
                            });

                            if (it != samePackContents->entries.end() &&
                                loadSkeletonEntry(packPath, *it, packPath.filename().string()))
                            {
                                skeletonName = it->name;
                                break;
                            }
                        }
                    }

                    if (skeleton.empty() && !skeletonName.empty() && allRecords)
                    {
                        AddLog(app, LogLevel::Info, "Resolving skeleton " + skeletonName + " for " + entry.name + "...");
                        // Find GRP containing this skeleton
                        bool skeletonFound = false;
                        for (const auto& record : *allRecords)
                        {
                            if (record.type == AssetIndex::AssetType::Grp)
                            {
                                auto grpContents = app.packContentsCache.Get(record.path);
                                if (!grpContents)
                                {
                                    auto newContents = AssetIndex::ParseGrpContents(record.path);
                                    if (newContents)
                                    {
                                        app.packContentsCache.Put(record.path, newContents);
                                        grpContents = newContents;
                                    }
                                }

                                if (grpContents)
                                {
                                    auto it = std::find_if(grpContents->entries.begin(), grpContents->entries.end(), [&](const auto& e) {
                                        return e.name == skeletonName;
                                    });

                                    if (it != grpContents->entries.end())
                                    {
                                        if (loadSkeletonEntry(record.path, *it, record.path.filename().string()))
                                        {
                                            skeletonFound = true;
                                        }
                                        break;
                                    }
                                }
                            }
                        }

                        if (!skeletonFound)
                        {
                            manifest.decodeWarnings.push_back("Could not resolve skeleton resource: " + skeletonName);
                        }
                    }

                    if (skeleton.empty())
                    {
                        manifest.decodeWarnings.push_back("No skeleton resolved; rigid DynModel parts will not receive scene-node transforms.");
                        AddLog(app, LogLevel::Warning, "No skeleton resolved for " + entry.name + "; GLB part placement may be incorrect.");
                    }

                    if (format == "GLB" || format == "glb")
                    {
                        outPath = exportRoot / (safeName + ".glb");
                        manifest.outputFiles.push_back(outPath.filename().string());
                        AddLog(app,
                            LogLevel::Info,
                            "Writing DynModel GLB for " + entry.name +
                            " with skeletonNodes=" + std::to_string(skeleton.size()) +
                            " to " + Platform::PathToUtf8(outPath));
                        success = DagorAssetsExtractor::Export::ModelExporter::ExportToGlb(outPath, dynLods, skeleton, manifest, materials);
                    }
                    else
                    {
                        AddLog(app, LogLevel::Warning, "OBJ export is not supported for skinned models. Use GLB instead.");
                    }
                }
                else
                {
                    std::string message = "Skipped export for " + entry.name + ": ";
                    const std::string className = AssetIndex::DescribeGrpClassId(entry.grpClassId);
                    if (!className.empty())
                    {
                        message += className + " entry";
                    }
                    else
                    {
                        message += "unsupported GRP payload";
                    }
                    message += " (magic " + FormatPayloadMagic(payload) + "). Only RendInst and DynModel meshes can be exported right now.";
                    AddLog(app, LogLevel::Warning, message);

                    // Add detailed diagnostics dump
                    std::string detailMessage = "Payload details for skipped asset " + entry.name + ":\n" +
                        "  GRP Pack: " + packPath.filename().string() + "\n" +
                        "  Offset: " + std::to_string(entry.offset) + "\n" +
                        "  Size: " + std::to_string(entry.size) + "\n" +
                        "  First 64 bytes hex/ASCII dump:\n" +
                        FormatPayloadHeader(payload, 64);
                    AddLog(app, LogLevel::Warning, detailMessage);
                    return;
                }

                if (success)
                {
                    AddLog(app, LogLevel::Info, "Exported model: " + entry.name + " to " + Platform::PathToUtf8(outPath));
                }
                else
                {
                    AddLog(app, LogLevel::Error, "Failed to export model: " + entry.name);
                }
            }
            catch (const std::exception& e)
            {
                AddLog(app, LogLevel::Error, "Exception while exporting model " + entry.name + ": " + e.what());
            }
        });
    }

    void RequestModelExport(AppState& app, const std::filesystem::path& packPath, const AssetIndex::SubAssetRecord& entry)
    {
        RequestModelExportInternal(app, packPath, entry, std::nullopt);
    }

    void RequestModelExport(AppState& app, const std::filesystem::path& packPath, const AssetIndex::SubAssetRecord& entry, bool includeTexturesForDynModel)
    {
        RequestModelExportInternal(app, packPath, entry, includeTexturesForDynModel);
    }

    void OpenDynModelExportModal(AppState& app, const std::filesystem::path& packPath, const AssetIndex::SubAssetRecord& entry)
    {
        if (!AssetIndex::CanExportGrpEntryAsSkinnedModel(entry))
        {
            AddLog(app,
                LogLevel::Warning,
                "Could not open dynamic export options for " + entry.name + ": the GRP entry is not a DynModel.");
            return;
        }

        PendingDynModelExport pending;
        pending.packPath = packPath;
        pending.entry = entry;
        pending.includeTextures = true;
        pending.includeSkeletonRig = true;
        pending.includeAnimations = false;
        pending.openRequested = true;
        app.pendingDynModelExport = std::move(pending);
    }

    bool OpenDynModelExportModalByName(AppState& app, const std::filesystem::path& packPath, const std::string& entryName)
    {
        auto contents = app.packContentsCache.Get(packPath);
        if (!contents)
        {
            auto newContents = AssetIndex::ParseGrpContents(packPath);
            if (newContents)
            {
                app.packContentsCache.Put(packPath, newContents);
                contents = newContents;
            }
        }

        if (!contents || !contents->error.empty())
        {
            AddLog(app,
                LogLevel::Error,
                "Could not parse GRP contents for DynModel export options from " +
                    Platform::PathToUtf8(packPath.filename()) +
                    (contents ? ": " + contents->error : ""));
            return false;
        }

        auto it = std::find_if(contents->entries.begin(), contents->entries.end(), [&](const auto& e) {
            return e.name == entryName;
        });

        if (it == contents->entries.end())
        {
            AddLog(app,
                LogLevel::Error,
                "Could not find DynModel " + entryName + " in " + Platform::PathToUtf8(packPath.filename()));
            return false;
        }

        if (!AssetIndex::CanExportGrpEntryAsSkinnedModel(*it))
        {
            AddLog(app,
                LogLevel::Warning,
                "Could not open dynamic export options for " + entryName + ": the resolved GRP entry is not a DynModel.");
            return false;
        }

        OpenDynModelExportModal(app, packPath, *it);
        return true;
    }

    void RequestModelExportByName(AppState& app, const std::filesystem::path& packPath, const std::string& entryName)
    {
        AddLog(app, LogLevel::Info, "Resolving model " + entryName + " in " + Platform::PathToUtf8(packPath.filename()) + "...");

        app.backgroundJobs.Start("ResolveAndExportModel_" + entryName, [&app, packPath, entryName]() {
            auto contents = app.packContentsCache.Get(packPath);
            if (!contents)
            {
                auto newContents = AssetIndex::ParseGrpContents(packPath);
                if (newContents)
                {
                    app.packContentsCache.Put(packPath, newContents);
                    contents = newContents;
                }
            }

            if (!contents || !contents->error.empty())
            {
                AddLog(app, LogLevel::Error, "Could not parse GRP contents for " + Platform::PathToUtf8(packPath.filename()) + (contents ? ": " + contents->error : ""));
                return;
            }

            auto it = std::find_if(contents->entries.begin(), contents->entries.end(), [&](const auto& e) {
                return e.name == entryName;
            });

            if (it == contents->entries.end())
            {
                AddLog(app, LogLevel::Error, "Could not find model " + entryName + " in " + Platform::PathToUtf8(packPath.filename()));
                return;
            }

            if (AssetIndex::CanExportGrpEntryAsSkinnedModel(*it))
            {
                AddLog(app,
                    LogLevel::Warning,
                    "Skipped DynModel export for " + entryName +
                    ": dynamic model export options must be confirmed in the Export Dynamic Model popup.");
                return;
            }

            // Delegate to the main export function
            RequestModelExport(app, packPath, *it);
        });
    }

    void RequestBatchTextureExport(AppState& app, const std::filesystem::path& packPath, std::shared_ptr<const AssetIndex::PackContents> contents)
    {
        if (!contents) return;

        AddLog(app, LogLevel::Info, "Starting batch export of textures from " + Platform::PathToUtf8(packPath.filename()) + "...");
        Export::TextureExportOptions options = GetTextureExportOptions(app);

        app.backgroundJobs.Start("BatchExportTextures_" + packPath.filename().string(), [&app, packPath, contents, options]() {
            auto result = Export::TextureExporter::ExportAllTextures(packPath, *contents, options, nullptr);

            std::ostringstream msg;
            msg << "Batch texture export completed: " << result.succeeded << " succeeded, "
                << result.failed << " failed, " << result.skipped << " skipped.";
            AddLog(app, LogLevel::Info, msg.str());

            for (const auto& err : result.errors)
            {
                AddLog(app, LogLevel::Error, err);
            }
            for (const auto& warn : result.warnings)
            {
                AddLog(app, LogLevel::Warning, warn);
            }
        });
    }

    bool IsOodleAvailable(const AppState& app)
    {
        (void)app;
        return !DagorIO::Decompressor::FindBundledOodleDll().empty();
    }

    size_t CountBlkPropertiesRecursive(const DagorFormats::BlkBlock& block)
    {
        size_t count = block.properties.size();
        for (const auto& child : block.children)
        {
            count += CountBlkPropertiesRecursive(child);
        }
        return count;
    }

    size_t CountBlkBlocksRecursive(const DagorFormats::BlkBlock& block)
    {
        size_t count = block.children.size();
        for (const auto& child : block.children)
        {
            count += CountBlkBlocksRecursive(child);
        }
        return count;
    }

    void CollectBlkPreviewLines(const DagorFormats::BlkBlock& block, BlkPreview& preview, int depth = 0)
    {
        if (preview.lines.size() >= 48)
        {
            return;
        }

        const std::string indent(static_cast<size_t>(depth) * 2, ' ');

        for (const auto& prop : block.properties)
        {
            if (preview.lines.size() >= 48)
            {
                return;
            }
            preview.lines.push_back(indent + prop.name + ": " + prop.value);
        }

        for (const auto& child : block.children)
        {
            if (preview.lines.size() >= 48)
            {
                return;
            }
            preview.lines.push_back(indent + child.name + " {");
            CollectBlkPreviewLines(child, preview, depth + 1);

            if (preview.lines.size() >= 48)
            {
                return;
            }
            preview.lines.push_back(indent + "}");
        }
    }

    BlkPreview LoadBlkPreview(const fs::path& path)
    {
        constexpr uintmax_t MaxPreviewBytes = 1024 * 1024;

        BlkPreview preview;
        preview.path = path;
        preview.loaded = true;

        std::error_code ec;
        const uintmax_t fileSize = fs::file_size(path, ec);
        if (ec)
        {
            preview.error = "Could not read BLK size: " + ec.message();
            return preview;
        }

        if (fileSize > MaxPreviewBytes)
        {
            preview.error = "BLK file is larger than the 1 MB preview limit.";
            return preview;
        }

        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            preview.error = "Could not open BLK file.";
            return preview;
        }

        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        if (DagorFormats::IsBinaryBlk(text))
        {
            preview.error = "This BLK file is in binary/packed format and cannot be previewed as text.";
            return preview;
        }

        try
        {
            DagorFormats::BlkReader reader(text);
            reader.Parse();

            const DagorFormats::BlkBlock& root = reader.GetRoot();
            preview.propertyCount = CountBlkPropertiesRecursive(root);
            preview.blockCount = CountBlkBlocksRecursive(root);
            CollectBlkPreviewLines(root, preview);
        }
        catch (const std::exception& exception)
        {
            preview.error = std::string("BLK parse error: ") + exception.what();
        }

        return preview;
    }

    std::shared_ptr<const BlkPreview> SnapshotBlkPreview(AppState& app, const std::filesystem::path& path)
    {
        std::lock_guard<std::mutex> lock(app.blkPreview.mutex);
        if (app.blkPreview.preview && app.blkPreview.preview->path == path)
        {
            return app.blkPreview.preview;
        }
        return nullptr;
    }

    bool IsBlkPreviewLoading(AppState& app, const std::filesystem::path& path)
    {
        std::lock_guard<std::mutex> lock(app.blkPreview.mutex);
        return app.blkPreview.loading && app.blkPreview.loadingPath == path;
    }

    void RequestBlkPreview(AppState& app, const std::filesystem::path& path)
    {
        {
            std::lock_guard<std::mutex> lock(app.blkPreview.mutex);
            if (app.blkPreview.loading && app.blkPreview.loadingPath == path)
            {
                return;
            }
            app.blkPreview.loading = true;
            app.blkPreview.loadingPath = path;
        }

        app.backgroundJobs.Start("BlkPreview_" + path.filename().string(), [&app, path]() {
            BlkPreview result = LoadBlkPreview(path);

            std::lock_guard<std::mutex> lock(app.blkPreview.mutex);
            if (app.blkPreview.loadingPath == path)
            {
                app.blkPreview.preview = std::make_shared<const BlkPreview>(std::move(result));
                app.blkPreview.loading = false;
            }
        });
    }

    void RequestBlkResourceLinks(AppState& app, std::shared_ptr<const std::vector<AssetIndex::ResourceRecord>> records)
    {
        if (!records) return;

        {
            std::lock_guard<std::mutex> lock(app.blkLinks.mutex);
            if (app.blkLinks.loading) return;
            app.blkLinks.loading = true;
            app.blkLinks.status = "Building relationships...";
            app.blkLinks.loadingRecords = records;
        }

        app.backgroundJobs.Start("BlkResourceLinks", [&app, records]() {
            try
            {
                auto result = AssetIndex::BuildBlkResourceLinks(*records);
                const auto links = std::make_shared<const std::vector<AssetIndex::BlkResourceLink>>(std::move(result.links));

                {
                    std::lock_guard<std::mutex> lock(app.blkLinks.mutex);
                    if (app.blkLinks.loadingRecords != records)
                    {
                        return;
                    }

                    app.blkLinks.links = links;
                    app.blkLinks.status = "Built " + std::to_string(app.blkLinks.links->size()) + " relationships";
                    app.blkLinks.loading = false;
                    app.blkLinks.loadingRecords.reset();
                }

                {
                    std::lock_guard<std::mutex> lock(app.descriptorLinks.mutex);
                    app.descriptorLinks.status = "Not built";
                    app.descriptorLinks.links.reset();
                    app.descriptorLinks.loadingRecords.reset();
                    app.descriptorLinks.cacheLoadingRecords.reset();
                    app.descriptorLinks.loadingBlkLinks.reset();
                    app.descriptorLinks.cancelRequested.store(false);
                    app.descriptorLinks.globalLinks = false;
                    app.descriptorLinks.scopePath.reset();
                    app.descriptorLinks.cacheStatus = "Invalidated by BLK relationship rebuild";
                    app.descriptorLinks.loading = false;
                    app.descriptorLinks.cacheLoading = false;
                }
            }
            catch (const std::exception& e)
            {
                std::lock_guard<std::mutex> lock(app.blkLinks.mutex);
                if (app.blkLinks.loadingRecords != records)
                {
                    return;
                }

                app.blkLinks.status = "Failed: " + std::string(e.what());
                app.blkLinks.loading = false;
                app.blkLinks.loadingRecords.reset();
            }
        });
    }

    std::shared_ptr<const std::vector<AssetIndex::BlkResourceLink>> SnapshotBlkResourceLinks(AppState& app)
    {
        std::lock_guard<std::mutex> lock(app.blkLinks.mutex);
        return app.blkLinks.links;
    }

    std::string GetBlkResourceLinkStatus(AppState& app)
    {
        std::lock_guard<std::mutex> lock(app.blkLinks.mutex);
        return app.blkLinks.status;
    }

    bool IsBlkResourceLinksLoading(AppState& app)
    {
        std::lock_guard<std::mutex> lock(app.blkLinks.mutex);
        return app.blkLinks.loading;
    }

    void RequestDescriptorResourceLinksInternal(
        AppState& app,
        std::shared_ptr<const std::vector<AssetIndex::ResourceRecord>> records,
        std::optional<AssetIndex::ResourceRecord> selected)
    {
        if (!records)
        {
            return;
        }

        const bool scopedBuild = selected.has_value();
        const std::shared_ptr<const std::vector<AssetIndex::BlkResourceLink>> blkLinks = SnapshotBlkResourceLinks(app);

        {
            std::lock_guard<std::mutex> lock(app.descriptorLinks.mutex);
            if (app.descriptorLinks.loading)
            {
                AddLog(app, LogLevel::Info, "Descriptor relationship build is already running.");
                return;
            }
            if (app.descriptorLinks.cacheLoading)
            {
                AddLog(app, LogLevel::Info, "Descriptor-link cache is loading; wait for it to finish or rebuild after it is skipped.");
                return;
            }

            app.descriptorLinks.loading = true;
            app.descriptorLinks.status = scopedBuild
                ? "Building selected descriptor relationships..."
                : "Building descriptor relationships...";
            app.descriptorLinks.cancelRequested.store(false);
            app.descriptorLinks.loadingRecords = records;
            app.descriptorLinks.loadingBlkLinks = blkLinks;
            app.descriptorLinks.scopePath = scopedBuild ? std::optional<fs::path>(selected->path) : std::nullopt;
        }

        const std::string targetName = scopedBuild
            ? Platform::PathToUtf8(selected->path.filename())
            : std::string();
        BeginProgressJob(
            app,
            scopedBuild ? "Descriptor Resource Links - Selected Asset" : "Descriptor Resource Links",
            scopedBuild ? "Starting selected descriptor relationship build" : "Starting descriptor relationship build",
            records->size());
        AddProgressJobLog(app, LogLevel::Info, "Preparing descriptor, GRP, and DXP metadata indexes.");
        std::string buildLogPrefix = "Building descriptor relationships: ";
        if (scopedBuild)
        {
            buildLogPrefix = "Building selected descriptor relationships: target=" + targetName + ", ";
        }
        AddLog(app,
            LogLevel::Info,
            buildLogPrefix +
            "records=" + std::to_string(records->size()) +
            ", blkLinks=" + (blkLinks ? std::to_string(blkLinks->size()) : std::string("not built")) +
            ", using universal metadata indexes.");

        app.backgroundJobs.Start(scopedBuild ? "DescriptorResourceLinks_Selected" : "DescriptorResourceLinks", [&app, records, blkLinks, selected, scopedBuild, targetName]() {
            try
            {
                std::string lastProgressStage;
                uint64_t lastLogCurrent = 0;

                AssetIndex::DescriptorResourceLinkBuildOptions options;
                options.blkLinks = blkLinks ? blkLinks.get() : nullptr;
                if (selected)
                {
                    options.focusPath = selected->path;
                }
                options.progress = [&app, records, &lastProgressStage, &lastLogCurrent](const AssetIndex::DescriptorResourceLinkProgress& progress) {
                    UpdateProgressJob(
                        app,
                        static_cast<uint64_t>(progress.current),
                        static_cast<uint64_t>(progress.total),
                        progress.stage,
                        progress.item);

                    {
                        std::lock_guard<std::mutex> lock(app.descriptorLinks.mutex);
                        if (app.descriptorLinks.loadingRecords == records)
                        {
                            std::ostringstream status;
                            status << progress.stage;
                            if (progress.total > 0)
                            {
                                status << " " << progress.current << '/' << progress.total;
                            }
                            app.descriptorLinks.status = status.str();
                        }
                    }

                    if (progress.stage != lastProgressStage ||
                        progress.current == progress.total ||
                        progress.current >= lastLogCurrent + 250)
                    {
                        std::ostringstream line;
                        line << progress.stage;
                        if (!progress.item.empty())
                        {
                            line << ": " << progress.item;
                        }
                        if (progress.total > 0)
                        {
                            line << " [" << progress.current << '/' << progress.total << ']';
                        }
                        line << " links=" << progress.linksBuilt
                             << " descriptors=" << progress.descriptorFilesParsed
                             << " grpPacks=" << progress.grpPacksParsed
                             << " dxpPacks=" << progress.dxpPacksParsed;
                        AddProgressJobLog(app, LogLevel::Info, line.str());
                        AddLog(app, LogLevel::Info, "Descriptor link progress: " + line.str());
                        lastProgressStage = progress.stage;
                        lastLogCurrent = static_cast<uint64_t>(progress.current);
                    }

                    return !app.descriptorLinks.cancelRequested.load() &&
                           !app.progressJob.cancelRequested.load();
                };

                auto result = AssetIndex::BuildDescriptorResourceLinks(*records, options);

                const size_t warningCount = result.warnings.size();
                const size_t parseErrors = result.parseErrors;
                const size_t descriptorFilesParsed = result.descriptorFilesParsed;
                const size_t descriptorFilesSkipped = result.descriptorFilesSkipped;
                const size_t grpPacksParsed = result.grpPacksParsed;
                const size_t dxpPacksParsed = result.dxpPacksParsed;
                const auto links = std::make_shared<const std::vector<AssetIndex::DescriptorResourceLink>>(std::move(result.links));

                std::ostringstream status;
                if (result.canceled)
                {
                    status << "Canceled after building " << links->size()
                           << (scopedBuild ? " selected" : "")
                           << " descriptor links; previous links kept";
                }
                else
                {
                    status << "Built " << links->size()
                           << (scopedBuild ? " selected" : "")
                           << " descriptor links from " << descriptorFilesParsed
                           << " descriptor file" << (descriptorFilesParsed == 1 ? "" : "s");
                }
                if (descriptorFilesSkipped > 0)
                {
                    status << ", skipped " << descriptorFilesSkipped;
                }
                if (parseErrors > 0)
                {
                    status << ", " << parseErrors << " parse error" << (parseErrors == 1 ? "" : "s");
                }
                if (warningCount > 0)
                {
                    status << ", " << warningCount << " warning" << (warningCount == 1 ? "" : "s");
                }

                {
                    std::lock_guard<std::mutex> lock(app.descriptorLinks.mutex);
                    if (app.descriptorLinks.loadingRecords != records)
                    {
                        return;
                    }

                    if (!result.canceled)
                    {
                        app.descriptorLinks.links = links;
                        app.descriptorLinks.globalLinks = !scopedBuild;
                        app.descriptorLinks.scopePath = scopedBuild && selected
                            ? std::optional<fs::path>(selected->path)
                            : std::nullopt;
                    }
                    app.descriptorLinks.status = status.str();
                    app.descriptorLinks.loading = false;
                    app.descriptorLinks.loadingRecords.reset();
                    app.descriptorLinks.loadingBlkLinks.reset();
                    app.descriptorLinks.cancelRequested.store(false);
                }

                std::ostringstream log;
                log << (result.canceled ? "Descriptor relationship build canceled: " : "Descriptor relationships built: ")
                    << links->size() << " resources, "
                    << grpPacksParsed << " GRP packs indexed, "
                    << dxpPacksParsed << " DXP packs indexed.";
                const LogLevel finalLevel = result.canceled
                    ? LogLevel::Warning
                    : (warningCount > 0 || parseErrors > 0 ? LogLevel::Warning : LogLevel::Info);
                AddProgressJobLog(app, finalLevel, log.str());
                AddLog(app, finalLevel, log.str());

                if (!result.canceled && !scopedBuild && !app.descriptorLinksCachePath.empty())
                {
                    std::string cacheError;
                    const auto cacheSaveStarted = std::chrono::steady_clock::now();
                    if (AssetIndex::SaveDescriptorResourceLinkCache(
                            app.descriptorLinksCachePath,
                            app.settings.gameRoot,
                            *records,
                            *links,
                            options,
                            cacheError))
                    {
                        const auto cacheSaveMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - cacheSaveStarted).count();
                        std::error_code ec;
                        const uintmax_t cacheBytes = fs::file_size(app.descriptorLinksCachePath, ec);
                        std::string cacheMessage =
                            "Descriptor-link cache saved with " + std::to_string(links->size()) +
                            " links in " + std::to_string(cacheSaveMs) + " ms";
                        if (!ec)
                        {
                            cacheMessage += ", size=" + std::to_string(cacheBytes) + " bytes";
                        }
                        cacheMessage += ".";
                        {
                            std::lock_guard<std::mutex> lock(app.descriptorLinks.mutex);
                            app.descriptorLinks.cacheStatus = cacheMessage;
                        }
                        AddProgressJobLog(app, LogLevel::Info, cacheMessage);
                        AddLog(app, LogLevel::Info, cacheMessage);
                    }
                    else
                    {
                        {
                            std::lock_guard<std::mutex> lock(app.descriptorLinks.mutex);
                            app.descriptorLinks.cacheStatus = "Save failed: " + cacheError;
                        }
                        AddProgressJobLog(app, LogLevel::Warning, cacheError);
                        AddLog(app, LogLevel::Warning, cacheError);
                    }
                }

                EndProgressJob(app, status.str());

                for (size_t i = 0; i < std::min<size_t>(warningCount, 5); ++i)
                {
                    AddProgressJobLog(app, LogLevel::Warning, result.warnings[i]);
                    AddLog(app, LogLevel::Warning, result.warnings[i]);
                }
            }
            catch (const std::exception& e)
            {
                {
                    std::lock_guard<std::mutex> lock(app.descriptorLinks.mutex);
                    if (app.descriptorLinks.loadingRecords != records)
                    {
                        return;
                    }

                    app.descriptorLinks.status = "Failed: " + std::string(e.what());
                    app.descriptorLinks.loading = false;
                    app.descriptorLinks.loadingRecords.reset();
                    app.descriptorLinks.loadingBlkLinks.reset();
                    app.descriptorLinks.cancelRequested.store(false);
                }

                AddProgressJobLog(app, LogLevel::Error, "Descriptor relationship build failed: " + std::string(e.what()));
                EndProgressJob(app, "Failed: " + std::string(e.what()));
                AddLog(app, LogLevel::Error, "Descriptor relationship build failed: " + std::string(e.what()));
            }
        });
    }

    void RequestDescriptorResourceLinks(AppState& app, std::shared_ptr<const std::vector<AssetIndex::ResourceRecord>> records)
    {
        RequestDescriptorResourceLinksInternal(app, std::move(records), std::nullopt);
    }

    void RequestDescriptorResourceLinksForSelected(
        AppState& app,
        std::shared_ptr<const std::vector<AssetIndex::ResourceRecord>> records,
        AssetIndex::ResourceRecord selected)
    {
        RequestDescriptorResourceLinksInternal(app, std::move(records), std::move(selected));
    }

    std::shared_ptr<const std::vector<AssetIndex::DescriptorResourceLink>> SnapshotDescriptorResourceLinks(AppState& app)
    {
        std::lock_guard<std::mutex> lock(app.descriptorLinks.mutex);
        return app.descriptorLinks.links;
    }

    std::string GetDescriptorResourceLinkStatus(AppState& app)
    {
        std::lock_guard<std::mutex> lock(app.descriptorLinks.mutex);
        return app.descriptorLinks.status;
    }

    bool HasGlobalDescriptorResourceLinks(AppState& app)
    {
        std::lock_guard<std::mutex> lock(app.descriptorLinks.mutex);
        return app.descriptorLinks.globalLinks && app.descriptorLinks.links && !app.descriptorLinks.links->empty();
    }

    bool IsDescriptorResourceLinksLoading(AppState& app)
    {
        std::lock_guard<std::mutex> lock(app.descriptorLinks.mutex);
        return app.descriptorLinks.loading || app.descriptorLinks.cacheLoading;
    }

    void RequestDescriptorResourceLinkCancel(AppState& app)
    {
        app.descriptorLinks.cancelRequested.store(true);
        app.progressJob.cancelRequested.store(true);
        AddProgressJobLog(app, LogLevel::Warning, "Cancel requested by user.");
        AddLog(app, LogLevel::Warning, "Descriptor relationship build cancel requested.");
    }

    ProgressJobSnapshot SnapshotProgressJob(AppState& app)
    {
        std::lock_guard<std::mutex> lock(app.progressJob.mutex);
        ProgressJobSnapshot snapshot;
        snapshot.running = app.progressJob.running.load();
        snapshot.cancelRequested = app.progressJob.cancelRequested.load();
        snapshot.windowOpen = app.progressJob.windowOpen;
        snapshot.title = app.progressJob.title;
        snapshot.status = app.progressJob.status;
        snapshot.currentItem = app.progressJob.currentItem;
        snapshot.current = app.progressJob.current;
        snapshot.total = app.progressJob.total;
        snapshot.logs = app.progressJob.logs;
        return snapshot;
    }

    void RequestProgressJobCancel(AppState& app)
    {
        app.progressJob.cancelRequested.store(true);
        AddProgressJobLog(app, LogLevel::Warning, "Cancel requested by user.");
    }

    void JoinFinishedBackgroundJobs(AppState& app)
    {
        app.backgroundJobs.JoinFinished();
    }

    void WaitForBackgroundJobs(AppState& app)
    {
        app.backgroundJobs.JoinAll();
    }
}
