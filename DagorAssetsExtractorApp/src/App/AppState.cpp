#include "App/AppState.h"
#include "Platform/Platform.h"
#include <fstream>
#include <chrono>

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

    void AddLog(AppState& app, LogLevel level, std::string message)
    {
        std::lock_guard<std::mutex> lock(app.logMutex);
        app.logs.push_back(LogEntry{ NowString(), level, std::move(message) });
        if (app.logs.size() > 1000)
        {
            app.logs.erase(app.logs.begin(), app.logs.begin() + 250);
        }
    }

    std::vector<ResourceRecord> SnapshotRecords(ScanState& scan)
    {
        std::lock_guard<std::mutex> lock(scan.mutex);
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
            else if (key == "preserveOriginalDDS")
            {
                settings.preserveOriginalDDS = ParseBool(value, settings.preserveOriginalDDS);
            }
            else if (key == "useOodleFromGameFolder")
            {
                settings.useOodleFromGameFolder = ParseBool(value, settings.useOodleFromGameFolder);
            }
            else if (key == "indexCacheEnabled")
            {
                settings.indexCacheEnabled = ParseBool(value, settings.indexCacheEnabled);
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
        file << "preserveOriginalDDS=" << (settings.preserveOriginalDDS ? "true" : "false") << '\n';
        file << "useOodleFromGameFolder=" << (settings.useOodleFromGameFolder ? "true" : "false") << '\n';
        file << "indexCacheEnabled=" << (settings.indexCacheEnabled ? "true" : "false") << '\n';
        file << "lastOpenedPackFilters=" << settings.lastOpenedPackFilters << '\n';
        return true;
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
            app.scan.records.clear();
            app.scan.status = std::move(status);
            app.scan.cacheStatus = app.settings.indexCacheEnabled ? "Not loaded" : "Disabled";
            app.scan.cacheLoaded = false;
        }

        app.selectedPath.clear();
        app.scan.filesVisited.store(0);
        app.scan.recordsFound.store(0);
        app.scan.completed.store(false);
        app.scan.failed.store(false);
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

        {
            std::lock_guard<std::mutex> lock(app.scan.mutex);
            app.scan.records = result.records;
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
            app.scan.records.clear();
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
                options.onRecordFound = [&app](const ResourceRecord& record) {
                    size_t recordsFound = 0;
                    {
                        std::lock_guard<std::mutex> lock(app.scan.mutex);
                        app.scan.records.push_back(record);
                        recordsFound = app.scan.records.size();
                        std::ostringstream status;
                        status << "Scanning - " << recordsFound << " records found";
                        app.scan.status = status.str();
                    }
                    app.scan.recordsFound.store(static_cast<uint64_t>(recordsFound));
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
                    std::ostringstream message;
                    message << "Resource discovery completed: "
                            << app.scan.recordsFound.load()
                            << " relevant files found after visiting "
                            << app.scan.filesVisited.load()
                            << " paths.";
                    SetScanStatus(app.scan, "Completed");
                    AddLog(app, LogLevel::Info, message.str());
                    app.scan.completed.store(true);

                    if (writeIndexCache)
                    {
                        std::string cacheError;
                        const std::vector<ResourceRecord> records = SnapshotRecords(app.scan);
                        if (AssetIndex::SaveIndexCacheRecords(cachePath, root, records, cacheError))
                        {
                            std::ostringstream cacheMessage;
                            cacheMessage << "Index cache saved with " << records.size() << " records.";
                            SetCacheStatus(app.scan, cacheMessage.str(), true);
                            AddLog(app, LogLevel::Info, cacheMessage.str());
                        }
                        else
                        {
                            SetCacheStatus(app.scan, "Cache save failed", false);
                            AddLog(app, LogLevel::Warning, cacheError);
                        }
                    }
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
}
