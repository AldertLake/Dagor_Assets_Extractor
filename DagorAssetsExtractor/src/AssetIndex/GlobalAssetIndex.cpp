#include "AssetIndex/GlobalAssetIndex.h"

#include "AssetIndex/DependencyGraph.h"
#include "AssetIndex/TextureQuality.h"
#include "DagorFormats/GrpReader.h"
#include "DagorIO/MappedFile.h"
#include "Jobs/JobSystem.h"
#include "Platform/Platform.h"

#include <algorithm>
#include <cctype>
#include <cwctype>

namespace DagorAssetsExtractor::AssetIndex
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr uint32_t kAnim2DataClassId = 0x40C586F9u;

        std::string ToLowerAsciiLocal(std::string_view value)
        {
            std::string out(value);
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return out;
        }

        uint16_t ReadLe16Safe(std::span<const uint8_t> data, size_t offset, bool& ok)
        {
            if (offset + 2 > data.size())
            {
                ok = false;
                return 0;
            }
            return static_cast<uint16_t>(data[offset]) |
                   static_cast<uint16_t>(static_cast<uint16_t>(data[offset + 1]) << 8);
        }

        uint32_t ReadLe32Safe(std::span<const uint8_t> data, size_t offset, bool& ok)
        {
            if (offset + 4 > data.size())
            {
                ok = false;
                return 0;
            }
            return static_cast<uint32_t>(data[offset]) |
                   (static_cast<uint32_t>(data[offset + 1]) << 8) |
                   (static_cast<uint32_t>(data[offset + 2]) << 16) |
                   (static_cast<uint32_t>(data[offset + 3]) << 24);
        }

        bool ParseGrpPackForIndex(const fs::path& packPath, std::vector<IndexedGrpEntry>& outEntries)
        {
            DagorIO::MappedFile mapped;
            std::string error;
            if (!mapped.Open(packPath, error))
            {
                return false;
            }

            const std::span<const uint8_t> data = mapped.Span();
            if (data.size() < 8)
            {
                return false;
            }

            try
            {
                DagorFormats::GrpReader reader(data);
                reader.Parse();
                const auto& files = reader.GetFiles();
                outEntries.reserve(files.size());
                for (const DagorFormats::GrpFileInfo& file : files)
                {
                    outEntries.push_back(IndexedGrpEntry{
                        packPath,
                        file.name,
                        file.classId,
                        file.offset,
                        file.size
                    });
                }
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        bool ParseDxpPackNamesForIndex(const fs::path& packPath, std::vector<std::string>& outNames)
        {
            DagorIO::MappedFile mapped;
            std::string error;
            if (!mapped.Open(packPath, error))
            {
                return false;
            }

            const std::span<const uint8_t> data = mapped.Span();
            if (data.size() < 0x48 ||
                std::string_view(reinterpret_cast<const char*>(data.data()), 4) != "DxP2")
            {
                return false;
            }

            bool ok = true;
            const uint16_t totalFiles = ReadLe16Safe(data, 0x8, ok);
            const uint32_t block3Offset = ReadLe32Safe(data, 0x30, ok) + 0x10;
            const size_t requiredSize = static_cast<size_t>(block3Offset) + static_cast<size_t>(totalFiles) * 0x18;
            if (!ok || requiredSize > data.size())
            {
                return false;
            }

            std::vector<std::string> rawNames;
            rawNames.reserve(totalFiles);
            size_t offset = 0x48;
            while (rawNames.size() < totalFiles)
            {
                const size_t start = offset;
                while (offset < block3Offset && data[offset] != 0)
                {
                    ++offset;
                }
                if (offset >= block3Offset)
                {
                    return false;
                }
                rawNames.emplace_back(reinterpret_cast<const char*>(data.data() + start), offset - start);
                ++offset;
            }

            outNames.reserve(totalFiles);
            for (uint16_t i = 0; i < totalFiles; ++i)
            {
                const size_t entryStart = static_cast<size_t>(block3Offset) + static_cast<size_t>(i) * 0x18;
                const uint32_t payloadSize = ReadLe32Safe(data, entryStart + 0x10, ok);
                if (ok && payloadSize > 0)
                {
                    outNames.push_back(std::move(rawNames[i]));
                }
            }
            return true;
        }
    }

    std::wstring GlobalAssetIndex::MakePathKey(const fs::path& path)
    {
        if (path.empty())
        {
            return {};
        }

        std::wstring key = path.lexically_normal().wstring();
        std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) {
            return static_cast<wchar_t>(std::towlower(c));
        });
        while (key.size() > 1 && (key.back() == L'\\' || key.back() == L'/'))
        {
            if (key.size() == 3 && key[1] == L':')
            {
                break;
            }
            key.pop_back();
        }
        return key;
    }

    std::string GlobalAssetIndex::NormalizeTextureName(std::string_view name)
    {
        std::string normalized(name);
        for (char& c : normalized)
        {
            if (c >= 'A' && c <= 'Z')
            {
                c = static_cast<char>(c - 'A' + 'a');
            }
            else if (c == '\\')
            {
                c = '/';
            }
        }

        const size_t start = normalized.find_first_not_of(" \t\r\n");
        if (start == std::string::npos)
        {
            return {};
        }
        const size_t end = normalized.find_last_not_of(" \t\r\n");
        if (start != 0 || end + 1 != normalized.size())
        {
            normalized = normalized.substr(start, end - start + 1);
        }

        const size_t slash = normalized.find_last_of('/');
        const size_t dot = normalized.find_last_of('.');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        {
            normalized.resize(dot);
        }

        if (const size_t star = normalized.find('*'); star != std::string::npos)
        {
            normalized.resize(star);
        }
        if (const size_t dollar = normalized.find('$'); dollar != std::string::npos)
        {
            normalized.resize(dollar);
        }

        return normalized;
    }

    const IndexedGrpEntry* GlobalAssetIndex::FindGrpEntry(
        std::string_view name,
        uint32_t preferredClassId,
        const fs::path* preferredPack) const
    {
        if (name.empty())
        {
            return nullptr;
        }

        const auto pickBest = [&](const std::vector<size_t>& candidates) -> const IndexedGrpEntry* {
            const IndexedGrpEntry* best = nullptr;
            int bestScore = -1;
            const std::wstring preferredPackKey = preferredPack ? MakePathKey(*preferredPack) : std::wstring{};

            for (const size_t idx : candidates)
            {
                if (idx >= grpEntries.size())
                {
                    continue;
                }
                const IndexedGrpEntry& entry = grpEntries[idx];
                int score = 0;
                if (preferredClassId == 0 || entry.classId == preferredClassId || entry.classId == 0)
                {
                    score += (preferredClassId != 0 && entry.classId == preferredClassId) ? 4 : 2;
                }
                if (!preferredPackKey.empty() && MakePathKey(entry.packPath) == preferredPackKey)
                {
                    score += 8;
                }
                if (entry.size > 0)
                {
                    score += 1;
                }
                if (score > bestScore)
                {
                    bestScore = score;
                    best = &entry;
                }
            }
            return best;
        };

        if (const auto it = grpByExactName.find(name); it != grpByExactName.end())
        {
            if (const IndexedGrpEntry* found = pickBest(it->second))
            {
                return found;
            }
        }

        const std::string lowered = ToLowerAsciiLocal(name);
        if (const auto it = grpByLowerName.find(lowered); it != grpByLowerName.end())
        {
            return pickBest(it->second);
        }

        return nullptr;
    }

    const std::vector<IndexedDxpTexture>* GlobalAssetIndex::FindDxpTextures(
        std::string_view rawOrNormalizedTexName) const
    {
        const std::string normalized = NormalizeTextureName(rawOrNormalizedTexName);
        if (normalized.empty())
        {
            return nullptr;
        }

        const auto it = dxpByNormalizedTextureName.find(normalized);
        if (it == dxpByNormalizedTextureName.end() || it->second.empty())
        {
            return nullptr;
        }
        return &it->second;
    }

    const DescriptorResourceLink* GlobalAssetIndex::FindDescriptorLink(
        const std::vector<DescriptorResourceLink>& links,
        std::string_view entryName,
        const fs::path& packPath) const
    {
        const auto it = linksByResourceOrGrpName.find(entryName);
        if (it == linksByResourceOrGrpName.end())
        {
            return nullptr;
        }

        const std::wstring targetPackKey = MakePathKey(packPath);
        for (const size_t linkIdx : it->second)
        {
            if (linkIdx >= links.size())
            {
                continue;
            }

            const DescriptorResourceLink& link = links[linkIdx];
            const bool nameMatch =
                (link.matchedGrpEntryName && *link.matchedGrpEntryName == entryName) ||
                (link.resourceName == entryName);
            const bool packMatch =
                !link.matchedGrpPath ||
                MakePathKey(*link.matchedGrpPath) == targetPackKey;
            if (nameMatch && packMatch)
            {
                return &link;
            }
        }

        return nullptr;
    }

    std::shared_ptr<GlobalAssetIndex> BuildGlobalAssetIndex(
        const std::vector<DescriptorResourceLink>* links,
        const CachedPackMetadata* packMetadata)
    {
        auto index = std::make_shared<GlobalAssetIndex>();

        if (packMetadata != nullptr)
        {
            index->hasPackMetadata = !packMetadata->grpPacks.empty() || !packMetadata->dxpPacks.empty();

            size_t totalGrpEntries = 0;
            for (const CachedGrpPackSnapshot& grpPack : packMetadata->grpPacks)
            {
                totalGrpEntries += grpPack.entries.size();
            }
            index->grpEntries.reserve(totalGrpEntries);
            index->grpByExactName.reserve(totalGrpEntries);
            index->grpByLowerName.reserve(totalGrpEntries);

            for (const CachedGrpPackSnapshot& grpPack : packMetadata->grpPacks)
            {
                for (const IndexedGrpEntry& entry : grpPack.entries)
                {
                    const size_t entryIdx = index->grpEntries.size();
                    index->grpEntries.push_back(entry);
                    if (index->grpEntries.back().packPath.empty())
                    {
                        index->grpEntries.back().packPath = grpPack.path;
                    }

                    index->grpByExactName[entry.name].push_back(entryIdx);
                    const std::string lowerName = ToLowerAsciiLocal(entry.name);
                    if (lowerName != entry.name)
                    {
                        index->grpByLowerName[lowerName].push_back(entryIdx);
                    }
                    else
                    {
                        index->grpByLowerName[entry.name].push_back(entryIdx);
                    }

                    if (entry.classId == kAnim2DataClassId)
                    {
                        index->anim2DataIndices.push_back(entryIdx);
                    }
                }
            }

            size_t totalDxpEntries = 0;
            for (const CachedDxpPackSnapshot& dxpPack : packMetadata->dxpPacks)
            {
                totalDxpEntries += dxpPack.entryNames.size();
            }
            index->dxpByNormalizedTextureName.reserve(totalDxpEntries / 2 + 16);

            for (const CachedDxpPackSnapshot& dxpPack : packMetadata->dxpPacks)
            {
                const int packBaseQuality = TextureQualityPriority(dxpPack.path, {});
                for (const std::string& entryName : dxpPack.entryNames)
                {
                    const std::string norm = GlobalAssetIndex::NormalizeTextureName(entryName);
                    if (norm.empty())
                    {
                        continue;
                    }

                    const int rank = (entryName.find('$') != std::string::npos)
                        ? TextureQualityPriority(dxpPack.path, entryName)
                        : packBaseQuality;

                    auto& bucket = index->dxpByNormalizedTextureName[norm];
                    bucket.push_back(IndexedDxpTexture{
                        dxpPack.path,
                        entryName,
                        rank
                    });
                }
            }

            for (auto& [_, bucket] : index->dxpByNormalizedTextureName)
            {
                std::sort(bucket.begin(), bucket.end(), [](const IndexedDxpTexture& a, const IndexedDxpTexture& b) {
                    if (a.qualityRank != b.qualityRank)
                    {
                        return a.qualityRank > b.qualityRank;
                    }
                    return a.entryName < b.entryName;
                });
            }
        }

        if (links != nullptr && !links->empty())
        {
            index->linksByResourceOrGrpName.reserve(links->size() * 2);

            for (size_t linkIdx = 0; linkIdx < links->size(); ++linkIdx)
            {
                const DescriptorResourceLink& link = (*links)[linkIdx];
                if (!link.descriptorPath.empty())
                {
                    index->linksByDescriptorPath[GlobalAssetIndex::MakePathKey(link.descriptorPath)].push_back(linkIdx);
                }

                if (link.matchedGrpPath && !link.matchedGrpPath->empty())
                {
                    index->linksByGrpPath[GlobalAssetIndex::MakePathKey(*link.matchedGrpPath)].push_back(linkIdx);
                }

                for (size_t texIdx = 0; texIdx < link.textureLinks.size(); ++texIdx)
                {
                    const DescriptorTextureLink& texLink = link.textureLinks[texIdx];
                    if (texLink.matchedDxpPath && !texLink.matchedDxpPath->empty())
                    {
                        index->linksByDxpPath[GlobalAssetIndex::MakePathKey(*texLink.matchedDxpPath)].emplace_back(
                            linkIdx,
                            texIdx);
                    }
                }

                if (!link.resourceName.empty())
                {
                    auto& list = index->linksByResourceOrGrpName[link.resourceName];
                    if (list.empty() || list.back() != linkIdx)
                    {
                        list.push_back(linkIdx);
                    }
                }
                if (link.matchedGrpEntryName &&
                    !link.matchedGrpEntryName->empty() &&
                    *link.matchedGrpEntryName != link.resourceName)
                {
                    auto& list = index->linksByResourceOrGrpName[*link.matchedGrpEntryName];
                    if (list.empty() || list.back() != linkIdx)
                    {
                        list.push_back(linkIdx);
                    }
                }
            }
        }

        return index;
    }

    std::shared_ptr<GlobalAssetIndex> BuildPackOnlyGlobalAssetIndex(const std::vector<ResourceRecord>& records)
    {
        std::vector<const ResourceRecord*> grpRecords;
        std::vector<const ResourceRecord*> dxpRecords;
        for (const ResourceRecord& record : records)
        {
            if (record.type == AssetType::Grp)
            {
                grpRecords.push_back(&record);
            }
            else if (record.type == AssetType::Dxp)
            {
                dxpRecords.push_back(&record);
            }
        }

        CachedPackMetadata metadata;
        metadata.grpPacks.resize(grpRecords.size());
        metadata.dxpPacks.resize(dxpRecords.size());

        Jobs::JobSystem::RunParallelFor(
            grpRecords.size(),
            1,
            [&](size_t i, size_t) {
                const ResourceRecord& rec = *grpRecords[i];
                CachedGrpPackSnapshot& snap = metadata.grpPacks[i];
                snap.path = rec.path;
                snap.sizeBytes = rec.sizeBytes;
                snap.lastWriteTicks = rec.lastWriteTicks;
                ParseGrpPackForIndex(rec.path, snap.entries);
            });

        Jobs::JobSystem::RunParallelFor(
            dxpRecords.size(),
            1,
            [&](size_t i, size_t) {
                const ResourceRecord& rec = *dxpRecords[i];
                CachedDxpPackSnapshot& snap = metadata.dxpPacks[i];
                snap.path = rec.path;
                snap.sizeBytes = rec.sizeBytes;
                snap.lastWriteTicks = rec.lastWriteTicks;
                ParseDxpPackNamesForIndex(rec.path, snap.entryNames);
            });

        return BuildGlobalAssetIndex(nullptr, &metadata);
    }
}
