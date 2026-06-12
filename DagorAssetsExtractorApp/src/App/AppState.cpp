#include "App/AppState.h"
#include "Platform/Platform.h"
#include "DagorIO/Decompression.h"
#include "Export/MaterialTextureResolver.h"
#include "Export/TextureExporter.h"
#include "Export/ModelExporter.h"
#include "DagorFormats/BlkReader.h"
#include "DagorFormats/DdsxReader.h"
#include "DagorFormats/RendInstReader.h"
#include "DagorFormats/DynModelReader.h"
#include "DagorFormats/GeomNodeTreeReader.h"
#include "DagorFormats/Anim2DataReader.h"
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

    bool ContainsAscii(std::string_view haystack, std::string_view needle)
    {
        return haystack.find(needle) != std::string_view::npos;
    }

    std::string DescribeResolvedTexturePack(const fs::path& path)
    {
        const std::string lowered = ToLowerLocal(Platform::PathToUtf8(path));
        std::vector<std::string> tags;

        if (ContainsAscii(lowered, "/content.hq/"))
        {
            tags.push_back("content.hq");
        }
        else if (ContainsAscii(lowered, "/content/"))
        {
            tags.push_back("content");
        }

        if (ContainsAscii(lowered, "/uhq_vehicles/"))
        {
            tags.push_back("uhq_vehicles");
        }
        if (ContainsAscii(lowered, "/hq_tex/"))
        {
            tags.push_back("hq_tex");
        }
        if (ContainsAscii(lowered, "/pkg_cockpits/"))
        {
            tags.push_back("pkg_cockpits");
        }
        if (ContainsAscii(lowered, "/patch/"))
        {
            tags.push_back("patch");
        }

        if (tags.empty())
        {
            return "unclassified";
        }

        std::ostringstream out;
        for (size_t i = 0; i < tags.size(); ++i)
        {
            if (i != 0)
            {
                out << ",";
            }
            out << tags[i];
        }
        return out.str();
    }

    std::string BuildDdsxHeaderLogLine(const DagorFormats::DdsxHeader& header)
    {
        constexpr uint32_t kFlagNeedPairedBaseTex = 0x00020000u;
        constexpr uint32_t kFlagRevMipOrder = 0x00040000u;
        constexpr uint32_t kFlagHqPart = 0x00080000u;

        std::ostringstream out;
        out << "DDSx header: fmt=" << header.FormatName()
            << " (" << header.d3dFormat << ")"
            << ", size=" << header.width << "x" << header.height
            << ", mips=" << static_cast<unsigned int>(header.mipCount)
            << ", hqPartLevels=" << static_cast<unsigned int>(header.hqPartLevels)
            << ", lQmip=" << static_cast<unsigned int>(header.lQmip)
            << ", mQmip=" << static_cast<unsigned int>(header.mQmip)
            << ", uQmip=" << static_cast<unsigned int>(header.uQmip)
            << ", dxtShift=" << static_cast<unsigned int>(header.dxtShift)
            << ", flags=0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << header.flags
            << std::nouppercase << std::dec << std::setfill(' ')
            << ", compression=" << header.CompressionName()
            << ", memSz=" << header.memorySize
            << ", packedSz=" << header.packedSize
            << ", HQ_PART=" << (((header.flags & kFlagHqPart) != 0) ? "yes" : "no")
            << ", NEED_PAIRED_BASETEX=" << (((header.flags & kFlagNeedPairedBaseTex) != 0) ? "yes" : "no")
            << ", REV_MIP_ORDER=" << (((header.flags & kFlagRevMipOrder) != 0) ? "yes" : "no");
        return out.str();
    }

    bool TryParseTextureSlotIndex(std::string_view slotName, size_t& slotIndex)
    {
        if (slotName.size() < 2 || slotName[0] != 't')
        {
            return false;
        }

        size_t value = 0;
        for (size_t i = 1; i < slotName.size(); ++i)
        {
            const unsigned char c = static_cast<unsigned char>(slotName[i]);
            if (std::isdigit(c) == 0)
            {
                return false;
            }
            value = (value * 10) + static_cast<size_t>(slotName[i] - '0');
        }

        if (value >= 16)
        {
            return false;
        }

        slotIndex = value;
        return true;
    }

    bool AssignExportedTexture(
        const std::string& textureName,
        const std::unordered_map<std::string, std::string>& exportedTexturePaths,
        const std::unordered_map<std::string, fs::path>& exportedTextureSourcePaths,
        std::string& outputTexturePath,
        fs::path& outputTextureSourcePath)
    {
        if (textureName.empty())
        {
            return false;
        }

        const auto pathIt = exportedTexturePaths.find(textureName);
        if (pathIt == exportedTexturePaths.end())
        {
            return false;
        }

        outputTexturePath = pathIt->second;
        const auto srcIt = exportedTextureSourcePaths.find(textureName);
        if (srcIt != exportedTextureSourcePaths.end())
        {
            outputTextureSourcePath = srcIt->second;
        }

        return true;
    }

    std::string ResolveDescriptorTextureName(
        const AssetIndex::DescriptorResourceLink& link,
        uint32_t textureIndex)
    {
        if (textureIndex < link.textureNames.size())
        {
            const std::string& textureName = link.textureNames[textureIndex];
            if (!textureName.empty())
            {
                return textureName;
            }
        }

        for (const AssetIndex::DescriptorTextureLink& textureLink : link.textureLinks)
        {
            if (textureLink.textureIndex == textureIndex)
            {
                return textureLink.textureName;
            }
        }

        return {};
    }

    std::vector<DagorFormats::InternalMaterial> BuildDescriptorMaterialSlots(
        const AssetIndex::DescriptorResourceLink* link)
    {
        if (link == nullptr || link->materialTextureBindings.empty())
        {
            return {};
        }

        size_t materialCount = std::max(link->materialNames.size(), link->materialShaderClasses.size());
        for (const AssetIndex::DescriptorMaterialTextureRef& binding : link->materialTextureBindings)
        {
            if (binding.materialIndex < 100000)
            {
                materialCount = std::max(materialCount, static_cast<size_t>(binding.materialIndex) + 1);
            }
        }

        std::vector<DagorFormats::InternalMaterial> materials(materialCount);
        for (size_t materialIndex = 0; materialIndex < materials.size(); ++materialIndex)
        {
            if (materialIndex < link->materialShaderClasses.size())
            {
                materials[materialIndex].shaderClass = link->materialShaderClasses[materialIndex];
            }
        }

        for (const AssetIndex::DescriptorMaterialTextureRef& binding : link->materialTextureBindings)
        {
            if (binding.materialIndex >= materials.size())
            {
                continue;
            }

            size_t slotIndex = 0;
            if (!TryParseTextureSlotIndex(binding.slotName, slotIndex))
            {
                continue;
            }

            const std::string textureName = ResolveDescriptorTextureName(*link, binding.textureIndex);
            if (!textureName.empty())
            {
                materials[binding.materialIndex].textureSlots[slotIndex] = textureName;
            }
        }

        return materials;
    }

    void MergeDescriptorMaterialSlots(
        std::vector<DagorFormats::InternalMaterial>& materials,
        const std::vector<DagorFormats::InternalMaterial>& descriptorMaterials)
    {
        if (descriptorMaterials.empty())
        {
            return;
        }

        if (materials.size() < descriptorMaterials.size())
        {
            materials.resize(descriptorMaterials.size());
        }

        for (size_t materialIndex = 0; materialIndex < descriptorMaterials.size(); ++materialIndex)
        {
            const DagorFormats::InternalMaterial& descriptorMaterial = descriptorMaterials[materialIndex];
            DagorFormats::InternalMaterial& material = materials[materialIndex];

            if (material.shaderClass.empty() && !descriptorMaterial.shaderClass.empty())
            {
                material.shaderClass = descriptorMaterial.shaderClass;
            }

            for (size_t slotIndex = 0; slotIndex < 16; ++slotIndex)
            {
                if (material.textureSlots[slotIndex].empty() &&
                    !descriptorMaterial.textureSlots[slotIndex].empty())
                {
                    material.textureSlots[slotIndex] = descriptorMaterial.textureSlots[slotIndex];
                }
            }
        }
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
        app.logger.AddLog(level, message);
    }

    void AddProgressJobLog(AppState& app, LogLevel level, std::string message)
    {
        app.logger.AddLog(level, std::move(message));
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
                bool isCorrupt = false;
                if (result.message.find("unsupported") != std::string::npos ||
                    result.message.find("malformed") != std::string::npos ||
                    result.message.find("truncated") != std::string::npos ||
                    result.message.find("sparse") != std::string::npos)
                {
                    isCorrupt = true;
                }

                if (isCorrupt)
                {
                    const std::string errMsg = "Descriptor cache corruption detected: " + result.message + ". Auto-cleared descriptor cache.";
                    AddLog(app, LogLevel::Error, errMsg);
                    std::error_code ec;
                    fs::remove(cachePath, ec);
                }
                else
                {
                    AddLog(app, LogLevel::Info, "Descriptor-link cache skipped: " + result.message);
                }
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

    constexpr std::string_view SharedSettingsSectionHeader = "[DagorAssetsExtractor.Settings]";

    bool IsSectionHeader(std::string_view line)
    {
        return line.size() >= 2 && line.front() == '[' && line.back() == ']';
    }

    bool IsAppSettingKey(std::string_view key)
    {
        return key == "gameRoot"
            || key == "exportRoot"
            || key == "textureExportFormat"
            || key == "staticModelMaxLods"
            || key == "exportTexturesWithModels"
            || key == "embedTexturesInGlb"
            || key == "exportJsonManifest"
            || key == "preserveOriginalDDS"
            || key == "indexCacheEnabled"
            || key == "maxLoadedPackContents"
            || key == "showVromfs"
            || key == "showGrp"
            || key == "showDxp"
            || key == "showDescriptors"
            || key == "showBlk"
            || key == "groupByBlk"
            || key == "lastOpenedPackFilters";
    }

    void ApplySettingValue(AppSettings& settings, const std::string& key, const std::string& value)
    {
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
        else if (key == "staticModelMaxLods")
        {
            settings.staticModelMaxLods = static_cast<int>(ParseSize(value, settings.staticModelMaxLods));
        }
        else if (key == "exportTexturesWithModels")
        {
            settings.exportTexturesWithModels = ParseBool(value, settings.exportTexturesWithModels);
        }
        else if (key == "embedTexturesInGlb")
        {
            settings.embedTexturesInGlb = ParseBool(value, settings.embedTexturesInGlb);
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

    std::string ReadTextFile(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            return {};
        }

        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    bool ReplaceFileWithTemp(const fs::path& tempPath, const fs::path& destinationPath, std::string& error)
    {
#if defined(_WIN32)
        if (MoveFileExW(
                tempPath.wstring().c_str(),
                destinationPath.wstring().c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0)
        {
            return true;
        }

        const std::error_code ec(static_cast<int>(GetLastError()), std::system_category());
        error = "Could not replace settings file: " + ec.message();
        return false;
#else
        std::error_code ec;
        fs::rename(tempPath, destinationPath, ec);
        if (!ec)
        {
            return true;
        }

        fs::remove(destinationPath, ec);
        ec.clear();
        fs::rename(tempPath, destinationPath, ec);
        if (ec)
        {
            error = "Could not replace settings file: " + ec.message();
            return false;
        }
        return true;
#endif
    }

    std::string BuildAppSettingsSection(const AppSettings& settings)
    {
        std::ostringstream stream;
        stream << SharedSettingsSectionHeader << '\n';
        stream << "gameRoot=" << Platform::PathToUtf8(settings.gameRoot) << '\n';
        stream << "exportRoot=" << Platform::PathToUtf8(settings.exportRoot) << '\n';
        stream << "textureExportFormat=" << settings.textureExportFormat << '\n';
        stream << "staticModelMaxLods=" << settings.staticModelMaxLods << '\n';
        stream << "exportTexturesWithModels=" << (settings.exportTexturesWithModels ? "true" : "false") << '\n';
        stream << "embedTexturesInGlb=" << (settings.embedTexturesInGlb ? "true" : "false") << '\n';
        stream << "exportJsonManifest=" << (settings.exportJsonManifest ? "true" : "false") << '\n';
        stream << "preserveOriginalDDS=" << (settings.preserveOriginalDDS ? "true" : "false") << '\n';
        stream << "indexCacheEnabled=" << (settings.indexCacheEnabled ? "true" : "false") << '\n';
        stream << "maxLoadedPackContents=" << settings.maxLoadedPackContents << '\n';
        stream << "showVromfs=" << (settings.showVromfs ? "true" : "false") << '\n';
        stream << "showGrp=" << (settings.showGrp ? "true" : "false") << '\n';
        stream << "showDxp=" << (settings.showDxp ? "true" : "false") << '\n';
        stream << "showDescriptors=" << (settings.showDescriptors ? "true" : "false") << '\n';
        stream << "showBlk=" << (settings.showBlk ? "true" : "false") << '\n';
        stream << "groupByBlk=" << (settings.groupByBlk ? "true" : "false") << '\n';
        stream << "lastOpenedPackFilters=" << settings.lastOpenedPackFilters << '\n';
        return stream.str();
    }

    std::string ExtractNonAppSettingsPayload(std::string_view text)
    {
        std::istringstream stream{std::string(text)};
        std::ostringstream result;
        std::string line;
        bool hasSeenAnySection = false;
        bool inAppSection = false;

        while (std::getline(stream, line))
        {
            const std::string trimmed = Trim(line);
            if (IsSectionHeader(trimmed))
            {
                hasSeenAnySection = true;
                inAppSection = trimmed == SharedSettingsSectionHeader;
                if (!inAppSection)
                {
                    result << line << '\n';
                }
                continue;
            }

            if (inAppSection)
            {
                continue;
            }

            if (!hasSeenAnySection)
            {
                const size_t separator = line.find('=');
                if (separator != std::string::npos)
                {
                    const std::string key = Trim(line.substr(0, separator));
                    if (IsAppSettingKey(key))
                    {
                        continue;
                    }
                }
            }

            result << line << '\n';
        }

        return result.str();
    }

    void ParseSettingsText(AppSettings& settings, std::string_view text)
    {
        std::istringstream stream{std::string(text)};
        std::string line;
        bool hasSeenAnySection = false;
        bool inAppSection = false;

        while (std::getline(stream, line))
        {
            line = Trim(line);
            if (line.empty() || line[0] == '#')
            {
                continue;
            }

            if (IsSectionHeader(line))
            {
                hasSeenAnySection = true;
                inAppSection = line == SharedSettingsSectionHeader;
                continue;
            }

            if (hasSeenAnySection && !inAppSection)
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
            if (!IsAppSettingKey(key))
            {
                continue;
            }

            ApplySettingValue(settings, key, value);
        }
    }

    std::string LoadSharedSettingsText(const fs::path& path)
    {
        if (const std::string currentText = ReadTextFile(path); !currentText.empty())
        {
            return currentText;
        }

        AppSettings migratedSettings;
        migratedSettings.exportRoot = Platform::GetDefaultExportRoot();
        bool hasLegacySettings = false;

        const fs::path legacySettingsPath = path.parent_path() / L"settings";
        if (legacySettingsPath != path)
        {
            const std::string legacySettingsText = ReadTextFile(legacySettingsPath);
            if (!legacySettingsText.empty())
            {
                ParseSettingsText(migratedSettings, legacySettingsText);
                hasLegacySettings = true;
            }
        }

        std::string imguiPayload;
        std::vector<fs::path> legacyImGuiPaths;
        legacyImGuiPaths.push_back(fs::current_path() / L"imgui.ini");
        legacyImGuiPaths.push_back(Platform::GetExecutableDirectory() / L"imgui.ini");

        for (const fs::path& legacyImGuiPath : legacyImGuiPaths)
        {
            if (legacyImGuiPath == path)
            {
                continue;
            }

            const std::string legacyImGuiText = ReadTextFile(legacyImGuiPath);
            if (!legacyImGuiText.empty())
            {
                imguiPayload = legacyImGuiText;
                break;
            }
        }

        if (!hasLegacySettings && imguiPayload.empty())
        {
            return {};
        }

        std::string combined = BuildAppSettingsSection(migratedSettings);
        if (!imguiPayload.empty())
        {
            combined += '\n';
            combined += imguiPayload;
            if (!combined.empty() && combined.back() != '\n')
            {
                combined.push_back('\n');
            }
        }

        return combined;
    }

    void LoadSettings(AppSettings& settings, const fs::path& path)
    {
        settings.exportRoot = Platform::GetDefaultExportRoot();

        const std::string text = LoadSharedSettingsText(path);
        if (text.empty())
        {
            return;
        }

        ParseSettingsText(settings, text);
    }

    std::string LoadImGuiSettings(const fs::path& path)
    {
        return ExtractNonAppSettingsPayload(LoadSharedSettingsText(path));
    }

    bool SaveSharedSettings(const AppSettings& settings, const fs::path& path, std::string_view imguiIniData, std::string& error)
    {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        if (ec)
        {
            error = "Could not create settings folder: " + ec.message();
            return false;
        }

        std::string payload(imguiIniData);
        if (payload.empty())
        {
            payload = ExtractNonAppSettingsPayload(LoadSharedSettingsText(path));
        }

        std::string fileText = BuildAppSettingsSection(settings);
        if (!payload.empty())
        {
            fileText += '\n';
            fileText += payload;
            if (!fileText.empty() && fileText.back() != '\n')
            {
                fileText.push_back('\n');
            }
        }

        fs::path tempPath = path;
        tempPath += ".tmp";

        std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            error = "Could not open settings file for writing.";
            return false;
        }

        file.write(fileText.data(), static_cast<std::streamsize>(fileText.size()));
        if (!file)
        {
            error = "Could not finish writing settings file.";
            return false;
        }

        file.close();
        return ReplaceFileWithTemp(tempPath, path, error);
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
        ClearTexturePreview(app);

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

        bool isCorrupt = false;
        if (result.fileFound && result.records.empty())
        {
            isCorrupt = true;
        }
        else if (result.message.find("unsupported") != std::string::npos ||
                 result.message.find("malformed") != std::string::npos ||
                 result.message.find("truncated") != std::string::npos ||
                 result.message.find("missing") != std::string::npos)
        {
            isCorrupt = true;
        }

        if (isCorrupt)
        {
            const std::string errMsg = "Cache corruption detected: " + result.message + ". Auto-cleared index cache.";
            AddLog(app, LogLevel::Error, errMsg);
            std::error_code ec;
            fs::remove(app.indexCachePath, ec);
            ClearResourceIndex(app, "Not built");
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
        RequestBlkResourceLinks(app, loadedRecords, false);
        TryLoadDescriptorResourceLinkCache(app, loadedRecords);
    }

    void SetGameRoot(AppState& app, const fs::path& gameRoot)
    {
        StopScan(app);
        if (app.settings.gameRoot != gameRoot)
        {
            std::error_code ec;
            if (!app.indexCachePath.empty() && fs::exists(app.indexCachePath))
            {
                fs::remove(app.indexCachePath, ec);
            }
            if (!app.descriptorLinksCachePath.empty() && fs::exists(app.descriptorLinksCachePath))
            {
                fs::remove(app.descriptorLinksCachePath, ec);
            }
            AddLog(app, LogLevel::Info, "Game folder changed. Disk caches invalidated and cleared.");
        }
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
        SaveSettingsWithLog(app, {});
    }

    void SaveSettingsWithLog(AppState& app, std::string_view imguiIniData)
    {
        std::string error;
        UpdateSettingsFromUiState(app);
        if (SaveSharedSettings(app.settings, app.settingsPath, imguiIniData, error))
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

                    RequestBlkResourceLinks(app, discoveredRecords);
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

    Export::TexturePreviewOptions GetTexturePreviewOptions(const AppState& app)
    {
        Export::TexturePreviewOptions options;
        options.gameRoot = app.settings.gameRoot;
        options.oodleDllPath = DagorIO::Decompressor::FindBundledOodleDll();
        options.useOodleFromGameFolder = false;
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

    std::string NormalizePathKey(const fs::path& path)
    {
        return ToLowerLocal(Platform::PathToUtf8(path.lexically_normal()));
    }

    bool SamePathKey(const fs::path& left, const fs::path& right)
    {
        return NormalizePathKey(left) == NormalizePathKey(right);
    }

    std::string NormalizeAssetNameForMatch(std::string_view value)
    {
        std::string normalized = ToLowerLocal(value);
        const size_t slash = normalized.find_last_of("/\\");
        if (slash != std::string::npos)
        {
            normalized.erase(0, slash + 1);
        }

        const size_t dot = normalized.find_last_of('.');
        if (dot != std::string::npos)
        {
            normalized.erase(dot);
        }

        for (char& c : normalized)
        {
            if (!std::isalnum(static_cast<unsigned char>(c)))
            {
                c = '_';
            }
        }
        while (normalized.find("__") != std::string::npos)
        {
            const size_t pos = normalized.find("__");
            normalized.replace(pos, 2, "_");
        }

        constexpr std::string_view suffixes[] = {
            "_dynmodel",
            "_dynamic",
            "_model",
            "_skeleton",
            "_anim_tree",
            "_anim",
            "_a2d"
        };
        for (std::string_view suffix : suffixes)
        {
            if (normalized.size() > suffix.size() &&
                normalized.compare(normalized.size() - suffix.size(), suffix.size(), suffix) == 0)
            {
                normalized.erase(normalized.size() - suffix.size());
                break;
            }
        }
        return normalized;
    }

    std::vector<std::string> SplitNameTokens(const std::string& normalized)
    {
        std::vector<std::string> tokens;
        size_t start = 0;
        while (start < normalized.size())
        {
            const size_t end = normalized.find('_', start);
            const size_t length = (end == std::string::npos ? normalized.size() : end) - start;
            if (length >= 3)
            {
                tokens.push_back(normalized.substr(start, length));
            }
            if (end == std::string::npos)
            {
                break;
            }
            start = end + 1;
        }
        return tokens;
    }

    int NameAffinityScore(std::string_view modelName, std::string_view candidateName)
    {
        const std::string model = NormalizeAssetNameForMatch(modelName);
        const std::string candidate = NormalizeAssetNameForMatch(candidateName);
        if (model.empty() || candidate.empty())
        {
            return 0;
        }
        if (model == candidate)
        {
            return 100;
        }
        if (candidate.find(model) != std::string::npos || model.find(candidate) != std::string::npos)
        {
            return 80;
        }

        size_t commonPrefix = 0;
        const size_t commonLimit = std::min(model.size(), candidate.size());
        while (commonPrefix < commonLimit && model[commonPrefix] == candidate[commonPrefix])
        {
            ++commonPrefix;
        }
        int score = commonPrefix >= 5 ? static_cast<int>(40 + std::min<size_t>(commonPrefix, 20)) : 0;

        const std::vector<std::string> modelTokens = SplitNameTokens(model);
        const std::vector<std::string> candidateTokens = SplitNameTokens(candidate);
        size_t sharedTokens = 0;
        for (const std::string& modelToken : modelTokens)
        {
            if (std::find(candidateTokens.begin(), candidateTokens.end(), modelToken) != candidateTokens.end())
            {
                ++sharedTokens;
            }
        }
        if (sharedTokens > 0)
        {
            score = std::max(score, static_cast<int>(25 + (sharedTokens * 12)));
        }
        return score;
    }

    std::optional<std::vector<uint8_t>> ReadSubAssetPayload(
        const fs::path& packPath,
        const AssetIndex::SubAssetRecord& entry,
        std::string& error)
    {
        std::ifstream file(packPath, std::ios::binary);
        if (!file)
        {
            error = "could not open pack " + Platform::PathToUtf8(packPath);
            return std::nullopt;
        }

        file.seekg(static_cast<std::streamoff>(entry.offset), std::ios::beg);
        std::vector<uint8_t> payload(entry.size);
        if (!file.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size())))
        {
            error = "could not read payload for " + entry.name;
            return std::nullopt;
        }
        return payload;
    }

    struct AnimationCandidate
    {
        fs::path packPath;
        AssetIndex::SubAssetRecord entry;
        int score = 0;
        std::string source;
    };

    struct DynModelAnimationTargets
    {
        std::unordered_set<std::string> skeletonNodeNames;
        std::unordered_set<std::string> modelNodeNames;
    };

    struct AnimationCompatibilityStats
    {
        size_t matchedSkeletonNodes = 0;
        size_t matchedModelNodes = 0;
        size_t unmatchedNodes = 0;
        size_t animatedNodeCount = 0;
        size_t animatedClipCount = 0;
        size_t animatedTrackCount = 0;
    };

    struct DecodedAnimationCandidate
    {
        AnimationCandidate candidate;
        DagorFormats::Anim2DataInfo animInfo;
        AnimationCompatibilityStats compatibility;
        int finalScore = 0;
    };

    std::string NormalizeAnimNodeName(std::string_view value)
    {
        return ToLowerLocal(Trim(std::string(value)));
    }

    void AddAnimationTargetName(std::unordered_set<std::string>& names, std::string_view value)
    {
        const std::string normalized = NormalizeAnimNodeName(value);
        if (!normalized.empty())
        {
            names.insert(normalized);
        }
    }

    DynModelAnimationTargets BuildDynModelAnimationTargets(
        const std::vector<DagorFormats::DynModelMesh>& lods,
        const std::vector<DagorFormats::GeomNodeTreeNode>& skeleton)
    {
        DynModelAnimationTargets targets;
        targets.skeletonNodeNames.reserve(skeleton.size());
        for (const DagorFormats::GeomNodeTreeNode& node : skeleton)
        {
            AddAnimationTargetName(targets.skeletonNodeNames, node.name);
        }

        for (const DagorFormats::DynModelMesh& mesh : lods)
        {
            AddAnimationTargetName(targets.modelNodeNames, mesh.name);
            AddAnimationTargetName(targets.modelNodeNames, mesh.parentNodeName);
            for (const std::string& jointName : mesh.skinJointNodeNames)
            {
                AddAnimationTargetName(targets.modelNodeNames, jointName);
            }
        }
        return targets;
    }

    AnimationCompatibilityStats EvaluateAnimationCompatibility(
        const DagorFormats::Anim2DataInfo& animInfo,
        const DynModelAnimationTargets& targets)
    {
        AnimationCompatibilityStats stats;
        std::unordered_set<std::string> seenNodeNames;
        seenNodeNames.reserve(animInfo.nodeNames.size());

        for (const DagorFormats::Anim2DataClipInfo& clip : animInfo.clips)
        {
            if (clip.nodes.empty())
            {
                continue;
            }

            ++stats.animatedClipCount;
            for (const DagorFormats::Anim2DataNodeAnimation& nodeAnim : clip.nodes)
            {
                if (nodeAnim.nodeName.empty() || nodeAnim.times.empty())
                {
                    continue;
                }

                ++stats.animatedTrackCount;
                const std::string normalized = NormalizeAnimNodeName(nodeAnim.nodeName);
                if (normalized.empty() || !seenNodeNames.insert(normalized).second)
                {
                    continue;
                }

                ++stats.animatedNodeCount;
                if (targets.skeletonNodeNames.contains(normalized))
                {
                    ++stats.matchedSkeletonNodes;
                }
                else if (targets.modelNodeNames.contains(normalized))
                {
                    ++stats.matchedModelNodes;
                }
                else
                {
                    ++stats.unmatchedNodes;
                }
            }
        }

        return stats;
    }

    int ScoreAnimationCompatibility(const AnimationCompatibilityStats& stats)
    {
        const size_t matchedNodes = stats.matchedSkeletonNodes + stats.matchedModelNodes;
        int score = static_cast<int>(stats.matchedSkeletonNodes * 45);
        score += static_cast<int>(stats.matchedModelNodes * 12);
        score += static_cast<int>(std::min<size_t>(stats.animatedTrackCount, 64));
        score += static_cast<int>(stats.animatedClipCount * 3);
        score -= static_cast<int>(stats.unmatchedNodes * 14);

        if (stats.animatedNodeCount > 0)
        {
            score += static_cast<int>((matchedNodes * 100) / stats.animatedNodeCount);
        }
        if (stats.matchedSkeletonNodes == 0)
        {
            score -= 40;
        }
        if (matchedNodes == 0)
        {
            score -= 120;
        }

        return score;
    }

    void AddAnimationCandidate(
        std::vector<AnimationCandidate>& candidates,
        const fs::path& packPath,
        const AssetIndex::SubAssetRecord& entry,
        int score,
        std::string source)
    {
        constexpr uint32_t kAnim2DataClassId = 0x40C586F9u;
        if (entry.grpClassId != kAnim2DataClassId)
        {
            return;
        }

        const std::string pathKey = NormalizePathKey(packPath);
        for (AnimationCandidate& candidate : candidates)
        {
            if (NormalizePathKey(candidate.packPath) == pathKey && candidate.entry.name == entry.name)
            {
                if (score > candidate.score)
                {
                    candidate.score = score;
                    candidate.source = std::move(source);
                }
                return;
            }
        }

        AnimationCandidate candidate;
        candidate.packPath = packPath;
        candidate.entry = entry;
        candidate.score = score;
        candidate.source = std::move(source);
        candidates.push_back(std::move(candidate));
    }

    void AddAnimationCandidateByName(
        AppState& app,
        std::vector<AnimationCandidate>& candidates,
        const fs::path& packPath,
        const std::string& entryName,
        int score,
        std::string source)
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
        if (!contents)
        {
            return;
        }

        const auto it = std::find_if(contents->entries.begin(), contents->entries.end(), [&](const auto& candidate) {
            return candidate.name == entryName;
        });
        if (it != contents->entries.end())
        {
            AddAnimationCandidate(candidates, packPath, *it, score, std::move(source));
        }
    }

    std::vector<Export::AnimationClip> ResolveAnimationClipsForDynModel(
        AppState& app,
        const fs::path& packPath,
        const AssetIndex::SubAssetRecord& modelEntry,
        const std::vector<DagorFormats::DynModelMesh>& lods,
        const std::vector<DagorFormats::GeomNodeTreeNode>& skeleton,
        const AssetIndex::DescriptorResourceLink* matchedLink,
        const std::shared_ptr<const std::vector<AssetIndex::DescriptorResourceLink>>& descriptorLinks,
        const std::shared_ptr<const std::vector<AssetIndex::ResourceRecord>>& allRecords,
        Export::ExportManifest& manifest)
    {
        constexpr uint32_t kAnim2DataClassId = 0x40C586F9u;
        std::vector<AnimationCandidate> candidates;
        const DynModelAnimationTargets animationTargets = BuildDynModelAnimationTargets(lods, skeleton);

        if (matchedLink && descriptorLinks)
        {
            for (const AssetIndex::DescriptorResourceLink& link : *descriptorLinks)
            {
                if (link.matchedGrpClassId != kAnim2DataClassId ||
                    !link.matchedGrpPath ||
                    !link.matchedGrpEntryName ||
                    !SamePathKey(link.descriptorPath, matchedLink->descriptorPath))
                {
                    continue;
                }

                int score = 70 + (NameAffinityScore(modelEntry.name, link.resourceName) / 3);
                if (SamePathKey(*link.matchedGrpPath, packPath))
                {
                    score += 15;
                }
                AddAnimationCandidateByName(
                    app,
                    candidates,
                    *link.matchedGrpPath,
                    *link.matchedGrpEntryName,
                    score,
                    "descriptor link " + Platform::PathToUtf8(link.descriptorPath.filename()));
            }
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
            for (const AssetIndex::SubAssetRecord& candidate : samePackContents->entries)
            {
                if (candidate.grpClassId != kAnim2DataClassId)
                {
                    continue;
                }

                const int nameScore = NameAffinityScore(modelEntry.name, candidate.name);
                AddAnimationCandidate(
                    candidates,
                    packPath,
                    candidate,
                    35 + nameScore,
                    "same GRP pack");
            }
        }

        if (allRecords)
        {
            const std::string modelName = NormalizeAssetNameForMatch(modelEntry.name);
            for (const AssetIndex::ResourceRecord& record : *allRecords)
            {
                if (record.type != AssetIndex::AssetType::Grp || SamePathKey(record.path, packPath))
                {
                    continue;
                }

                const std::string packStem = NormalizeAssetNameForMatch(record.path.stem().string());
                int packScore = NameAffinityScore(modelName, packStem);
                if (packScore < 35 && (!matchedLink || !SamePathKey(record.path.parent_path(), matchedLink->descriptorPath.parent_path())))
                {
                    continue;
                }

                auto contents = app.packContentsCache.Get(record.path);
                if (!contents)
                {
                    auto newContents = AssetIndex::ParseGrpContents(record.path);
                    if (newContents)
                    {
                        app.packContentsCache.Put(record.path, newContents);
                        contents = newContents;
                    }
                }
                if (!contents)
                {
                    continue;
                }

                for (const AssetIndex::SubAssetRecord& candidate : contents->entries)
                {
                    if (candidate.grpClassId != kAnim2DataClassId)
                    {
                        continue;
                    }

                    const int entryScore = NameAffinityScore(modelEntry.name, candidate.name);
                    const int score = packScore + entryScore;
                    if (score >= 45)
                    {
                        AddAnimationCandidate(
                            candidates,
                            record.path,
                            candidate,
                            score,
                            "name-affinity GRP search");
                    }
                }
            }
        }

        std::sort(candidates.begin(), candidates.end(), [](const AnimationCandidate& left, const AnimationCandidate& right) {
            if (left.score != right.score)
            {
                return left.score > right.score;
            }
            return left.entry.name < right.entry.name;
        });

        if (candidates.size() > 24)
        {
            manifest.decodeWarnings.push_back(
                "Animation resolver found " + std::to_string(candidates.size()) +
                " Anim2Data candidates; compatibility-scoring the 24 highest-scored name matches.");
            candidates.resize(24);
        }

        AddLog(app,
            LogLevel::Info,
            "Anim2Data resolver for " + modelEntry.name +
            ": candidates=" + std::to_string(candidates.size()));

        std::vector<DecodedAnimationCandidate> decodedCandidates;
        std::vector<Export::AnimationClip> resolvedClips;
        for (const AnimationCandidate& candidate : candidates)
        {
            AddLog(app,
                LogLevel::Info,
                "Anim2Data candidate for " + modelEntry.name +
                ": " + candidate.entry.name +
                " from " + Platform::PathToUtf8(candidate.packPath.filename()) +
                " score=" + std::to_string(candidate.score) +
                " source=" + candidate.source);

            std::string readError;
            std::optional<std::vector<uint8_t>> payload = ReadSubAssetPayload(candidate.packPath, candidate.entry, readError);
            if (!payload)
            {
                const std::string warning = "Could not read Anim2Data candidate " + candidate.entry.name + ": " + readError;
                manifest.decodeWarnings.push_back(warning);
                AddLog(app, LogLevel::Warning, warning);
                continue;
            }

            try
            {
                DagorFormats::Anim2DataReader animReader(*payload);
                animReader.Parse();
                const DagorFormats::Anim2DataInfo& animInfo = animReader.GetInfo();
                const AnimationCompatibilityStats compatibility =
                    EvaluateAnimationCompatibility(animInfo, animationTargets);
                const int compatibilityScore = ScoreAnimationCompatibility(compatibility);

                DecodedAnimationCandidate decoded;
                decoded.candidate = candidate;
                decoded.animInfo = animInfo;
                decoded.compatibility = compatibility;
                decoded.finalScore = candidate.score + compatibilityScore;
                decodedCandidates.push_back(std::move(decoded));

                AddLog(app,
                    LogLevel::Info,
                    "Decoded Anim2Data " + candidate.entry.name +
                    ": finalScore=" + std::to_string(candidate.score + compatibilityScore) +
                    ", matchedSkeletonNodes=" + std::to_string(compatibility.matchedSkeletonNodes) +
                    ", matchedModelNodes=" + std::to_string(compatibility.matchedModelNodes) +
                    ", unmatchedNodes=" + std::to_string(compatibility.unmatchedNodes) +
                    ", animatedNodes=" + std::to_string(compatibility.animatedNodeCount) +
                    ": clips=" + std::to_string(animInfo.clips.size()) +
                    ", exportNodeTracks=" + std::to_string(compatibility.animatedTrackCount) +
                    ", channels=" + std::to_string(animInfo.channels.size()) +
                    ", nodes=" + std::to_string(animInfo.nodeNames.size()) +
                    ", labels=" + std::to_string(animInfo.labels.size()));
                for (const std::string& warning : animInfo.decodeWarnings)
                {
                    AddLog(app, LogLevel::Warning, "Anim2Data warning for " + candidate.entry.name + ": " + warning);
                }
            }
            catch (const std::exception& exception)
            {
                const std::string warning =
                    "Could not decode Anim2Data candidate " + candidate.entry.name + ": " + exception.what();
                manifest.decodeWarnings.push_back(warning);
                AddLog(app, LogLevel::Warning, warning);
            }
        }

        std::sort(decodedCandidates.begin(), decodedCandidates.end(), [](const DecodedAnimationCandidate& left, const DecodedAnimationCandidate& right) {
            if (left.finalScore != right.finalScore)
            {
                return left.finalScore > right.finalScore;
            }
            if (left.compatibility.matchedSkeletonNodes != right.compatibility.matchedSkeletonNodes)
            {
                return left.compatibility.matchedSkeletonNodes > right.compatibility.matchedSkeletonNodes;
            }
            return left.candidate.entry.name < right.candidate.entry.name;
        });

        std::vector<const DecodedAnimationCandidate*> selectedCandidates;
        if (!decodedCandidates.empty())
        {
            const int bestScore = decodedCandidates.front().finalScore;
            for (const DecodedAnimationCandidate& decoded : decodedCandidates)
            {
                const size_t matchedNodes =
                    decoded.compatibility.matchedSkeletonNodes + decoded.compatibility.matchedModelNodes;
                if (matchedNodes == 0)
                {
                    continue;
                }
                if (decoded.finalScore + 35 < bestScore)
                {
                    continue;
                }

                selectedCandidates.push_back(&decoded);
                if (selectedCandidates.size() >= 3)
                {
                    break;
                }
            }

            if (selectedCandidates.empty() &&
                decodedCandidates.front().compatibility.matchedSkeletonNodes > 0)
            {
                selectedCandidates.push_back(&decodedCandidates.front());
            }
        }

        if (!decodedCandidates.empty())
        {
            manifest.decodeWarnings.push_back(
                "Animation resolver compatibility-ranked " + std::to_string(decodedCandidates.size()) +
                " decoded Anim2Data candidates and kept " + std::to_string(selectedCandidates.size()) + ".");
        }

        for (const DecodedAnimationCandidate* decoded : selectedCandidates)
        {
            manifest.decodeWarnings.push_back(
                "Resolved animation " + decoded->candidate.entry.name +
                " from " + Platform::PathToUtf8(decoded->candidate.packPath.filename()) +
                " (score=" + std::to_string(decoded->finalScore) +
                ", matchedSkeletonNodes=" + std::to_string(decoded->compatibility.matchedSkeletonNodes) +
                ", matchedModelNodes=" + std::to_string(decoded->compatibility.matchedModelNodes) + ").");

            for (const DagorFormats::Anim2DataClipInfo& animClip : decoded->animInfo.clips)
            {
                if (animClip.nodes.empty())
                {
                    continue;
                }

                Export::AnimationClip exportClip;
                exportClip.name = decoded->candidate.entry.name;
                exportClip.additive = decoded->animInfo.additive;
                if (!animClip.name.empty() && decoded->animInfo.clips.size() > 1)
                {
                    exportClip.name += "_" + animClip.name;
                }
                exportClip.diagnostics = animClip.warnings;
                exportClip.diagnostics.insert(
                    exportClip.diagnostics.end(),
                    decoded->animInfo.decodeWarnings.begin(),
                    decoded->animInfo.decodeWarnings.end());
                exportClip.nodeTracks.reserve(animClip.nodes.size());

                for (const DagorFormats::Anim2DataNodeAnimation& nodeAnim : animClip.nodes)
                {
                    Export::AnimationNodeTrack track;
                    track.nodeName = nodeAnim.nodeName;
                    track.times = nodeAnim.times;
                    track.translations = nodeAnim.translations;
                    track.rotations = nodeAnim.rotations;
                    track.scales = nodeAnim.scales;
                    if (!track.nodeName.empty() && !track.times.empty())
                    {
                        exportClip.nodeTracks.push_back(std::move(track));
                    }
                }

                if (!exportClip.nodeTracks.empty())
                {
                    resolvedClips.push_back(std::move(exportClip));
                }
            }
        }

        if (resolvedClips.empty())
        {
            manifest.decodeWarnings.push_back("Animation export requested, but no exportable Anim2Data curves were resolved.");
            AddLog(app,
                LogLevel::Warning,
                "Animation export requested for " + modelEntry.name +
                ", but no exportable Anim2Data curves were resolved.");
        }

        return resolvedClips;
    }

    void RequestTexturePreview(
        AppState& app,
        const std::filesystem::path& packPath,
        const AssetIndex::SubAssetRecord& entry)
    {
        constexpr uint64_t MaxDecodedPreviewBytes = 256ull * 1024ull * 1024ull;

        uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(app.texturePreview.mutex);
            generation = ++app.texturePreview.generation;
            app.texturePreview.packPath = packPath;
            app.texturePreview.entryName = entry.name;
            app.texturePreview.entryOffset = entry.offset;
            app.texturePreview.sourceWidth = entry.texWidth;
            app.texturePreview.sourceHeight = entry.texHeight;
            app.texturePreview.mipCount = entry.texMips;
            app.texturePreview.format = entry.texFormat;
            app.texturePreview.compression = entry.texCompression;
            app.texturePreview.loading = true;
            app.texturePreview.error.clear();
            app.texturePreview.warnings.clear();
            app.texturePreview.pendingImage.reset();
        }

        const uint64_t decodedBytes =
            static_cast<uint64_t>(entry.texWidth) *
            static_cast<uint64_t>(entry.texHeight) * 4ull;
        if (decodedBytes == 0 || decodedBytes > MaxDecodedPreviewBytes)
        {
            std::lock_guard<std::mutex> lock(app.texturePreview.mutex);
            if (app.texturePreview.generation == generation)
            {
                app.texturePreview.loading = false;
                app.texturePreview.error = decodedBytes == 0
                    ? "Texture dimensions are unavailable; reload the DXP contents."
                    : "Texture exceeds the 256 MB decoded preview limit.";
            }
            return;
        }

        const Export::TexturePreviewOptions options = GetTexturePreviewOptions(app);
        app.backgroundJobs.Start("PreviewTexture_" + entry.name, [&app, packPath, entry, options, generation]() {
            Export::TexturePreviewResult result =
                Export::TextureExporter::DecodePreview(packPath, entry, options);

            std::lock_guard<std::mutex> lock(app.texturePreview.mutex);
            if (app.texturePreview.generation != generation)
            {
                return;
            }

            app.texturePreview.loading = false;
            app.texturePreview.error = std::move(result.error);
            app.texturePreview.warnings = std::move(result.warnings);
            app.texturePreview.pendingImage.reset();

            if (!result.success)
            {
                return;
            }

            auto image = std::make_shared<TexturePreviewImage>();
            image->generation = generation;
            image->packPath = packPath;
            image->entryName = entry.name;
            image->entryOffset = entry.offset;
            image->width = result.width;
            image->height = result.height;
            image->stride = result.stride;
            image->srgb = result.srgb;
            image->bgraPixels = std::move(result.bgraPixels);
            app.texturePreview.pendingImage = std::move(image);
        });
    }

    void ClearTexturePreview(AppState& app)
    {
        std::lock_guard<std::mutex> lock(app.texturePreview.mutex);
        ++app.texturePreview.generation;
        app.texturePreview.packPath.clear();
        app.texturePreview.entryName.clear();
        app.texturePreview.entryOffset = 0;
        app.texturePreview.sourceWidth = 0;
        app.texturePreview.sourceHeight = 0;
        app.texturePreview.mipCount = 0;
        app.texturePreview.format.clear();
        app.texturePreview.compression.clear();
        app.texturePreview.loading = false;
        app.texturePreview.error.clear();
        app.texturePreview.warnings.clear();
        app.texturePreview.pendingImage.reset();
    }

    TexturePreviewSnapshot SnapshotTexturePreview(AppState& app)
    {
        std::lock_guard<std::mutex> lock(app.texturePreview.mutex);
        TexturePreviewSnapshot snapshot;
        snapshot.generation = app.texturePreview.generation;
        snapshot.packPath = app.texturePreview.packPath;
        snapshot.entryName = app.texturePreview.entryName;
        snapshot.entryOffset = app.texturePreview.entryOffset;
        snapshot.sourceWidth = app.texturePreview.sourceWidth;
        snapshot.sourceHeight = app.texturePreview.sourceHeight;
        snapshot.mipCount = app.texturePreview.mipCount;
        snapshot.format = app.texturePreview.format;
        snapshot.compression = app.texturePreview.compression;
        snapshot.loading = app.texturePreview.loading;
        snapshot.error = app.texturePreview.error;
        snapshot.warnings = app.texturePreview.warnings;
        return snapshot;
    }

    std::shared_ptr<TexturePreviewImage> TakePendingTexturePreview(AppState& app)
    {
        std::lock_guard<std::mutex> lock(app.texturePreview.mutex);
        std::shared_ptr<TexturePreviewImage> image = std::move(app.texturePreview.pendingImage);
        app.texturePreview.pendingImage.reset();
        return image;
    }

    void SetTexturePreviewUploadError(AppState& app, uint64_t generation, std::string error)
    {
        std::lock_guard<std::mutex> lock(app.texturePreview.mutex);
        if (app.texturePreview.generation == generation)
        {
            app.texturePreview.error = std::move(error);
        }
    }

    void RequestTextureExport(AppState& app, const std::filesystem::path& packPath, const AssetIndex::SubAssetRecord& entry)
    {
        AddLog(app, LogLevel::Info, "Exporting texture: " + entry.name + "...");
        Export::TextureExportOptions options = GetTextureExportOptions(app);

        BeginExportProgress(app, "Exporting " + entry.name, "Exporting texture: " + entry.name, 1);
        SetExportProgressOutputDir(app, Platform::PathToUtf8(options.outputDir));

        app.backgroundJobs.Start("ExportTexture_" + entry.name, [&app, packPath, entry, options]() {
            UpdateExportProgress(app, 0, 1, "Exporting texture...", entry.name);
            AddExportProgressLog(app, "Starting export for texture: " + entry.name);
            auto result = Export::TextureExporter::ExportTexture(packPath, entry, options);
            if (result.success)
            {
                AddLog(app, LogLevel::Info, "Exported texture: " + entry.name + " to " + Platform::PathToUtf8(result.outputPath));
                AddExportProgressLog(app, "Export successful: " + Platform::PathToUtf8(result.outputPath.filename()));
                UpdateExportProgress(app, 1, 1, "Export complete", entry.name);
            }
            else
            {
                AddLog(app, LogLevel::Error, "Failed to export texture: " + entry.name + ". Error: " + result.error);
                AddExportProgressLog(app, "Export failed: " + result.error);
                UpdateExportProgress(app, 0, 1, "Export failed: " + result.error, entry.name);
            }
            EndExportProgress(app);
        });
    }

    void RequestModelExportInternal(
        AppState& app,
        const std::filesystem::path& packPath,
        const AssetIndex::SubAssetRecord& entry,
        std::optional<bool> dynModelIncludeTexturesOverride,
        std::optional<bool> dynModelIncludeSkeletonRigOverride,
        std::optional<bool> dynModelIncludeAnimationsOverride)
    {
        if (entry.grpClassId != 0 && 
            !AssetIndex::CanExportGrpEntryAsStaticModel(entry) && 
            !AssetIndex::CanExportGrpEntryAsSkinnedModel(entry))
        {
            const std::string reason = AssetIndex::GetUnsupportedExportTooltip(entry);
            AddLog(app, LogLevel::Warning,
                "Skipped export for " + entry.name + ": " + reason);
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
        std::string format = "GLB";
        bool globalExportTextures = app.settings.exportTexturesWithModels;
        bool exportJsonManifest = app.settings.exportJsonManifest;
        
        auto descriptorLinks = SnapshotDescriptorResourceLinks(app);
        auto allRecords = SnapshotRecords(app.scan);
        auto baseTextureExportOptions = GetTextureExportOptions(app);
        baseTextureExportOptions.outputDir = exportRoot / "Textures";
        const bool exportGlb = true;
        Export::GlbExportOptions glbOptions;
        glbOptions.embedTexturesInGlb = app.settings.embedTexturesInGlb;
        glbOptions.diagnosticLog = [&app, assetName = entry.name](const std::string& message)
        {
            AddLog(app, LogLevel::Info, "DynModel GLB diagnostic for " + assetName + ": " + message);
        };

        // Calculate total steps for progress
        const AssetIndex::DescriptorResourceLink* matchedLinkOnMain = nullptr;
        if (descriptorLinks)
        {
            for (const auto& link : *descriptorLinks)
            {
                bool nameMatch = (link.matchedGrpEntryName && *link.matchedGrpEntryName == entry.name) || (link.resourceName == entry.name);
                bool packMatch = !link.matchedGrpPath || (*link.matchedGrpPath == packPath);
                if (nameMatch && packMatch)
                {
                    matchedLinkOnMain = &link;
                    break;
                }
            }
        }

        constexpr uint32_t kDynModelClassIdOnMain = 0xB4B7D9C4u;
        const bool isDynModelOnMain = (entry.grpClassId == kDynModelClassIdOnMain) ||
                                      (entry.grpClassId == 0 && entry.size > 8);
        const bool exportTexturesOnMain = isDynModelOnMain
            ? (dynModelIncludeTexturesOverride.value_or(globalExportTextures))
            : globalExportTextures;

        uint64_t initialTotalSteps = 1; // 1 step for model itself
        if (matchedLinkOnMain && exportTexturesOnMain)
        {
            initialTotalSteps += matchedLinkOnMain->textureLinks.size();
        }

        BeginExportProgress(app, "Exporting " + entry.name, "Preparing export...", initialTotalSteps);
        SetExportProgressOutputDir(app, Platform::PathToUtf8(exportRoot));
        AddExportProgressLog(app, "Starting model export for: " + entry.name);

        app.backgroundJobs.Start("ExportModel_" + entry.name, [&app, packPath, entry, exportRoot, format, globalExportTextures, exportJsonManifest, descriptorLinks, allRecords, baseTextureExportOptions, exportGlb, glbOptions, dynModelIncludeTexturesOverride, dynModelIncludeSkeletonRigOverride, dynModelIncludeAnimationsOverride, initialTotalSteps]() {
            struct ExportProgressGuard
            {
                AppState& guardApp;
                ~ExportProgressGuard() { EndExportProgress(guardApp); }
            };
            ExportProgressGuard progressGuard{app};
            uint64_t currentStep = 0;

            try
            {
                UpdateExportProgress(app, currentStep, initialTotalSteps, "Opening pack file...", entry.name);
                AddExportProgressLog(app, "Opening GRP pack file: " + Platform::PathToUtf8(packPath.filename()));
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

                std::string currentFormat = "GLB";
                const bool currentExportGlb = true;

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
                const bool includeSkeletonRig = isDynModel
                    ? dynModelIncludeSkeletonRigOverride.value_or(true)
                    : false;
                const bool includeAnimations = isDynModel
                    ? (dynModelIncludeAnimationsOverride.value_or(false) && includeSkeletonRig)
                    : false;
                auto textureExportOptions = baseTextureExportOptions;
                if (currentExportGlb && exportTextures)
                {
                    textureExportOptions.format = "PNG";
                    textureExportOptions.preserveOriginalDDS = false;
                    textureExportOptions.splitPackedNormalMap = true;
                    AddLog(app,
                        LogLevel::Info,
                        "GLB material texture export for " + entry.name +
                        " will use PNG only; DDS/TGA are not core glTF image formats and will not be linked into GLB materials.");
                }

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

                const bool shouldParseRendInst = isRendInst || (!isDynModel && HasRendInstMagic(payload));
                std::vector<std::string> internalTextureNames;
                std::vector<DagorFormats::InternalMaterial> internalMaterials;
                std::unique_ptr<DagorFormats::DynModelReader> dynReader;
                std::unique_ptr<DagorFormats::RendInstReader> riReader;
                bool descriptorMaterialsPreferred = false;

                if (isDynModel)
                {
                    dynReader = std::make_unique<DagorFormats::DynModelReader>(payload);
                    if (matchedLink)
                    {
                        dynReader->SetExternalTextureNames(matchedLink->textureNames);
                    }
                    dynReader->Parse();
                    internalTextureNames = dynReader->GetTextureNames();
                    internalMaterials = dynReader->GetInternalMaterials();
                    descriptorMaterialsPreferred = dynReader->UsesExternalMaterialTextureCounts();
                }
                else if (shouldParseRendInst)
                {
                    riReader = std::make_unique<DagorFormats::RendInstReader>(payload);
                    if (matchedLink)
                    {
                        riReader->SetExternalTextureNames(matchedLink->textureNames);
                    }
                    riReader->Parse();
                    internalTextureNames = riReader->GetTextureNames();
                    internalMaterials = riReader->GetInternalMaterials();
                    descriptorMaterialsPreferred = riReader->UsesExternalMaterialTextureCounts();
                }

                const std::vector<DagorFormats::InternalMaterial> descriptorMaterialSlots =
                    BuildDescriptorMaterialSlots(matchedLink);
                if (!descriptorMaterialSlots.empty())
                {
                    if (descriptorMaterialsPreferred || internalMaterials.empty())
                    {
                        internalMaterials = descriptorMaterialSlots;
                    }
                    else
                    {
                        MergeDescriptorMaterialSlots(internalMaterials, descriptorMaterialSlots);
                    }

                    AddExportProgressLog(
                        app,
                        "Resolved descriptor material slots: " +
                            std::to_string(descriptorMaterialSlots.size()) +
                            (descriptorMaterialsPreferred
                                ? " descriptor-backed materials"
                                : " supplemental materials"));
                }

                std::vector<std::string> uniqueTexturesToExport;
                std::unordered_map<std::string, std::string> exportedTexturePaths;
                std::unordered_map<std::string, fs::path> exportedTextureSourcePaths;

                if (!internalMaterials.empty() && exportTextures)
                {
                    std::unordered_set<std::string> seenTex;
                    for (const auto& mat : internalMaterials)
                    {
                        for (int i = 0; i < 16; ++i)
                        {
                            if (!mat.textureSlots[i].empty() && !seenTex.contains(mat.textureSlots[i]))
                            {
                                seenTex.insert(mat.textureSlots[i]);
                                uniqueTexturesToExport.push_back(mat.textureSlots[i]);
                            }
                        }
                    }
                }
                else if (matchedLink && exportTextures)
                {
                    for (const auto& texLink : matchedLink->textureLinks)
                    {
                        uniqueTexturesToExport.push_back(texLink.textureName);
                    }
                }

                uint64_t totalSteps = 1 + uniqueTexturesToExport.size();

                auto normalizeTextureName = [](std::string name) -> std::string {
                    for (char& c : name)
                    {
                        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
                        else if (c == '\\') c = '/';
                    }
                    size_t start = name.find_first_not_of(" \t\r\n");
                    if (start != std::string::npos)
                    {
                        size_t end = name.find_last_not_of(" \t\r\n");
                        name = name.substr(start, end - start + 1);
                    }
                    else
                    {
                        name.clear();
                    }
                    const size_t slash = name.find_last_of('/');
                    const size_t dot = name.find_last_of('.');
                    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
                    {
                        name.resize(dot);
                    }
                    const size_t star = name.find('*');
                    if (star != std::string::npos)
                    {
                        name.resize(star);
                    }
                    size_t dollar = name.find('$');
                    if (dollar != std::string::npos)
                    {
                        name.resize(dollar);
                    }
                    return name;
                };

                struct ResolvedTextureCandidate
                {
                    fs::path dxpPath;
                    std::string dxpEntryName;
                    std::string resolutionSource;
                };

                auto getTextureQualityRank = [](const fs::path& dxpPath, std::string_view entryName) -> int {
                    const std::string pathKey = ToLowerLocal(Platform::PathToUtf8(dxpPath));
                    const std::string entryKey = ToLowerLocal(entryName);
                    int rank = 100; // Standard/base quality.

                    if (ContainsAscii(pathKey, "/content.hq/"))
                    {
                        rank += 400;
                    }
                    if (ContainsAscii(pathKey, "/uhq_vehicles/"))
                    {
                        rank += 320;
                    }
                    if (ContainsAscii(pathKey, "/hq_tex/"))
                    {
                        rank += 280;
                    }
                    if (ContainsAscii(pathKey, "-hq.dxp.bin") || ContainsAscii(pathKey, "_hq.dxp.bin"))
                    {
                        rank += 220;
                    }
                    if (ContainsAscii(entryKey, "$hq"))
                    {
                        rank += 120;
                    }
                    if (ContainsAscii(entryKey, "$tq") || ContainsAscii(entryKey, "$lq"))
                    {
                        rank -= 260;
                    }

                    return rank;
                };

                std::unordered_map<std::string, std::vector<std::pair<fs::path, std::string>>> globalTextureDxpLookup;
                if (descriptorLinks)
                {
                    for (const auto& link : *descriptorLinks)
                    {
                        for (const auto& texLink : link.textureLinks)
                        {
                            if (texLink.matchedDxpPath && texLink.matchedDxpEntryName)
                            {
                                const std::string normalizedTextureName = normalizeTextureName(texLink.textureName);
                                if (!normalizedTextureName.empty())
                                {
                                    auto& list = globalTextureDxpLookup[normalizedTextureName];
                                    std::pair<fs::path, std::string> candidate = { *texLink.matchedDxpPath, *texLink.matchedDxpEntryName };
                                    if (std::find(list.begin(), list.end(), candidate) == list.end())
                                    {
                                        list.push_back(candidate);
                                    }
                                }
                            }
                        }
                    }
                }

                // Sort descriptor lookup candidates so HQ variants are checked first, and low-quality last
                for (auto& [_, list] : globalTextureDxpLookup)
                {
                    std::sort(list.begin(), list.end(), [&](const auto& a, const auto& b) {
                        int aRank = getTextureQualityRank(a.first, a.second);
                        int bRank = getTextureQualityRank(b.first, b.second);
                        if (aRank != bRank) {
                            return aRank > bRank;
                        }
                        return a.second < b.second;
                    });
                }

                std::unordered_map<std::string, std::vector<std::pair<fs::path, std::string>>> fastDxpTextureIndex;
                bool fastDxpTextureIndexBuilt = false;

                auto buildFastDxpTextureIndex = [&]() {
                    if (fastDxpTextureIndexBuilt) return;
                    fastDxpTextureIndexBuilt = true;

                    if (!allRecords) return;

                    for (const auto& record : *allRecords)
                    {
                        if (record.type == AssetIndex::AssetType::Dxp)
                        {
                            std::ifstream file(record.path, std::ios::binary);
                            if (!file) continue;

                            char magic[4];
                            if (!file.read(magic, 4) || std::string_view(magic, 4) != "DxP2")
                            {
                                continue;
                            }

                            file.seekg(0x8, std::ios::beg);
                            uint16_t totalFiles = 0;
                            if (!file.read(reinterpret_cast<char*>(&totalFiles), 2))
                            {
                                continue;
                            }

                            file.seekg(0x48, std::ios::beg);
                            std::string currentName;
                            for (uint16_t i = 0; i < totalFiles; ++i)
                            {
                                currentName.clear();
                                char c;
                                while (file.get(c))
                                {
                                    if (c == '\0')
                                    {
                                        break;
                                    }
                                    currentName.push_back(c);
                                }
                                if (file.eof() && currentName.empty())
                                {
                                    break;
                                }

                                std::string normName = normalizeTextureName(currentName);
                                if (!normName.empty())
                                {
                                    auto& list = fastDxpTextureIndex[normName];
                                    std::pair<fs::path, std::string> candidate = { record.path, currentName };
                                    if (std::find(list.begin(), list.end(), candidate) == list.end())
                                    {
                                        list.push_back(candidate);
                                    }
                                }
                            }
                        }
                    }

                    // Sort candidates so HQ variants are checked first, and low-quality last
                    for (auto& [_, list] : fastDxpTextureIndex)
                    {
                        std::sort(list.begin(), list.end(), [&](const auto& a, const auto& b) {
                            int aRank = getTextureQualityRank(a.first, a.second);
                            int bRank = getTextureQualityRank(b.first, b.second);
                            if (aRank != bRank) {
                                return aRank > bRank;
                            }
                            return a.second < b.second;
                        });
                    }
                };

                auto findTextureInDxpPacks =
                    [&](const std::string& texName) -> std::vector<ResolvedTextureCandidate> {
                    std::vector<ResolvedTextureCandidate> results;
                    const std::string normTexName = normalizeTextureName(texName);

                    auto it = globalTextureDxpLookup.find(normTexName);
                    if (it != globalTextureDxpLookup.end())
                    {
                        for (const auto& candidate : it->second)
                        {
                            results.push_back({ candidate.first, candidate.second, "descriptor-link-cache" });
                        }
                    }

                    buildFastDxpTextureIndex();
                    auto indexIt = fastDxpTextureIndex.find(normTexName);
                    if (indexIt != fastDxpTextureIndex.end())
                    {
                        // Cache fast-dxp-index findings into globalTextureDxpLookup
                        auto& list = globalTextureDxpLookup[normTexName];
                        for (const auto& candidate : indexIt->second)
                        {
                            if (std::find(list.begin(), list.end(), candidate) == list.end())
                            {
                                list.push_back(candidate);
                            }
                        }

                        for (const auto& candidate : indexIt->second)
                        {
                            bool duplicate = false;
                            for (const auto& existing : results)
                            {
                                if (existing.dxpPath == candidate.first && existing.dxpEntryName == candidate.second)
                                {
                                    duplicate = true;
                                    break;
                                }
                            }
                            if (!duplicate)
                            {
                                results.push_back({ candidate.first, candidate.second, "fast-dxp-index" });
                            }
                        }
                    }

                    // Sort results: quality rank descending first, then by resolutionSource preference (descriptor-link-cache over fast-dxp-index)
                    std::sort(results.begin(), results.end(), [&](const auto& a, const auto& b) {
                        int aRank = getTextureQualityRank(a.dxpPath, a.dxpEntryName);
                        int bRank = getTextureQualityRank(b.dxpPath, b.dxpEntryName);
                        if (aRank != bRank) {
                            return aRank > bRank;
                        }
                        if (a.resolutionSource != b.resolutionSource) {
                            return a.resolutionSource == "descriptor-link-cache";
                        }
                        return a.dxpEntryName < b.dxpEntryName;
                    });

                    return results;
                };

                std::unordered_map<std::string, std::vector<ResolvedTextureCandidate>> resolvedTextureExports;
                if (exportTextures && !uniqueTexturesToExport.empty())
                {
                    for (const auto& texName : uniqueTexturesToExport)
                    {
                        if (app.exportProgress.cancelRequested.load())
                        {
                            AddLog(app, LogLevel::Warning, "Export canceled by user.");
                            AddExportProgressLog(app, "Export canceled by user.");
                            return;
                        }

                        auto candidates = findTextureInDxpPacks(texName);
                        if (!candidates.empty())
                        {
                            resolvedTextureExports[texName] = candidates;
                            std::string candidateListStr;
                            for (size_t cIdx = 0; cIdx < candidates.size(); ++cIdx)
                            {
                                if (cIdx > 0) candidateListStr += ", ";
                                candidateListStr += Platform::PathToUtf8(candidates[cIdx].dxpPath.filename()) + "::" + candidates[cIdx].dxpEntryName;
                            }
                            AddExportProgressLog(
                                app,
                                "Resolved texture dependency candidates for " + texName + ": [" + candidateListStr + "]");
                        }
                        else
                        {
                            const std::string warnMsg = "Could not resolve texture dependency: " + texName + " in any DXP packs.";
                            AddLog(app, LogLevel::Warning, warnMsg);
                            AddExportProgressLog(app, "Warning: " + warnMsg);
                            manifest.decodeWarnings.push_back(warnMsg);
                        }
                    }

                    std::unordered_set<std::wstring> preloadedTexturePacks;
                    for (const auto& [_, candidates] : resolvedTextureExports)
                    {
                        for (const auto& candidate : candidates)
                        {
                            const std::wstring packKey = candidate.dxpPath.lexically_normal().wstring();
                            if (!preloadedTexturePacks.insert(packKey).second)
                            {
                                continue;
                            }

                            auto dxpContents = app.packContentsCache.Get(candidate.dxpPath);
                            if (!dxpContents)
                            {
                                auto newContents = AssetIndex::ParseDxpContents(candidate.dxpPath);
                                if (newContents)
                                {
                                    app.packContentsCache.Put(candidate.dxpPath, newContents);
                                }
                            }
                        }
                    }
                }

                std::mutex exportedTextureMutex;
                auto exportTextureByName = [&](const std::string& texName, std::string& warning) -> bool {
                    if (texName.empty())
                    {
                        warning = "Texture dependency name is empty.";
                        return false;
                    }

                    {
                        std::lock_guard<std::mutex> lock(exportedTextureMutex);
                        if (exportedTexturePaths.contains(texName))
                        {
                            return true;
                        }
                    }

                    auto resolvedIt = resolvedTextureExports.find(texName);
                    if (resolvedIt == resolvedTextureExports.end())
                    {
                        warning = "Failed to find or export texture dependency: " + texName;
                        return false;
                    }

                    const auto& candidates = resolvedIt->second;
                    std::vector<std::string> candidateErrors;

                    for (const auto& candidate : candidates)
                    {
                        const fs::path& dxpPath = candidate.dxpPath;
                        const std::string& dxpEntryName = candidate.dxpEntryName;
                        auto dxpContents = app.packContentsCache.Get(dxpPath);
                        if (!dxpContents)
                        {
                            auto newContents = AssetIndex::ParseDxpContents(dxpPath);
                            if (newContents)
                            {
                                app.packContentsCache.Put(dxpPath, newContents);
                                dxpContents = newContents;
                            }
                        }

                        if (!dxpContents || !dxpContents->error.empty())
                        {
                            candidateErrors.push_back("Could not parse DXP dependency pack " +
                                Platform::PathToUtf8(dxpPath.filename()) + " for texture " + texName +
                                ": " + (dxpContents ? dxpContents->error : "unknown error"));
                            continue;
                        }

                        auto it = std::find_if(dxpContents->entries.begin(), dxpContents->entries.end(), [&](const auto& e) {
                            return e.name == dxpEntryName;
                        });
                        if (it == dxpContents->entries.end())
                        {
                            candidateErrors.push_back("Resolved texture dependency entry " + dxpEntryName +
                                " was not found in " + Platform::PathToUtf8(dxpPath.filename()));
                            continue;
                        }

                        // Verify candidate has payload data. If size is 0, skip to the next candidate (e.g. fallback from HQ to standard).
                        if (it->size == 0)
                        {
                            candidateErrors.push_back("Candidate entry " + dxpEntryName + " in " +
                                Platform::PathToUtf8(dxpPath.filename()) + " has no payload data (size 0).");
                            continue;
                        }

                        AddExportProgressLog(
                            app,
                            "Preparing texture export: " + texName +
                                " from " + Platform::PathToUtf8(dxpPath.filename()) +
                                " :: " + dxpEntryName +
                                " [" + DescribeResolvedTexturePack(dxpPath) + ", source=" + candidate.resolutionSource + "]");

                        if (it->ddsxHeader.size() >= 32)
                        {
                            try
                            {
                                DagorFormats::DdsxReader headerReader(it->ddsxHeader);
                                headerReader.Parse();
                                AddExportProgressLog(app, BuildDdsxHeaderLogLine(headerReader.GetHeader()));
                            }
                            catch (const std::exception& ex)
                            {
                                AddExportProgressLog(
                                    app,
                                    "DDSx header parse failed for " + texName + ": " + ex.what());
                            }
                        }
                        else
                        {
                            AddExportProgressLog(
                                app,
                                "DDSx header missing or truncated for " + texName +
                                    " in " + Platform::PathToUtf8(dxpPath.filename()));
                        }

                        auto texResult = Export::TextureExporter::ExportTexture(dxpPath, *it, textureExportOptions);
                        if (!texResult.success)
                        {
                            candidateErrors.push_back("Failed to export texture entry " + dxpEntryName + " from " +
                                Platform::PathToUtf8(dxpPath.filename()) + ": " + texResult.error);
                            continue;
                        }

                        const std::string textureFile = MakeExportRelativeUri(exportRoot, texResult.outputPath);
                        std::lock_guard<std::mutex> lock(exportedTextureMutex);
                        if (!candidateErrors.empty())
                        {
                            std::string combined = "Some earlier candidates failed for " + texName + ":";
                            for (const auto& err : candidateErrors)
                            {
                                combined += "\n  - " + err;
                            }
                            AddLog(app, LogLevel::Warning, combined);
                        }
                        exportedTexturePaths[texName] = textureFile;
                        exportedTextureSourcePaths[texName] = texResult.outputPath;
                        manifest.textureDependencies.push_back(textureFile);
                        return true;
                    }

                    if (!candidateErrors.empty())
                    {
                        std::string combined = "Failed to export texture dependency " + texName + " (tried " +
                            std::to_string(candidates.size()) + " candidates):";
                        for (const auto& err : candidateErrors)
                        {
                            combined += "\n  - " + err;
                        }
                        warning = combined;
                    }
                    else
                    {
                        warning = "Failed to find or export texture dependency: " + texName;
                    }
                    return false;
                };

                std::vector<Export::MaterialInfo> materials;
                size_t numMaterials = 0;
                if (matchedLink)
                {
                    numMaterials = matchedLink->materialNames.size();
                }
                numMaterials = std::max(numMaterials, internalMaterials.size());

                materials.resize(numMaterials);
                for (size_t i = 0; i < numMaterials; ++i)
                {
                    if (matchedLink && i < matchedLink->materialNames.size() && !matchedLink->materialNames[i].empty())
                    {
                        materials[i].name = matchedLink->materialNames[i];
                    }
                    else
                    {
                        materials[i].name = "material_" + std::to_string(i);
                    }
                }

                size_t exportedTextureCount = 0;
                if (exportTextures && !uniqueTexturesToExport.empty())
                {
                    AddExportProgressLog(app, "Exporting " + std::to_string(uniqueTexturesToExport.size()) + " texture dependencies...");
                    std::atomic_size_t completedTextureExports{ 0 };
                    std::atomic_size_t exportedTextureCountAtomic{ 0 };
                    std::atomic_bool textureExportCanceled{ false };
                    const uint64_t textureProgressBase = currentStep;

                    Jobs::JobSystem::RunBoundedWorkers(
                        uniqueTexturesToExport.size(),
                        [&](size_t textureIndex, size_t) {
                            if (textureExportCanceled.load(std::memory_order_acquire) ||
                                app.exportProgress.cancelRequested.load())
                            {
                                textureExportCanceled.store(true, std::memory_order_release);
                                return;
                            }

                            const std::string& texName = uniqueTexturesToExport[textureIndex];
                            std::string warning;
                            const bool exported = exportTextureByName(texName, warning);
                            const uint64_t completed = completedTextureExports.fetch_add(1, std::memory_order_relaxed) + 1;

                            UpdateExportProgress(
                                app,
                                textureProgressBase + completed,
                                totalSteps,
                                exported ? "Exported texture dependency..." : "Texture dependency warning...",
                                texName);

                            if (exported)
                            {
                                exportedTextureCountAtomic.fetch_add(1, std::memory_order_relaxed);
                                AddExportProgressLog(app, "Successfully exported texture dependency: " + texName);
                            }
                            else
                            {
                                if (warning.empty())
                                {
                                    warning = "Failed to find or export texture dependency: " + texName;
                                }

                                {
                                    std::lock_guard<std::mutex> lock(exportedTextureMutex);
                                    manifest.decodeWarnings.push_back(warning);
                                }

                                AddLog(app, LogLevel::Warning, warning);
                                AddExportProgressLog(app, "Warning: " + warning);
                            }
                        },
                        1);

                    if (textureExportCanceled.load(std::memory_order_acquire))
                    {
                        AddLog(app, LogLevel::Warning, "Export canceled by user.");
                        AddExportProgressLog(app, "Export canceled by user.");
                        return;
                    }

                    exportedTextureCount = exportedTextureCountAtomic.load(std::memory_order_relaxed);
                    currentStep = textureProgressBase + uniqueTexturesToExport.size();
                }

                if (!internalMaterials.empty())
                {
                    size_t assignedTextureCount = 0;
                    for (size_t matIdx = 0; matIdx < internalMaterials.size(); ++matIdx)
                    {
                        const auto& intMat = internalMaterials[matIdx];
                        const Export::ResolvedMaterialTextures resolvedTextures =
                            Export::ResolveMaterialTextures(intMat);

                        if (AssignExportedTexture(
                                resolvedTextures.baseColorTextureName,
                                exportedTexturePaths,
                                exportedTextureSourcePaths,
                                materials[matIdx].diffuseTexturePath,
                                materials[matIdx].diffuseTextureSourcePath))
                        {
                            assignedTextureCount++;
                        }

                        if (AssignExportedTexture(
                                resolvedTextures.normalTextureName,
                                exportedTexturePaths,
                                exportedTextureSourcePaths,
                                materials[matIdx].normalTexturePath,
                                materials[matIdx].normalTextureSourcePath))
                        {
                            assignedTextureCount++;
                        }

                        if (AssignExportedTexture(
                                resolvedTextures.metallicRoughnessTextureName,
                                exportedTexturePaths,
                                exportedTextureSourcePaths,
                                materials[matIdx].metallicRoughnessTexturePath,
                                materials[matIdx].metallicRoughnessTextureSourcePath))
                        {
                            assignedTextureCount++;
                        }

                        if (AssignExportedTexture(
                                resolvedTextures.occlusionTextureName,
                                exportedTexturePaths,
                                exportedTextureSourcePaths,
                                materials[matIdx].occlusionTexturePath,
                                materials[matIdx].occlusionTextureSourcePath))
                        {
                            assignedTextureCount++;
                        }

                        // If no metallicRoughness is found, but a normal map is present, assign the split _metalrough texture
                        if (resolvedTextures.metallicRoughnessTextureName.empty() && !resolvedTextures.normalTextureName.empty())
                        {
                            if (!materials[matIdx].normalTexturePath.empty())
                            {
                                std::string mrPath = materials[matIdx].normalTexturePath;
                                size_t dotPos = mrPath.find_last_of('.');
                                if (dotPos != std::string::npos)
                                {
                                    mrPath.insert(dotPos, "_metalrough");
                                }
                                else
                                {
                                    mrPath += "_metalrough";
                                }

                                fs::path mrSourcePath = materials[matIdx].normalTextureSourcePath;
                                std::string stem = mrSourcePath.stem().string();
                                std::string ext = mrSourcePath.extension().string();
                                mrSourcePath.replace_filename(stem + "_metalrough" + ext);

                                materials[matIdx].metallicRoughnessTexturePath = mrPath;
                                materials[matIdx].metallicRoughnessTextureSourcePath = mrSourcePath;
                                assignedTextureCount++;

                                if (materials[matIdx].occlusionTexturePath.empty())
                                {
                                    materials[matIdx].occlusionTexturePath = mrPath;
                                    materials[matIdx].occlusionTextureSourcePath = mrSourcePath;
                                    assignedTextureCount++;
                                }
                            }
                        }
                    }

                    AddLog(app,
                        LogLevel::Info,
                        "Model material dependency summary for " + entry.name +
                        ": internalMaterials=" + std::to_string(internalMaterials.size()) +
                        ", uniqueTextures=" + std::to_string(uniqueTexturesToExport.size()) +
                        ", exportedTextures=" + std::to_string(exportedTextureCount) +
                        ", assignedTextures=" + std::to_string(assignedTextureCount) +
                        ", gltfMaterialSlots=" + std::to_string(materials.size()));
                }
                else if (matchedLink)
                {
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

                        for (const AssetIndex::DescriptorMaterialTextureRef& binding : matchedLink->materialTextureBindings)
                        {
                            if (binding.materialIndex >= materials.size()) continue;

                            std::string texName = ResolveDescriptorTextureName(*matchedLink, binding.textureIndex);
                            if (texName.empty()) continue;

                            auto pathIt = exportedTexturePaths.find(texName);
                            if (pathIt != exportedTexturePaths.end())
                            {
                                const std::string shaderClass =
                                    binding.materialIndex < matchedLink->materialShaderClasses.size()
                                        ? matchedLink->materialShaderClasses[binding.materialIndex]
                                        : std::string();
                                const Export::MaterialTextureSemantic semantic =
                                    Export::ClassifyDescriptorBinding(shaderClass, binding.slotName, texName);

                                if (semantic == Export::MaterialTextureSemantic::BaseColor &&
                                    materials[binding.materialIndex].diffuseTexturePath.empty())
                                {
                                    materials[binding.materialIndex].diffuseTexturePath = pathIt->second;
                                    auto srcIt = exportedTextureSourcePaths.find(texName);
                                    if (srcIt != exportedTextureSourcePaths.end())
                                    {
                                        materials[binding.materialIndex].diffuseTextureSourcePath = srcIt->second;
                                    }
                                    assignedTextureCount++;
                                }
                                else if (semantic == Export::MaterialTextureSemantic::Normal &&
                                         materials[binding.materialIndex].normalTexturePath.empty())
                                {
                                    materials[binding.materialIndex].normalTexturePath = pathIt->second;
                                    auto srcIt = exportedTextureSourcePaths.find(texName);
                                    if (srcIt != exportedTextureSourcePaths.end())
                                    {
                                        materials[binding.materialIndex].normalTextureSourcePath = srcIt->second;
                                    }
                                    assignedTextureCount++;
                                }
                                else if (semantic == Export::MaterialTextureSemantic::MetallicRoughness &&
                                         materials[binding.materialIndex].metallicRoughnessTexturePath.empty())
                                {
                                    materials[binding.materialIndex].metallicRoughnessTexturePath = pathIt->second;
                                    auto srcIt = exportedTextureSourcePaths.find(texName);
                                    if (srcIt != exportedTextureSourcePaths.end())
                                    {
                                        materials[binding.materialIndex].metallicRoughnessTextureSourcePath = srcIt->second;
                                    }
                                    assignedTextureCount++;

                                    // If this is an ORM texture, also assign it to the occlusion slot
                                    if (Export::LooksLikePackedOrm(texName) &&
                                        materials[binding.materialIndex].occlusionTexturePath.empty())
                                    {
                                        materials[binding.materialIndex].occlusionTexturePath = pathIt->second;
                                        if (srcIt != exportedTextureSourcePaths.end())
                                        {
                                            materials[binding.materialIndex].occlusionTextureSourcePath = srcIt->second;
                                        }
                                        assignedTextureCount++;
                                    }
                                }
                                else if (semantic == Export::MaterialTextureSemantic::Occlusion &&
                                         materials[binding.materialIndex].occlusionTexturePath.empty())
                                {
                                    materials[binding.materialIndex].occlusionTexturePath = pathIt->second;
                                    auto srcIt = exportedTextureSourcePaths.find(texName);
                                    if (srcIt != exportedTextureSourcePaths.end())
                                    {
                                        materials[binding.materialIndex].occlusionTextureSourcePath = srcIt->second;
                                    }
                                    assignedTextureCount++;
                                }
                            }
                            else
                            {
                                skippedBindingCount++;
                            }
                        }
                    }
                    else if (materials.size() == 1 && !exportedTexturePaths.empty())
                    {
                        const auto textureIt = exportedTexturePaths.begin();
                        materials.front().diffuseTexturePath = textureIt->second;
                        auto srcIt = exportedTextureSourcePaths.find(textureIt->first);
                        if (srcIt != exportedTextureSourcePaths.end())
                        {
                            materials.front().diffuseTextureSourcePath = srcIt->second;
                        }
                        assignedTextureCount = 1;
                    }

                    // For descriptor-linked materials, if metallicRoughness is missing but normal map is present, assign the split _metalrough texture
                    for (size_t m = 0; m < materials.size(); ++m)
                    {
                        if (materials[m].metallicRoughnessTexturePath.empty() && !materials[m].normalTexturePath.empty())
                        {
                            std::string mrPath = materials[m].normalTexturePath;
                            size_t dotPos = mrPath.find_last_of('.');
                            if (dotPos != std::string::npos)
                            {
                                mrPath.insert(dotPos, "_metalrough");
                            }
                            else
                            {
                                mrPath += "_metalrough";
                            }

                            fs::path mrSourcePath = materials[m].normalTextureSourcePath;
                            std::string stem = mrSourcePath.stem().string();
                            std::string ext = mrSourcePath.extension().string();
                            mrSourcePath.replace_filename(stem + "_metalrough" + ext);

                            materials[m].metallicRoughnessTexturePath = mrPath;
                            materials[m].metallicRoughnessTextureSourcePath = mrSourcePath;
                            assignedTextureCount++;

                            if (materials[m].occlusionTexturePath.empty())
                            {
                                materials[m].occlusionTexturePath = mrPath;
                                materials[m].occlusionTextureSourcePath = mrSourcePath;
                                assignedTextureCount++;
                            }
                        }
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

                if (app.exportProgress.cancelRequested.load())
                {
                    AddLog(app, LogLevel::Warning, "Export canceled by user.");
                    AddExportProgressLog(app, "Export canceled by user.");
                    return;
                }
                UpdateExportProgress(app, currentStep++, totalSteps, "Exporting model geometry...", entry.name);
                AddExportProgressLog(app, "Exporting model geometry...");

                bool success = false;
                fs::path outPath;

                if (shouldParseRendInst)
                {
                    DagorFormats::RendInstReader& reader = *riReader;
                    AddLog(app,
                        LogLevel::Info,
                        "RendInst UV export summary for " + entry.name + ": " +
                            BuildUvExportSummary(reader.GetLods()));

                    manifest.assetType = "RendInst Static Model";
                    manifest.formatDecoderVersion = "RendInstReader v1.0";

                    std::vector<DagorFormats::RendInstMesh> exportLods = reader.GetLods();
                    int maxLods = std::max(1, app.settings.staticModelMaxLods);
                    if (exportLods.size() > static_cast<size_t>(maxLods))
                    {
                        exportLods.resize(maxLods);
                    }

                    outPath = exportRoot / (safeName + ".glb");
                    manifest.outputFiles.push_back(outPath.filename().string());
                    AddExportProgressLog(app, "Writing GLB static model...");
                    success = DagorAssetsExtractor::Export::ModelExporter::ExportToGlb(outPath, exportLods, manifest, materials, glbOptions);
                }
                else if (isDynModel)
                {
                    DagorFormats::DynModelReader& reader = *dynReader;

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

                    std::vector<DagorFormats::GeomNodeTreeNode> skeleton;
                    std::string skeletonName;
                    {
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
                            AddExportProgressLog(app, "Resolved skeleton " + skeletonEntry.name + " (" + std::to_string(skeleton.size()) + " nodes)");
                            return true;
                        };

                        if (matchedLink)
                        {
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
                                    constexpr uint32_t kGeomNodeTreeClassId = 0x56F81B6Du;
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
                    }

                    if (!includeSkeletonRig)
                    {
                        AddLog(app,
                            LogLevel::Info,
                            skeleton.empty()
                                ? ("Skeleton / rig export disabled for " + entry.name +
                                   "; no skeleton placement metadata was resolved.")
                                : ("Skeleton / rig export disabled for " + entry.name +
                                   "; using resolved skeleton only for rigid part placement metadata."));
                    }

                    std::vector<Export::AnimationClip> animations;
                    if (includeAnimations)
                    {
                        if (skeleton.empty())
                        {
                            const std::string warning =
                                "Animation export requested for " + entry.name +
                                ", but no skeleton was resolved; GLB animation targets cannot be mapped.";
                            manifest.decodeWarnings.push_back(warning);
                            AddLog(app, LogLevel::Warning, warning);
                        }
                        else
                        {
                            AddLog(app, LogLevel::Info, "Resolving Anim2Data animation clips for " + entry.name + "...");
                            animations = ResolveAnimationClipsForDynModel(
                                app,
                                packPath,
                                entry,
                                dynLods,
                                skeleton,
                                matchedLink,
                                descriptorLinks,
                                allRecords,
                                manifest);
                        }
                    }
                    else if (dynModelIncludeAnimationsOverride.value_or(false) && !includeSkeletonRig)
                    {
                        AddLog(app,
                            LogLevel::Info,
                            "Animation export disabled for " + entry.name +
                            " because skeleton / rig export is disabled.");
                    }

                    if (format == "GLB" || format == "glb")
                    {
                        outPath = exportRoot / (safeName + ".glb");
                        manifest.outputFiles.push_back(outPath.filename().string());
                        AddLog(app,
                            LogLevel::Info,
                            "Writing DynModel GLB for " + entry.name +
                            " with skeletonNodes=" + std::to_string(skeleton.size()) +
                            ", includeSkeletonRig=" + std::string(includeSkeletonRig ? "true" : "false") +
                            ", animationClips=" + std::to_string(animations.size()) +
                            " to " + Platform::PathToUtf8(outPath));
                        AddExportProgressLog(app, "Writing GLB skinned model (" + std::to_string(skeleton.size()) + " bones, " + std::to_string(animations.size()) + " animations)...");
                        success = DagorAssetsExtractor::Export::ModelExporter::ExportToGlb(outPath, dynLods, skeleton, manifest, materials, includeSkeletonRig, glbOptions, animations);
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
                    message += " (magic " + FormatPayloadMagic(payload) + "). " + AssetIndex::GetUnsupportedExportTooltip(entry);
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
                    AddExportProgressLog(app, "Model export completed successfully.");
                }
                else
                {
                    AddLog(app, LogLevel::Error, "Failed to export model: " + entry.name);
                    AddExportProgressLog(app, "Error: Failed to export model.");
                }
            }
            catch (const std::exception& e)
            {
                AddLog(app, LogLevel::Error, "Exception while exporting model " + entry.name + ": " + e.what());
                AddExportProgressLog(app, std::string("Error: Exception during model export: ") + e.what());
            }
        });
    }

    void RequestModelExport(AppState& app, const std::filesystem::path& packPath, const AssetIndex::SubAssetRecord& entry)
    {
        RequestModelExportInternal(app, packPath, entry, std::nullopt, std::nullopt, std::nullopt);
    }

    void RequestModelExport(AppState& app, const std::filesystem::path& packPath, const AssetIndex::SubAssetRecord& entry, bool includeTexturesForDynModel)
    {
        RequestModelExportInternal(app, packPath, entry, includeTexturesForDynModel, true, false);
    }

    void RequestModelExport(AppState& app, const std::filesystem::path& packPath, const AssetIndex::SubAssetRecord& entry, bool includeTexturesForDynModel, bool includeSkeletonRigForDynModel)
    {
        RequestModelExportInternal(app, packPath, entry, includeTexturesForDynModel, includeSkeletonRigForDynModel, false);
    }

    void RequestModelExport(AppState& app, const std::filesystem::path& packPath, const AssetIndex::SubAssetRecord& entry, bool includeTexturesForDynModel, bool includeSkeletonRigForDynModel, bool includeAnimationsForDynModel)
    {
        RequestModelExportInternal(app, packPath, entry, includeTexturesForDynModel, includeSkeletonRigForDynModel, includeAnimationsForDynModel);
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

        BeginExportProgress(app, "Exporting " + std::to_string(contents->entries.size()) + " Textures", "Preparing texture export...", contents->entries.size());
        SetExportProgressOutputDir(app, Platform::PathToUtf8(options.outputDir));
        AddExportProgressLog(app, "Starting batch export of textures from " + Platform::PathToUtf8(packPath.filename()) + "...");

        app.backgroundJobs.Start("BatchExportTextures_" + packPath.filename().string(), [&app, packPath, contents, options]() {
            auto result = Export::TextureExporter::ExportAllTextures(packPath, *contents, options, [&app](size_t current, size_t total, const std::string& name) {
                UpdateExportProgress(app, current, total, "Exporting textures...", name);
                AddExportProgressLog(app, "[" + std::to_string(current) + "/" + std::to_string(total) + "] Exporting: " + name);
                return !app.exportProgress.cancelRequested.load();
            });

            if (app.exportProgress.cancelRequested.load())
            {
                AddExportProgressLog(app, "Batch export canceled by user.");
            }

            std::ostringstream msg;
            msg << "Batch texture export completed: " << result.succeeded << " succeeded, "
                << result.failed << " failed, " << result.skipped << " skipped.";
            AddLog(app, LogLevel::Info, msg.str());
            AddExportProgressLog(app, msg.str());

            for (const auto& err : result.errors)
            {
                AddLog(app, LogLevel::Error, err);
                AddExportProgressLog(app, "Error: " + err);
            }
            for (const auto& warn : result.warnings)
            {
                AddLog(app, LogLevel::Warning, warn);
                AddExportProgressLog(app, "Warning: " + warn);
            }

            EndExportProgress(app);
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

    void RequestBlkResourceLinks(AppState& app, std::shared_ptr<const std::vector<AssetIndex::ResourceRecord>> records, bool invalidateDescriptors)
    {
        if (!records) return;

        {
            std::lock_guard<std::mutex> lock(app.blkLinks.mutex);
            if (app.blkLinks.loading) return;
            app.blkLinks.loading = true;
            app.blkLinks.status = "Building relationships...";
            app.blkLinks.loadingRecords = records;
        }

        app.backgroundJobs.Start("BlkResourceLinks", [&app, records, invalidateDescriptors]() {
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

                if (invalidateDescriptors)
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
            scopedBuild ? "Planning selected descriptor relationship build" : "Planning descriptor relationship build",
            0);
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
                std::mutex progressCallbackMutex;

                AssetIndex::DescriptorResourceLinkBuildOptions options;
                options.blkLinks = blkLinks ? blkLinks.get() : nullptr;
                if (selected)
                {
                    options.focusPath = selected->path;
                }
                options.progress = [&app, records, &lastProgressStage, &lastLogCurrent, &progressCallbackMutex](const AssetIndex::DescriptorResourceLinkProgress& progress) {
                    std::lock_guard<std::mutex> progressLock(progressCallbackMutex);
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
        return snapshot;
    }

    void RequestProgressJobCancel(AppState& app)
    {
        app.progressJob.cancelRequested.store(true);
        AddProgressJobLog(app, LogLevel::Warning, "Cancel requested by user.");
    }

    void BeginExportProgress(AppState& app, std::string title, std::string status, uint64_t total)
    {
        std::lock_guard<std::mutex> lock(app.exportProgress.mutex);
        app.exportProgress.title = std::move(title);
        app.exportProgress.status = std::move(status);
        app.exportProgress.currentItem.clear();
        app.exportProgress.outputDir.clear();
        app.exportProgress.current = 0;
        app.exportProgress.total = total;
        app.exportProgress.cancelRequested.store(false);
        app.exportProgress.running.store(true);
    }

    void UpdateExportProgress(AppState& app, uint64_t current, uint64_t total, std::string status, std::string item)
    {
        std::lock_guard<std::mutex> lock(app.exportProgress.mutex);
        app.exportProgress.current = current;
        app.exportProgress.total = total;
        app.exportProgress.status = std::move(status);
        app.exportProgress.currentItem = std::move(item);
    }

    void AddExportProgressLog(AppState& app, std::string message)
    {
        app.logger.AddLog(LogLevel::Info, std::move(message));
    }

    void EndExportProgress(AppState& app)
    {
        std::lock_guard<std::mutex> lock(app.exportProgress.mutex);
        app.exportProgress.running.store(false);
        app.exportProgress.cancelRequested.store(false);
    }

    void SetExportProgressOutputDir(AppState& app, std::string dir)
    {
        std::lock_guard<std::mutex> lock(app.exportProgress.mutex);
        app.exportProgress.outputDir = std::move(dir);
    }

    void JoinFinishedBackgroundJobs(AppState& app)
    {
        app.backgroundJobs.JoinFinished();
    }

    void WaitForBackgroundJobs(AppState& app)
    {
        app.backgroundJobs.JoinAll();
    }

    void InvalidateAndClearCache(AppState& app)
    {
        StopScan(app);
        RequestDescriptorResourceLinkCancel(app);

        // Delete disk caches
        std::error_code ec;
        if (!app.indexCachePath.empty() && fs::exists(app.indexCachePath))
        {
            fs::remove(app.indexCachePath, ec);
        }
        if (!app.descriptorLinksCachePath.empty() && fs::exists(app.descriptorLinksCachePath))
        {
            fs::remove(app.descriptorLinksCachePath, ec);
        }

        // Reset memory state
        ClearResourceIndex(app, "Not built");

        AddLog(app, LogLevel::Info, "Index, BLK, and descriptor link caches invalidated and cleared.");
    }
}
