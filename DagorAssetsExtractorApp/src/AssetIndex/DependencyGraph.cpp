#include "AssetIndex/DependencyGraph.h"

#include "AssetIndex/PackContents.h"
#include "Platform/Platform.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace DagorAssetsExtractor::AssetIndex
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr size_t MaxWarnings = 12;

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

        bool EndsWith(std::string_view value, std::string_view suffix)
        {
            if (suffix.size() > value.size())
            {
                return false;
            }

            return std::equal(suffix.rbegin(), suffix.rend(), value.rbegin());
        }

        std::string NormalizeSlashes(std::string value)
        {
            std::replace(value.begin(), value.end(), '\\', '/');
            return value;
        }

        std::string StripKnownPackSuffix(std::string value)
        {
            if (EndsWith(value, ".dxp.bin"))
            {
                value.resize(value.size() - std::string_view(".dxp.bin").size());
            }
            else if (EndsWith(value, ".vromfs.bin"))
            {
                value.resize(value.size() - std::string_view(".vromfs.bin").size());
            }
            else if (EndsWith(value, ".grp"))
            {
                value.resize(value.size() - std::string_view(".grp").size());
            }

            return value;
        }

        std::string BasenameKey(std::string value)
        {
            value = NormalizeSlashes(std::move(value));
            const size_t slash = value.find_last_of('/');
            if (slash != std::string::npos)
            {
                value = value.substr(slash + 1);
            }
            return StripKnownPackSuffix(ToLowerAscii(Trim(std::move(value))));
        }

        std::string FullPathKey(std::string value)
        {
            value = NormalizeSlashes(std::move(value));
            return StripKnownPackSuffix(ToLowerAscii(Trim(std::move(value))));
        }

        std::wstring PathKey(const fs::path& path)
        {
            return path.lexically_normal().wstring();
        }

        bool SamePath(const fs::path& left, const fs::path& right)
        {
            return PathKey(left) == PathKey(right);
        }

        void AppendUniquePath(std::vector<fs::path>& paths, const fs::path& path)
        {
            if (std::find_if(paths.begin(), paths.end(), [&](const fs::path& existing) {
                return SamePath(existing, path);
            }) == paths.end())
            {
                paths.push_back(path);
            }
        }

        void AppendUniqueUInt(std::vector<uint32_t>& values, uint32_t value)
        {
            if (std::find(values.begin(), values.end(), value) == values.end())
            {
                values.push_back(value);
            }
        }

        std::string StripFinalExtension(std::string value)
        {
            const size_t slash = value.find_last_of('/');
            const size_t dot = value.find_last_of('.');
            if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
            {
                value.resize(dot);
            }
            return value;
        }

        std::vector<std::string> BuildAssetNameKeys(std::string_view value)
        {
            std::vector<std::string> keys;
            auto addKey = [&keys](std::string key) {
                if (!key.empty() && std::find(keys.begin(), keys.end(), key) == keys.end())
                {
                    keys.push_back(std::move(key));
                }
            };

            std::string normalized = NormalizeSlashes(std::string(value));
            normalized = ToLowerAscii(Trim(std::move(normalized)));
            addKey(normalized);
            addKey(StripFinalExtension(normalized));

            const size_t slash = normalized.find_last_of('/');
            if (slash != std::string::npos)
            {
                std::string basename = normalized.substr(slash + 1);
                addKey(basename);
                addKey(StripFinalExtension(std::move(basename)));
            }

            return keys;
        }

        bool NamesMatch(std::string_view left, std::string_view right)
        {
            const std::vector<std::string> leftKeys = BuildAssetNameKeys(left);
            const std::vector<std::string> rightKeys = BuildAssetNameKeys(right);
            for (const std::string& leftKey : leftKeys)
            {
                if (std::find(rightKeys.begin(), rightKeys.end(), leftKey) != rightKeys.end())
                {
                    return true;
                }
            }
            return false;
        }

        struct DescriptorPackGroup
        {
            fs::path blkPath;
            std::vector<fs::path> grpPaths;
            std::vector<fs::path> dxpPaths;
        };

        std::vector<DescriptorPackGroup> BuildDescriptorPackGroups(const std::vector<BlkResourceLink>& links)
        {
            std::vector<DescriptorPackGroup> groups;
            std::unordered_map<std::wstring, size_t> groupByBlk;

            for (const BlkResourceLink& link : links)
            {
                if (!link.matchedPath)
                {
                    continue;
                }

                const std::wstring blkKey = PathKey(link.blkPath);
                auto [it, inserted] = groupByBlk.emplace(blkKey, groups.size());
                if (inserted)
                {
                    DescriptorPackGroup group;
                    group.blkPath = link.blkPath;
                    groups.push_back(std::move(group));
                }

                DescriptorPackGroup& group = groups[it->second];
                if (link.expectedType == AssetType::Grp)
                {
                    AppendUniquePath(group.grpPaths, *link.matchedPath);
                }
                else if (link.expectedType == AssetType::Dxp)
                {
                    AppendUniquePath(group.dxpPaths, *link.matchedPath);
                }
            }

            return groups;
        }

        std::vector<fs::path> CollectPackPaths(const std::vector<ResourceRecord>& records, AssetType type)
        {
            std::vector<fs::path> paths;
            for (const ResourceRecord& record : records)
            {
                if (record.type == type)
                {
                    AppendUniquePath(paths, record.path);
                }
            }
            return paths;
        }

        std::vector<fs::path> BuildGrpCandidatePaths(
            const std::vector<DescriptorPackGroup>& groups,
            const std::vector<fs::path>& allGrpPaths)
        {
            std::vector<fs::path> candidates;
            for (const DescriptorPackGroup& group : groups)
            {
                for (const fs::path& path : group.grpPaths)
                {
                    AppendUniquePath(candidates, path);
                }
            }
            for (const fs::path& path : allGrpPaths)
            {
                AppendUniquePath(candidates, path);
            }
            return candidates;
        }

        std::vector<fs::path> BuildDxpCandidatePaths(
            const std::vector<DescriptorPackGroup>& groups,
            const std::vector<fs::path>& allDxpPaths,
            const std::optional<fs::path>& matchedGrpPath)
        {
            std::vector<fs::path> candidates;

            if (matchedGrpPath)
            {
                for (const DescriptorPackGroup& group : groups)
                {
                    const bool groupContainsGrp = std::find_if(
                        group.grpPaths.begin(),
                        group.grpPaths.end(),
                        [&](const fs::path& path) {
                            return SamePath(path, *matchedGrpPath);
                        }) != group.grpPaths.end();

                    if (!groupContainsGrp)
                    {
                        continue;
                    }

                    for (const fs::path& path : group.dxpPaths)
                    {
                        AppendUniquePath(candidates, path);
                    }
                }
            }

            for (const fs::path& path : allDxpPaths)
            {
                AppendUniquePath(candidates, path);
            }

            return candidates;
        }

        void AddWarning(DescriptorResourceLinkBuildResult& result, std::string warning)
        {
            if (result.warnings.size() < MaxWarnings)
            {
                result.warnings.push_back(std::move(warning));
            }
        }

        std::shared_ptr<PackContents> LoadRelationshipPackContents(
            const fs::path& path,
            AssetType type,
            uintmax_t maxPackBytes,
            std::unordered_map<std::wstring, std::shared_ptr<PackContents>>& cache,
            DescriptorResourceLinkBuildResult& result)
        {
            const std::wstring key = PathKey(path);
            if (const auto it = cache.find(key); it != cache.end())
            {
                return it->second;
            }

            std::shared_ptr<PackContents> contents;
            if (type == AssetType::Grp)
            {
                contents = ParseGrpContents(path, static_cast<size_t>(maxPackBytes));
            }
            else if (type == AssetType::Dxp)
            {
                contents = ParseDxpContents(path, static_cast<size_t>(maxPackBytes));
            }

            cache.emplace(key, contents);

            if (!contents)
            {
                AddWarning(result, "Could not parse pack: " + Platform::PathToUtf8(path.filename()));
                return nullptr;
            }

            if (!contents->error.empty())
            {
                AddWarning(result, contents->error);
                return contents;
            }

            if (type == AssetType::Grp)
            {
                ++result.grpPacksParsed;
            }
            else if (type == AssetType::Dxp)
            {
                ++result.dxpPacksParsed;
            }

            return contents;
        }

        struct PackEntryMatch
        {
            fs::path packPath;
            std::string entryName;
        };

        std::optional<PackEntryMatch> FindEntryInPacks(
            const std::vector<fs::path>& packPaths,
            AssetType type,
            std::string_view entryName,
            uintmax_t maxPackBytes,
            std::unordered_map<std::wstring, std::shared_ptr<PackContents>>& cache,
            DescriptorResourceLinkBuildResult& result)
        {
            for (const fs::path& packPath : packPaths)
            {
                const std::shared_ptr<PackContents> contents =
                    LoadRelationshipPackContents(packPath, type, maxPackBytes, cache, result);

                if (!contents || !contents->error.empty())
                {
                    continue;
                }

                for (const SubAssetRecord& entry : contents->entries)
                {
                    if (NamesMatch(entry.name, entryName))
                    {
                        return PackEntryMatch{ packPath, entry.name };
                    }
                }
            }

            return std::nullopt;
        }
    }

    DescriptorResourceLinkBuildResult BuildDescriptorResourceLinks(
        const std::vector<ResourceRecord>& records,
        const std::vector<BlkResourceLink>* blkLinks,
        uintmax_t maxDescriptorBytes,
        uintmax_t maxPackBytes)
    {
        DescriptorResourceLinkBuildResult result;

        std::vector<BlkResourceLink> builtBlkLinks;
        if (blkLinks == nullptr)
        {
            BlkResourceLinkBuildResult blkResult = BuildBlkResourceLinks(records);
            builtBlkLinks = std::move(blkResult.links);
            blkLinks = &builtBlkLinks;

            for (const std::string& warning : blkResult.warnings)
            {
                AddWarning(result, "BLK relationship warning: " + warning);
            }
        }

        const std::vector<DescriptorPackGroup> packGroups = BuildDescriptorPackGroups(*blkLinks);
        const std::vector<fs::path> allGrpPaths = CollectPackPaths(records, AssetType::Grp);
        const std::vector<fs::path> allDxpPaths = CollectPackPaths(records, AssetType::Dxp);
        const std::vector<fs::path> grpCandidates = BuildGrpCandidatePaths(packGroups, allGrpPaths);

        std::unordered_map<std::wstring, std::shared_ptr<PackContents>> packCache;

        for (const ResourceRecord& record : records)
        {
            if (record.type != AssetType::Descriptor)
            {
                continue;
            }

            if (record.sizeBytes > maxDescriptorBytes)
            {
                ++result.descriptorFilesSkipped;
                AddWarning(result, "Skipped large descriptor: " + Platform::PathToUtf8(record.path.filename()));
                continue;
            }

            const std::shared_ptr<PackContents> descriptorContents =
                ParseDescriptorContents(record.path, static_cast<size_t>(maxDescriptorBytes));

            if (!descriptorContents)
            {
                ++result.parseErrors;
                AddWarning(result, "Could not parse descriptor: " + Platform::PathToUtf8(record.path.filename()));
                continue;
            }

            if (!descriptorContents->error.empty())
            {
                ++result.parseErrors;
                AddWarning(result, descriptorContents->error);
                continue;
            }

            ++result.descriptorFilesParsed;

            for (const SubAssetRecord& descriptorEntry : descriptorContents->entries)
            {
                DescriptorResourceLink link;
                link.descriptorPath = record.path;
                link.resourceName = descriptorEntry.name;
                link.resId = descriptorEntry.resId;
                link.textureNames = descriptorEntry.textureNames;
                link.materialNames = descriptorEntry.materialNames;
                link.materialTextureRefs = descriptorEntry.dependencyIds;

                const std::optional<PackEntryMatch> grpMatch = FindEntryInPacks(
                    grpCandidates,
                    AssetType::Grp,
                    descriptorEntry.name,
                    maxPackBytes,
                    packCache,
                    result);

                if (grpMatch)
                {
                    link.matchedGrpPath = grpMatch->packPath;
                    link.matchedGrpEntryName = grpMatch->entryName;
                }

                const std::vector<fs::path> dxpCandidates = BuildDxpCandidatePaths(
                    packGroups,
                    allDxpPaths,
                    link.matchedGrpPath);

                std::vector<uint32_t> textureIndices;
                if (!descriptorEntry.dependencyIds.empty())
                {
                    for (const uint32_t textureIndex : descriptorEntry.dependencyIds)
                    {
                        AppendUniqueUInt(textureIndices, textureIndex);
                    }
                }
                else
                {
                    for (size_t textureIndex = 0; textureIndex < descriptorEntry.textureNames.size(); ++textureIndex)
                    {
                        AppendUniqueUInt(textureIndices, static_cast<uint32_t>(textureIndex));
                    }
                }

                for (const uint32_t textureIndex : textureIndices)
                {
                    if (textureIndex >= descriptorEntry.textureNames.size())
                    {
                        std::ostringstream warning;
                        warning << "Descriptor " << Platform::PathToUtf8(record.path.filename())
                                << " resource " << descriptorEntry.name
                                << " references missing texture index " << textureIndex << '.';
                        AddWarning(result, warning.str());
                        continue;
                    }

                    DescriptorTextureLink textureLink;
                    textureLink.textureIndex = textureIndex;
                    textureLink.textureName = descriptorEntry.textureNames[textureIndex];

                    const std::optional<PackEntryMatch> dxpMatch = FindEntryInPacks(
                        dxpCandidates,
                        AssetType::Dxp,
                        textureLink.textureName,
                        maxPackBytes,
                        packCache,
                        result);

                    if (dxpMatch)
                    {
                        textureLink.matchedDxpPath = dxpMatch->packPath;
                        textureLink.matchedDxpEntryName = dxpMatch->entryName;
                    }

                    link.textureLinks.push_back(std::move(textureLink));
                }

                result.links.push_back(std::move(link));
            }
        }

        return result;
    }
}
