#include "AssetIndex/PackContents.h"
#include "DagorFormats/VromfsReader.h"
#include "DagorFormats/DxpReader.h"
#include "DagorFormats/DdsxReader.h"
#include "DagorFormats/GrpReader.h"
#include "DagorFormats/Anim2DataReader.h"
#include "DagorFormats/GameResDescReader.h"
#include "DagorFormats/GeomNodeTreeReader.h"

#include <fstream>
#include <algorithm>
#include <cstdio>
#include <sstream>
#include <utility>

namespace DagorAssetsExtractor::AssetIndex
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr uint32_t kDynModelGameResClassId = 0xB4B7D9C4u;
        constexpr uint32_t kRendInstGameResClassId = 0x77F8232Fu;
        constexpr uint32_t kGeomNodeTreeGameResClassId = 0x56F81B6Du;
        constexpr uint32_t kCharacterGameResClassId = 0xA6F87A9Bu;
        constexpr uint32_t kFastPhysDataGameResClassId = 0x855A1BE6u;
        constexpr uint32_t kAnim2DataGameResClassId = 0x40C586F9u;
        constexpr uint32_t kPhysSysGameResClassId = 0xDBBCB7D2u;
        constexpr uint32_t kPhysObjGameResClassId = 0xD543E771u;
        constexpr uint32_t kCollisionGameResClassId = 0xACE50000u;
        constexpr uint32_t kRagdollGameResClassId = 0xD403C21Cu;
        constexpr uint32_t kEffectGameResClassId = 0x88B7A117u;
        constexpr uint32_t kAnimBnlGameResClassId = 0x8F2A7018u;
        constexpr uint32_t kAnimGraphGameResClassId = 0x8F2A701Au;
    }

    std::string DescribeGrpClassId(uint32_t classId)
    {
        switch (classId)
        {
        case 0:
            return {};
        case kDynModelGameResClassId:
            return "DynModel";
        case kRendInstGameResClassId:
            return "RendInst";
        case kGeomNodeTreeGameResClassId:
            return "GeomNodeTree";
        case kCharacterGameResClassId:
            return "Character";
        case kFastPhysDataGameResClassId:
            return "FastPhysData";
        case kAnim2DataGameResClassId:
            return "Anim2Data";
        case kPhysSysGameResClassId:
            return "PhysSys";
        case kPhysObjGameResClassId:
            return "PhysObj";
        case kCollisionGameResClassId:
            return "CollisionResource";
        case kRagdollGameResClassId:
            return "Ragdoll";
        case kEffectGameResClassId:
            return "Effect";
        case kAnimBnlGameResClassId:
            return "AnimBnl";
        case kAnimGraphGameResClassId:
            return "AnimGraph";
        default:
        {
            char classIdText[32] = {};
            std::snprintf(classIdText, sizeof(classIdText), "class 0x%08X", classId);
            return classIdText;
        }
        }
    }

    bool CanExportGrpEntryAsStaticModel(const SubAssetRecord& entry)
    {
        return entry.grpClassId == 0 || entry.grpClassId == kRendInstGameResClassId || entry.grpClassId == kDynModelGameResClassId;
    }

    bool CanExportGrpEntryAsSkinnedModel(const SubAssetRecord& entry)
    {
        return entry.grpClassId == kDynModelGameResClassId;
    }

    bool IsExportSupported(const SubAssetRecord& entry, AssetType containerType)
    {
        if (containerType == AssetType::Grp)
        {
            return CanExportGrpEntryAsStaticModel(entry) || CanExportGrpEntryAsSkinnedModel(entry);
        }
        else if (containerType == AssetType::Dxp)
        {
            return true;
        }
        return false;
    }

    std::string GetUnsupportedExportTooltip(uint32_t grpClassId, const std::string& fallbackClassName)
    {
        switch (grpClassId)
        {
        case kGeomNodeTreeGameResClassId:
            return "Skeletal rigs / skeleton hierarchies are engine-only resources and cannot be exported as 3D meshes.";
        case kAnim2DataGameResClassId:
        case kAnimBnlGameResClassId:
        case kAnimGraphGameResClassId:
            return "Animations and animation structures are engine-only assets and cannot be exported directly.";
        case kCollisionGameResClassId:
            return "Collision geometry is an engine-only resource and cannot be exported as a 3D mesh.";
        case kPhysSysGameResClassId:
        case kPhysObjGameResClassId:
        case kRagdollGameResClassId:
        case kFastPhysDataGameResClassId:
            return "Physics parameters and physics objects are engine-only resources and cannot be exported.";
        case kCharacterGameResClassId:
            return "Character metadata definitions are engine-only resources and cannot be exported.";
        case kEffectGameResClassId:
            return "Particle effects are engine-only assets and cannot be exported.";
        default:
        {
            std::string name = fallbackClassName;
            if (name.empty())
            {
                name = DescribeGrpClassId(grpClassId);
            }
            if (name.empty())
            {
                char classIdText[32] = {};
                std::snprintf(classIdText, sizeof(classIdText), "class 0x%08X", grpClassId);
                name = classIdText;
            }
            return "This engine asset type (" + name + ") is an internal resource and cannot be exported.";
        }
        }
    }

    std::string GetUnsupportedExportTooltip(const SubAssetRecord& entry)
    {
        return GetUnsupportedExportTooltip(entry.grpClassId, entry.formatInfo);
    }

    // -- PackContentsCache ----------------------------------------------------

    std::shared_ptr<const PackContents> PackContentsCache::Get(const fs::path& packPath) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::wstring key = packPath.lexically_normal().wstring();
        const auto it = m_cache.find(key);
        if (it != m_cache.end())
        {
            it->second.lastAccess = m_nextAccess++;
            return it->second.contents;
        }
        return nullptr;
    }

    void PackContentsCache::Put(const fs::path& packPath, std::shared_ptr<PackContents> contents)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::wstring key = packPath.lexically_normal().wstring();
        m_cache[key] = CachedContents{ std::move(contents), m_nextAccess++ };
    }

    bool PackContentsCache::Has(const fs::path& packPath) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::wstring key = packPath.lexically_normal().wstring();
        return m_cache.find(key) != m_cache.end();
    }

    void PackContentsCache::Remove(const fs::path& packPath)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::wstring key = packPath.lexically_normal().wstring();
        m_cache.erase(key);
    }

    size_t PackContentsCache::Size() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cache.size();
    }

    size_t PackContentsCache::ClearExcept(const fs::path& keepPath)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::wstring keepKey = keepPath.lexically_normal().wstring();
        size_t removed = 0;
        for (auto it = m_cache.begin(); it != m_cache.end();)
        {
            if (it->first == keepKey)
            {
                it->second.lastAccess = m_nextAccess++;
                ++it;
                continue;
            }

            it = m_cache.erase(it);
            ++removed;
        }

        return removed;
    }

    size_t PackContentsCache::TrimToLimit(size_t maxEntries, const fs::path& keepPath)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (maxEntries == 0 || m_cache.size() <= maxEntries)
        {
            return 0;
        }

        const std::wstring keepKey = keepPath.lexically_normal().wstring();
        size_t removed = 0;
        while (m_cache.size() > maxEntries)
        {
            auto oldest = m_cache.end();
            for (auto it = m_cache.begin(); it != m_cache.end(); ++it)
            {
                if (it->first == keepKey)
                {
                    continue;
                }
                if (oldest == m_cache.end() || it->second.lastAccess < oldest->second.lastAccess)
                {
                    oldest = it;
                }
            }

            if (oldest == m_cache.end())
            {
                break;
            }

            m_cache.erase(oldest);
            ++removed;
        }

        if (const auto keep = m_cache.find(keepKey); keep != m_cache.end())
        {
            keep->second.lastAccess = m_nextAccess++;
        }

        return removed;
    }

    void PackContentsCache::Clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cache.clear();
    }

    // -- Helper: read a file into memory with size limit ----------------------

    namespace
    {
        bool ReadFileLimited(const fs::path& path, std::vector<uint8_t>& outData, size_t maxSize, std::string& error)
        {
            std::error_code ec;
            const uintmax_t fileSize = fs::file_size(path, ec);
            if (ec)
            {
                error = "Could not determine file size: " + ec.message();
                return false;
            }

            if (fileSize > maxSize)
            {
                error = "File too large for in-memory parsing (" + std::to_string(fileSize / (1024 * 1024)) + " MB).";
                return false;
            }

            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                error = "Could not open file for reading.";
                return false;
            }

            outData.resize(static_cast<size_t>(fileSize));
            file.read(reinterpret_cast<char*>(outData.data()), static_cast<std::streamsize>(fileSize));
            if (!file)
            {
                error = "Failed to read file contents.";
                return false;
            }

            return true;
        }
    }

    // -- Parse VROMFS ---------------------------------------------------------

    std::shared_ptr<PackContents> ParseVromfsContents(const fs::path& path, size_t maxFileSize)
    {
        auto contents = std::make_shared<PackContents>();
        contents->sourcePath = path;
        contents->containerType = "VROMFS";
        contents->parsed = true;

        std::vector<uint8_t> data;
        if (!ReadFileLimited(path, data, maxFileSize, contents->error))
        {
            return contents;
        }

        DagorFormats::VromfsReader reader(data);
        try
        {
            reader.Parse();

            for (const DagorFormats::VromfsFileInfo& file : reader.GetFiles())
            {
                SubAssetRecord entry;
                entry.name   = file.name;
                entry.offset = file.offset;
                entry.size   = file.size;
                contents->entries.push_back(std::move(entry));
            }
        }
        catch (const std::exception& ex)
        {
            contents->error = std::string("VROMFS parse error: ") + ex.what();
            const auto& diagnostics = reader.GetDiagnostics();
            if (!diagnostics.empty())
            {
                contents->error += "\nDiagnostics:";
                for (const std::string& line : diagnostics)
                {
                    contents->error += "\n  - " + line;
                }
            }
        }

        return contents;
    }

    // -- Parse DXP ------------------------------------------------------------

    std::shared_ptr<PackContents> ParseDxpContents(const fs::path& path, size_t maxFileSize)
    {
        auto contents = std::make_shared<PackContents>();
        contents->sourcePath = path;
        contents->containerType = "DXP";
        contents->parsed = true;

        std::vector<uint8_t> data;
        if (!ReadFileLimited(path, data, maxFileSize, contents->error))
        {
            return contents;
        }

        try
        {
            DagorFormats::DxpReader reader(data);
            reader.Parse();

            for (const DagorFormats::DxpFileInfo& file : reader.GetFiles())
            {
                SubAssetRecord entry;
                entry.name   = file.name;
                entry.offset = file.offset;
                entry.size   = file.size;

                // Try to parse the DDSx header for extra texture metadata
                if (file.headerDdsx.size() >= 32)
                {
                    entry.ddsxHeader.assign(file.headerDdsx.begin(), file.headerDdsx.end());

                    try
                    {
                        DagorFormats::DdsxReader ddsxReader(file.headerDdsx);
                        ddsxReader.Parse();
                        const DagorFormats::DdsxHeader& hdr = ddsxReader.GetHeader();

                        entry.texWidth       = hdr.width;
                        entry.texHeight      = hdr.height;
                        entry.texMips        = hdr.mipCount;
                        entry.texFormat      = hdr.FormatName();
                        entry.texCompression = hdr.CompressionName();
                        entry.formatInfo     = std::string(hdr.FormatName()) + " " +
                                               std::to_string(hdr.width) + "x" + std::to_string(hdr.height);
                    }
                    catch (const std::exception&)
                    {
                        entry.formatInfo = "DDSx header unreadable";
                    }
                }

                contents->entries.push_back(std::move(entry));
            }
        }
        catch (const std::exception& ex)
        {
            contents->error = std::string("DXP parse error: ") + ex.what();
        }

        return contents;
    }

    // -- Parse GRP ------------------------------------------------------------

    std::shared_ptr<PackContents> ParseGrpContents(const fs::path& path, size_t maxFileSize)
    {
        auto contents = std::make_shared<PackContents>();
        contents->sourcePath = path;
        contents->containerType = "GRP";
        contents->parsed = true;

        std::vector<uint8_t> data;
        if (!ReadFileLimited(path, data, maxFileSize, contents->error))
        {
            return contents;
        }

        try
        {
            DagorFormats::GrpReader reader(data);
            reader.Parse();

            for (const DagorFormats::GrpFileInfo& file : reader.GetFiles())
            {
                SubAssetRecord entry;
                entry.name   = file.name;
                entry.offset = file.offset;
                entry.size   = file.size;
                entry.grpClassId = file.classId;
                entry.formatInfo = DescribeGrpClassId(file.classId);

                if (file.classId == kGeomNodeTreeGameResClassId)
                {
                    try
                    {
                        DagorFormats::GeomNodeTreeReader skeletonReader(reader.GetFileData(file));
                        skeletonReader.Parse();

                        entry.skeletonNodeCount = static_cast<uint32_t>(skeletonReader.GetNodes().size());
                        entry.skeletonImportantNodeCount = skeletonReader.GetImportantNodeCount();
                        if (!skeletonReader.GetNodes().empty())
                        {
                            entry.skeletonRootName = skeletonReader.GetNodes().front().name;
                        }

                        std::ostringstream summary;
                        summary << "GeomNodeTree " << entry.skeletonNodeCount << " node";
                        if (entry.skeletonNodeCount != 1)
                        {
                            summary << "s";
                        }
                        if (entry.skeletonImportantNodeCount > 0 &&
                            entry.skeletonImportantNodeCount != entry.skeletonNodeCount)
                        {
                            summary << " (" << entry.skeletonImportantNodeCount << " important)";
                        }
                        if (!entry.skeletonRootName.empty())
                        {
                            summary << ", root=" << entry.skeletonRootName;
                        }

                        entry.formatInfo = summary.str();
                    }
                    catch (const std::exception&)
                    {
                        entry.formatInfo = "GeomNodeTree (metadata unavailable)";
                    }
                }
                else if (file.classId == kAnim2DataGameResClassId)
                {
                    try
                    {
                        DagorFormats::Anim2DataReader animReader(reader.GetFileData(file));
                        animReader.Parse();
                        const DagorFormats::Anim2DataInfo& info = animReader.GetInfo();

                        entry.animVersion = info.version;
                        entry.animChannelCount = static_cast<uint32_t>(info.channels.size());
                        entry.animNodeCount = static_cast<uint32_t>(info.nodeNames.size());
                        entry.animLabelCount = static_cast<uint32_t>(info.labels.size());
                        entry.animAdditive = info.additive;

                        std::ostringstream summary;
                        summary << "Anim2Data ANIM 0x" << std::hex << std::uppercase << info.version << std::dec
                                << ", channels=" << entry.animChannelCount
                                << ", nodes=" << entry.animNodeCount;
                        if (entry.animLabelCount > 0)
                        {
                            summary << ", labels=" << entry.animLabelCount;
                        }
                        if (entry.animAdditive)
                        {
                            summary << ", additive";
                        }
                        if (info.compressed)
                        {
                            summary << ", compressed";
                        }

                        entry.formatInfo = summary.str();
                    }
                    catch (const std::exception&)
                    {
                        entry.formatInfo = "Anim2Data (metadata unavailable)";
                    }
                }

                contents->entries.push_back(std::move(entry));
            }
        }
        catch (const std::exception& ex)
        {
            contents->error = std::string("GRP parse error: ") + ex.what();
        }

        return contents;
    }

    // -- Parse Descriptor -----------------------------------------------------

    std::shared_ptr<PackContents> ParseDescriptorContents(
        const fs::path& path,
        size_t maxFileSize,
        const std::vector<std::string>* resourceNameFilter)
    {
        auto contents = std::make_shared<PackContents>();
        contents->sourcePath = path;
        contents->containerType = "Descriptor";
        contents->parsed = true;

        std::vector<uint8_t> data;
        if (!ReadFileLimited(path, data, maxFileSize, contents->error))
        {
            return contents;
        }

        try
        {
            DagorFormats::GameResDescReader reader(data);
            reader.Parse(resourceNameFilter);

            for (const auto& entry : reader.GetEntries())
            {
                SubAssetRecord sub;
                sub.name = entry.name;
                sub.formatInfo = entry.className;
                sub.resId = entry.resId;
                sub.dependencyIds = entry.dependencyIds;
                sub.textureNames = entry.textures;
                sub.materialNames = entry.materials;
                sub.materialDomains = entry.materialDomains;
                sub.materialShaderClasses = entry.materialShaderClasses;
                sub.materialTextureRefs.reserve(entry.materialTextureRefs.size());
                for (const DagorFormats::GameResDescMaterialTextureRef& ref : entry.materialTextureRefs)
                {
                    DescriptorMaterialTextureRef materialRef;
                    materialRef.materialIndex = ref.materialIndex;
                    materialRef.textureIndex = ref.textureIndex;
                    materialRef.slotName = ref.slotName;
                    sub.materialTextureRefs.push_back(std::move(materialRef));
                }
                sub.skeletonName = entry.skeletonName;
                sub.dynModelName = entry.dynModelName;
                contents->entries.push_back(std::move(sub));
            }
        }
        catch (const std::exception& ex)
        {
            contents->error = std::string("Descriptor parse error: ") + ex.what();
        }

        return contents;
    }

} // namespace DagorAssetsExtractor::AssetIndex
