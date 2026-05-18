#include "App/AppState.h"
#include "Platform/Platform.h"
#include "DagorIO/Decompression.h"
#include "Export/TextureExporter.h"
#include "Export/ModelExporter.h"
#include "DagorFormats/BlkReader.h"
#include "DagorFormats/RendInstReader.h"
#include <fstream>
#include <chrono>
#include <sstream>
#include <algorithm>

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
            app.scan.records = nullptr;
            app.scan.status = std::move(status);
            app.scan.cacheStatus = app.settings.indexCacheEnabled ? "Not loaded" : "Disabled";
            app.scan.cacheLoaded = false;
        }

        app.selectedPath.clear();
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
            app.descriptorLinks.status = "Not built";
            app.descriptorLinks.loadingRecords.reset();
            app.descriptorLinks.loadingBlkLinks.reset();
            app.descriptorLinks.links.reset();
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

        {
            std::lock_guard<std::mutex> lock(app.scan.mutex);
            app.scan.records = std::make_shared<std::vector<ResourceRecord>>(result.records);
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
        options.oodleDllPath = app.settings.oodleDllPath;
        options.format = app.settings.textureExportFormat;
        options.overwrite = true;
        options.useOodleFromGameFolder = app.settings.useOodleFromGameFolder;
        return options;
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

    void RequestModelExport(AppState& app, const std::filesystem::path& packPath, const AssetIndex::SubAssetRecord& entry)
    {
        AddLog(app, LogLevel::Info, "Exporting model: " + entry.name + "...");

        std::filesystem::path exportRoot = app.settings.exportRoot;
        std::string format = app.settings.modelExportFormat;
        bool exportTextures = app.settings.exportTexturesWithModels;
        
        auto descriptorLinks = SnapshotDescriptorResourceLinks(app);
        auto textureExportOptions = GetTextureExportOptions(app);
        // Use the model export folder as the output folder for textures too
        textureExportOptions.outputDir = exportRoot;

        app.backgroundJobs.Start("ExportModel_" + entry.name, [&app, packPath, entry, exportRoot, format, exportTextures, descriptorLinks, textureExportOptions]() {
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

                DagorFormats::RendInstReader reader(payload);
                reader.Parse();

                std::string safeName = entry.name;
                std::replace(safeName.begin(), safeName.end(), '/', '_');
                std::replace(safeName.begin(), safeName.end(), '\\', '_');
                std::replace(safeName.begin(), safeName.end(), ':', '_');

                std::error_code ec;
                fs::create_directories(exportRoot, ec);

                DagorAssetsExtractor::Export::ExportManifest manifest;
                manifest.sourcePack = packPath.filename().string();
                manifest.sourceResource = entry.name;
                manifest.assetType = "RendInst Static Model";
                manifest.appVersion = "1.0.0";
                manifest.formatDecoderVersion = "RendInstReader v1.0";

                std::vector<Export::MaterialInfo> materials;
                
                if (exportTextures && descriptorLinks)
                {
                    // Find matching descriptor link for this GRP entry
                    for (const auto& link : *descriptorLinks)
                    {
                        // Match by name and by GRP path if available
                        bool nameMatch = (link.resourceName == entry.name);
                        bool packMatch = !link.matchedGrpPath || (*link.matchedGrpPath == packPath);

                        if (nameMatch && packMatch)
                        {
                            for (const auto& texLink : link.textureLinks)
                            {
                                if (texLink.matchedDxpPath && texLink.matchedDxpEntryName)
                                {
                                    // Resolve SubAssetRecord for the texture
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
                                                Export::MaterialInfo mat;
                                                mat.name = texLink.textureName;
                                                mat.diffuseTexturePath = texResult.outputPath.filename().string();
                                                materials.push_back(mat);
                                                manifest.textureDependencies.push_back(texResult.outputPath.filename().string());
                                            }
                                            else
                                            {
                                                manifest.decodeWarnings.push_back("Failed to export texture dependency: " + texLink.textureName + " (" + texResult.error + ")");
                                            }
                                        }
                                    }
                                }
                                else
                                {
                                    manifest.decodeWarnings.push_back("Could not find pack for texture dependency: " + texLink.textureName);
                                }
                            }
                            break;
                        }
                    }
                }

                bool success = false;
                fs::path outPath;

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
                    else
                    {
                        AddLog(app, LogLevel::Error, "No LODs available to export as OBJ.");
                        return;
                    }
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
        std::string gameRoot = app.settings.useOodleFromGameFolder
            ? Platform::PathToUtf8(app.settings.gameRoot)
            : std::string();
        std::string oodleDllPath = Platform::PathToUtf8(app.settings.oodleDllPath);
        return DagorIO::Decompressor::IsOodleAvailable(gameRoot, oodleDllPath);
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
                    app.descriptorLinks.loadingBlkLinks.reset();
                    app.descriptorLinks.loading = false;
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

    void RequestDescriptorResourceLinks(AppState& app, std::shared_ptr<const std::vector<AssetIndex::ResourceRecord>> records)
    {
        if (!records)
        {
            return;
        }

        const std::shared_ptr<const std::vector<AssetIndex::BlkResourceLink>> blkLinks = SnapshotBlkResourceLinks(app);

        {
            std::lock_guard<std::mutex> lock(app.descriptorLinks.mutex);
            if (app.descriptorLinks.loading)
            {
                return;
            }

            app.descriptorLinks.loading = true;
            app.descriptorLinks.status = "Building descriptor relationships...";
            app.descriptorLinks.loadingRecords = records;
            app.descriptorLinks.loadingBlkLinks = blkLinks;
        }

        app.backgroundJobs.Start("DescriptorResourceLinks", [&app, records, blkLinks]() {
            try
            {
                auto result = AssetIndex::BuildDescriptorResourceLinks(
                    *records,
                    blkLinks ? blkLinks.get() : nullptr);

                const size_t warningCount = result.warnings.size();
                const size_t parseErrors = result.parseErrors;
                const size_t descriptorFilesParsed = result.descriptorFilesParsed;
                const size_t descriptorFilesSkipped = result.descriptorFilesSkipped;
                const size_t grpPacksParsed = result.grpPacksParsed;
                const size_t dxpPacksParsed = result.dxpPacksParsed;
                const auto links = std::make_shared<const std::vector<AssetIndex::DescriptorResourceLink>>(std::move(result.links));

                std::ostringstream status;
                status << "Built " << links->size()
                       << " descriptor links from " << descriptorFilesParsed
                       << " descriptor file" << (descriptorFilesParsed == 1 ? "" : "s");
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

                    app.descriptorLinks.links = links;
                    app.descriptorLinks.status = status.str();
                    app.descriptorLinks.loading = false;
                    app.descriptorLinks.loadingRecords.reset();
                    app.descriptorLinks.loadingBlkLinks.reset();
                }

                std::ostringstream log;
                log << "Descriptor relationships built: " << links->size()
                    << " resources, " << grpPacksParsed << " GRP packs parsed, "
                    << dxpPacksParsed << " DXP packs parsed.";
                AddLog(app, warningCount > 0 || parseErrors > 0 ? LogLevel::Warning : LogLevel::Info, log.str());

                for (size_t i = 0; i < std::min<size_t>(warningCount, 5); ++i)
                {
                    AddLog(app, LogLevel::Warning, result.warnings[i]);
                }
            }
            catch (const std::exception& e)
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
            }
        });
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

    bool IsDescriptorResourceLinksLoading(AppState& app)
    {
        std::lock_guard<std::mutex> lock(app.descriptorLinks.mutex);
        return app.descriptorLinks.loading;
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
